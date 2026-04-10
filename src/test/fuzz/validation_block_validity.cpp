// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/merkle.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/block_validity.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <validation.h>

static ValidationBlockValidityTestingSetup* g_setup;

void initialize_validation_block_validity()
{
    static auto setup = MakeNoLogFileContext<ValidationBlockValidityTestingSetup>();
    g_setup = setup.get();
}

namespace {
CScript ConsumeVarietyScript(FuzzedDataProvider& fuzzed_data_provider)
{
    CScript script;
    CallOneOf(
        fuzzed_data_provider,
        [&] { script << OP_TRUE; },
        [&] {
            // P2WSH(OP_TRUE)
            script = GetScriptForDestination(WitnessV0ScriptHash(CScript() << OP_TRUE));
        },
        [&] {
            // P2SH(OP_TRUE)
            script = GetScriptForDestination(ScriptHash(CScript() << OP_TRUE));
        },
        [&] {
            // P2TR output (OP_1 + 32-byte program)
            const std::vector<unsigned char> bytes = fuzzed_data_provider.ConsumeBytes<unsigned char>(32);
            if (bytes.size() == 32) {
                script << OP_1 << bytes;
            } else {
                script << OP_TRUE;
            }
        },
        [&] {
            const std::vector<unsigned char> bytes = fuzzed_data_provider.ConsumeBytes<unsigned char>(10);
            script << bytes;
        });
    return script;
}

CBlock MakeBlock(const node::NodeContext& node, FuzzedDataProvider& fuzzed_data_provider)
{
    const Chainstate& chainstate = node.chainman->ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    assert(tip);
    if (fuzzed_data_provider.ConsumeBool()) {
        CBlock block;
        block.nVersion = VERSIONBITS_LAST_OLD_BLOCK_VERSION;
        block.hashPrevBlock = *tip->phashBlock;
        block.nTime = tip->GetBlockTime() + 1;
        block.nBits = tip->nBits;
        block.nNonce = 0;
        CMutableTransaction coinbase_tx;
        coinbase_tx.vin.resize(1);
        // BIP34: block height must be the first item in the coinbase scriptSig.
        coinbase_tx.vin[0].scriptSig = CScript() << (tip->nHeight + 1) << OP_0;
        coinbase_tx.vout.resize(1);
        coinbase_tx.vout[0].nValue = GetBlockSubsidy(tip->nHeight + 1, node.chainman->GetParams().GetConsensus());
        coinbase_tx.vout[0].scriptPubKey = ConsumeVarietyScript(fuzzed_data_provider);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase_tx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        return block;
    } else {
        node::BlockAssembler::Options assembler_options;
        assembler_options.coinbase_output_script = CScript() << OP_TRUE;
        assembler_options.include_dummy_extranonce = true;
        if (node.args) {
            node::ApplyArgsManOptions(*node.args, assembler_options);
        }
        auto block_ptr = PrepareBlock(node, assembler_options);
        assert(block_ptr);
        return *block_ptr;
    }
}

void MutateHeader(CBlock& block, FuzzedDataProvider& fuzzed_data_provider)
{
    if (fuzzed_data_provider.remaining_bytes() == 0) return;
    CallOneOf(
        fuzzed_data_provider,
        [&] { block.nVersion = fuzzed_data_provider.ConsumeIntegral<int32_t>(); },
        [&] { block.nTime = fuzzed_data_provider.ConsumeIntegral<uint32_t>(); },
        [&] { block.nBits = fuzzed_data_provider.ConsumeIntegral<uint32_t>(); },
        [&] {
            // Force nonce to 0 or fuzz it.
            block.nNonce = fuzzed_data_provider.ConsumeBool() ? 0 : fuzzed_data_provider.ConsumeIntegral<uint32_t>();
        },
        [&] { block.hashPrevBlock = ConsumeUInt256(fuzzed_data_provider); });
}

void MutateTransactionsStateless(CBlock& block, FuzzedDataProvider& fuzzed_data_provider, CBlockUndo* blockundo = nullptr)
{
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 5)
    {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                // Duplicate a random transaction and its corresponding undo entry.
                if (!block.vtx.empty()) {
                    const size_t tx_index = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, block.vtx.size() - 1);
                    block.vtx.push_back(block.vtx[tx_index]);
                    if (blockundo) {
                        if (tx_index == 0) {
                            // Coinbase copy lands at a non-zero position, so ConnectBlock will
                            // look up its undo entry. vprevout must match vin size.
                            CTxUndo txundo;
                            for (size_t j = 0; j < block.vtx[tx_index]->vin.size(); ++j) {
                                txundo.vprevout.emplace_back(CTxOut(0, CScript()), 0, false);
                            }
                            blockundo->vtxundo.push_back(std::move(txundo));
                        } else if (tx_index - 1 < blockundo->vtxundo.size()) {
                            blockundo->vtxundo.push_back(blockundo->vtxundo[tx_index - 1]);
                        }
                    }
                }
            },
            [&] {
                // Swap two random transactions and their corresponding undo entries.
                if (block.vtx.size() >= 2) {
                    const size_t tx_index1 = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, block.vtx.size() - 1);
                    const size_t tx_index2 = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, block.vtx.size() - 1);
                    std::swap(block.vtx[tx_index1], block.vtx[tx_index2]);
                    if (blockundo && tx_index1 > 0 && tx_index2 > 0 &&
                        tx_index1 - 1 < blockundo->vtxundo.size() &&
                        tx_index2 - 1 < blockundo->vtxundo.size()) {
                        std::swap(blockundo->vtxundo[tx_index1 - 1], blockundo->vtxundo[tx_index2 - 1]);
                    }
                }
            },
            [&] {
                block.vtx.clear();
                if (blockundo) blockundo->vtxundo.clear();
            },
            [&] {
                // Insert a transaction with fuzz-derived inputs and outputs at a random position.
                // When blockundo is set, only insert at positions >= 1 to avoid displacing the
                // coinbase to vtx[1], which would require its undo entry to match its vin size.
                const size_t min_pos = blockundo ? 1 : 0;
                if (block.vtx.size() <= min_pos) return;
                CMutableTransaction malformed_tx;
                const size_t vin_size = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 5);
                malformed_tx.vin.resize(vin_size);
                for (auto& vin : malformed_tx.vin) {
                    vin.prevout.hash = Txid::FromUint256(ConsumeUInt256(fuzzed_data_provider));
                    vin.prevout.n = fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(0, 10);
                }
                malformed_tx.vout.resize(fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, 5));
                for (auto& vout : malformed_tx.vout) {
                    vout.nValue = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY + 1);
                    vout.scriptPubKey = ConsumeVarietyScript(fuzzed_data_provider);
                }
                const size_t insertion_pos = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(min_pos, block.vtx.size() - 1);
                block.vtx.insert(block.vtx.begin() + insertion_pos, MakeTransactionRef(std::move(malformed_tx)));
                if (blockundo) {
                    CTxUndo txundo;
                    for (size_t j = 0; j < vin_size; ++j) {
                        txundo.vprevout.emplace_back(CTxOut(0, CScript()), 0, false);
                    }
                    blockundo->vtxundo.insert(blockundo->vtxundo.begin() + insertion_pos - 1, std::move(txundo));
                }
            },
            [&] {
                // Remove a random transaction, and its corresponding undo entry when present.
                if (!block.vtx.empty()) {
                    const size_t tx_index = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, block.vtx.size() - 1);
                    block.vtx.erase(block.vtx.begin() + tx_index);
                    if (blockundo) {
                        if (tx_index == 0 && !blockundo->vtxundo.empty()) {
                            blockundo->vtxundo.erase(blockundo->vtxundo.begin());
                        } else if (tx_index > 0 && tx_index - 1 < blockundo->vtxundo.size()) {
                            blockundo->vtxundo.erase(blockundo->vtxundo.begin() + tx_index - 1);
                        }
                    }
                }
            });
    }
}

void MutateTransactionsContextual(CBlock& block, FuzzedDataProvider& fuzzed_data_provider, CBlockUndo* blockundo = nullptr)
{
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 5)
    {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                // Append a signed transaction spending a randomly selected matured coinbase output.
                if (g_setup->m_coinbase_txns.empty()) return;
                const int next_height = WITH_LOCK(g_setup->m_node.chainman->GetMutex(),
                                                  return g_setup->m_node.chainman->ActiveChain().Height()) +
                                        1;
                if (next_height <= COINBASE_MATURITY) return;
                const size_t n_matured = static_cast<size_t>(next_height - COINBASE_MATURITY);
                if (n_matured > g_setup->m_coinbase_txns.size()) return;
                const size_t coinbase_idx = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, n_matured - 1);
                const auto& matured_coinbase_tx = g_setup->m_coinbase_txns[coinbase_idx];
                // vout[0] must exist and have enough value for at least a 1-sat output after a fuzzed fee.
                if (matured_coinbase_tx->vout.empty() || matured_coinbase_tx->vout[0].nValue <= 1000) return;
                const CAmount fee = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(1000, matured_coinbase_tx->vout[0].nValue - 1);
                auto [valid_tx, actual_fee] = g_setup->CreateValidTransaction(
                    /*input_transactions=*/{matured_coinbase_tx},
                    /*inputs=*/{COutPoint(matured_coinbase_tx->GetHash(), 0)},
                    /*input_height=*/static_cast<int>(coinbase_idx + 1),
                    /*input_signing_keys=*/{g_setup->coinbaseKey},
                    /*outputs=*/{CTxOut(matured_coinbase_tx->vout[0].nValue - fee, CScript() << OP_TRUE)},
                    /*feerate=*/std::nullopt,
                    /*fee_output=*/std::nullopt);
                block.vtx.push_back(MakeTransactionRef(std::move(valid_tx)));
                if (blockundo) {
                    CTxUndo txundo;
                    txundo.vprevout.emplace_back(matured_coinbase_tx->vout[0],
                                                 static_cast<uint32_t>(coinbase_idx + 1),
                                                 /*fCoinBase=*/true);
                    blockundo->vtxundo.push_back(std::move(txundo));
                }
            },
            [&] {
                // Mutate the coinbase (vtx[0]).
                if (block.vtx.empty() || block.vtx[0]->vin.empty()) return;
                CMutableTransaction mutable_coinbase_tx(*block.vtx[0]);
                CallOneOf(
                    fuzzed_data_provider,
                    [&] {
                        // Push the fuzzed block height.
                        mutable_coinbase_tx.vin[0].scriptSig = CScript() << fuzzed_data_provider.ConsumeIntegral<int>();
                    },
                    [&] {
                        // Exceed the 100-byte coinbase scriptSig consensus limit.
                        const size_t script_len = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(101, 128);
                        const std::vector<unsigned char> bytes = fuzzed_data_provider.ConsumeBytes<unsigned char>(script_len);
                        mutable_coinbase_tx.vin[0].scriptSig = CScript{bytes.begin(), bytes.end()};
                    },
                    [&] {
                        // Give the coinbase multiple outputs.
                        mutable_coinbase_tx.vout.resize(fuzzed_data_provider.ConsumeIntegralInRange<size_t>(2, 5));
                    });
                block.vtx[0] = MakeTransactionRef(std::move(mutable_coinbase_tx));
            });
    }
}

void MutateBlock(CBlock& block, FuzzedDataProvider& fuzzed_data_provider, CBlockUndo* blockundo = nullptr)
{
    MutateHeader(block, fuzzed_data_provider);
    MutateTransactionsContextual(block, fuzzed_data_provider, blockundo);
    MutateTransactionsStateless(block, fuzzed_data_provider, blockundo);
}

// Add a transaction to block spending a synthetic UTXO (not in the chainstate),
// with a matching CTxUndo entry for that transaction appended to blockundo.
void AddSyntheticSpend(CBlock& block, CBlockUndo& blockundo, FuzzedDataProvider& fuzzed_data_provider)
{
    const CScript script = CScript() << OP_TRUE;
    const CAmount value = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
    const uint32_t height = fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(0, 200);
    const bool is_coinbase = fuzzed_data_provider.ConsumeBool();
    const COutPoint prevout{Txid::FromUint256(ConsumeUInt256(fuzzed_data_provider)), 0};
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = prevout;
    tx.vout.resize(1);
    tx.vout[0].nValue = value > 1000 ? value - 1000 : 0;
    tx.vout[0].scriptPubKey = script;
    block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    CTxUndo txundo;
    txundo.vprevout.emplace_back(CTxOut(value, script), height, is_coinbase);
    blockundo.vtxundo.push_back(std::move(txundo));
}

void MutateUndo(const CBlock& block, CBlockUndo& blockundo, FuzzedDataProvider& fuzzed_data_provider)
{
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 5)
    {
        if (blockundo.vtxundo.empty()) break;
        const size_t undo_idx = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, blockundo.vtxundo.size() - 1);
        CTxUndo& txundo = blockundo.vtxundo[undo_idx];
        // Mutate a random coin's fields.
        if (txundo.vprevout.empty()) continue;
        const size_t coin_idx = fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, txundo.vprevout.size() - 1);
        Coin& coin = txundo.vprevout[coin_idx];
        CallOneOf(
            fuzzed_data_provider,
            [&] { coin.out.nValue = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY + 1); },
            [&] { coin.nHeight = fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(0, 1000); },
            [&] { coin.fCoinBase = fuzzed_data_provider.ConsumeBool(); });
    }
}
} // namespace

FUZZ_TARGET(validation_block_validity_with_undo, .init = initialize_validation_block_validity)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    SteadyClockContext steady_ctx{};
    SetMockTime(WITH_LOCK(g_setup->m_node.chainman->GetMutex(),
                          return g_setup->m_node.chainman->ActiveTip()->Time()));
    Chainstate& chainstate = g_setup->m_chainstate;
    CBlock block;
    {
        LOCK(cs_main);
        block = MakeBlock(g_setup->m_node, fuzzed_data_provider);
    }
    // Build blockundo before mutating the block using one of two approaches:
    // - SpendBlock: derives undo data from real UTXOs in the chainstate.
    // - Synthetic: appends transactions spending UTXOs absent from the chainstate,
    //   supplying the coin data directly in the blockundo.
    CBlockUndo blockundo;
    if (fuzzed_data_provider.ConsumeBool()) {
        auto populated = g_setup->PopulateBlockUndo(block);
        if (populated) blockundo = std::move(*populated);
    } else {
        LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 5)
        {
            AddSyntheticSpend(block, blockundo, fuzzed_data_provider);
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }

    MutateBlock(block, fuzzed_data_provider, &blockundo);
    MutateUndo(block, blockundo, fuzzed_data_provider);
    if (fuzzed_data_provider.ConsumeBool()) {
        block.hashMerkleRoot = ConsumeUInt256(fuzzed_data_provider);
    } else {
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
    const bool check_pow = fuzzed_data_provider.ConsumeBool();
    const bool check_merkle = fuzzed_data_provider.ConsumeBool();
    LOCK(cs_main);
    const int height_before = chainstate.m_chain.Height();
    const BlockValidationState state = g_setup->TestValidityWithUndo(block, blockundo, check_pow, check_merkle);
    assert(chainstate.m_chain.Height() == height_before);
    assert(state.IsValid() + state.IsInvalid() + state.IsError() == 1);
}

FUZZ_TARGET(validation_block_validity, .init = initialize_validation_block_validity)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    SteadyClockContext steady_ctx{};
    SetMockTime(WITH_LOCK(g_setup->m_node.chainman->GetMutex(),
                          return g_setup->m_node.chainman->ActiveTip()->Time()));
    Chainstate& chainstate = g_setup->m_chainstate;
    CBlock block;
    {
        LOCK(cs_main);
        block = MakeBlock(g_setup->m_node, fuzzed_data_provider);
    }
    MutateBlock(block, fuzzed_data_provider);
    if (fuzzed_data_provider.ConsumeBool()) {
        block.hashMerkleRoot = ConsumeUInt256(fuzzed_data_provider);
    } else {
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
    const bool check_pow = fuzzed_data_provider.ConsumeBool();
    const bool check_merkle = fuzzed_data_provider.ConsumeBool();
    LOCK(cs_main);
    const int height_before = chainstate.m_chain.Height();
    const BlockValidationState state = TestBlockValidity(chainstate, block, check_pow, check_merkle);
    assert(chainstate.m_chain.Height() == height_before);
    assert(state.IsValid() + state.IsInvalid() + state.IsError() == 1);
    // Whenever SpendBlock succeeds (blockundo is populated), TestBlockValidityWithUndo
    // must agree with TestBlockValidity on the result.
    auto blockundo = g_setup->PopulateBlockUndo(block);
    if (state.IsValid()) assert(blockundo);
    if (blockundo) {
        const BlockValidationState state_with_undo = g_setup->TestValidityWithUndo(block, *blockundo, check_pow, check_merkle);
        assert(state_with_undo.IsValid() + state_with_undo.IsInvalid() + state_with_undo.IsError() == 1);
        assert(state_with_undo.IsValid() == state.IsValid());
        assert(chainstate.m_chain.Height() == height_before);
    }
}

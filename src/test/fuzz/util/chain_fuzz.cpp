// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/util/chain_fuzz.h>

#include <addresstype.h>
#include <consensus/merkle.h>
#include <node/kernel_notifications.h>
#include <pow.h>
#include <pubkey.h>
#include <test/fuzz/util.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <validationinterface.h>

#include <algorithm>
#include <memory>

const CScript P2SH_OP_TRUE = CScript() << OP_HASH160 << ToByteVector(ScriptHash(CScript() << OP_TRUE)) << OP_EQUAL;
const CScript P2SH_OP_TRUE_UNLOCK = CScript() << MakeUCharSpan(CScript() << OP_TRUE);

ChainValidationFuzzSetup::ChainValidationFuzzSetup(ChainType chain_type, TestOpts opts)
    : TestingSetup(chain_type, opts)
{
    Assert(EnableFuzzDeterminism());
    ResetChainman();
    auto [script, witness] = BuildP2TROpTrueScript();
    m_taproot_op_true = std::move(script);
    m_taproot_op_true_witness = std::move(witness);
    LoadCurrentChain();
    AddExtraTxsInMempool();
    node::BlockAssembler::Options options;
    options.coinbase_output_script = P2WSH_OP_TRUE;
    MineBlock(m_node, options);
    Assert(m_node.chainman->ActiveChainstate().GetMempool()->size() == 0);
    LOCK(::cs_main);
    auto& chainstate = Assert(m_node.chainman)->ActiveChainstate();
    LoadCurrentBlock(chainstate, chainstate.m_chain.Tip());
    chainstate.ForceFlushStateToDisk();
}

void ChainValidationFuzzSetup::ResetChainman()
{
    Assert(m_node.chainman);
    SetMockTime(m_node.chainman->GetParams().GenesisBlock().Time());
    m_node.chainman.reset();
    m_node.notifications->m_shutdown_on_fatal_error = false;
    m_make_chainman();
    LoadVerifyActivateChainstate();
    node::BlockAssembler::Options options;
    options.coinbase_output_script = P2WSH_OP_TRUE;
    options.include_dummy_extranonce = true;
    for (int i = 0; i < 2 * COINBASE_MATURITY; ++i) {
        MineBlock(m_node, options);
    }
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
}

void ChainValidationFuzzSetup::LoadCurrentChain()
{
    m_all_utxo_inputs.clear();
    m_list_blocks.clear();
    {
        LOCK(::cs_main);
        auto& chainstate = Assert(m_node.chainman)->ActiveChainstate();
        Assert(chainstate.GetMempool());
        auto* tip = chainstate.m_chain.Tip();
        Assert(tip != nullptr);
        for (auto* b = tip; b != nullptr; b = b->pprev)
            LoadCurrentBlock(chainstate, b);
    }
    std::reverse(m_all_utxo_inputs.begin(), m_all_utxo_inputs.end());
}

void ChainValidationFuzzSetup::LoadCurrentBlock(Chainstate& chainstate, CBlockIndex* current_block)
{
    Assert(current_block != nullptr);
    Assert(current_block->nHeight >= 0);
    auto block = std::make_shared<CBlock>();
    Assert(chainstate.m_blockman.ReadBlock(*block, *current_block));
    if (m_list_blocks.size() <= static_cast<size_t>(current_block->nHeight))
        m_list_blocks.resize(current_block->nHeight + 1);
    m_list_blocks[current_block->nHeight] = block;
    for (const auto& tx : block->vtx) {
        for (size_t i = 0; i < tx->vout.size(); ++i) {
            if (tx->vout[i].scriptPubKey.IsUnspendable()) continue;
            m_all_utxo_inputs.push_back(MakeSpendingInput(*tx, i));
        }
    }
}

void ChainValidationFuzzSetup::AddExtraTxsInMempool()
{
    auto& chainman = *Assert(m_node.chainman);
    auto& mempool = *Assert(chainman.ActiveChainstate().GetMempool());
    Assert(mempool.size() == 0);
    for (unsigned i = 1; i <= 10; ++i) {
        CMutableTransaction mtx;
        mtx.version = CTransaction::CURRENT_VERSION;
        mtx.vin.resize(1);
        mtx.vin[0] = m_all_utxo_inputs[i];
        mtx.vout.resize(4);
        mtx.vout[0] = {CAmount(15 * COIN), P2WSH_OP_TRUE};
        mtx.vout[1] = {CAmount(15 * COIN), P2SH_OP_TRUE};
        mtx.vout[2] = {CAmount(10 * COIN), m_taproot_op_true};
        mtx.vout[3] = {CAmount(10 * COIN), CScript()};
        const auto result = WITH_LOCK(::cs_main, return chainman.ProcessTransaction(MakeTransactionRef(mtx)));
        Assert(result.m_result_type == MempoolAcceptResult::ResultType::VALID);
        Assert(mempool.size() == i);
        mempool.PrioritiseTransaction(mtx.GetHash(), COIN);
    }
}

CTxIn ChainValidationFuzzSetup::MakeSpendingInput(const CTransaction& tx, unsigned vout_index) const
{
    Assert(vout_index < tx.vout.size());
    CTxIn input{COutPoint(tx.GetHash(), vout_index)};
    const CScript& spk = tx.vout[vout_index].scriptPubKey;
    if (spk.IsUnspendable()) {
    } else if (spk == P2WSH_OP_TRUE) {
        input.scriptWitness.stack.push_back(WITNESS_STACK_ELEM_OP_TRUE);
    } else if (spk == P2SH_OP_TRUE) {
        input.scriptSig = P2SH_OP_TRUE_UNLOCK;
    } else if (spk == CScript()) {
        input.scriptSig = CScript() << OP_TRUE;
    } else if (spk == m_taproot_op_true) {
        input.scriptWitness.stack = m_taproot_op_true_witness;
    }
    return input;
}

void ChainValidationFuzzSetup::ConsumeOutputs(FuzzedDataProvider& fuzzed_data_provider, CMutableTransaction& mtx) const
{
    int num_output = fuzzed_data_provider.ConsumeIntegralInRange<int>(1, 10);
    mtx.vout.resize(num_output);
    for (int i = 0; i < num_output; ++i) {
        mtx.vout[i].nValue = fuzzed_data_provider.ConsumeIntegral<int64_t>();
        switch (fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 4)) {
        case 0: mtx.vout[i].scriptPubKey = P2WSH_OP_TRUE; break;
        case 1: mtx.vout[i].scriptPubKey = P2SH_OP_TRUE; break;
        case 2: mtx.vout[i].scriptPubKey = m_taproot_op_true; break;
        case 3: mtx.vout[i].scriptPubKey = CScript(); break;
        default: {
            mtx.vout[i].scriptPubKey << ConsumeRandomLengthByteVector<unsigned char>(fuzzed_data_provider, 100);
            break;
        }
        }
    }
}

CTransactionRef ChainValidationFuzzSetup::ConsumeCoinbaseTx(FuzzedDataProvider& fuzzed_data_provider, unsigned target_height) const
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
    if (fuzzed_data_provider.ConsumeBool()) {
        mtx.vin[0].scriptSig = CScript() << target_height << OP_0;
    } else {
        mtx.vin[0].scriptSig << ConsumeRandomLengthByteVector<unsigned char>(fuzzed_data_provider, 100);
    }
    ConsumeOutputs(fuzzed_data_provider, mtx);
    if (fuzzed_data_provider.ConsumeBool()) mtx.version = fuzzed_data_provider.ConsumeIntegral<int32_t>();
    if (fuzzed_data_provider.ConsumeBool()) mtx.nLockTime = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    // TODO: Add coinbase output to additional utxo's and make them available for spending when they are matured.
    return MakeTransactionRef(mtx);
}

CTransactionRef ChainValidationFuzzSetup::ConsumeNonCoinbaseTx(FuzzedDataProvider& fuzzed_data_provider, std::vector<CTxIn>& additional_utxo)
{
    CMutableTransaction mtx;
    int num_input = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10);
    mtx.vin.resize(num_input);
    if (num_input > 0) {
        Assert(!m_all_utxo_inputs.empty());
        const size_t total_utxos = m_all_utxo_inputs.size() + additional_utxo.size();
        for (int i = 0; i < num_input; ++i) {
            uint32_t idx = fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(0, total_utxos - 1);
            mtx.vin[i] = idx < m_all_utxo_inputs.size() ? m_all_utxo_inputs[idx] : additional_utxo[idx - m_all_utxo_inputs.size()];
            MutateTxInput(fuzzed_data_provider, mtx.vin[i]);
        }
    }
    ConsumeOutputs(fuzzed_data_provider, mtx);
    if (fuzzed_data_provider.ConsumeBool()) mtx.version = fuzzed_data_provider.ConsumeIntegral<int32_t>();
    if (fuzzed_data_provider.ConsumeBool()) mtx.nLockTime = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    auto tx_ref = MakeTransactionRef(mtx);
    for (size_t i = 0; i < tx_ref->vout.size(); ++i)
        additional_utxo.emplace_back(MakeSpendingInput(*tx_ref, i));
    return tx_ref;
}

void ChainValidationFuzzSetup::AddBlockTransactions(FuzzedDataProvider& fuzzed_data_provider, CBlock& block, std::vector<CTxIn>& additional_utxo, unsigned target_height)
{
    block.vtx.push_back(ConsumeCoinbaseTx(fuzzed_data_provider, target_height));
    int num_tx = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 5);
    for (int i = 0; i < num_tx; ++i)
        block.vtx.push_back(ConsumeNonCoinbaseTx(fuzzed_data_provider, additional_utxo));
    if (num_tx > 0) m_node.chainman->GenerateCoinbaseCommitment(block, nullptr);
}

void ChainValidationFuzzSetup::FindValidNonce(CBlock& block) const
{
    const uint32_t chain_bits = m_list_blocks.back()->nBits;
    const auto& consensus = m_node.chainman->GetConsensus();
    const auto& blockman = m_node.chainman->m_blockman;
    static constexpr uint32_t nonce_limit{1u << 20};
    for (block.nNonce = 0; block.nNonce < nonce_limit; ++block.nNonce) {
        if (CheckProofOfWork(block.GetHash(), chain_bits, consensus) && !blockman.LookupBlockIndex(block.GetHash())) return;
    }
    Assert(false); // No valid nonce found within nonce_limit iterations (should not happen at low difficulty).
}

CBlock ChainValidationFuzzSetup::ConsumeBlock(FuzzedDataProvider& fuzzed_data_provider, const CBlock& prev_block, unsigned target_height, std::vector<CTxIn>& additional_utxo, bool force_valid_block)
{
    CBlock block;
    static_cast<CBlockHeader&>(block) = InitBlockHeader(prev_block);
    if (!force_valid_block) MutateBlockHeader(fuzzed_data_provider, block);
    AddBlockTransactions(fuzzed_data_provider, block, additional_utxo, target_height);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    if (!force_valid_block && fuzzed_data_provider.ConsumeBool()) block.hashMerkleRoot = ConsumeUInt256(fuzzed_data_provider);
    const auto& blockman = m_node.chainman->m_blockman;
    bool need_valid_nonce = force_valid_block || fuzzed_data_provider.ConsumeBool() || blockman.LookupBlockIndex(block.GetHash());
    if (need_valid_nonce)
        FindValidNonce(block);
    else
        block.nNonce = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    return block;
}

std::pair<CScript, std::vector<std::vector<uint8_t>>> BuildP2TROpTrueScript()
{
    uint256 leaf_hash = ComputeTapleafHash(0xc0, MakeUCharSpan(CScript() << OP_TRUE));
    uint256 internal_key{std::vector<uint8_t>(32, 1)};
    auto tweaked = XOnlyPubKey(internal_key).CreateTapTweak(&leaf_hash);
    Assert(tweaked.has_value());
    std::vector<uint8_t> control;
    control.reserve(33);
    control.push_back(0xc0 | tweaked->second);
    const auto key_bytes = ToByteVector(internal_key);
    control.insert(control.end(), key_bytes.begin(), key_bytes.end());
    CScript script = CScript() << OP_1 << ToByteVector(tweaked->first);
    std::vector<std::vector<uint8_t>> witness;
    witness.emplace_back(ToByteVector(CScript() << OP_TRUE));
    witness.emplace_back(std::move(control));
    return {script, witness};
}

void MutateTxInput(FuzzedDataProvider& fuzzed_data_provider, CTxIn& txin)
{
    if (fuzzed_data_provider.ConsumeBool()) txin.nSequence = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    if (fuzzed_data_provider.ConsumeBool()) txin.prevout.n = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    if (fuzzed_data_provider.ConsumeBool()) txin.prevout.hash = Txid::FromUint256(ConsumeUInt256(fuzzed_data_provider));
    if (fuzzed_data_provider.ConsumeBool()) txin.scriptSig << ConsumeRandomLengthByteVector<unsigned char>(fuzzed_data_provider, 100);
    if (fuzzed_data_provider.ConsumeBool()) {
        txin.scriptWitness.stack.clear();
        int num_wit = fuzzed_data_provider.ConsumeIntegralInRange<int>(0, 10);
        for (int j = 0; j < num_wit; ++j)
            txin.scriptWitness.stack.push_back(ConsumeRandomLengthByteVector<unsigned char>(fuzzed_data_provider, 100));
    }
}

CBlockHeader InitBlockHeader(const CBlock& prev_block)
{
    CBlockHeader header;
    header.nVersion = prev_block.nVersion;
    header.hashPrevBlock = prev_block.GetHash();
    header.nTime = prev_block.nTime + 2;
    header.nBits = prev_block.nBits;
    return header;
}

void MutateBlockHeader(FuzzedDataProvider& fuzzed_data_provider, CBlockHeader& header)
{
    if (fuzzed_data_provider.ConsumeBool()) header.nVersion = fuzzed_data_provider.ConsumeIntegral<int32_t>();
    if (fuzzed_data_provider.ConsumeBool()) header.hashPrevBlock = ConsumeUInt256(fuzzed_data_provider);
    if (fuzzed_data_provider.ConsumeBool()) header.nTime = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    if (fuzzed_data_provider.ConsumeBool()) header.nBits = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
}

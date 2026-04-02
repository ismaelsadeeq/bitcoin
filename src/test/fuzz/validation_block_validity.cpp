// Copyright (c) present The Bitcoin Core developers
// Distributed under the MIT software license.

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <test/util/time.h>
#include <validation.h>

static const TestChain100Setup* g_setup;

void initialize_validation_block_validity()
{
    static const auto setup = MakeNoLogFileContext<TestChain100Setup>();
    g_setup = setup.get();
}

namespace {
CBlock MakeBlock(const Chainstate& cs)
{
    CBlock block;
    const CBlockIndex* tip = cs.m_chain.Tip();
    assert(tip);
    block.nVersion = 1;
    block.hashPrevBlock = *tip->phashBlock;
    block.nTime = tip->GetBlockTime() + 1;
    block.nBits = tip->nBits;
    block.nNonce = 0;
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig = CScript() << OP_TRUE;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 50 * COIN;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

void MutateBlock(CBlock& block, FuzzedDataProvider& fuzzed_data_provider)
{
    if (fuzzed_data_provider.ConsumeBool()) block.nVersion = fuzzed_data_provider.ConsumeIntegral<int32_t>();
    if (fuzzed_data_provider.ConsumeBool()) block.nTime    = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    if (fuzzed_data_provider.ConsumeBool()) block.nBits    = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    if (fuzzed_data_provider.ConsumeBool()) block.nNonce   = fuzzed_data_provider.ConsumeIntegral<uint32_t>();
    if (fuzzed_data_provider.ConsumeBool()) block.hashPrevBlock = ConsumeUInt256(fuzzed_data_provider);
    if (fuzzed_data_provider.ConsumeBool()) {
        block.hashMerkleRoot = ConsumeUInt256(fuzzed_data_provider);
    } else {
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
    if (!block.vtx.empty() && fuzzed_data_provider.ConsumeBool()) {
        CMutableTransaction coinbase(*block.vtx[0]);
        if (!coinbase.vout.empty() && fuzzed_data_provider.ConsumeBool()) {
            coinbase.vout[0].nValue =
                fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
        }
        if (!coinbase.vin.empty() && fuzzed_data_provider.ConsumeBool()) {
            auto bytes = fuzzed_data_provider.ConsumeBytes<unsigned char>(20);
            coinbase.vin[0].scriptSig = CScript(bytes.begin(), bytes.end());
        }
        block.vtx[0] = MakeTransactionRef(std::move(coinbase));
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }
}
} // namespace

FUZZ_TARGET(test_block_validity, .init = initialize_validation_block_validity)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    SteadyClockContext steady_ctx{};
    SetMockTime(WITH_LOCK(g_setup->m_node.chainman->GetMutex(),
                          return g_setup->m_node.chainman->ActiveTip()->Time()));
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    Chainstate& chainstate = g_setup->m_node.chainman->ActiveChainstate();
    LOCK(cs_main);
    CBlock block = MakeBlock(chainstate);
    MutateBlock(block, fuzzed_data_provider);
    bool check_pow    = fuzzed_data_provider.ConsumeBool();
    bool check_merkle = fuzzed_data_provider.ConsumeBool();
    int height_before = chainstate.m_chain.Height();
    auto state = TestBlockValidity(chainstate, block, check_pow, check_merkle);
    assert(chainstate.m_chain.Height() == height_before);
    assert(state.IsValid() + state.IsInvalid() + state.IsError() == 1);
}

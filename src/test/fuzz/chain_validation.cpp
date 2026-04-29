// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/fuzz/fuzz.h>
#include <test/fuzz/util/chain_fuzz.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <validationinterface.h>

namespace {

ChainValidationFuzzSetup* g_setup;

static void initialize_setup()
{
    static const auto setup = MakeNoLogFileContext<ChainValidationFuzzSetup>(
        ChainType::REGTEST, TestOpts{.extra_args = {"-minrelaytxfee=0", "-acceptnonstdtxn"}});
    g_setup = setup.get();
}

FUZZ_TARGET(chainstate_connect_block, .init = initialize_setup)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    SetMockTime(g_setup->LastBlock()->GetBlockTime() + 2);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    Chainstate& active_chainstate = g_setup->m_node.chainman->ActiveChainstate();
    CBlockIndex* active_tip = active_chainstate.m_chain.Tip();
    Assert(active_tip != nullptr);
    std::vector<CTxIn> additional_utxo;
    CBlock block;
    {
        LOCK(::cs_main);
        block = g_setup->ConsumeBlock(fuzzed_data_provider, *g_setup->LastBlock(), active_tip->nHeight + 1, additional_utxo);
    }
    uint256 current_hash = block.GetHash();
    CBlockIndex new_index(block);
    new_index.pprev = active_tip;
    new_index.nHeight = active_tip->nHeight + 1;
    new_index.phashBlock = &current_hash;
    BlockValidationState state;
    bool success;
    {
        LOCK(::cs_main);
        CCoinsViewCache active_coins(&active_chainstate.CoinsTip());
        success = active_chainstate.ConnectBlock(block, state, &new_index, active_coins, /*fJustCheck=*/true);
    }
    Assert(success == state.IsValid());
}

// Tests ActivateBestChainStep across a forward extension and a potential single-step reorg:
// writes up to 3 valid blocks on one branch then activates step-by-step, writes
// up to 5 blocks on a competing branch from the same origin tip then activates
// step-by-step again. Stateful: restores the chain at the start of each iteration.
FUZZ_TARGET(activate_best_chain_step, .init = initialize_setup)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    SetMockTime(g_setup->LastBlock()->GetBlockTime() + 2);
    // Restore chain to known state from previous iteration.
    // m_block_index_modified is set when WriteBlock adds entries to the in-memory
    // block index that cannot be removed by disconnecting; we must destroy and
    // recreate the chainman via RecreateAndReplayChain to get a clean slate.
    // We also reinit if the active tip has drifted (e.g. a prior ActivateBestChainStep
    // call extended the chain) so each iteration starts from the same known tip.
    const uint256 expected_tip = g_setup->LastBlock()->GetHash();
    if (g_setup->IsBlockIndexModified() ||
        WITH_LOCK(::cs_main, return g_setup->m_node.chainman->ActiveTip()->GetBlockHash()) != expected_tip) {
        g_setup->RecreateAndReplayChain();
    } else {
        g_setup->ClearMemPool();
    }
    // Ensure restoration succeeded before consuming any fuzzer bytes, so a
    // failed restore doesn't silently corrupt the iteration's starting state.
    Assert(WITH_LOCK(::cs_main, return g_setup->m_node.chainman->ActiveTip()->GetBlockHash()) == expected_tip);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    TestChainstate& test_chainstate = g_setup->GetTestChainstate();
    CBlockIndex* origin_tip = test_chainstate.m_chain.Tip();
    std::vector<CTxIn> additional_utxo;
    std::shared_ptr<const CBlock> current_block;
    CBlockIndex* current_tip_index = origin_tip;
    // Build branch 1: up to 3 blocks extending the current tip. WriteBlock
    // only indexes the block without activating it, so the chain tip stays at
    // origin_tip until ActivateBestChainStep is called below.
    {
        LOCK(::cs_main);
        // WriteBlock does not update the block list, so LastBlock() still points
        // to the pre-harness tip regardless of how many blocks are written.
        current_block = g_setup->LastBlock();
        LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 3)
        {
            CBlock block = g_setup->ConsumeBlock(fuzzed_data_provider, *current_block, current_tip_index->nHeight + 1, additional_utxo, true);
            CBlockIndex* block_index = g_setup->WriteBlock(block);
            if (!block_index) return;
            // pprev mismatch means AddToBlockIndex rejected the block as non-extending.
            if (block_index->pprev != current_tip_index) return;
            current_block = std::make_shared<CBlock>(block);
            current_tip_index = block_index;
        }
    }
    // Activate branch 1 one step at a time. Both locks are held for the full
    // loop because ActivateBestChainStep updates the mempool during
    // disconnect/connect and must not release cs_main mid-reorg.
    BlockValidationState state;
    {
        LOCK(::cs_main);
        LOCK(test_chainstate.MempoolMutex());
        do {
            std::vector<ConnectedBlock> connected_blocks;
            bool found_invalid = false;
            if (!test_chainstate.ActivateBestChainStep(state, *current_tip_index, current_block, found_invalid, connected_blocks)) return;
            if (found_invalid) return;
        } while (test_chainstate.m_chain.Tip()->nHeight < current_tip_index->nHeight);
    }
    // Build branch 2: up to 5 blocks from the same origin tip, giving the
    // fuzzer a chance to trigger a reorg if branch 2 ends up with more work.
    additional_utxo.clear();
    current_block = nullptr;
    {
        LOCK(::cs_main);
        current_block = g_setup->LastBlock();
        current_tip_index = origin_tip;
        LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 5)
        {
            CBlock block = g_setup->ConsumeBlock(fuzzed_data_provider, *current_block, current_tip_index->nHeight + 1, additional_utxo, true);
            CBlockIndex* block_index = g_setup->WriteBlock(block);
            if (!block_index) return;
            if (block_index->pprev != current_tip_index) return;
            current_block = std::make_shared<CBlock>(block);
            current_tip_index = block_index;
        }
    }
    // Activate branch 2; if it has more work than branch 1 a reorg occurs.
    {
        LOCK(::cs_main);
        LOCK(test_chainstate.MempoolMutex());
        do {
            std::vector<ConnectedBlock> connected_blocks;
            bool found_invalid = false;
            if (!test_chainstate.ActivateBestChainStep(state, *current_tip_index, current_block, found_invalid, connected_blocks)) return;
            if (found_invalid) return;
        } while (test_chainstate.m_chain.Tip()->nHeight < current_tip_index->nHeight);
    }
}

} // namespace

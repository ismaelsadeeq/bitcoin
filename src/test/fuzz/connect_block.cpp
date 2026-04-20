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

FUZZ_TARGET(connect_block, .init = initialize_setup)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    SetMockTime(g_setup->m_list_blocks.back()->GetBlockTime() + 2);
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    Chainstate& active_chainstate = g_setup->m_node.chainman->ActiveChainstate();
    CBlockIndex* active_tip = active_chainstate.m_chain.Tip();
    Assert(active_tip != nullptr);
    std::vector<CTxIn> additional_utxo;
    CBlock block;
    {
        LOCK(::cs_main);
        block = g_setup->ConsumeBlock(fuzzed_data_provider, *g_setup->m_list_blocks.back(), active_tip->nHeight + 1, additional_utxo);
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
    SetMockTime(g_setup->m_list_blocks.back()->GetBlockTime() + 2);
    // Restore chain to known state from previous iteration.
    // m_block_index_modified is set when WriteBlock adds entries to the in-memory
    // block index that cannot be removed by disconnecting; we must destroy and
    // recreate the chainman via RecreateAndReplayChain to get a clean slate.
    // We also reinit if the active tip has drifted (e.g. a prior ActivateBestChainStep
    // call extended the chain) so each iteration starts from the same known tip.
    const uint256 expected_tip = g_setup->m_list_blocks.back()->GetHash();
    if (g_setup->m_block_index_modified ||
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
        // WriteBlock does not update m_list_blocks, so back() still points
        // to the pre-harness tip regardless of how many blocks are written.
        current_block = g_setup->m_list_blocks.back();
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
            if (!test_chainstate.ActivateBestChainStep(state, current_tip_index, current_block, found_invalid, connected_blocks)) return;
            if (found_invalid) return;
        } while (test_chainstate.m_chain.Tip()->nHeight < current_tip_index->nHeight);
    }
    // Build branch 2: up to 5 blocks from the same origin tip, giving the
    // fuzzer a chance to trigger a reorg if branch 2 ends up with more work.
    additional_utxo.clear();
    current_block = nullptr;
    {
        LOCK(::cs_main);
        current_block = g_setup->m_list_blocks.back();
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
            if (!test_chainstate.ActivateBestChainStep(state, current_tip_index, current_block, found_invalid, connected_blocks)) return;
            if (found_invalid) return;
        } while (test_chainstate.m_chain.Tip()->nHeight < current_tip_index->nHeight);
    }
}

// Tests ActivateBestChain across a guaranteed reorg: writes and activates two
// branches (up to 3 and up to 5 blocks) built from the same origin tip via the
// full AcceptBlock path. Branch 2 is always longer than branch 1, ensuring a
// reorg occurs on the second activation. Stateful: always recreates the chainman.
FUZZ_TARGET(activate_best_chain, .init = initialize_setup)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    SetMockTime(g_setup->m_list_blocks.back()->GetBlockTime() + 2);
    // WriteAndActivateBlock may modify the in-memory block index, so
    // we must recreate the chainman every iteration to start from a clean slate.
    g_setup->RecreateAndReplayChain();
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    Chainstate& active_chainstate = g_setup->m_node.chainman->ActiveChainstate();
    BlockValidationState state;
    std::vector<CTxIn> additional_utxo;
    const auto& consensus = g_setup->m_node.chainman->GetConsensus();
    std::vector<std::shared_ptr<CBlock>> branch_1;
    // local_indices holds locally-constructed CBlockIndex objects that stand in
    // for the real block index entries during contextual validation, since the
    // blocks are not committed to the chainman's index until WriteAndActivateBlock.
    std::vector<std::unique_ptr<CBlockIndex>> local_indices;
    std::shared_ptr<const CBlock> current_block;
    CBlockIndex* origin_tip = active_chainstate.m_chain.Tip();
    CBlockIndex* current_tip_index = origin_tip;
    {
        LOCK(::cs_main);
        current_block = g_setup->m_list_blocks.back();
        LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 3)
        {
            branch_1.push_back(std::make_shared<CBlock>(g_setup->ConsumeBlock(fuzzed_data_provider, *current_block, current_tip_index->nHeight + 1, additional_utxo, true)));
            current_block = branch_1.back();
            // Validate before writing to disk so invalid blocks are skipped
            // without leaving partial state in the block index.
            if (!ContextualCheckBlockHeader(*current_block, state, g_setup->m_node.chainman->m_blockman, *g_setup->m_node.chainman, current_tip_index) ||
                !CheckBlock(*current_block, state, consensus) ||
                !ContextualCheckBlock(*current_block, state, *g_setup->m_node.chainman, current_tip_index)) {
                return;
            }
            local_indices.emplace_back(std::make_unique<CBlockIndex>(*current_block));
            local_indices.back()->nHeight = current_tip_index->nHeight + 1;
            local_indices.back()->pprev = current_tip_index;
            current_tip_index = local_indices.back().get();
        }
    }
    std::vector<std::shared_ptr<CBlock>> branch_2;
    additional_utxo.clear();
    local_indices.clear();
    current_block = nullptr;
    {
        LOCK(::cs_main);
        current_block = g_setup->m_list_blocks.back();
        current_tip_index = origin_tip;
        // Keep consuming until branch_2 is strictly longer than branch_1 so the
        // second ActivateBestChain call is guaranteed to trigger a reorg.
        LIMITED_WHILE(fuzzed_data_provider.ConsumeBool() || branch_2.size() <= branch_1.size(), 5)
        {
            branch_2.push_back(std::make_shared<CBlock>(g_setup->ConsumeBlock(fuzzed_data_provider, *current_block, current_tip_index->nHeight + 1, additional_utxo, true)));
            current_block = branch_2.back();
            if (!ContextualCheckBlockHeader(*current_block, state, g_setup->m_node.chainman->m_blockman, *g_setup->m_node.chainman, current_tip_index) ||
                !CheckBlock(*current_block, state, consensus) ||
                !ContextualCheckBlock(*current_block, state, *g_setup->m_node.chainman, current_tip_index)) {
                return;
            }
            local_indices.emplace_back(std::make_unique<CBlockIndex>(*current_block));
            local_indices.back()->nHeight = current_tip_index->nHeight + 1;
            local_indices.back()->pprev = current_tip_index;
            current_tip_index = local_indices.back().get();
        }
    }
    // Write branch 1 to the block index via the full AcceptBlock path and
    // activate it. current_block tracks the tip so ActivateBestChain can use
    // it as a hint to avoid re-reading the block from disk.
    current_tip_index = origin_tip;
    for (const auto& blk : branch_1) {
        LOCK(::cs_main);
        CBlockIndex* block_index = g_setup->WriteAndActivateBlock(*blk);
        if (!block_index) return;
        if (block_index->pprev != current_tip_index) return;
        current_tip_index = block_index;
        current_block = blk;
    }
    if (!active_chainstate.ActivateBestChain(state, current_block)) return;
    // Write branch 2 and activate; since branch 2 is longer it triggers a reorg.
    current_tip_index = origin_tip;
    for (const auto& blk : branch_2) {
        LOCK(::cs_main);
        CBlockIndex* block_index = g_setup->WriteAndActivateBlock(*blk);
        if (!block_index) return;
        if (block_index->pprev != current_tip_index) return;
        current_tip_index = block_index;
        current_block = blk;
    }
    if (!active_chainstate.ActivateBestChain(state, current_block)) return;
}

} // namespace

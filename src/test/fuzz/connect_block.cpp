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

} // namespace

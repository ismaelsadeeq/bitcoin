// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/block_validity.h>

#include <test/util/mining.h>
#include <undo.h>

namespace {

//! Default number of trials for AddCoin to find a unique random outpoint
static constexpr int MAX_ADDCOIN_TRIALS = 5;
} // namespace

CBlock ValidationBlockValidityTestingSetup::MakeBlock(BlockAssembler::Options options)
{
    auto block_ptr = PrepareBlock(m_node, options);
    Assert(block_ptr);
    return *block_ptr;
}

BlockValidationState ValidationBlockValidityTestingSetup::TestValidity(const CBlock& block, bool check_pow, bool check_merkle)
{
    LOCK(cs_main);
    return TestBlockValidity(m_chainstate, block, check_pow, check_merkle);
}

std::optional<CBlockUndo> ValidationBlockValidityTestingSetup::PopulateBlockUndo(const CBlock& block)
{
    LOCK(cs_main);
    const CBlockIndex* tip{Assert(m_chainstate.m_chain.Tip())};
    CBlockIndex index_dummy{block};
    uint256 block_hash{block.GetHash()};
    index_dummy.pprev = const_cast<CBlockIndex*>(tip);
    index_dummy.nHeight = tip->nHeight + 1;
    index_dummy.phashBlock = &block_hash;
    CCoinsViewCache view_dummy{&m_chainstate.CoinsTip()};
    CBlockUndo blockundo;
    BlockValidationState state;
    if (!m_chainstate.SpendBlock(block, &index_dummy, view_dummy, state, blockundo, /*fJustCheck=*/true)) {
        return std::nullopt;
    }
    return blockundo;
}

BlockValidationState ValidationBlockValidityTestingSetup::TestValidityWithUndo(const CBlock& block, const CBlockUndo& blockundo, bool check_pow, bool check_merkle)
{
    LOCK(cs_main);
    return TestBlockValidityWithUndo(m_chainstate, block, blockundo, block.hashPrevBlock, check_pow, check_merkle);
}

std::optional<COutPoint> ValidationBlockValidityTestingSetup::AddCoin(const CScript& script_pub_key, CAmount amount)
{
    for (int trial_idx = 0; trial_idx < MAX_ADDCOIN_TRIALS; ++trial_idx) {
        COutPoint outpoint{Txid::FromUint256(m_rng.rand256()), 0};
        LOCK(cs_main);
        if (m_chainstate.CoinsTip().AccessCoin(outpoint).IsSpent()) {
            m_chainstate.CoinsTip().AddCoin(outpoint, Coin(CTxOut(amount, script_pub_key), 1, false), true);
            return outpoint;
        }
    }
    return std::nullopt;
}

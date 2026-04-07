// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/block_validity.h>

#include <test/util/mining.h>

#include <set>

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

BlockValidationState ValidationBlockValidityTestingSetup::TestValidityWithSpentOutputs(const CBlock& block, std::vector<TxOutput> block_spent_txouts, bool check_pow, bool check_merkle)
{
    LOCK(cs_main);
    const uint256 tip_hash{m_chainstate.m_chain.Tip()->GetBlockHash()};
    return TestBlockValidityWithSpentTxOuts(m_chainstate, tip_hash, block, std::move(block_spent_txouts), check_pow, check_merkle);
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

std::vector<TxOutput> ValidationBlockValidityTestingSetup::CollectSpentOutputs(const CBlock& block)
{
    std::vector<TxOutput> spent;
    std::set<COutPoint> seen;
    LOCK(cs_main);
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        for (const auto& vin : block.vtx[i]->vin) {
            if (!seen.insert(vin.prevout).second) continue;
            auto coin = m_chainstate.CoinsTip().GetCoin(vin.prevout);
            if (coin) {
                spent.emplace_back(vin.prevout, std::move(*coin));
            }
        }
    }
    return spent;
}

// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <policy/mempool_fees.h>

#include <node/miner.h>
#include <policy/policy.h>

using node::GetMempoolHistogram;


CMemPoolPolicyEstimator::CMemPoolPolicyEstimator(Chainstate& chainstate)
    : m_chainstate(&chainstate)
{
}

CMemPoolPolicyEstimator::~CMemPoolPolicyEstimator() = default;

CFeeRate CMemPoolPolicyEstimator::EstimateFeeWithMemPool(CTxMemPool& mempool, unsigned int confTarget) const
{
    {
        if (!mempool.GetLoadTried()) {
            LogPrintf("Mempool did not finish loading, can't get accurate fee rate estimate.\n");
            return CFeeRate(0);
        }
    }

    std::map<CFeeRate, uint64_t> mempool_fee_stats;
    {
        LOCK(mempool.cs);
        mempool_fee_stats = GetMempoolHistogram(*m_chainstate, mempool);
    }
    if (mempool_fee_stats.empty()) {
        return CFeeRate(0); // Return an appropriate default value (no txs in the mempool)
    }
    unsigned int block_count{0};
    auto start = mempool_fee_stats.rbegin();
    auto current = mempool_fee_stats.rbegin();
    std::optional<CFeeRate> fee_rate_estimate = std::nullopt;

    // Iterate through the mempool fee stats to estimate the fee rate for `confTarget` blocks
    while (block_count < confTarget) {
        fee_rate_estimate = EstimateBlockFee(start, current, mempool_fee_stats);
        start = current;
        ++block_count;
    }
    return fee_rate_estimate.value_or(CFeeRate(0)); // Return the estimated fee rate or a default value if unavailable
}

std::optional<CFeeRate> CMemPoolPolicyEstimator::EstimateBlockFee(
    std::map<CFeeRate, uint64_t>::reverse_iterator& start,
    std::map<CFeeRate, uint64_t>::reverse_iterator& it,
    std::map<CFeeRate, uint64_t>& mempool_fee_stats) const
{
    uint64_t current_block_size = 0;
    while (current_block_size < DEFAULT_BLOCK_MAX_WEIGHT && (it != mempool_fee_stats.rend())) {
        // Exit the loop if adding the current block would exceed the maximum weight
        if ((current_block_size + it->second) > DEFAULT_BLOCK_MAX_WEIGHT) {
            break;
        }
        current_block_size += it->second;
        ++it;
    }

    if (current_block_size < (DEFAULT_BLOCK_MAX_WEIGHT / 2)) {
        return std::nullopt; // Insufficient block size for the desired fee estimate (less than half block)
    }
    return CalculateMedianFeeRate(start, it); // Calculate the median fee rate for the block
}

CFeeRate CMemPoolPolicyEstimator::CalculateMedianFeeRate(
    std::map<CFeeRate, uint64_t>::reverse_iterator& start_it,
    std::map<CFeeRate, uint64_t>::reverse_iterator& end_it) const
{
    std::size_t size = std::distance(start_it, end_it);

    if (size % 2 == 0) {
        // If the number of txs is even, average the two middle fee rates
        auto first_mid_it = std::next(start_it, size / 2);
        auto second_mid_it = std::next(start_it, (size / 2) + 1);
        auto mid_fee = (first_mid_it->first.GetFeePerK() + second_mid_it->first.GetFeePerK()) / 2;
        return CFeeRate(mid_fee);
    } else {
        // If the number of txs is odd, return the fee rate of the middle tx
        auto mid_it = std::next(start_it, (size / 2) + 1);
        return mid_it->first;
    }
}

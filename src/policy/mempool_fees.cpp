// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/mempool_fees.h>

#include <node/miner.h>
#include <policy/policy.h>

using node::GetMempoolHistogram;

CMemPoolPolicyEstimator::CMemPoolPolicyEstimator(CTxMemPool& mempool, Chainstate& chainstate)
    : m_mempool(&mempool), m_chainstate(&chainstate)
{
}

CMemPoolPolicyEstimator::~CMemPoolPolicyEstimator() = default;

CFeeRate CMemPoolPolicyEstimator::EstimateMempoolFeeEstimate(int confTarget) const
{
    LOCK(m_mempool->cs);
    std::map<CFeeRate, uint64_t> mempool_fee_stats = GetMempoolHistogram(*m_chainstate, *m_mempool);

    if (mempool_fee_stats.empty()) {
        return CFeeRate(0); // Return an appropriate default value
    }
    int block_count{0};
    auto start = mempool_fee_stats.rbegin();
    auto current = mempool_fee_stats.rbegin();
    std::optional<CFeeRate> fee_rate_estimate = std::nullopt;

    while (block_count < confTarget) {
        std::size_t index = std::distance(mempool_fee_stats.rbegin(), start);
        fee_rate_estimate = GetBlocksFeeEstimate(start, current, mempool_fee_stats, index);
        start = current;
        ++block_count;
    }
    return fee_rate_estimate.value_or(CFeeRate(0));
}

std::optional<CFeeRate> CMemPoolPolicyEstimator::GetBlocksFeeEstimate(
    std::map<CFeeRate, uint64_t>::reverse_iterator& start,
    std::map<CFeeRate, uint64_t>::reverse_iterator& it,
    std::map<CFeeRate, uint64_t>& mempool_fee_stats,
    std::size_t index) const
{
    uint64_t current_block_size = 0;
    std::size_t start_index = index;

    // Calculate the total block size until reaching the maximum weight or below it
    while (current_block_size < DEFAULT_BLOCK_MAX_WEIGHT && (it != mempool_fee_stats.rend())) {
        // Exit the loop if adding the current block would exceed the maximum weight
        if ((current_block_size + it->second) > DEFAULT_BLOCK_MAX_WEIGHT) {
            break;
        }
        current_block_size += it->second;
        ++index;
        ++it;
    }

    if (current_block_size < (DEFAULT_BLOCK_MAX_WEIGHT / 2)) {
        return std::nullopt; // Insufficient block size for the desired fee estimate
    }
    return CalculateMedianFeeRate(start, it, start_index, index);
}

CFeeRate CMemPoolPolicyEstimator::CalculateMedianFeeRate(
    std::map<CFeeRate, uint64_t>::reverse_iterator& start_it,
    std::map<CFeeRate, uint64_t>::reverse_iterator& end_it,
    std::size_t start,
    std::size_t end) const
{
    std::size_t size = std::distance(start_it, end_it);

    if (size % 2 == 0) {
        auto first_mid_it = std::next((start_it), (size / 2));
        auto second_mid_it = std::next((start_it), ((size / 2) + 1));
        auto mid_fee = (first_mid_it->first.GetFeePerK() + second_mid_it->first.GetFeePerK()) / 2;
        return CFeeRate(mid_fee);
    }
    auto mid_it = std::next(start_it, ((size / 2) + 1));
    return mid_it->first;
}

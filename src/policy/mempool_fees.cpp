// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/consensus.h>
#include <logging.h>
#include <node/miner.h>
#include <policy/mempool_fees.h>
#include <policy/policy.h>


using node::GetCustomBlockFeeRateHistogram;

MemPoolPolicyEstimator::MemPoolPolicyEstimator() {}

MempoolFeeEstimationResult MemPoolPolicyEstimator::EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget, const bool force, std::string& err_message) const
{
    std::optional<MempoolFeeEstimationResult> cached_fee{std::nullopt};
    std::map<uint64_t, MempoolFeeEstimationResult> blocks_fee_rates_estimates;
    MempoolFeeEstimationResult fee_rate_estimate_result;

    if (confTarget > MAX_CONF_TARGET) {
        err_message = strprintf("Confirmation target %s is above maximum limit of %s, mempool conditions might change and estimates above %s are unreliable.\n", confTarget, MAX_CONF_TARGET, MAX_CONF_TARGET);
        return fee_rate_estimate_result;
    }

    if (!mempool.GetLoadTried()) {
        err_message = "Mempool not finished loading, can't get accurate fee rate estimate.";
        return fee_rate_estimate_result;
    }
    if (!force) {
        cached_fee = cache.get(confTarget);
    }

    if (!cached_fee) {
        std::vector<std::tuple<CFeeRate, uint64_t>> mempool_fee_stats;
        // Always get stats for MAX_CONF_TARGET blocks because current algo
        // fast enough to run that far while we're locked and in here
        {
            LOCK2(cs_main, mempool.cs);
            mempool_fee_stats = GetCustomBlockFeeRateHistogram(chainstate, &mempool, MAX_BLOCK_WEIGHT * MAX_CONF_TARGET);
        }
        if (mempool_fee_stats.empty()) {
            err_message = "No transactions available in the mempool yet.";
            return fee_rate_estimate_result;
        }
        blocks_fee_rates_estimates = EstimateBlockFeeRatesWithMempool(mempool_fee_stats, MAX_CONF_TARGET);
        cache.update(blocks_fee_rates_estimates);
        const auto it = blocks_fee_rates_estimates.find(confTarget);
        if (it != blocks_fee_rates_estimates.end()) {
            fee_rate_estimate_result = it->second;
        }

    } else {
        fee_rate_estimate_result = *cached_fee;
    }

    if (fee_rate_estimate_result.empty()) {
        err_message = "Insufficient mempool transactions to perform an estimate.";
    }

    return fee_rate_estimate_result;
}

std::map<uint64_t, MempoolFeeEstimationResult> MemPoolPolicyEstimator::EstimateBlockFeeRatesWithMempool(
    const std::vector<std::tuple<CFeeRate, uint64_t>>& mempool_fee_stats, unsigned int confTarget) const
{
    std::map<uint64_t, MempoolFeeEstimationResult> blocks_fee_rates_estimates;
    if (mempool_fee_stats.empty()) return blocks_fee_rates_estimates;

    auto start = mempool_fee_stats.crbegin();
    auto cur = mempool_fee_stats.crbegin();
    auto end = mempool_fee_stats.crend();

    size_t block_number{confTarget};
    size_t block_weight{0};

    while (block_number >= 1 && cur != end) {
        size_t tx_weight = std::get<1>(*cur) * WITNESS_SCALE_FACTOR;
        block_weight += tx_weight;
        auto next_cur = std::next(cur);
        if (block_weight >= DEFAULT_BLOCK_MAX_WEIGHT || next_cur == end) {
            blocks_fee_rates_estimates[block_number] = CalculateBlockPercentiles(start, cur);
            block_number--;
            block_weight = 0;
            start = next_cur;
        }
        cur = next_cur;
    }
    return blocks_fee_rates_estimates;
}

MempoolFeeEstimationResult MemPoolPolicyEstimator::CalculateBlockPercentiles(
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator start,
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator end) const
{
    unsigned int total_weight = 0;
    const auto p5_Size = DEFAULT_BLOCK_MAX_WEIGHT / 20;
    const auto p25_Size = DEFAULT_BLOCK_MAX_WEIGHT / 4;
    const auto p50_Size = DEFAULT_BLOCK_MAX_WEIGHT / 2;
    const auto p75_Size = (3 * DEFAULT_BLOCK_MAX_WEIGHT) / 4;

    MempoolFeeEstimationResult res;

    for (auto rit = start; rit != end; ++rit) {
        total_weight += std::get<1>(*rit) * WITNESS_SCALE_FACTOR;
        if (total_weight >= p5_Size && res.p5 == CFeeRate(0)) {
            res.p5 = std::get<0>(*rit);
        }
        if (total_weight >= p25_Size && res.p25 == CFeeRate(0)) {
            res.p25 = std::get<0>(*rit);
        }
        if (total_weight >= p50_Size && res.p50 == CFeeRate(0)) {
            res.p50 = std::get<0>(*rit);
        }
        if (total_weight >= p75_Size && res.p75 == CFeeRate(0)) {
            res.p75 = std::get<0>(*rit);
        }
    }
    // Block weight should be at least half of default maximum block weight size
    // for estimates to be reliable.
    if (total_weight < (DEFAULT_BLOCK_MAX_WEIGHT / 2)) {
        MempoolFeeEstimationResult empty_res;
        return empty_res;
    }
    return res;
}

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

CFeeRate MemPoolPolicyEstimator::EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget, const bool force, std::string& err_message) const
{
    std::optional<CFeeRate> cached_fee{std::nullopt};
    std::map<uint64_t, CFeeRate> fee_rates;
    CFeeRate block_fee_rate{0};

    if (confTarget > MAX_CONF_TARGET) {
        err_message = strprintf("Confirmation target %s is above maximum limit of %s, mempool conditions might change and estimates above %s are unreliable.\n", confTarget, MAX_CONF_TARGET, MAX_CONF_TARGET);
        return CFeeRate(0);
    }

    if (!mempool.GetLoadTried()) {
        err_message = "Mempool not finished loading, can't get accurate fee rate estimate.";
        return CFeeRate(0);
    }
    if (!force) {
        cached_fee = cache.get(confTarget);
    }

    if (!cached_fee) {
        std::vector<std::tuple<CFeeRate, uint64_t>> mempool_fee_stats;
        // Always get stats for MAX_CONF_TARGET blocks (3) because current algo
        // fast enough to run that far while we're locked and in here
        {
            LOCK2(cs_main, mempool.cs);
            mempool_fee_stats = GetCustomBlockFeeRateHistogram(chainstate, &mempool, MAX_BLOCK_WEIGHT * MAX_CONF_TARGET);
        }
        if (mempool_fee_stats.empty()) {
            err_message = "No transactions available in the mempool yet.";
            return CFeeRate(0);
        }
        fee_rates = EstimateBlockFeeRatesWithMempool(mempool_fee_stats, MAX_CONF_TARGET);
        cache.update(fee_rates);
        block_fee_rate = fee_rates[confTarget];
    } else {
        block_fee_rate = *cached_fee;
    }

    if (block_fee_rate == CFeeRate(0)) {
        err_message = "Insufficient mempool transactions to perform an estimate.";
    }
    return block_fee_rate;
}

std::map<uint64_t, CFeeRate> MemPoolPolicyEstimator::EstimateBlockFeeRatesWithMempool(
    const std::vector<std::tuple<CFeeRate, uint64_t>>& mempool_fee_stats, unsigned int confTarget) const
{
    std::map<uint64_t, CFeeRate> fee_rates;
    if (mempool_fee_stats.empty()) return fee_rates;

    auto start = mempool_fee_stats.begin();
    auto cur = mempool_fee_stats.begin();
    auto end = mempool_fee_stats.end();

    size_t block_number{1};
    size_t block_weight{0};

    while (block_number <= confTarget && cur != end) {
        size_t tx_weight = std::get<1>(*cur) * WITNESS_SCALE_FACTOR;
        block_weight += tx_weight;
        auto next_cur = std::next(cur);
        if (block_weight >= DEFAULT_BLOCK_MAX_WEIGHT || next_cur == end) {
            fee_rates[block_number] = CalculateMedianFeeRate(start, cur);
            block_number++;
            block_weight = 0;
            start = next_cur;
        }
        cur = next_cur;
    }
    return fee_rates;
}

CFeeRate MemPoolPolicyEstimator::CalculateMedianFeeRate(
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_iterator start,
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_iterator end) const
{
    unsigned int total_weight = 0;
    auto mid_size = DEFAULT_BLOCK_MAX_WEIGHT / 2;
    for (auto rit = start; rit != end; ++rit) {
        total_weight += std::get<1>(*rit) * WITNESS_SCALE_FACTOR;
        if (total_weight >= mid_size) {
            return std::get<0>(*rit);
        }
    }
    // the block weight is not enough to provide a decent estimate
    return CFeeRate(0);
}

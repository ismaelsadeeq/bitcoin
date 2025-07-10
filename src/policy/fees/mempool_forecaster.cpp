// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <node/miner.h>
#include <policy/fees/forecaster.h>
#include <policy/fees/forecaster_util.h>
#include <policy/fees/mempool_forecaster.h>
#include <policy/policy.h>
#include <util/feefrac.h>
#include <validation.h>

#include <vector>
#include <cmath>
#include <limits>

ForecastResult MemPoolForecaster::ForecastFeeRate(int target, bool conservative)
{
    ForecastResult result;
    result.forecaster = m_forecast_type;
    LOCK2(cs_main, m_mempool->cs);
    auto activeTip = m_chainstate->m_chainman.ActiveTip();
    if (!activeTip) {
        result.error = "No active chainstate available";
        return result;
    }
    result.current_block_height = static_cast<unsigned int>(activeTip->nHeight);

    if (target > MEMPOOL_FORECAST_MAX_TARGET) {
        result.error = strprintf("Confirmation target %s exceeds the maximum limit of %s. mempool conditions might change",
                                   target, MEMPOOL_FORECAST_MAX_TARGET);
        return result;
    }
    result.returned_target = MEMPOOL_FORECAST_MAX_TARGET;

    const auto cached_estimate = cache.get_cached_forecast();
    const auto known_chain_tip_hash = cache.get_chain_tip_hash();
    if (cached_estimate && *activeTip->phashBlock == known_chain_tip_hash) {
        result.feerate = conservative ? cached_estimate->p50 : cached_estimate->p75;
        return result;
    }

    node::BlockAssembler::Options options;
    options.test_block_validity = false;
    node::BlockAssembler assembler(*m_chainstate, m_mempool, options);

    const auto pblocktemplate = assembler.CreateNewBlock();
    auto& package_feerates = pblocktemplate->m_package_feerates;
    const auto percentiles = CalculatePercentiles(package_feerates, DEFAULT_BLOCK_MAX_WEIGHT);
    if (percentiles.empty()) {
        result.error = "Forecaster unable to provide a fee rate due to insufficient data";
        return result;
    }

    LogDebug(BCLog::MEMPOOL,
        "%s: Block height %s, Block template 25th percentile fee rate: %s %s/kvB, "
        "50th percentile fee rate: %s %s/kvB, 75th percentile fee rate: %s %s/kvB, "
        "95th percentile fee rate: %s %s/kvB\n",
        forecastTypeToString(m_forecast_type), result.current_block_height,
        CFeeRate(percentiles.p25.fee, percentiles.p25.size).GetFeePerK(), CURRENCY_ATOM,
        CFeeRate(percentiles.p50.fee, percentiles.p50.size).GetFeePerK(), CURRENCY_ATOM,
        CFeeRate(percentiles.p75.fee, percentiles.p75.size).GetFeePerK(), CURRENCY_ATOM,
        CFeeRate(percentiles.p95.fee, percentiles.p95.size).GetFeePerK(), CURRENCY_ATOM);

    std::sort(
        package_feerates.begin(), package_feerates.end(),
        [](const FeeFrac& a, const FeeFrac& b)
        {
            return FeeRateCompare(a, b) == std::weak_ordering::less;
        }
    );
    std::map<int64_t, int32_t> feerate_buckets;
    for (const auto& package : package_feerates) {
        auto feerate = package.EvaluateFeeDown(/*size=*/1000);
        auto& entry = feerate_buckets[feerate];
        entry += package.size;
    }
    auto inflows = GetInflow();
    // Add buffer
    for(auto& inflow: inflows) {
        auto& entry = feerate_buckets[inflow.first];
        entry += inflow.second / 2;
    }

    Percentiles estimate;
    auto economical_feerate = SimulateMining(MEMPOOL_FORECAST_MAX_TARGET, economical_blocks, feerate_buckets, inflows);
    estimate.p50 = FeeFrac(economical_feerate * 1000, 1000);
    auto conservative_feerate = SimulateMining(MEMPOOL_FORECAST_MAX_TARGET, conservative_blocks, feerate_buckets, inflows);
    estimate.p75 = FeeFrac(conservative_feerate * 1000, 1000);
    cache.update(estimate, *activeTip->phashBlock);
    result.feerate = conservative ? estimate.p75 : estimate.p50;
    return result;
    
}

int64_t MemPoolForecaster::SimulateMining(int block_target, int expected_blocks, std::map<int64_t, int32_t>& feerate_buckets, std::map<int64_t, int32_t>& total_inflow) const {
    double normalization_factor = block_target / expected_blocks;
    auto added_inflow_per_block = total_inflow;
    for (auto& inflow: added_inflow_per_block) {
        inflow.second = std::lround(static_cast<double>(inflow.second) * normalization_factor);
    }

    auto current_bucket = feerate_buckets; 
    for (int i = 0; i < expected_blocks; i++)
    {
        for (auto& inflow: added_inflow_per_block) {
            auto& entry = current_bucket[inflow.first];
            entry += inflow.second;
        }
        MineBlock(current_bucket);
    }
    return current_bucket.begin()->first;
}

void MemPoolForecaster::MineBlock(std::map<int64_t, int32_t>& feerate_buckets)
const {
    int block_size = DEFAULT_BLOCK_MAX_WEIGHT / WITNESS_SCALE_FACTOR;
    while (!feerate_buckets.empty() && block_size > 0) {
        auto it = feerate_buckets.begin(); //  Highest fee rate
        if (it->second <= block_size) {
            block_size -= it->second;
            feerate_buckets.erase(it);
        } else {
            it->second -= block_size;
            break;
        }
    }
}

void MemPoolForecaster::CaptureMempoolSnapshot()
{
    if (m_chainstate->m_chainman.IsInitialBlockDownload()) return;
    node::BlockAssembler::Options options;
    options.test_block_validity = false;
    node::BlockAssembler assembler(*m_chainstate, m_mempool, options);
    auto packages_feerates = assembler.CreateNewBlock()->m_package_feerates;
    std::sort(
        packages_feerates.begin(), packages_feerates.end(),
        [](const FeeFrac& a, const FeeFrac& b)
        {
            return FeeRateCompare(a, b) == std::weak_ordering::less;
        }
    );
    MempoolSnapshot snapshot;
    for (const auto& package : packages_feerates) {
        auto feerate = package.EvaluateFeeDown(/*size=*/1000);
        auto& entry = snapshot.m_feerate_buckets[feerate];
        entry += package.size;
    }

    snapshot.m_timestamp = NodeClock::now();
    unsigned int block_height;
    {
        LOCK(cs_main);
        block_height = m_chainstate->m_chainman.ActiveTip()->nHeight;
    }
    {
        LOCK(m_cs);
        // When we have no snapshots yet
        if (snapshots.empty()) {
            snapshots.emplace_back(block_height, snapshot, snapshot);
        // When block height changes add new snapshot
        } else if (snapshots.back().m_block_height != block_height) {
            snapshots.emplace_back(block_height, snapshot, snapshot);
            
        // Else update the last inflow with latest
        } else {
            snapshots.back().m_last = snapshot;
        }
        // Prune old snapshots
        auto oldest_timestamp = snapshot.m_timestamp - INFLOW_DURATION;
        while (!snapshots.empty() && snapshots.front().m_first.m_timestamp < oldest_timestamp) {
            snapshots.pop_front();
        }
    }
}

std::map<int64_t, int32_t> MemPoolForecaster::GetInflow()
{
    LOCK(m_cs);
    if (snapshots.empty()) return {};
    std::map<int64_t, int32_t> total_inflow;
    double total_timespan{0.0};
    for (const auto& snapshot : snapshots) {
        const auto& first = snapshot.m_first.m_feerate_buckets;
        const auto& last = snapshot.m_last.m_feerate_buckets;

        for (const auto& [last_feerate, size] : last) {
            int32_t delta;
            auto it = first.find(last_feerate);
            if (it != first.end()) {
                const int32_t& first_size = it->second;
                // Compute delta
                if (size > first_size) {
                    delta = size - first_size;
                } else {
                    continue; // Ignore negative or zero deltas
                }
            } else {
                // Entirely new bucket
                delta = size;
            }
            // Accumulate
            auto& inflow = total_inflow[last_feerate];
            inflow += delta;
        }

        total_timespan += std::chrono::duration<double>(snapshot.m_last.m_timestamp - snapshot.m_first.m_timestamp).count();
    }

    if (total_timespan > 0) {
        double ten_minutes{10 * 60};
        double normalization_factor = ten_minutes / total_timespan;
        for (auto& inflow: total_inflow) {
            inflow.second = std::lround(static_cast<double>(inflow.second) * normalization_factor);
        }
    }
    return total_inflow;
}


// Poisson cumulative distribution function (CDF)
// Computes P(X ≤ k) where X ~ Poisson(λ)
double PoissonCDF(double lambda, int k) {
    if (k < 0) return 0.0;

    double sum = 0.0;
    double term = std::exp(-lambda);  // P(X = 0)
    sum += term;

    // Use recurrence: P(X = k) = λ / k * P(X = k - 1)
    for (int i = 1; i <= k; ++i) {
        term *= lambda / i;
        sum += term;
    }
    return sum;
}

/**
 * Estimate a block count threshold for confirmation within a given time interval,
 * using the Poisson distribution to model block arrivals.
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │                     Poisson Distribution                    │
 * ├─────────────────────────────────────────────────────────────┤
 * │ X = number of blocks mined in the interval (10 min)         │
 * │ λ = expected number of blocks in interval (e.g. 1.0)        │
 * │                                                             │
 * │ CDF: P(X ≤ k) = Σ (e^(-λ) * λ^i / i!)  for i = 0 to k       │
 * │ Tail: P(X ≥ x) = 1 - P(X ≤ x - 1)                           │
 * └─────────────────────────────────────────────────────────────┘
 *
 * For a target λ, we compute P(X ≥ x) for x = 0 .. 4λ, and return the
 * **maximum x** such that this tail probability exceeds our confidence threshold.
 *
 * Confidence thresholds:
 *   - Conservative: 75% of blocks confirm (P ≥ 0.75)
 *   - Economical:   50% of blocks confirm (P ≥ 0.50)
 *
 */
void MemPoolForecaster::GetBlockTarget()
{
    LOCK(m_cs);
    constexpr double lambda = MEMPOOL_FORECAST_MAX_TARGET;  // Expected number of blocks (usually ~1.0)
    constexpr double conservative_threshold = 0.75;
    constexpr double economical_threshold = 0.5;

    const int max_blocks = static_cast<int>(lambda * 4);  // Safety margin (most of Poisson mass is in [0, 4λ])

    for (int x = 0; x <= max_blocks; ++x) {
        // P(X ≥ x) = 1 - P(X ≤ x - 1)
        double tail_prob = (x == 0) ? 1.0 : 1.0 - PoissonCDF(lambda, x - 1);

        // Track largest x where tail probability ≥ thresholds
        if (tail_prob >= conservative_threshold) {
            conservative_blocks = x;
        }
        if (tail_prob >= economical_threshold) {
            economical_blocks = x;
        }
    }
}

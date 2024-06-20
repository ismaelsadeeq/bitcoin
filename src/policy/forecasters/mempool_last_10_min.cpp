// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <node/miner.h>
#include <policy/fee_estimator.h>
#include <policy/fees_util.h>
#include <policy/forecasters/mempool_last_10_min.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <txmempool.h>
#include <util/trace.h>
#include <validation.h>

using node::GetNextBlockFeeRateAndVsize;

ForecastResult MemPoolLast10MinForecaster::EstimateFee(unsigned int targetBlocks)
{
    return EstimateFeeWithMemPool(targetBlocks);
}

ForecastResult MemPoolLast10MinForecaster::EstimateFeeWithMemPool(unsigned int targetBlocks)
{
    LOCK2(cs_main, m_mempool->cs);
    ForecastResult::ForecastOptions forecast_options;
    forecast_options.m_forecaster = MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR;
    forecast_options.m_block_height = m_chainstate->m_chainman.ActiveTip()->nHeight;

    if (targetBlocks <= 0) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target must be greater than zero", MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR));
    }

    if (targetBlocks > MEMPOOL_LAST_10_MIN_FORECAST_MAX_TARGET) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target %s is above maximum limit of %s, mempool conditions might change and forecasts above %s block may be unreliable",
                                                          MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR, targetBlocks, MEMPOOL_LAST_10_MIN_FORECAST_MAX_TARGET, MEMPOOL_LAST_10_MIN_FORECAST_MAX_TARGET));
    }

    if (!m_mempool->GetLoadTried()) {
        return ForecastResult(forecast_options, strprintf("%s: Mempool not finished loading; can't get accurate feerate forecast", MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR));
    }        

    auto linearizationResult = GetNextBlockFeeRateAndVsize(*m_chainstate, m_mempool);

    std::vector<std::tuple<CFeeRate, uint64_t>> block_fee_stats;
    uint64_t block_weight{0};
    const auto ten_minutes = std::chrono::minutes{10};
    const auto time_minus_ten_mins = GetTime<std::chrono::seconds>() - std::chrono::duration_cast<std::chrono::seconds>(ten_minutes);
    size_t current_index{0};
    while (current_index < linearizationResult.first.size()) {
        const auto package_size = std::get<1>(linearizationResult.first[current_index]);
        // Stop if the total block_weight will exceeds the default block weight
        if ((block_weight + package_size) > DEFAULT_BLOCK_MAX_WEIGHT) break;
        Assume(current_index < linearizationResult.second.size());
        const auto entry_ptr = m_mempool->GetEntry(linearizationResult.second[current_index]);
        if (entry_ptr->GetTime() >= time_minus_ten_mins) {
            // If the sponsor transaction was received within time_minus_ten_mins;
            // make it count twice.
            block_weight += package_size * WITNESS_SCALE_FACTOR;
            block_fee_stats.emplace_back(linearizationResult.first[current_index]);
        }
        block_weight += package_size * WITNESS_SCALE_FACTOR;
        block_fee_stats.emplace_back(linearizationResult.first[current_index]);
        ++current_index;
    }

    BlockPercentiles fee_rate_estimate_result = CalculateBlockPercentiles(block_fee_stats);
    if (fee_rate_estimate_result.empty() || fee_rate_estimate_result.p75 == CFeeRate(0)) {
        return ForecastResult(forecast_options, strprintf("%s: Not enough transactions in the mempool to provide a feerate forecast", MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR));
    }

    LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s: Block height %s, 75th percentile feerate %s %s/kvB, 50th percentile feerate %s %s/kvB, 25th percentile feerate %s %s/kvB, 5th percentile feerate %s %s/kvB \n",
             MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR, forecast_options.m_block_height, fee_rate_estimate_result.p75.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p50.GetFeePerK(), CURRENCY_ATOM,
             fee_rate_estimate_result.p25.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p5.GetFeePerK(), CURRENCY_ATOM);
    TRACE7(feerate_forecast, forecast_generated,
           targetBlocks,
           forecast_options.m_block_height,
           MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR.c_str(),
           fee_rate_estimate_result.p5.GetFeePerK(),
           fee_rate_estimate_result.p25.GetFeePerK(),
           fee_rate_estimate_result.p50.GetFeePerK(),
           fee_rate_estimate_result.p75.GetFeePerK());

    forecast_options.m_l_priority_estimate = fee_rate_estimate_result.p25;
    forecast_options.m_h_priority_estimate = fee_rate_estimate_result.p50;
    return ForecastResult(forecast_options, std::nullopt);
}

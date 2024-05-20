// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <node/miner.h>
#include <policy/fee_estimator.h>
#include <policy/forecasters/mempool.h>
#include <policy/policy.h>
#include <util/trace.h>
#include <validation.h>


using node::GetNextBlockFeeRateAndVsize;

ForecastResult MemPoolForecaster::EstimateFee(unsigned int targetBlocks)
{
    return EstimateFeeWithMemPool(targetBlocks);
}

ForecastResult MemPoolForecaster::EstimateFeeWithMemPool(unsigned int targetBlocks)
{
    LOCK2(cs_main, m_mempool->cs);
    ForecastResult::ForecastOptions forecast_options;
    forecast_options.m_forecaster = MEMPOOL_FORECAST_NAME_STR;
    forecast_options.m_block_height = m_chainstate->m_chainman.ActiveTip()->nHeight;

    if (targetBlocks <= 0) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target must be greater than zero", MEMPOOL_FORECAST_NAME_STR));
    }

    if (targetBlocks > MEMPOOL_FORECAST_MAX_TARGET) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target %s is above maximum limit of %s, mempool conditions might change and forecasts above %s block may be unreliable",
                                                          MEMPOOL_FORECAST_NAME_STR, targetBlocks, MEMPOOL_FORECAST_MAX_TARGET, MEMPOOL_FORECAST_MAX_TARGET));
    }

    if (!m_mempool->GetLoadTried()) {
        return ForecastResult(forecast_options, strprintf("%s: Mempool not finished loading; can't get accurate feerate forecast", MEMPOOL_FORECAST_NAME_STR));
    }

    const auto cached_estimate = cache.get();
    if (cached_estimate) {
        forecast_options.m_l_priority_estimate = cached_estimate->p25;
        forecast_options.m_h_priority_estimate = cached_estimate->p50;
        return ForecastResult(forecast_options, std::nullopt);
    }

    std::vector<std::tuple<CFeeRate, uint64_t>> block_fee_stats = GetNextBlockFeeRateAndVsize(*m_chainstate, m_mempool);

    if (block_fee_stats.empty()) {
        return ForecastResult(forecast_options, strprintf("%s: No transactions available in the mempool", MEMPOOL_FORECAST_NAME_STR));
    }

    BlockPercentiles fee_rate_estimate_result = CalculateBlockPercentiles(block_fee_stats);
    if (fee_rate_estimate_result.empty() || fee_rate_estimate_result.p75 == CFeeRate(0)) {
        return ForecastResult(forecast_options, strprintf("%s: Not enough transactions in the mempool to provide a feerate forecast", MEMPOOL_FORECAST_NAME_STR));
    }

    LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s: Block height %s, 75th percentile feerate %s %s/kvB, 50th percentile feerate %s %s/kvB, 25th percentile feerate %s %s/kvB, 5th percentile feerate %s %s/kvB \n",
             MEMPOOL_FORECAST_NAME_STR, forecast_options.m_block_height, fee_rate_estimate_result.p75.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p50.GetFeePerK(), CURRENCY_ATOM,
             fee_rate_estimate_result.p25.GetFeePerK(), CURRENCY_ATOM, fee_rate_estimate_result.p5.GetFeePerK(), CURRENCY_ATOM);
    TRACE7(feerate_forecast, forecast_generated,
           targetBlocks,
           forecast_options.m_block_height,
           MEMPOOL_FORECAST_NAME_STR.c_str(),
           fee_rate_estimate_result.p5.GetFeePerK(),
           fee_rate_estimate_result.p25.GetFeePerK(),
           fee_rate_estimate_result.p50.GetFeePerK(),
           fee_rate_estimate_result.p75.GetFeePerK());

    cache.update(fee_rate_estimate_result);
    forecast_options.m_l_priority_estimate = fee_rate_estimate_result.p25;
    forecast_options.m_h_priority_estimate = fee_rate_estimate_result.p50;
    return ForecastResult(forecast_options, std::nullopt);
}

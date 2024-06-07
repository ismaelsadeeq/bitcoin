// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/mempool_entry.h>
#include <logging.h>
#include <policy/fee_estimator.h>
#include <policy/forecasters/last_block.h>
#include <util/trace.h>


void LastBlockForecaster::MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight)
{
    chain_tip_height = nBlockHeight;
    const std::vector<std::tuple<CFeeRate, uint64_t>> size_per_feerate = LinearizeTransactions(txs_removed_for_block).size_per_feerate;
    BlockPercentiles percentiles = CalculateBlockPercentiles(size_per_feerate);
    if (percentiles.p75 != CFeeRate(0)) {
        blocks_percentile = percentiles;
    }
}

ForecastResult LastBlockForecaster::EstimateFee(unsigned int targetBlocks)
{
    ForecastResult::ForecastOptions forecast_options;
    forecast_options.m_forecaster = LAST_BLOCK_FORECAST_NAME_STR;
    forecast_options.m_block_height = chain_tip_height;
    if (targetBlocks <= 0) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target must be greater than zero", LAST_BLOCK_FORECAST_NAME_STR));
    }

    if (targetBlocks > LAST_BLOCK_FORECAST_MAX_TARGET) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target %u is above the maximum limit of %u",
                                                          LAST_BLOCK_FORECAST_NAME_STR, targetBlocks, LAST_BLOCK_FORECAST_MAX_TARGET));
    }

    if (blocks_percentile.empty()) {
        return ForecastResult(forecast_options, strprintf("%s: Insufficient block data to perform an estimate", LAST_BLOCK_FORECAST_NAME_STR));
    }


    LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s: Block height %s, 75th percentile fee rate %s %s/kvB, 50th percentile fee rate %s %s/kvB, 25th percentile fee rate %s %s/kvB, 5th percentile fee rate %s %s/kvB\n",
             LAST_BLOCK_FORECAST_NAME_STR, chain_tip_height, blocks_percentile.p75.GetFeePerK(), CURRENCY_ATOM, blocks_percentile.p50.GetFeePerK(), CURRENCY_ATOM, blocks_percentile.p25.GetFeePerK(), CURRENCY_ATOM, blocks_percentile.p5.GetFeePerK(), CURRENCY_ATOM);

    TRACE7(feerate_forecast, forecast_generated,
           targetBlocks,
           LAST_BLOCK_FORECAST_NAME_STR.c_str(),
           chain_tip_height,
           blocks_percentile.p5.GetFeePerK(),
           blocks_percentile.p25.GetFeePerK(),
           blocks_percentile.p50.GetFeePerK(),
           blocks_percentile.p75.GetFeePerK());

    forecast_options.m_l_priority_estimate = blocks_percentile.p25;
    forecast_options.m_h_priority_estimate = blocks_percentile.p50;
    return ForecastResult(forecast_options, std::nullopt);
}

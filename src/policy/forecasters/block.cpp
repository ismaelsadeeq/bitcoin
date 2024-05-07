// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/mempool_entry.h>
#include <logging.h>
#include <policy/fee_estimator.h>
#include <policy/forecasters/block.h>
#include <util/trace.h>

#include <queue>

void BlockForecaster::MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight)
{
    chain_tip_height = nBlockHeight;
    const std::vector<std::tuple<CFeeRate, uint64_t>> size_per_feerate = LinearizeTransactions(txs_removed_for_block).size_per_feerate;
    BlockPercentiles percentiles = CalculateBlockPercentiles(size_per_feerate);
    if (percentiles.p75 != CFeeRate(0)) {
        if (blocks_percentiles.size() == MAX_NUMBER_OF_BLOCKS) {
            blocks_percentiles.pop();
        }
        blocks_percentiles.push(percentiles);
    }
}

ForecastResult BlockForecaster::EstimateFee(unsigned int targetBlocks)
{
    ForecastResult::ForecastOptions forecast_options;
    forecast_options.m_forecaster = BLOCK_FORECAST_NAME_STR;
    forecast_options.m_block_height = chain_tip_height;
    if (targetBlocks <= 0) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target must be greater than zero", BLOCK_FORECAST_NAME_STR));
    }

    if (targetBlocks > BLOCK_FORECAST_MAX_TARGET) {
        return ForecastResult(forecast_options, strprintf("%s: Confirmation target %u is above the maximum limit of %u",
                                                          BLOCK_FORECAST_NAME_STR, targetBlocks, BLOCK_FORECAST_MAX_TARGET));
    }

    if (blocks_percentiles.size() < MAX_NUMBER_OF_BLOCKS) {
        return ForecastResult(forecast_options, strprintf("%s: Insufficient block data to perform an estimate", BLOCK_FORECAST_NAME_STR));
    }

    BlockPercentiles percentiles_average;

    std::queue<BlockPercentiles> blocks_percentiles_cp = blocks_percentiles;
    while (!blocks_percentiles_cp.empty()) {
        const auto& curr_percentile = blocks_percentiles_cp.front();
        blocks_percentiles_cp.pop();
        percentiles_average.p5 += curr_percentile.p5;
        percentiles_average.p25 += curr_percentile.p25;
        percentiles_average.p50 += curr_percentile.p50;
        percentiles_average.p75 += curr_percentile.p75;
    }

    percentiles_average.p5 = CFeeRate(percentiles_average.p5.GetFeePerK() / MAX_NUMBER_OF_BLOCKS);
    percentiles_average.p25 = CFeeRate(percentiles_average.p25.GetFeePerK() / MAX_NUMBER_OF_BLOCKS);
    percentiles_average.p50 = CFeeRate(percentiles_average.p50.GetFeePerK() / MAX_NUMBER_OF_BLOCKS);
    percentiles_average.p75 = CFeeRate(percentiles_average.p75.GetFeePerK() / MAX_NUMBER_OF_BLOCKS);

    LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s: Block height %s, 75th percentile fee rate %s %s/kvB, 50th percentile fee rate %s %s/kvB, 25th percentile fee rate %s %s/kvB, 5th percentile fee rate %s %s/kvB\n",
             BLOCK_FORECAST_NAME_STR, chain_tip_height, percentiles_average.p75.GetFeePerK(), CURRENCY_ATOM, percentiles_average.p50.GetFeePerK(), CURRENCY_ATOM, percentiles_average.p25.GetFeePerK(), CURRENCY_ATOM, percentiles_average.p5.GetFeePerK(), CURRENCY_ATOM);

    TRACE7(feerate_forecast, forecast_generated,
           targetBlocks,
           BLOCK_FORECAST_NAME_STR.c_str(),
           chain_tip_height,
           percentiles_average.p5.GetFeePerK(),
           percentiles_average.p25.GetFeePerK(),
           percentiles_average.p50.GetFeePerK(),
           percentiles_average.p75.GetFeePerK());

    forecast_options.m_l_priority_estimate = percentiles_average.p25;
    forecast_options.m_h_priority_estimate = percentiles_average.p50;
    return ForecastResult(forecast_options, std::nullopt);
}

// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/feerate.h>
#include <policy/forecasters/ntime.h>
#include <policy/forecaster.h>
#include <policy/fees_util.h>
#include <node/mini_miner.h>
#include <validationinterface.h>
#include <util/time.h>
#include <consensus/consensus.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <chrono>
#include <utility>
#include <logging.h>

using node::MiniMiner;

NTime::NTime()
    : Forecaster{ForecastType::NTIME}, tracking_stats{initStats()} {}

NTime::TrackingVector NTime::initStats()
{
    TrackingVector temp_stats(MAX_HOURS);
    for (int i = 0; i < MAX_HOURS; ++i) {
        temp_stats[i].resize(i + 1);
    }
    return temp_stats;
}

void NTime::addTxToStats(const ConfirmedTx& tx)
{
    auto calculateHoursIndex = [](int64_t start, int64_t end) {
        return std::max(0, static_cast<int>(std::ceil(static_cast<double>(end - start) / SECONDS_IN_HOUR)) - 1);
    };
    auto interval = calculateHoursIndex(tx.m_receivedTime, tx.m_confirmedTime);
    if (interval >= MAX_HOURS) {
        LogDebug(BCLog::ESTIMATEFEE, "%s: Transaction age is more than the maximum that can be tracked.\n", forecastTypeToString(m_forecastType));
        return;
    }
    LogDebug(BCLog::ESTIMATEFEE, "%s: Adding new transaction to tracking stats. Arrived at %d, removed at %d, added to %d hour ago bucket, in confirmed after %d hours sub-bucket.\n",
             forecastTypeToString(m_forecastType), tx.m_receivedTime, tx.m_confirmedTime, interval, interval);

    tracking_stats[interval][interval].emplace_back(tx);
}

void NTime::UpdateTrackingStats()
{
    LogDebug(BCLog::ESTIMATEFEE,":%s:, Updating tracking stats.\n", forecastTypeToString(m_forecastType));
    auto temp_stats = initStats();
    for (int i = 0; i < (MAX_HOURS - 1); i++) {
        temp_stats[i + 1] = tracking_stats[i];
        temp_stats[i + 1].emplace_back(std::vector<ConfirmedTx>{});
    }
    tracking_stats = temp_stats;
}

void NTime::MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight)
{
    std::map<Txid, const RemovedMempoolTransactionInfo&> tx_caches;
    for (const auto& tx : txs_removed_for_block) {
        tx_caches.emplace(tx.info.m_tx->GetHash(), tx);
    }

    auto mini_miner_input = GetMiniMinerInput(txs_removed_for_block);
    auto linearizedResult = MiniMiner(std::get<0>(mini_miner_input), std::get<1>(mini_miner_input)).Linearize();
    auto current_time = GetTime();
    for (const auto& tx : linearizedResult.inclusion_order) {
        auto it = tx_caches.find(tx.first);
        if (it != tx_caches.end()) {
            ConfirmedTx confTx{it->second.nTime.count(), current_time, std::get<0>(linearizedResult.size_per_feerate[tx.second]), 
                               std::get<1>(linearizedResult.size_per_feerate[tx.second])};
            addTxToStats(confTx);
        }
    }
    LogDebug(BCLog::ESTIMATEFEE, "%s: Tracked %zu transactions after new block is connected at height %u.\n",
            forecastTypeToString(m_forecastType), txs_removed_for_block.size(), nBlockHeight);
}

 NTime::PackagesAndWeight NTime::GetTxsWithinTime(int start_hr, int end_hr) const
{
    std::vector<std::tuple<CFeeRate, uint32_t>> txs_within_range;
    auto current_time = GetTime();
    auto start_timestamp = current_time - (start_hr * SECONDS_IN_HOUR);
    auto end_timestamp = current_time - (end_hr * SECONDS_IN_HOUR);
    auto max_conf_index = (start_hr - end_hr) - 1;
    unsigned int total_weight{0};

    // Loop through all the previously seen hours.
    // Also check 1 hour before in case tracking stats were
    // updated recently.
    for (int i = start_hr; i >= end_hr; i--) {
        // If the hour is not tracked; skip
        if (tracking_stats.size() <= static_cast<size_t>(start_hr)) continue;
        // Loop through all the confirmation hours in the previously seen hours.
        for (int j = 0; j <= max_conf_index; j++) {
            // If the seen hour does can't track that confirmation hour; skip
            if (tracking_stats[i].size() < static_cast<size_t>(j + 1)) continue;
            auto txs = tracking_stats[i][j];
            for (const auto& tx : txs) {
                // Add all transactions that were seen after and confirmed within the time range.
                if (tx.m_receivedTime >= start_timestamp && tx.m_confirmedTime <= end_timestamp) {
                    // Its confirmation time has to be before the ending timestamp.
                    txs_within_range.emplace_back(tx.fee_rate, tx.vsize);
                    total_weight += tx.vsize / WITNESS_SCALE_FACTOR;
                }
            }
        }
    }
    // Sort all added transactions by increasing fee rate.
    std::sort(txs_within_range.begin(), txs_within_range.end(), [](const auto& tx1, const auto& tx2) {
        return std::get<0>(tx1).GetFeePerK() < std::get<0>(tx2).GetFeePerK();
    });
    return std::make_pair(txs_within_range, total_weight);
}

BlockPercentiles NTime::GetWindowEstimate(int hours) const
{
    auto packages_and_weight = GetTxsWithinTime(hours, /*end_h=*/0);
    LogDebug(BCLog::ESTIMATEFEE, "Calling calculate percentile in window with %s txs and weight %s \n", packages_and_weight.first.size(), packages_and_weight.second);
    return CalculateBlockPercentiles(packages_and_weight.first, packages_and_weight.second);
}

BlockPercentiles NTime::GetHistoricalEstimate(int hours) const
{
    int start_hr = (std::ceil(static_cast<double>(hours) / 24) * 24);
    int end_hr = start_hr - hours;
    auto packages_and_weight = GetTxsWithinTime(start_hr, end_hr);
    LogDebug(BCLog::ESTIMATEFEE, "Calling calculate percentile in historical with %s txs and weight %u \n", packages_and_weight.first.size(), packages_and_weight.second);
    return CalculateBlockPercentiles(packages_and_weight.first, packages_and_weight.second);
}

ForecastResult NTime::EstimateFee(int targetHours)
{
    ForecastResult::ForecastOptions forecast_options;
    forecast_options.forecaster = m_forecastType;

    if (targetHours > MAX_HOURS) {
         return ForecastResult(forecast_options, strprintf("Confirmation target %s is above maximum limit of %s.",
                                                          targetHours, MAX_HOURS));
    }

    // Get Window Estimate
    auto window_percentiles = GetWindowEstimate(targetHours);
    if (window_percentiles.empty()) {
        return ForecastResult(forecast_options, strprintf("%s: Not enough tracked data to provide window estimate.\n", forecastTypeToString(m_forecastType)));
    }
    // Log the window estimate
    LogDebug(BCLog::ESTIMATEFEE, "%s: Window: %d hours, 75th percentile fee rate: %s %s/kvB, 50th percentile feerate %s %s/kvB, 25th percentile feerate %s %s/kvB, 5th percentile feerate %s %s/kvB \n",
            forecastTypeToString(m_forecastType), targetHours, window_percentiles.p75.GetFeePerK(), CURRENCY_ATOM, window_percentiles.p50.GetFeePerK(), CURRENCY_ATOM,
            window_percentiles.p25.GetFeePerK(), CURRENCY_ATOM, window_percentiles.p5.GetFeePerK(), CURRENCY_ATOM);

    // Get Historical Estimate
    auto historical_percentiles = GetHistoricalEstimate(targetHours);
    if (historical_percentiles.empty()) {
        return ForecastResult(forecast_options, strprintf("%s: Not enough tracked data to provide historical estimate.\n", forecastTypeToString(m_forecastType)));
    }
    // Log the historical estimate
    LogDebug(BCLog::ESTIMATEFEE, "%s: Historical: %d hours, 75th percentile fee rate: %s %s/kvB, 50th percentile feerate %s %s/kvB, 25th percentile feerate %s %s/kvB, 5th percentile feerate %s %s/kvB \n",
            forecastTypeToString(m_forecastType), targetHours, historical_percentiles.p75.GetFeePerK(), CURRENCY_ATOM, historical_percentiles.p50.GetFeePerK(), CURRENCY_ATOM,
            historical_percentiles.p25.GetFeePerK(), CURRENCY_ATOM, historical_percentiles.p5.GetFeePerK(), CURRENCY_ATOM);

    // Compare Window and Historical Estimates
    if (window_percentiles.p75 < historical_percentiles.p75) {
        forecast_options.low_priority = window_percentiles.p25;
        forecast_options.high_priority = window_percentiles.p50;
        return ForecastResult(forecast_options, std::nullopt);
    }

    forecast_options.low_priority = historical_percentiles.p25;
    forecast_options.high_priority = historical_percentiles.p50;
    return ForecastResult(forecast_options, std::nullopt);
}
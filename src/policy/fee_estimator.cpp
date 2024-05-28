// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <policy/fee_estimator.h>
#include <policy/feerate.h>
#include <util/trace.h>

void FeeEstimator::RegisterForecaster(std::shared_ptr<Forecaster> forecaster)
{
    forecasters.push_back(forecaster);
}

std::pair<ForecastResult, std::vector<std::string>> FeeEstimator::GetFeeEstimateFromForecasters(unsigned int targetBlocks)
{
    // Request estimates from all registered forecasters and select the lowest
    ForecastResult::ForecastOptions opts;
    ForecastResult forecast = ForecastResult(opts, std::nullopt);

    std::vector<std::string> err_messages;
    for (auto& forecaster : forecasters) {
        auto currForecast = forecaster->EstimateFee(targetBlocks);
        if (!currForecast.empty()) {
            if (currForecast < forecast || forecast.empty()) {
                forecast = currForecast;
            }
        } else if (currForecast.m_err_message) {
            LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s Block height %s, Error:  %s.\n",
                     forecast.m_forecast_opt.m_forecaster, forecast.m_forecast_opt.m_block_height, currForecast.m_err_message.value());
            err_messages.push_back(currForecast.m_err_message.value());
        }
    }

    if (!forecast.empty()) {
        LogPrint(BCLog::ESTIMATEFEE, "FeeEst: %s, Block height %s, low priority feerate %s %s/kvB, high priority feerate %s %s/kvB.\n",
                 forecast.m_forecast_opt.m_forecaster, forecast.m_forecast_opt.m_block_height, forecast.m_forecast_opt.m_l_priority_estimate.GetFeePerK(),
                 CURRENCY_ATOM, forecast.m_forecast_opt.m_h_priority_estimate.GetFeePerK(), CURRENCY_ATOM);

        TRACE5(fee_estimator, estimate_calculated,
               targetBlocks,
               forecast.m_forecast_opt.m_block_height,
               forecast.m_forecast_opt.m_forecaster.c_str(),
               forecast.m_forecast_opt.m_l_priority_estimate.GetFeePerK(),
               forecast.m_forecast_opt.m_h_priority_estimate.GetFeePerK());
    }
    return std::make_pair(forecast, err_messages);
};

void FeeEstimator::GetAllEstimates(unsigned int targetBlocks)
{
    // Request estimates from all registered forecasters and select the lowest
    std::vector<std::string> err_messages;
    for (auto& forecaster : forecasters) {
        auto forecast = forecaster->EstimateFee(targetBlocks);
        if (!forecast.empty()) {
            LogInfo("FeeEst Forecaster: %s, %s, %s, %s\n",
                 forecast.m_forecast_opt.m_forecaster, forecast.m_forecast_opt.m_block_height, forecast.m_forecast_opt.m_l_priority_estimate.GetFeePerK(),
                 forecast.m_forecast_opt.m_h_priority_estimate.GetFeePerK());
        }
    }
    if (legacy_estimator) {
        FeeCalculation feeCalc;
        bool conservative = true;
        CFeeRate feeRate_conservative{legacy_estimator.value()->estimateSmartFee(targetBlocks, &feeCalc, conservative)};
        CFeeRate feeRate_economical{legacy_estimator.value()->estimateSmartFee(targetBlocks, &feeCalc, !conservative)};
        LogInfo("FeeEstLog PolicyEstimator: %s, %s, %s\n", feeCalc.bestheight, feeRate_conservative.GetFeePerK(), feeRate_economical.GetFeePerK());
    }
};

unsigned int FeeEstimator::MaxForecastingTarget()
{
    unsigned int max_target = 0;
    for (auto& forecaster : forecasters) {
        max_target = std::max(forecaster->MaxTarget(), max_target);
    }
    return max_target;
}

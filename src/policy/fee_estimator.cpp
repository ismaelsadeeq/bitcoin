// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/fee_estimator.h>
#include <policy/forecaster.h>
#include <policy/forecaster_util.h>

void FeeEstimator::RegisterForecaster(std::shared_ptr<Forecaster> forecaster)
{
    forecasters.emplace(forecaster->GetForecastType(), forecaster);
}

ForecastResult FeeEstimator::GetPolicyEstimatorEstimate(int targetBlocks)
{
    ForecastResult::ForecastOptions forecast_options;
    bool conservative = true;
    FeeCalculation feeCalcConservative;
    CFeeRate feeRate_conservative{block_policy_estimator.value()->estimateSmartFee(targetBlocks, &feeCalcConservative, conservative)};
    forecast_options.high_priority = feeRate_conservative;
    FeeCalculation feeCalcEconomical;
    CFeeRate feeRate_economical{block_policy_estimator.value()->estimateSmartFee(targetBlocks, &feeCalcEconomical, !conservative)};
    forecast_options.low_priority = feeRate_economical;
    forecast_options.forecaster = ForecastType::BLOCK_POLICY_ESTIMATOR;
    forecast_options.block_height = feeCalcEconomical.bestheight;
    return ForecastResult(forecast_options, std::nullopt);
}

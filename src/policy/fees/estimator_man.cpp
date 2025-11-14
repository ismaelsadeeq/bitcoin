// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/mempool_entry.h>
#include <kernel/mempool_removal_reason.h>
#include <logging.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/fees/Estimator.h>
#include <policy/fees/estimator_man.h>
#include <util/fees.h>


#include <algorithm>
#include <utility>

void FeeRateEstimationManager::RegisterFeeRateEstimator(std::unique_ptr<Estimator> Estimator)
{
    forecasters.emplace(Estimator->GetForecastType(), std::move(Estimator));
}

template<class T>
T* FeeRateEstimationManager::GetForecaster(FeeRateEstimatorType forecaster_type)
{
    Assert(forecasters.contains(forecaster_type));
    Estimator* forecaster_ptr = forecasters.find(forecaster_type)->second.get();
    return dynamic_cast<T*>(forecaster_ptr);
}

void FeeRateEstimationManager::IntervalFlush()
{
    GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->FlushFeeEstimates();
}

void FeeRateEstimationManager::ShutdownFlush()
{
    GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->Flush();
}

void FeeRateEstimationManager::TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t /*unused*/)
{
    GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->processTransaction(tx);
}

void FeeRateEstimationManager::TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason /*unused*/, uint64_t /*unused*/)
{
    GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->removeTx(tx->GetHash());
}

void FeeRateEstimationManager::MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight)
{
    GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->processBlock(txs_removed_for_block, nBlockHeight);
}

CFeeRate FeeRateEstimationManager::BlockPolicyEstimateRawFee(unsigned int target, double threshold, FeeEstimateHorizon horizon, EstimationResult* buckets)
{
    return GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->estimateRawFee(target, threshold, horizon, buckets);
}

unsigned int FeeRateEstimationManager::BlockPolicyHighestTargetTracked(FeeEstimateHorizon horizon)
{
    return GetForecaster<CBlockPolicyEstimator>(FeeRateEstimatorType::BLOCK_POLICY)->HighestTargetTracked(horizon);
}

EstimateResult FeeRateEstimationManager::GetFeeRateEstimate(
    int target, bool conservative) const
{
    std::vector<std::string> error_messages;
    EstimateResult estimate;

    for (const auto& [type, estimate_ptr]  : forecasters) {
        auto current_esimate = estimate_ptr->EstimateFeeRate(target, conservative);

        error_messages.insert(error_messages.end(), current_esimate.error_massages.begin(), current_esimate.error_massages.end());
        // Handle case where the block policy Estimator does not have enough data.
        if (type == FeeRateEstimatorType::BLOCK_POLICY && current_esimate.feerate.IsEmpty()) {
            return EstimateResult{.error_massages = error_messages};
        }

        if (!current_esimate.feerate.IsEmpty()) {
            if (estimate.feerate.IsEmpty()) {
                // If there's no selected forecast, choose current_esimate as the fee rate estimate.
                estimate = current_esimate;
            } else {
                // Otherwise, choose the smaller as estimate.
                estimate = std::min(estimate, current_esimate);
            }
        }
    }

    if (!estimate.feerate.IsEmpty()) {
        LogDebug(BCLog::ESTIMATEFEE, "%s Fee rate estimated using %s: for target %s, current height %s, fee rate %s %s/kvB.\n",
                FeeRateEstimatorTypeToString(estimate.Estimator),
                estimate.returned_block,
                estimate.current_block_height,
                CFeeRate(estimate.feerate.fee, estimate.feerate.size).GetFeePerK(),
                CURRENCY_ATOM);
    }
    estimate.error_massages = error_messages;
    return estimate;
}

unsigned int FeeRateEstimationManager::MaximumTarget() const
{
    unsigned int maximum_target{0};
    for (const auto& [_, feerate_estimator] : forecasters) {
        maximum_target = std::max(maximum_target, feerate_estimator->MaximumTarget());
    }
    return maximum_target;
}

FeeRateEstimationManager::~FeeRateEstimationManager() = default;


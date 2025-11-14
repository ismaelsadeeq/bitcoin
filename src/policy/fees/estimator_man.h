// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FEES_ESTIMATOR_MAN_H
#define BITCOIN_POLICY_FEES_ESTIMATOR_MAN_H

#include <primitives/transaction.h>
#include <validationinterface.h>

#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>

// How often to flush data to disk
static constexpr std::chrono::hours FLUSH_INTERVAL{1};

class CFeeRate;
class FeeRateEstimator;
struct EstimateResult;
struct EstimationResult;

enum class FeeRateEstimatorType;
enum class FeeEstimateHorizon;
struct NewMempoolTransactionInfo;


enum class MemPoolRemovalReason;
struct RemovedMempoolTransactionInfo;

/** \class FeeRateEstimationManager
 * Manages multiple fee rate estimators.
 */
class FeeRateEstimationManager: public CValidationInterface
{
private:
    //! Map of all registered feerate estimator to their unique pointers.
    std::unordered_map<FeeRateEstimatorType, std::unique_ptr<FeeRateEstimator>> feerate_estimators;

    /*
     * Return the pointer to a FeeRateEstimator given a forecast type.
     */
    template<class T>
    T* GetForecaster(FeeRateEstimatorType forecaster_type);
public:
    virtual ~FeeRateEstimationManager();
    /**
     * Register a Fee rate Estimator.
     * @param[in] Estimator unique pointer to a FeeRateEstimator instance.
     */
    void RegisterFeeRateEstimator(std::unique_ptr<FeeRateEstimator> feerate_estimator);

    /**
     * Get a fee rate estimate from all registered feerate estimator for a given confirmation target.
     *
     * Polls all registered feerate estimator and selects the lowest fee rate.
     *
     * @param[in] target The target within which the transaction should be confirmed.
     * @param[in] conservative True if the package cannot be fee bumped later.
     * @return forecast result
     */
    virtual EstimateResult GetFeeRateEstimate(int target, bool conservative) const;

    /* Flush recorded data to disk. */
    void IntervalFlush();

    /* Flush recorded data to disk as part of shutdown sequence*/
    void ShutdownFlush();

    /**
     * @brief Returns the maximum supported confirmation target from all feerate estimator.
     */
    unsigned int MaximumTarget() const;

    /**
    * @brief call block policy estimator estimaterawfee (test-only).
    * 
    **/
    CFeeRate BlockPolicyEstimateRawFee(unsigned int target, double threshold, FeeEstimateHorizon horizon, EstimationResult* buckets);

    /**
    * @brief Returns the maximum supported confirmation target of block policy estimator (test-only).
    * 
    */
    unsigned int BlockPolicyHighestTargetTracked(FeeEstimateHorizon horizon);

    /** Overridden from CValidationInterface. */
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t /*unused*/) override;
    void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason /*unused*/, uint64_t /*unused*/) override;
    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight) override;
};

#endif // BITCOIN_POLICY_FEES_ESTIMATOR_MAN_H

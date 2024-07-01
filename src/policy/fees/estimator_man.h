// Copyright (c) 2025 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FEES_ESTIMATOR_MAN_H
#define BITCOIN_POLICY_FEES_ESTIMATOR_MAN_H

#include <primitives/transaction.h>
<<<<<<< HEAD
#include <validationinterface.h>

#include <chrono>
=======
#include <sync.h>
#include <threadsafety.h>
#include <validationinterface.h>

>>>>>>> c6abe192b8a (fees: introduce lock to forecaster manager and sanity check)
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// How often to flush data to disk
static constexpr std::chrono::hours FLUSH_INTERVAL{1};

<<<<<<< HEAD:src/policy/fees/estimator_man.h
class CFeeRate;
class FeeRateEstimator;
struct EstimateResult;
=======
class Forecaster;
<<<<<<< HEAD
struct ForecastResult;
>>>>>>> 4216c91ce6f (fees: introduce lock to forecaster manager and sanity check):src/policy/fees/forecaster_man.h
struct EstimationResult;

enum class FeeRateEstimatorType;
enum class FeeEstimateHorizon;
<<<<<<< HEAD:src/policy/fees/estimator_man.h
struct NewMempoolTransactionInfo;
=======
=======

enum class ForecastType;
enum class MemPoolRemovalReason;

struct RemovedMempoolTransactionInfo;
struct NewMempoolTransactionInfo;
struct ForecastResult;

// Constants for mempool sanity checks.
constexpr size_t NUMBER_OF_BLOCKS = 6;
constexpr double HEALTHY_BLOCK_PERCENTILE = 0.75;
>>>>>>> c6abe192b8a (fees: introduce lock to forecaster manager and sanity check)
>>>>>>> 4216c91ce6f (fees: introduce lock to forecaster manager and sanity check):src/policy/fees/forecaster_man.h


enum class MemPoolRemovalReason;
struct RemovedMempoolTransactionInfo;

/** \class FeeRateEstimationManager
 * Manages multiple fee rate estimators.
 */
<<<<<<< HEAD:src/policy/fees/estimator_man.h
class FeeRateEstimationManager: public CValidationInterface
{
private:
    //! Map of all registered feerate estimator to their unique pointers.
    std::unordered_map<FeeRateEstimatorType, std::unique_ptr<FeeRateEstimator>> feerate_estimators;
=======
<<<<<<< HEAD
class FeeRateForecasterManager: public CValidationInterface
=======
class FeeRateForecasterManager : public CValidationInterface
>>>>>>> c6abe192b8a (fees: introduce lock to forecaster manager and sanity check)
{
private:
    //! Map of all registered forecasters to their shared pointers.
<<<<<<< HEAD
    std::unordered_map<ForecastType, std::unique_ptr<Forecaster>> forecasters;
>>>>>>> 4216c91ce6f (fees: introduce lock to forecaster manager and sanity check):src/policy/fees/forecaster_man.h

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
<<<<<<< HEAD:src/policy/fees/estimator_man.h
    void RegisterFeeRateEstimator(std::unique_ptr<FeeRateEstimator> feerate_estimator);

    /**
     * Get a fee rate estimate from all registered feerate estimator for a given confirmation target.
     *
     * Polls all registered feerate estimator and selects the lowest fee rate.
=======
    void RegisterForecaster(std::unique_ptr<Forecaster> forecaster);
<<<<<<< HEAD
=======
=======
    void RegisterForecaster(std::shared_ptr<Forecaster> forecaster);
=======
    std::unordered_map<ForecastType, std::shared_ptr<Forecaster>> forecasters GUARDED_BY(cs);

    mutable RecursiveMutex cs;

    //! Structure to track the health of mined blocks.
    struct BlockData {
        size_t m_height;                      //!< Block height.
        bool empty{false};                    //!< Whether the block is empty.
        size_t m_removed_block_txs_weight;    //!< Removed transaction weight from the mempool.
        std::optional<size_t> m_block_weight; //!< Weight of the block.

        BlockData(size_t height, size_t removed_block_txs_weight)
            : m_height(height), m_removed_block_txs_weight(removed_block_txs_weight) {}
    };

    //! Tracks the statistics of previously mined blocks.
    std::vector<BlockData> prev_mined_blocks GUARDED_BY(cs);

    //! Checks if recent mined blocks indicate a healthy mempool state.
    bool IsMempoolHealthy() const EXCLUSIVE_LOCKS_REQUIRED(cs);

    //! Computes the total weight of transactions in a block.
    size_t CalculateBlockWeight(const std::vector<CTransactionRef>& txs) const EXCLUSIVE_LOCKS_REQUIRED(cs);

protected:
    /** Overridden from CValidationInterface. */
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t /*unused*/) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason /*unused*/, uint64_t /*unused*/) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs);

    void BlockConnected(ChainstateRole /*unused*/, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs);


public:
    FeeRateForecasterManager() EXCLUSIVE_LOCKS_REQUIRED(!cs);
    virtual ~FeeRateForecasterManager();
>>>>>>> 3001496d3bd (fees: introduce lock to forecaster manager and sanity check)

    //! Registers a new fee forecaster.
    void RegisterForecaster(std::shared_ptr<Forecaster> forecaster) EXCLUSIVE_LOCKS_REQUIRED(!cs);

    //! Returns a pointer to the block policy estimator.
    CBlockPolicyEstimator* GetBlockPolicyEstimator() EXCLUSIVE_LOCKS_REQUIRED(!cs);

    //! Retrieves block data as a human-readable string.
    std::vector<std::string> GetPreviouslyMinedBlockDataStr() const EXCLUSIVE_LOCKS_REQUIRED(!cs);
>>>>>>> 84001097bb2 (fees: introduce lock to forecaster manager and sanity check)

    /**
     * Estimates a fee rate using registered forecasters for a given confirmation target.
     *
     * Iterates through all registered forecasters and selects the lowest viable fee estimate
     * with acceptable confidence.
>>>>>>> 4216c91ce6f (fees: introduce lock to forecaster manager and sanity check):src/policy/fees/forecaster_man.h
     *
     * @param[in] target The target within which the transaction should be confirmed.
<<<<<<< HEAD
     * @param[in] conservative True if the package cannot be fee bumped later.
     * @return forecast result
=======
     * @param[in] conservative If true, returns a higher fee rate for greater confirmation probability.
     * @return A pair consisting of the forecast result and a vector of error messages.
>>>>>>> 6706609a8ce (rpc: update `estimatefeesmartfee` to use forecaster_man)
     */
<<<<<<< HEAD:src/policy/fees/estimator_man.h
    virtual EstimateResult GetFeeRateEstimate(int target, bool conservative) const;
=======
<<<<<<< HEAD
    virtual ForecastResult ForecastFeeRateFromForecasters(int target, bool conservative) const;
>>>>>>> 4216c91ce6f (fees: introduce lock to forecaster manager and sanity check):src/policy/fees/forecaster_man.h

    /* Flush recorded data to disk. */
    void IntervalFlush();

    /* Flush recorded data to disk as part of shutdown sequence*/
    void ShutdownFlush();
=======
    std::pair<ForecastResult, std::vector<std::string>> ForecastFeeRateFromForecasters(int target, bool conservative) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs);
>>>>>>> c6abe192b8a (fees: introduce lock to forecaster manager and sanity check)

    /**
     * @brief Returns the maximum supported confirmation target from all feerate estimator.
     */
<<<<<<< HEAD
    unsigned int MaximumTarget() const;
<<<<<<< HEAD

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
=======
<<<<<<< HEAD
=======
<<<<<<< HEAD
>>>>>>> d48c7c42b3e (fees: add `ForecastFeeRateFromForecasters` method)
=======
>>>>>>> c01e8929381 (fees: add block policy estimator to forecaster manager)
=======
    unsigned int MaximumTarget() const EXCLUSIVE_LOCKS_REQUIRED(!cs);
>>>>>>> 3001496d3bd (fees: introduce lock to forecaster manager and sanity check)
>>>>>>> 4996cfd9381 (fees: introduce lock to forecaster manager and sanity check)
>>>>>>> 84001097bb2 (fees: introduce lock to forecaster manager and sanity check)
>>>>>>> c6abe192b8a (fees: introduce lock to forecaster manager and sanity check)
};

#endif // BITCOIN_POLICY_FEES_ESTIMATOR_MAN_H

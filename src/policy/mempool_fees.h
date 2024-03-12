// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_MEMPOOL_FEES_H
#define BITCOIN_POLICY_MEMPOOL_FEES_H

#include <chrono>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <util/hasher.h>
#include <validationinterface.h>

#include <logging.h>
#include <policy/feerate.h>

class CBlockPolicyEstimator;
class Chainstate;
class ChainstateManager;
class CTxMemPool;

// Fee rate estimates above this confirmation target are not reliable,
// mempool condition might likely change.
static const unsigned int MAX_CONF_TARGET{1};

static const unsigned int MAX_UNCONF_COUNT{5};

static constexpr std::chrono::minutes FEE_ESTIMATE_INTERVAL{1};

// Fee estimation result containing percentiles (in sat/kvB).
struct MempoolFeeEstimationResult {
    CFeeRate p5;  // 5th percentile
    CFeeRate p25; // 25th percentile
    CFeeRate p50; // 50th percentile
    CFeeRate p75; // 75th percentile

    // Default constructor initializes all percentiles to CFeeRate(0).
    MempoolFeeEstimationResult() : p5(CFeeRate(0)), p25(CFeeRate(0)), p50(CFeeRate(0)), p75(CFeeRate(0)) {}

    // Check if all percentiles are CFeeRate(0).
    bool empty() const
    {
        return p5 == CFeeRate(0) && p25 == CFeeRate(0) && p50 == CFeeRate(0) && p75 == CFeeRate(0);
    }
};
;

/**
 * CachedMempoolEstimates holds a cache of recent mempool-based fee estimates.
 * Running the block-building algorithm multiple times is undesriable due to
 * locking.
 */
struct CachedMempoolEstimates {
private:
    // shared_mutex allows for multiple concurrent reads, but only a single update
    mutable std::shared_mutex cache_mutex;
    static constexpr std::chrono::seconds cache_life{30};
    std::map<uint64_t, MempoolFeeEstimationResult> estimates;
    std::chrono::steady_clock::time_point last_updated;

    bool isStale() const
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return (last_updated + cache_life) < std::chrono::steady_clock::now();
    }

public:
    CachedMempoolEstimates() : last_updated(std::chrono::steady_clock::now() - cache_life - std::chrono::seconds(1)) {}
    CachedMempoolEstimates(const CachedMempoolEstimates&) = delete;
    CachedMempoolEstimates& operator=(const CachedMempoolEstimates&) = delete;

    std::optional<MempoolFeeEstimationResult> get(uint64_t number_of_blocks) const
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        if (isStale()) return std::nullopt;
        LogPrint(BCLog::MEMPOOL, "CachedMempoolEstimates : cache is not stale, using cached value\n");

        auto it = estimates.find(number_of_blocks);
        if (it != estimates.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void update(const std::map<uint64_t, MempoolFeeEstimationResult>& newEstimates)
    {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        // Overwrite the entire map with the new data to avoid old
        // estimates remaining.
        estimates = newEstimates;
        last_updated = std::chrono::steady_clock::now();
        LogPrint(BCLog::MEMPOOL, "CachedMempoolEstimates: updated cache\n");
    }
};

/**
 * MemPoolPolicyEstimator estimates the fee rate that a tx should pay
 * to be included in a confirmation target based on the mempool
 * txs and their fee rates.
 *
 * The estimator works by generating template block up to a given confirmation target and then calculate the median
 * fee rate of the txs in the confirmation target block as the approximate fee rate that a tx will pay to
 * likely be included in the block.
 */
class MemPoolPolicyEstimator : public CValidationInterface
{
public:
    MemPoolPolicyEstimator();

    virtual ~MemPoolPolicyEstimator() = default;

    /**
     * Estimate the fee rate from mempool txs data given a confirmation target.
     *
     * @param[in] chainstate The reference to the active chainstate.
     * @param[in] mempool The reference to the mempool from which we will estimate the fee rate.
     * @param[in] confTarget The confirmation target of transactions.
     * @param[out] err_message  optional error message.
     * @return The estimated fee rates.
     */
    MempoolFeeEstimationResult EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget, const bool force, std::string& err_message) const;
    void EstimateFeeWithMemPool(const ChainstateManager& chainstate, const CTxMemPool& mempool, const CBlockPolicyEstimator* fee_estimator) const;

private:
    mutable CachedMempoolEstimates cache;
    /**
     * Calculate the fee rate estimate for blocks of txs up to num_blocks.
     *
     * @param[in] mempool_fee_stats The mempool fee statistics (fee rate and size).
     * @param[in] num_blocks The numbers of blocks to calculate fees for.
     * @return The MempoolFeeEstimationResult of a given number of blocks.
     */
    std::map<uint64_t, MempoolFeeEstimationResult> EstimateBlockFeeRatesWithMempool(const std::vector<std::tuple<CFeeRate, uint64_t>>& mempool_fee_stats, unsigned int num_blocks) const;

    /**
     * Calculate the median fee rate for a range of txs in the mempool.
     *
     * @param[in] start_it The iterator pointing to the beginning of the range.
     * @param[in] end_it The iterator pointing to the end of the range.
     * @return MempoolFeeEstimationResult of a given block.
     */
    MempoolFeeEstimationResult CalculateBlockPercentiles(std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator start_it, std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator end_it) const;
    CFeeRate CalculateMedianFeeRate(std::vector<std::tuple<CFeeRate, uint64_t>>::const_iterator start_it, std::vector<std::tuple<CFeeRate, uint64_t>>::const_iterator end_it) const;

    struct block_info {
        unsigned int height;
        bool roughly_synced;
    };

    std::array<block_info, 3> top_blocks;

    std::map<Txid, unsigned int> expectedMinedTxs;

    // Whenever we receive a new block we record it's status if its in sync or not.
    void UpdateTopBlocks(const block_info& new_blk_info);

    // Determine whether the last that we tracked are sequential.
    bool AreTopBlocksInOrder() const;

    bool RoughlySynced() const; // Tells us whether our mempool is roughly in sync with miners mempool.

    void InsertNewBlock(const block_info& new_blk_info);

    void IncrementTxsCount(const std::set<Txid>& txs);

    std::set<Txid> GetTxsToExclude() const;

protected:
    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, const std::vector<CTransactionRef>& expected_block_txs, const std::vector<CTransactionRef>& block_txs, unsigned int nBlockHeight) override;
};
#endif // BITCOIN_POLICY_MEMPOOL_FEES_H

// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_MEMPOOL_FEES_H
#define BITCOIN_POLICY_MEMPOOL_FEES_H

#include <map>
#include <optional>

#include <policy/feerate.h>
#include <primitives/block.h>

class Chainstate;
class CTxMemPool;
class CTxMemPoolEntry;

// In Every five minutes we build a block template that we expect to be mined
// based on the mempool transactions
static constexpr std::chrono::minutes APPROX_TIME_TO_GENERATE_BLK{5};

/**
 * MemPoolPolicyEstimator estimates the fee rate that a tx should pay
 * to be included in a confirmation target based on the mempool
 * txs and their fee rates.
 *
 * The estimator works by generating template block up to a given confirmation target and then calculate the median
 * fee rate of the txs in the confirmation target block as the approximate fee rate that a tx will
 * to be included in that block.
 */
class MemPoolPolicyEstimator
{
public:

    MemPoolPolicyEstimator();

    ~MemPoolPolicyEstimator();

    /**
     * Estimate the fee rate from the mempool given a confirmation target.
     *
     * @param chainstate The reference to the active chainstate.
     * @param mempool The reference to the mempool from which we will estimate the fee rate.
     * @param confTarget The next block we wish to a tx to be included in.
     * @return The estimated fee rate.
     */
    CFeeRate EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget) const;

    void BuildExpectedBlockTemplate(Chainstate& chainstate, const CTxMemPool* mempool);

    // Update our sanity check when ever we receive a new block, so that we wont make inaccurate estimate.
    void processBlock(unsigned int nBlockHeight, bool block_roughly_synced);

    bool RoughlySynced() const; // Tells us whether our mempool is rougly in sync with miners mempool.

private:
    /**
     * Calculate the fee rate estimate for a block of txs.
     *
     * @param start The reverse iterator pointing to the beginning of the block range.
     * @param it The reverse iterator pointing to the end of the block range.
     * @param mempool_fee_stats The mempool fee statistics (fee rate and size).
     * @return The optional fee rate estimate in satoshis per kilobyte.
     */
    std::optional<CFeeRate> EstimateBlockFee(std::map<CFeeRate, uint64_t>::reverse_iterator& start, std::map<CFeeRate, uint64_t>::reverse_iterator& it, std::map<CFeeRate, uint64_t>& mempool_fee_stats) const;

    /**
     * Calculate the median fee rate for a range of txs in the mempool.
     *
     * @param start_it The reverse iterator pointing to the beginning of the range.
     * @param end_it The reverse iterator pointing to the end of the range.
     * @return The median fee rate.
     */
    CFeeRate CalculateMedianFeeRate(std::map<CFeeRate, uint64_t>::reverse_iterator& start_it, std::map<CFeeRate, uint64_t>::reverse_iterator& end_it) const;

    // Determine whether the last that we tracked are sequencial.
    bool top_block_in_order() const;

private:
    std::optional<CBlock> block_template{std::nullopt};
    struct block_info {
        unsigned int height;
        bool roughly_synced;
    };
    //  Tracks the last three blocks that was mined whether they are roughly in sync with the mempool or not.
    std::vector<block_info> top_blocks;

    // Whenever we receive a new block we record it's status if its in sync or not.
    void recordBlockStatus(block_info& new_blk_info);
};
#endif // BITCOIN_POLICY_MEMPOOL_FEES_H
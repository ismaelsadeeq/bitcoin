// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_MEMPOOL_FEES_H
#define BITCOIN_POLICY_MEMPOOL_FEES_H

#include <map>
#include <optional>

#include <policy/feerate.h>

class Chainstate;
class CTxMemPool;


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

};
#endif // BITCOIN_POLICY_MEMPOOL_FEES_H
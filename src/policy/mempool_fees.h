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
 * CMemPoolPolicyEstimator estimates the fee rate that a tx should pay
 * to be included in a confirmation target based on the mempool
 * txs and their fee rates.
 *
 * The estimator works by generating template blocks and then calculates the median
 * fee rate for the txs in those blocks.
 */
class CMemPoolPolicyEstimator
{
public:
    /**
     * Construct a CMemPoolPolicyEstimator object.
     *
     * @param chainstate A reference to the Chainstate object used for estimation.
     */
    CMemPoolPolicyEstimator(Chainstate& chainstate);

    /**
     * Destructor for CMemPoolPolicyEstimator.
     */
    ~CMemPoolPolicyEstimator();

    /**
     * Estimate the fee rate from the mempool given a confirmation target.
     *
     * @param mempool The reference to the mempool from which to estimate the fee rate.
     * @param confTarget The desired number of blocks for the tx to be confirmed.
     * @return The estimated fee rate.
     */
    CFeeRate EstimateFeeWithMemPool(CTxMemPool& mempool, unsigned int confTarget) const;

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

private:
    Chainstate* m_chainstate;
};
#endif // BITCOIN_POLICY_MEMPOOL_FEES_H
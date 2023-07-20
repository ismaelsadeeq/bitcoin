// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_MEMPOOL_FEES_H
#define BITCOIN_POLICY_MEMPOOL_FEES_H

#include <map>
#include <optional>

#include <policy/feerate.h>


class CTxMemPool;
class Chainstate;

class CMemPoolPolicyEstimator
{
public:
    explicit CMemPoolPolicyEstimator(CTxMemPool& mempool, Chainstate& chainstate);
    ~CMemPoolPolicyEstimator();

    // Estimate the fee rate from the mempool given confirmation target
    CFeeRate EstimateMempoolFeeEstimate(int confTarget) const;

private:
    // Calculate the fee rate estimate for a range of blocks in the mempool
    std::optional<CFeeRate> GetBlocksFeeEstimate(std::map<CFeeRate, uint64_t>::reverse_iterator& start, std::map<CFeeRate, uint64_t>::reverse_iterator& it, std::map<CFeeRate, uint64_t>& mempool_fee_stats, std::size_t index) const;

    // Calculate the median fee rate for a range of blocks in the mempool
    CFeeRate CalculateMedianFeeRate(std::map<CFeeRate, uint64_t>::reverse_iterator& start_it, std::map<CFeeRate, uint64_t>::reverse_iterator& end_it, std::size_t start, std::size_t end) const;

private:
    CTxMemPool* m_mempool{nullptr};
    Chainstate* m_chainstate{nullptr};
};
#endif // BITCOIN_POLICY_MEMPOOL_FEES_H
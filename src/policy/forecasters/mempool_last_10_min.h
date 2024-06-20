// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTERS_MEMPOOL_LAST_10_MIN_H
#define BITCOIN_POLICY_FORECASTERS_MEMPOOL_LAST_10_MIN_H

#include <policy/fee_estimator.h>
#include <policy/feerate.h>

#include <string>

class Chainstate;
class CTxMemPool;

// Fee rate estimates above this confirmation target are not reliable,
// mempool condition might likely change.
const unsigned int MEMPOOL_LAST_10_MIN_FORECAST_MAX_TARGET{2};
const std::string MEMPOOL_LAST_10_MIN_FORECAST_NAME_STR{"Mempool Last 10 min Forecast"};


/** \class MemPoolLast10MinForecaster
 * This fee estimate forecaster estimates the fee rate that a transaction will pay
 * to be included in a block as soon as possible.
 * It uses the unconfirmed transactions in the mempool seen within the last 10 minutes
 * to generate the next block template that will likely be mined.
 * The percentile fee rate's are computed, and the bottom 25th percentile and 50th percentile fee rate's are returned.
 */
class MemPoolLast10MinForecaster : public Forecaster
{
private:
    CTxMemPool* m_mempool;
    Chainstate* m_chainstate;

public:
    MemPoolLast10MinForecaster(CTxMemPool* mempool, Chainstate* chainstate) : m_mempool(mempool), m_chainstate(chainstate){};
    ~MemPoolLast10MinForecaster() = default;

    /**
     * Estimate the fee rate from mempool transactions given a confirmation target.
     * @param[in] targetBlocks The confirmation target to provide estimate for.
     * @return The forecasted fee rates.
     */
    ForecastResult EstimateFee(unsigned int targetBlocks) override;

    /* Return the maximum confirmation target this forecaster can forecast */
    unsigned int MaxTarget() override
    {
        return MEMPOOL_LAST_10_MIN_FORECAST_MAX_TARGET;
    }

private:
    /**
     * @param[in] targetBlocks The confirmation target to provide estimate for.
     * @return The forecasted fee rates.
     */
    ForecastResult EstimateFeeWithMemPool(unsigned int targetBlocks);
};
#endif // BITCOIN_POLICY_FORECASTERS_MEMPOOL_LAST_10_MIN_H

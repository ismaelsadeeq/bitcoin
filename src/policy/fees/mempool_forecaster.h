// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FEES_MEMPOOL_FORECASTER_H
#define BITCOIN_POLICY_FEES_MEMPOOL_FORECASTER_H

#include <logging.h>
#include <policy/fees/forecaster.h>
#include <policy/fees/forecaster_util.h>
#include <sync.h>
#include <uint256.h>
#include <util/time.h>

#include <chrono>

class BlockTemplateManager;

// Fee rate forecasts above this confirmation target are not reliable,
// as mempool conditions are likely to change.
constexpr int MEMPOOL_FORECAST_MAX_TARGET{2};
constexpr std::chrono::seconds CACHE_LIFE{7};


/** \class MemPoolForecaster
 * @brief Forecasts the fee rate required for a transaction to be included in the next block.
 *
 * This forecaster uses Bitcoin Core's block-building algorithm to generate the block
 * template that will likely be mined from unconfirmed transactions in the mempool. It calculates percentile
 * fee rates from the selected packages of the template: the 75th percentile fee rate is used as the economical
 * forecast, and the 50th fee rate percentile as the conservative forecast.
 */
class MemPoolForecaster : public Forecaster
{
public:
    MemPoolForecaster(BlockTemplateManager* blocktemplateman)
        : Forecaster(ForecastType::MEMPOOL_FORECAST), m_blocktemplateman(blocktemplateman) {};
    ~MemPoolForecaster() = default;

    /** Overridden from Forecaster. */
    ForecastResult ForecastFeeRate(int target, bool conservative) const override;
    unsigned int MaximumTarget() const override
    {
        return MEMPOOL_FORECAST_MAX_TARGET;
    };

private:
    BlockTemplateManager* m_blocktemplateman;
};
#endif // BITCOIN_POLICY_FEES_MEMPOOL_FORECASTER_H

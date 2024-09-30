// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTER_H
#define BITCOIN_POLICY_FORECASTER_H

#include <policy/forecaster_util.h>

/** \class Forecaster
 * Abstract base class for fee rate forecasters.
 *
 * Derived classes must provide concrete implementations for all virtual methods.
 */
class Forecaster
{
protected:
    const ForecastType m_forecastType;

public:
    Forecaster(ForecastType forecastType) : m_forecastType(forecastType) {}

    ForecastType GetForecastType() const { return m_forecastType; }

    /**
     * Estimate the fee rate required for transaction confirmation.
     *
     * This pure virtual function must be overridden by derived classes to
     * provide a ForecastResult for the specified number of target hours.
     *
     * @param[in] targetHours The number of hours within which the transaction should be confirmed.
     * @return ForecastResult containing the estimated fee rate.
     */
    virtual ForecastResult EstimateFee(int targetHours) = 0;

    /**
     * Retrieve the maximum target hours this forecaster can handle for fee estimation.
     *
     * This pure virtual function must be overridden by derived classes to
     * provide the maximum number of hours for which a fee rate estimate may
     * be returned.
     *
     * @return int representing the maximum target hours range.
     */
    virtual int MaxTarget() = 0;

    virtual ~Forecaster() = default;
};

#endif // BITCOIN_POLICY_FORECASTER_H

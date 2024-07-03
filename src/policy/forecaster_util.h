// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTER_UTIL_H
#define BITCOIN_POLICY_FORECASTER_UTIL_H

#include <policy/feerate.h>

#include <optional>
#include <string>

enum class ForecastType {
    BLOCK_POLICY_ESTIMATOR,
};

struct ForecastResult {
    struct ForecastOptions {
        CFeeRate low_priority;
        CFeeRate high_priority;
        unsigned int block_height{0};
        ForecastType forecaster;
    };

    ForecastOptions m_opt;
    std::optional<std::string> m_error_ptr;

    ForecastResult(ForecastResult::ForecastOptions& options, const std::optional<std::string> error_ptr)
        : m_opt(options), m_error_ptr(error_ptr) {}

    bool empty() const
    {
        return m_opt.low_priority == CFeeRate() && m_opt.high_priority == CFeeRate();
    }

    bool operator<(const ForecastResult& forecast) const
    {
        return m_opt.high_priority < forecast.m_opt.high_priority;
    }

    ~ForecastResult() = default;
};

#endif // BITCOIN_POLICY_FORECASTER_UTIL_H

// Copyright (c) 2024 Bitcoin Core Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTER_NTIME_H
#define BITCOIN_POLICY_FORECASTER_NTIME_H
#include <policy/forecaster.h>
#include <validationinterface.h>

#include <vector>
#include <tuple>
#include <cstdint>
#include <chrono>
#include <map>

static constexpr int MAX_HOURS = 504;

static constexpr int SECONDS_IN_HOUR = 1 * 60 * 60;
// How often to flush fee estimates to fee_estimates.dat.
static constexpr std::chrono::hours STATS_UPDATE_INTERVAL{1};

struct RemovedMempoolTransactionInfo;
class CFeeRate;
struct BlockPercentiles;

// Class for tracking and forecasting transaction confirmation times.
// In other to provide fee estimates for transaction to confirm
// within a particular time interval.
class NTime : public Forecaster, public CValidationInterface {
public:
    NTime();
    virtual ~NTime() = default;

    void UpdateTrackingStats();

    ForecastResult EstimateFee(int targetHours) override;

    int MaxTarget() override { return MAX_HOURS; }

protected:
    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block,
                                            unsigned int nBlockHeight) override;

private:
    struct ConfirmedTx {
        int64_t m_receivedTime; // In seconds since epoch
        int64_t m_confirmedTime; // In seconds since epoch
        CFeeRate fee_rate; // Tx mining score
        uint32_t vsize;
    };

    using TrackingVector = std::vector<std::vector<std::vector<ConfirmedTx>>>;
    using PackagesAndWeight = std::pair<std::vector<std::tuple<CFeeRate, uint32_t>>, unsigned int>;
    TrackingVector tracking_stats;

    TrackingVector initStats();

    void addTxToStats(const ConfirmedTx& tx);

    PackagesAndWeight GetTxsWithinTime(int start_hr, int end_hr) const;

    BlockPercentiles GetWindowEstimate(int hours) const;
    BlockPercentiles GetHistoricalEstimate(int hours) const;
};

#endif // BITCOIN_POLICY_FORECASTER_NTIME_H

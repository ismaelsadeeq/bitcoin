// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_POLICY_FORECASTERS_LAST_BLOCK_H
#define BITCOIN_POLICY_FORECASTERS_LAST_BLOCK_H

#include <policy/fees_util.h>
#include <policy/forecaster.h>
#include <validationinterface.h>

#include <queue>


struct RemovedMempoolTransactionInfo;
class Forecaster;
class CValidationInterface;
struct ForecastResult;

const std::string LAST_BLOCK_FORECAST_NAME_STR{"Last Block Forecast"};
const unsigned int LAST_BLOCK_FORECAST_MAX_TARGET{2};

/** \class LastBlockForecaster
 * LastBlockForecaster fee rate forecaster estimates the fee rate that a transaction will pay
 * to be included in a block as soon as possible.
 * LastBlockForecaster uses the mining score of the transactions that were confirmed in
 * the last block that the node mempool sees.
 * LastBlockForecaster calculates the percentiles mining score
 * It returns the 25th and 50th percentiles as the fee rate estimate.
 */
class LastBlockForecaster : public CValidationInterface, public Forecaster
{
private:
    BlockPercentiles blocks_percentile;
    unsigned int chain_tip_height;

protected:
    void MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, unsigned int nBlockHeight) override;

public:
    LastBlockForecaster(){};

    ForecastResult EstimateFee(unsigned int targetBlocks) override;
    unsigned int MaxTarget() override
    {
        return LAST_BLOCK_FORECAST_MAX_TARGET;
    }
};
#endif // BITCOIN_POLICY_FORECASTERS_LAST_BLOCK_H

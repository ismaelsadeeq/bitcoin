// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license. See the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#include <policy/fees_util.h>
#include <primitives/transaction.h>

TxAncestorsAndDescendants GetTxAncestorsAndDescendants(const std::vector<RemovedMempoolTransactionInfo>& transactions)
{
    TxAncestorsAndDescendants visited_txs;
    for (auto& tx_info : transactions) {
        const auto& tx_ref = tx_info.info.m_tx;
        const auto txid = tx_ref->GetHash();

        std::set<Txid> tx_vec{tx_info.info.m_tx->GetHash()};
        visited_txs.emplace(txid, std::make_tuple(tx_vec, tx_vec));

        for (const auto& input : tx_ref->vin) {
            // If a parent has been visited add all the parent ancestors to the set of transaction ancestor
            // Also add the transaction into each ancestor descendant set
            if (visited_txs.find(input.prevout.hash) != visited_txs.end()) {
                auto& parent_ancestors = std::get<0>(visited_txs[input.prevout.hash]);
                auto& tx_ancestors = std::get<0>(visited_txs[txid]);
                for (auto& ancestor : parent_ancestors) {
                    auto& ancestor_descendants = std::get<1>(visited_txs[ancestor]);
                    ancestor_descendants.insert(txid);
                    tx_ancestors.insert(ancestor);
                }
            }
        }
    }
    return visited_txs;
}

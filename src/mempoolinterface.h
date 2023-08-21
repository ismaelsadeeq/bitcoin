// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MEMPOOLINTERFACE_H
#define BITCOIN_MEMPOOLINTERFACE_H

#include <kernel/cs_main.h>
#include <primitives/transaction.h> // CTransaction(Ref)


class MempoolInterface;
class CMainSignals;
enum class MemPoolRemovalReason;

/** Register subscriber to receive updates from mempool */
void RegisterMempoolInterface(MempoolInterface* callbacks);
/** Unregister subscriber from mempool updates*/
void UnregisterMempoolInterface(MempoolInterface* callbacks);

// Alternate registration functions that release a shared_ptr after the last
// notification is sent. These are useful for race-free cleanup, since
// unregistration is nonblocking and can return before the last notification is
// processed.
/** Register subscriber to receive updates from mempool */
void RegisterSharedMempoolInterface(std::shared_ptr<MempoolInterface> callbacks);
/** Unregister subscriber from mempool updates */
void UnregisterSharedMempoolInterface(std::shared_ptr<MempoolInterface> callbacks);

/** Unregister all mempool subscribers */
void UnregisterAllMempoolInterfaces();

/**
 * Implement this to subscribe to events generated in mempool.
 * An interface to get callbacks about transactions entering and leaving
 * mempool.
 */
class MempoolInterface
{
protected:
    /**
     * Protected destructor so that instances can only be deleted by derived classes.
     * If that restriction is no longer desired, this should be made public and virtual.
     */
    ~MempoolInterface() = default;
    /**
     * Notifies listeners of a transaction having been added to mempool.
     *
     * Called on a background thread.
     */
    virtual void TransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence) {}
    /**
     * Notifies listeners of a transaction leaving mempool.
     *
     * This notification fires for transactions that are removed from the
     * mempool for the following reasons:
     *
     * - EXPIRY (expired from mempool after -mempoolexpiry hours)
     * - SIZELIMIT (removed in size limiting if the mempool exceeds -maxmempool megabytes)
     * - REORG (removed during a reorg)
     * - CONFLICT (removed because it conflicts with in-block transaction)
     * - REPLACED (removed due to RBF replacement)
     *
     * This does not fire for transactions that are removed from the mempool
     * because they have been included in a block. Any client that is interested
     * in transactions removed from the mempool for inclusion in a block can learn
     * about those transactions from the BlockConnected notification.
     *
     * Transactions that are removed from the mempool because they conflict
     * with a transaction in the new block will have
     * TransactionRemovedFromMempool events fired *before* the BlockConnected
     * event is fired. If multiple blocks are connected in one step, then the
     * ordering could be:
     *
     * - TransactionRemovedFromMempool(tx1 from block A)
     * - TransactionRemovedFromMempool(tx2 from block A)
     * - TransactionRemovedFromMempool(tx1 from block B)
     * - TransactionRemovedFromMempool(tx2 from block B)
     * - BlockConnected(A)
     * - BlockConnected(B)
     *
     * Called on a background thread.
     */
    virtual void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) {}
    friend class CMainSignals;
};

#endif // BITCOIN_MEMPOOLINTERFACE_H

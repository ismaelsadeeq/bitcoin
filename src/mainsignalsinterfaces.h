// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MAINSIGNAL_H
#define BITCOIN_MAINSIGNAL_H

#include <mempoolinterface.h>
#include <validationinterface.h>

#include <kernel/cs_main.h>
#include <primitives/transaction.h> // CTransaction(Ref)
#include <sync.h>

#include <functional>
#include <memory>

class BlockValidationState;
class CBlock;
class CBlockIndex;
struct CBlockLocator;
class CValidationInterface;
class CScheduler;
class MempoolInterface;
enum class MemPoolRemovalReason;

/**
 * Pushes a function to callback onto the notification queue, guaranteeing any
 * callbacks generated prior to now are finished when the function is called.
 *
 * Be very careful blocking on func to be called if any locks are held -
 * in validation or mempool interface clients may not be able to make
 * progress as they often wait for things like cs_main, so blocking
 * until func is called with cs_main will result in a deadlock
 * (that DEBUG_LOCKORDER will miss).
 */
void CallFunctionInInterfaceQueue(std::function<void()> func);
/**
 * This is a synonym for the following, which asserts certain locks are not
 * held:
 *     std::promise<void> promise;
 *     CallFunctionInInterfaceQueue([&promise] {
 *         promise.set_value();
 *     });
 *     promise.get_future().wait();
 */
void SyncWithInterfaceQueue() LOCKS_EXCLUDED(cs_main);

/**
 * Any class which extends both MempoolInterface and CValidationInterface will
 * see all callbacks across both well-ordered (see individual callback text for
 * details on the order guarantees).
 *
 * Callbacks called on a background thread have a separate order from those
 * called on the thread generating the callbacks.
 */

class MainSignalsImpl;
class CMainSignals
{
private:
    std::unique_ptr<MainSignalsImpl> m_internals;

    friend void ::RegisterSharedValidationInterface(std::shared_ptr<CValidationInterface>);
    friend void ::RegisterSharedMempoolInterface(std::shared_ptr<MempoolInterface>);
    friend void ::UnregisterValidationInterface(CValidationInterface*);
    friend void ::UnregisterMempoolInterface(MempoolInterface*);
    friend void ::UnregisterAllValidationInterfaces();
    friend void ::UnregisterAllMempoolInterfaces();
    friend void ::CallFunctionInInterfaceQueue(std::function<void()> func);

public:
    /** Register a CScheduler to give callbacks which should run in the background (may only be called once) */
    void RegisterBackgroundSignalScheduler(CScheduler& scheduler);
    /** Unregister a CScheduler to give callbacks which should run in the background - these callbacks will now be dropped! */
    void UnregisterBackgroundSignalScheduler();
    /** Call any remaining callbacks on the calling thread */
    void FlushBackgroundCallbacks();

    size_t CallbacksPending();


    void UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool fInitialDownload);
    void TransactionAddedToMempool(const CTransactionRef&, uint64_t mempool_sequence);
    void TransactionRemovedFromMempool(const CTransactionRef&, MemPoolRemovalReason, uint64_t mempool_sequence);
    void BlockConnected(const std::shared_ptr<const CBlock>&, const CBlockIndex* pindex);
    void BlockDisconnected(const std::shared_ptr<const CBlock>&, const CBlockIndex* pindex);
    void ChainStateFlushed(const CBlockLocator&);
    void BlockChecked(const CBlock&, const BlockValidationState&);
    void NewPoWValidBlock(const CBlockIndex*, const std::shared_ptr<const CBlock>&);
};

CMainSignals& GetMainSignals();

#endif // BITCOIN_MAINSIGNAL_H

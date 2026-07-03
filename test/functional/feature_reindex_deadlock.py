#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a shutdown requested during -reindex terminates the node.

While the wiped chainstate is being rebuilt, the main thread waits on
KernelNotifications::m_tip_block_cv until the import thread activates the
genesis block. A shutdown request does not notify m_tip_block_cv, and a
reindex interrupted in ImportBlocks returns without activating genesis,
so nothing wakes the wait: the node previously hung forever and could
only be terminated with SIGKILL.

To interrupt the import before genesis activation deterministically,
replace blk00000.dat with a file that contains no genesis block, only
repeated header-only entries with an unknown parent. Every entry is
treated as out-of-order during reindex, so the whole file scan runs
before genesis can be activated, and a SIGINT sent mid-scan interrupts
the import inside that window.
"""

import signal
import time

from test_framework.messages import (
    CBlockHeader,
    MAGIC_BYTES,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    util_xor,
)

# The reindex scan of these entries is the window during which the
# shutdown request must arrive, so it must comfortably outlast the sleep
# below.
UNKNOWN_ENTRIES = 2_000_000


class ReindexDeadlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_platform_not_posix()

    def run_test(self):
        node = self.nodes[0]
        self.stop_node(0)

        self.log.info(f"Replace blk00000.dat with {UNKNOWN_ENTRIES} genesis-free out-of-order entries")
        # A block file storage record (see STORAGE_HEADER_BYTES): magic,
        # 4-byte length, then a null block header whose all-zero parent is
        # unknown, so the entry is treated as out-of-order during reindex.
        header = CBlockHeader().serialize()
        entry = MAGIC_BYTES["regtest"] + len(header).to_bytes(4, "little") + header
        # The entry size is a multiple of the xor key size, so the xor'd
        # entry can be repeated at any entry boundary.
        assert_equal(len(entry) % len(node.read_xor_key()), 0)
        entry = util_xor(entry, node.read_xor_key(), offset=0)
        (node.blocks_path / "blk00000.dat").write_bytes(entry * UNKNOWN_ENTRIES)

        self.log.info("Request shutdown while the reindex scan is before genesis activation")
        node.start(extra_args=["-reindex"])
        # "UpdateTip" would mean genesis was activated before the shutdown
        # request, making this test prove nothing. If that happens,
        # increase UNKNOWN_ENTRIES.
        with node.assert_debug_log(expected_msgs=["Interrupt requested. Exit reindexing."],
                                   unexpected_msgs=["UpdateTip"], timeout=60):
            node.busy_wait_for_debug_log([b"Reindexing block file blk00000.dat"])
            # Give the main thread time to park in init's genesis wait.
            time.sleep(0.5)
            node.process.send_signal(signal.SIGINT)

        self.log.info("Check that the node terminates")
        try:
            node.wait_until_stopped()
        except AssertionError:
            node.kill_process()
            raise AssertionError("node hung in init's genesis wait after shutdown was requested")

        self.log.info("Check that a restart resumes the reindex and recovers genesis")
        self.start_node(0)
        assert_equal(node.getblockcount(), 0)


if __name__ == '__main__':
    ReindexDeadlockTest(__file__).main()

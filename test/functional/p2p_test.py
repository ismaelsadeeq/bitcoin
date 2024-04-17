from test_framework.test_framework import BitcoinTestFramework
from test_framework.p2p import P2PInterface
from test_framework.wallet import MiniWallet

class ExerciseTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False, descriptors=True)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3

    def run_test(self):
        self.log.info("Starting test")
        self.nodes[0].createwallet(wallet_name="w1")
        self.nodes[0].cli('-generate', 110).send_cli()
        peer = self.nodes[0].add_outbound_p2p_connection(P2PInterface(), p2p_idx=0)
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1, fee_rate=10)

        peer.wait_for_tx(txid, timeout=60)

if __name__ == '__main__':
    ExerciseTest().main()
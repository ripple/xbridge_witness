#!/usr/bin/env python3
"""
Script to test and debug sidechains.

The mainchain exe location can be set through the command line or
the environment variable RIPPLED_MAINCHAIN_EXE

The sidechain exe location can be set through the command line or
the environment variable RIPPLED_SIDECHAIN_EXE

The configs_dir (generated with create_config_files.py) can be set through the command line
or the environment variable RIPPLED_SIDECHAIN_CFG_DIR
"""

import argparse
import json
from multiprocessing import Process, Value
import os
import pathlib
import sys
import time
from typing import Callable, Dict, List, Optional

from app import App, single_client_app, testnet_app, configs_for_testnet
from command import (
    AccountInfo,
    AccountTx,
    LedgerAccept,
    LogLevel,
    Subscribe,
    WalletPropose,
)
from common import Account, Asset, eprint, disable_eprint, XRP, Bridge
from config_file import ConfigFile
import interactive
from log_analyzer import convert_log
from test_utils import (
    mc_wait_for_payment_detect,
    sc_wait_for_payment_detect,
    mc_connect_subscription,
    sc_connect_subscription,
)
from transaction import (
    AccountSet,
    Payment,
    SignerListSet,
    SetRegularKey,
    XChainCreateBridge,
    Trust,
)
from witness_app import witness_servers


def parse_args_helper(parser: argparse.ArgumentParser):

    parser.add_argument(
        "--debug_sidechain",
        "-ds",
        action="store_true",
        help=("Mode to debug sidechain (prompt to run sidechain in gdb)"),
    )

    parser.add_argument(
        "--debug_mainchain",
        "-dm",
        action="store_true",
        help=("Mode to debug mainchain (prompt to run sidechain in gdb)"),
    )

    parser.add_argument(
        "--debug_witness",
        "-dw",
        action="store_true",
        help=("Mode to debug mainchain (prompt to run sidechain in gdb)"),
    )

    parser.add_argument(
        "--exe_mainchain",
        "-em",
        help=("path to mainchain rippled executable"),
    )

    parser.add_argument(
        "--exe_sidechain",
        "-es",
        help=("path to mainchain rippled executable"),
    )

    parser.add_argument(
        "--exe_witness",
        "-ew",
        help=("path to witness executable"),
    )

    parser.add_argument(
        "--cfgs_dir",
        "-c",
        help=("path to configuration file dir (generated with create_config_files.py)"),
    )

    parser.add_argument(
        "--standalone",
        "-a",
        action="store_true",
        help=("run standalone tests"),
    )

    parser.add_argument(
        "--interactive",
        "-i",
        action="store_true",
        help=("run interactive repl"),
    )

    parser.add_argument(
        "--quiet",
        "-q",
        action="store_true",
        help=("Disable printing errors (eprint disabled)"),
    )

    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help=("Enable printing errors (eprint enabled)"),
    )

    # Pauses are use for attaching debuggers and looking at logs are know checkpoints
    parser.add_argument(
        "--with_pauses",
        "-p",
        action="store_true",
        help=('Add pauses at certain checkpoints in tests until "enter" key is hit'),
    )

    parser.add_argument(
        "--hooks_dir",
        help=("path to hooks dir"),
    )


def parse_args():
    parser = argparse.ArgumentParser(description=("Test and debug sidechains"))
    parse_args_helper(parser)
    return parser.parse_known_args()[0]


def witness_config_files(
    parent_dir: str, sub_dir_prefix: str, file_name: str
) -> List[str]:
    """
    Return a list of witness config files.
    These are files of the form "parent_dir/sub_dir_prefix<anything>/file_name".
    For example, if th parent_dir was "data", "sub_dir_prefix" was "witness" and filename
    was "witness.json", then a file with a path "data/witness0/witness.json" would be loaded
    and added to the returned list
    """
    result = []
    subdirs = [d for d in pathlib.Path(parent_dir).iterdir() if d.is_dir()]
    for d in subdirs:
        if not d.name.startswith(sub_dir_prefix):
            continue
        for f in d.iterdir():
            if f.name == file_name:
                result.append(str(f))
    return result


def account_id_from_secret(app: App, key: str, key_type="ed25519") -> str:
    # TODO: keytype should be a parameter from the config file
    r = app(WalletPropose(seed=key, key_type=key_type))
    return r["account_id"]


class Params:
    def __init__(self, *, configs_dir: Optional[str] = None):
        args = parse_args()

        self.debug_sidechain = False
        if args.debug_sidechain:
            self.debug_sidechain = args.debug_sidechain
        self.debug_mainchain = False
        if args.debug_mainchain:
            self.debug_mainchain = args.debug_mainchain
        self.debug_witness = False
        if args.debug_witness:
            self.debug_witness = args.debug_witness

        # Undocumented feature: if the environment variable RIPPLED_SIDECHAIN_RR is set, it is
        # assumed to point to the rr executable. Sidechain server 0 will then be run under rr.
        self.sidechain_rr = None
        if "RIPPLED_SIDECHAIN_RR" in os.environ:
            self.sidechain_rr = os.environ["RIPPLED_SIDECHAIN_RR"]

        self.standalone = args.standalone
        self.with_pauses = args.with_pauses
        self.interactive = args.interactive
        self.quiet = args.quiet
        self.verbose = args.verbose

        self.mainchain_exe = None
        if "RIPPLED_MAINCHAIN_EXE" in os.environ:
            self.mainchain_exe = os.environ["RIPPLED_MAINCHAIN_EXE"]
        if args.exe_mainchain:
            self.mainchain_exe = args.exe_mainchain

        self.sidechain_exe = None
        if "RIPPLED_SIDECHAIN_EXE" in os.environ:
            self.sidechain_exe = os.environ["RIPPLED_SIDECHAIN_EXE"]
        if args.exe_sidechain:
            self.sidechain_exe = args.exe_sidechain

        self.witness_exe = None
        if "RIPPLED_WITNESS_EXE" in os.environ:
            self.witness_exe = os.environ["RIPPLED_WITNESS_EXE"]
        if args.exe_witness:
            self.witness_exe = args.exe_witness

        self.configs_dir = None
        if "RIPPLED_SIDECHAIN_CFG_DIR" in os.environ:
            self.configs_dir = os.environ["RIPPLED_SIDECHAIN_CFG_DIR"]
        if args.cfgs_dir:
            self.configs_dir = args.cfgs_dir
        if configs_dir is not None:
            self.configs_dir = configs_dir

        self.hooks_dir = None
        if "RIPPLED_SIDECHAIN_HOOKS_DIR" in os.environ:
            self.hooks_dir = os.environ["RIPPLED_SIDECHAIN_HOOKS_DIR"]
        if args.hooks_dir:
            self.hooks_dir = args.hooks_dir

        if not self.configs_dir:
            self.witness_configs = None
            self.witness_config_files = None
            self.mainchain_config = None
            self.sidechain_config = None
            self.genesis_account = None
            self.mc_door_account = None
            self.user_account = None
            self.sc_door_account = None
            return

        self.witness_config_filenames = witness_config_files(
            self.configs_dir, "witness", "witness.json"
        )
        if not self.witness_config_filenames:
            raise "Must have at least one witness config"
        self.witness_configs = []
        for fn in self.witness_config_filenames:
            with open(fn, "r") as f:
                self.witness_configs.append(json.load(f))
        self.bootstrap_config = None
        with open(f"{self.configs_dir}/sidechain_bootstrap.json", "r") as f:
            self.bootstrap_config = json.load(f)
        if self.standalone:
            self.mainchain_config = ConfigFile(
                file_name=f"{self.configs_dir}/mainchain/rippled.cfg"
            )
            self.sidechain_config = ConfigFile(
                file_name=f"{self.configs_dir}/sidechain/rippled.cfg"
            )
        else:
            self.mainchain_config = ConfigFile(
                file_name=f"{self.configs_dir}/mainchain_testnet/mainchain0/rippled.cfg"
            )
            self.sidechain_config = ConfigFile(
                file_name=f"{self.configs_dir}/sidechain_testnet/sidechain0/rippled.cfg"
            )

        self.genesis_account = Account(
            account_id="rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            secret_key="masterpassphrase",
            nickname="genesis",
        )
        self.mc_door_account = Account(
            account_id=self.bootstrap_config["mainchain_door"]["id"],
            secret_key=self.bootstrap_config["mainchain_door"]["secret_key"],
            nickname="door",
        )
        self.user_account = Account(
            account_id="rJynXY96Vuq6B58pST9K5Ak5KgJ2JcRsQy",
            secret_key="snVsJfrr2MbVpniNiUU6EDMGBbtzN",
            nickname="alice",
        )

        self.sc_door_account = Account(
            account_id=self.bootstrap_config["sidechain_door"]["id"],
            secret_key=self.bootstrap_config["sidechain_door"]["secret_key"],
            nickname="door",
        )

    def check_error(self) -> str:
        """
        Check for errors. Return `None` if no errors,
        otherwise return a string describing the error
        """
        if not self.mainchain_exe:
            return "Missing mainchain_exe location. Either set the env variable RIPPLED_MAINCHAIN_EXE or use the --exe_mainchain command line switch"
        if not self.sidechain_exe:
            return "Missing sidechain_exe location. Either set the env variable RIPPLED_SIDECHAIN_EXE or use the --exe_sidechain command line switch"
        if not self.configs_dir:
            return "Missing configs directory location. Either set the env variable RIPPLED_SIDECHAIN_CFG_DIR or use the --cfgs_dir command line switch"
        if self.verbose and self.quiet:
            return "Cannot specify both verbose and quiet options at the same time"


def setup_mainchain(mc_app: App, params: Params, setup_user_accounts: bool = True):
    mc_app.add_to_keymanager(params.mc_door_account)
    if setup_user_accounts:
        mc_app.add_to_keymanager(params.user_account)

    mc_app(LogLevel("fatal"))

    # Allow rippling through the genesis account
    mc_app(AccountSet(account=params.genesis_account).set_default_ripple(True))
    mc_app.maybe_ledger_accept()

    # Create and fund the mc door account
    mc_app(
        Payment(
            account=params.genesis_account, dst=params.mc_door_account, amt=XRP(10_000)
        )
    )
    mc_app.maybe_ledger_accept()

    # Create a trust line so USD/root account ious can be sent cross chain
    mc_app(
        Trust(
            account=params.mc_door_account,
            limit_amt=Asset(
                value=1_000_000, currency="USD", issuer=params.genesis_account
            ),
        )
    )

    mc_app.maybe_ledger_accept()

    # create the bridge object
    bridge = Bridge(from_rpc_result=params.witness_configs[0]["XChainBridge"])
    divide = 4 * len(params.witness_configs)
    by = 5
    quorum = (divide + by - 1) // by
    account_ids = [
        account_id_from_secret(
            app=mc_app,
            key=wc["SigningKeySeed"],
            key_type=wc["SigningKeyKeyType"],
        )
        for wc in params.witness_configs
    ]
    r = mc_app(
        SignerListSet(
            account=params.mc_door_account,
            quorum=quorum,
            keys=account_ids,
        )
    )
    r = mc_app(
        XChainCreateBridge(
            account=params.mc_door_account,
            bridge=bridge,
            reward=XRP(1),
            min_account_create=XRP(1),
        )
    )
    mc_app.maybe_ledger_accept()
    # mc_app(AccountSet(account=params.mc_door_account).set_disable_master())
    # mc_app.maybe_ledger_accept()

    if setup_user_accounts:
        # Create and fund a regular user account
        mc_app(
            Payment(
                account=params.genesis_account, dst=params.user_account, amt=XRP(2_000)
            )
        )
        mc_app.maybe_ledger_accept()


def setup_sidechain(sc_app: App, params: Params, setup_user_accounts: bool = True):
    # TODO: Merge with function above
    sc_app.add_to_keymanager(params.sc_door_account)
    sc_app(LogLevel("fatal"))

    # create the sidechain object
    bridge = Bridge(from_rpc_result=params.witness_configs[0]["XChainBridge"])
    divide = 4 * len(params.witness_configs)
    by = 5
    quorum = (divide + by - 1) // by
    account_ids = [
        account_id_from_secret(
            app=sc_app,
            key=wc["SigningKeySeed"],
            key_type=wc["SigningKeyKeyType"],
        )
        for wc in params.witness_configs
    ]
    sc_app(
        SignerListSet(
            account=params.sc_door_account,
            quorum=quorum,
            keys=account_ids,
        )
    )
    sc_app(
        XChainCreateBridge(
            account=params.sc_door_account,
            bridge=bridge,
            reward=XRP(1),
            min_account_create=XRP(1),
        )
    )
    sc_app.maybe_ledger_accept()

    if setup_user_accounts:
        sc_app.add_to_keymanager(params.user_account)


def _xchain_transfer(
    from_chain: App,
    to_chain: App,
    src: Account,
    dst: Account,
    amt: Asset,
    from_chain_door: Account,
    to_chain_door: Account,
):
    # TODO
    pass


def main_to_side_transfer(
    mc_app: App, sc_app: App, src: Account, dst: Account, amt: Asset, params: Params
):
    _xchain_transfer(
        mc_app, sc_app, src, dst, amt, params.mc_door_account, params.sc_door_account
    )


def side_to_main_transfer(
    mc_app: App, sc_app: App, src: Account, dst: Account, amt: Asset, params: Params
):
    _xchain_transfer(
        sc_app, mc_app, src, dst, amt, params.sc_door_account, params.mc_door_account
    )


def _rm_debug_log(config: ConfigFile):
    try:
        debug_log = config.debug_logfile.get_line()
        if debug_log:
            print(f"removing debug file: {debug_log}", flush=True)
            os.remove(debug_log)
    except:
        pass


def _standalone_with_callback(
    params: Params,
    callback: Callable[[App, App], None],
    setup_user_accounts: bool = True,
):

    if params.debug_mainchain:
        input("Start mainchain server and press enter to continue: ")
    else:
        _rm_debug_log(params.mainchain_config)
    with single_client_app(
        config=params.mainchain_config,
        exe=params.mainchain_exe,
        standalone=True,
        run_server=not params.debug_mainchain,
    ) as mc_app:

        mc_connect_subscription(mc_app, params.mc_door_account)
        setup_mainchain(mc_app, params, setup_user_accounts)

        if params.debug_sidechain:
            input("Start sidechain server and press enter to continue: ")
        else:
            _rm_debug_log(params.sidechain_config)
        with single_client_app(
            config=params.sidechain_config,
            exe=params.sidechain_exe,
            standalone=True,
            run_server=not params.debug_sidechain,
        ) as sc_app:
            if params.debug_witness:
                input("Start witness server and press enter to continue: ")
            with witness_servers(
                config_filenames=params.witness_config_filenames,
                exe=params.witness_exe,
                server_out="/home/swd/test/xbridge/xbridge_log.txt",
                run_servers=not params.debug_witness,
            ) as witness_app:
                sc_connect_subscription(sc_app, params.sc_door_account)
                setup_sidechain(sc_app, params, setup_user_accounts)
                callback(mc_app, sc_app)


def _convert_log_files_to_json(to_convert: List[ConfigFile], suffix: str):
    """
    Convert the log file to json
    """
    for c in to_convert:
        try:
            debug_log = c.debug_logfile.get_line()
            if not os.path.exists(debug_log):
                continue
            converted_log = f"{debug_log}.{suffix}"
            if os.path.exists(converted_log):
                os.remove(converted_log)
            print(f"Converting log {debug_log} to {converted_log}", flush=True)
            convert_log(debug_log, converted_log, pure_json=True)
        except:
            eprint(f"Exception converting log")


def _multinode_with_callback(
    params: Params,
    callback: Callable[[App, App], None],
    setup_user_accounts: bool = True,
):

    mainchain_cfg = ConfigFile(
        file_name=f"{params.configs_dir}/sidechain_testnet/main.no_shards.mainchain_0/rippled.cfg"
    )
    _rm_debug_log(mainchain_cfg)
    if params.debug_mainchain:
        input("Start mainchain server and press enter to continue: ")
    with single_client_app(
        config=mainchain_cfg,
        exe=params.mainchain_exe,
        standalone=True,
        run_server=not params.debug_mainchain,
    ) as mc_app:

        if params.with_pauses:
            input("Pausing after mainchain start (press enter to continue)")

        mc_connect_subscription(mc_app, params.mc_door_account)
        setup_mainchain(mc_app, params, setup_user_accounts)
        if params.with_pauses:
            input("Pausing after mainchain setup (press enter to continue)")

        testnet_configs = configs_for_testnet(
            f"{params.configs_dir}/sidechain_testnet/sidechain_"
        )
        for c in testnet_configs:
            _rm_debug_log(c)

        run_server_list = [True] * len(testnet_configs)
        if params.debug_sidechain:
            run_server_list[0] = False
            input(
                f"Start testnet server {testnet_configs[0].get_file_name()} and press enter to continue: "
            )

        with testnet_app(
            exe=params.sidechain_exe,
            configs=testnet_configs,
            run_server=run_server_list,
            sidechain_rr=params.sidechain_rr,
        ) as n_app:

            if params.with_pauses:
                input("Pausing after testnet start (press enter to continue)")

            sc_connect_subscription(n_app, params.sc_door_account)
            setup_sidechain(n_app, params, setup_user_accounts)
            if params.with_pauses:
                input("Pausing after sidechain setup (press enter to continue)")
            callback(mc_app, n_app)


# The mainchain runs in standalone mode. Most operations - like cross chain
# paymens - will automatically close ledgers. However, some operations, like
# refunds need an extra close. This loop automatically closes ledgers.
def close_mainchain_ledgers(stop_token: Value, params: Params, sleep_time=4):
    with single_client_app(
        config=params.mainchain_config,
        exe=params.mainchain_exe,
        standalone=True,
        run_server=False,
    ) as mc_app:
        while stop_token.value != 0:
            mc_app.maybe_ledger_accept()
            time.sleep(sleep_time)


def close_sidechain_ledgers(stop_token: Value, params: Params, sleep_time=4):
    # TODO: Merge with function above
    with single_client_app(
        config=params.sidechain_config,
        exe=params.sidechain_exe,
        standalone=True,
        run_server=False,
    ) as sc_app:
        while stop_token.value != 0:
            sc_app.maybe_ledger_accept()
            time.sleep(sleep_time)


def standalone_interactive_repl(params: Params):
    def callback(mc_app: App, sc_app: App):
        # process will run while stop token is non-zero
        mc_stop_token = Value("i", 1)
        sc_stop_token = Value("i", 1)
        p = None
        if mc_app.standalone:
            mc_p = Process(target=close_mainchain_ledgers, args=(mc_stop_token, params))
            mc_p.start()
        if sc_app.standalone:
            sc_p = Process(target=close_sidechain_ledgers, args=(sc_stop_token, params))
            sc_p.start()
        try:
            interactive.repl(mc_app, sc_app)
        finally:
            if mc_p:
                mc_stop_token.value = 0
                mc_p.join()
            if sc_p:
                sc_stop_token.value = 0
                sc_p.join()

    _standalone_with_callback(params, callback, setup_user_accounts=False)


def multinode_interactive_repl(params: Params):
    def callback(mc_app: App, sc_app: App):
        # process will run while stop token is non-zero
        stop_token = Value("i", 1)
        p = None
        if mc_app.standalone:
            p = Process(target=close_mainchain_ledgers, args=(stop_token, params))
            p.start()
        try:
            interactive.repl(mc_app, sc_app)
        finally:
            if p:
                stop_token.value = 0
                p.join()

    _multinode_with_callback(params, callback, setup_user_accounts=False)


def main():
    params = Params()
    interactive.set_hooks_dir(params.hooks_dir)

    if err_str := params.check_error():
        eprint(err_str)
        sys.exit(1)

    if params.quiet:
        print("Disabling eprint")
        disable_eprint()

    if params.interactive:
        if params.standalone:
            standalone_interactive_repl(params)
        else:
            multinode_interactive_repl(params)


if __name__ == "__main__":
    main()

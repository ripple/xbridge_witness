from contextlib import contextmanager
import json
import logging
import os
from pathlib import Path
import signal
import subprocess
import time
from typing import Callable, Dict, List, Optional, Set, Union


class WitnessApp:
    def __init__(self):
        pass


# Start a witness server
@contextmanager
def witness_servers(
    *,
    config_filenames: List[str],
    server_out=os.devnull,
    run_servers: bool = True,
    exe: str,
    extra_args: Optional[List[str]] = None,
):
    """Start a witness server and return an app"""
    try:
        if extra_args is None:
            extra_args = []
        app = None
        to_runs = []
        processes = []
        if run_servers:
            for config_filename in config_filenames:
                to_runs.append([exe, "--verbose", "--conf", config_filename])
                fout = open(server_out, "w")
                processes.append(
                    subprocess.Popen(
                        to_runs[-1] + extra_args, stdout=fout, stderr=subprocess.STDOUT
                    )
                )
                print(
                    f"started witness server: config: {config_filename} PID: {processes[-1].pid}",
                    flush=True,
                )
            time.sleep(1.5)  # give processes time to startup
        app = WitnessApp()
        yield app
    finally:
        if app:
            pass
        if run_servers and to_runs:
            for r, p in zip(to_runs, processes):
                # subprocess.Popen(r + ["stop"], stdout=fout, stderr=subprocess.STDOUT)
                print(
                    f"killing witness server: PID: {p.pid}...",
                    flush=True,
                )
                # TODO: Make this SIGINT
                os.kill(p.pid, signal.SIGKILL)
                p.wait()
                print(
                    f"finished killing witness server: PID: {p.pid}",
                    flush=True,
                )

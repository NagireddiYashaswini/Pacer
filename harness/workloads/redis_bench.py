"""Redis + redis-benchmark wrapper with disjoint client/server CPU pinning."""

import os
import re
import shutil
import subprocess
import time
from typing import Callable, List, Optional, Tuple

from pool_config import load_pools_config
from workloads.base import WorkloadRunResult, WorkloadWrapper


class RedisBenchmarkWorkload(WorkloadWrapper):
    name = "redis"

    def _find_bins(self) -> Tuple[Optional[str], Optional[str]]:
        local_dir = os.path.join("third_party", "redis", "src")
        srv = os.path.join(local_dir, "redis-server")
        cli = os.path.join(local_dir, "redis-benchmark")
        if not (os.path.isfile(srv) and os.access(srv, os.X_OK)):
            srv = shutil.which("redis-server")
        if not (os.path.isfile(cli) and os.access(cli, os.X_OK)):
            cli = shutil.which("redis-benchmark")
        return srv, cli

    def check(self) -> Tuple[bool, str]:
        srv, cli = self._find_bins()
        if srv and cli:
            return True, f"redis-server={srv}, redis-benchmark={cli}"
        return False, "redis-server and/or redis-benchmark not found (run scripts/fetch_workloads.sh)"

    def launch(
        self,
        run_dir: str,
        variant_cfg: Optional[dict] = None,
        place_fn: Optional[Callable[[List[int]], None]] = None,
        client_cpulist: Optional[str] = None,
    ) -> WorkloadRunResult:
        ok, reason = self.check()
        if not ok:
            raise FileNotFoundError(reason)
        srv_bin, cli_bin = self._find_bins()
        cfg = variant_cfg or {}
        pools = load_pools_config()
        # Client must NOT share cores with server; run on control-plane CPUs
        if client_cpulist is None:
            client_cpulist = pools["control_plane_cpus_str"]

        os.makedirs(run_dir, exist_ok=True)
        srv_out = os.path.join(run_dir, "redis_server.stdout.log")
        srv_err = os.path.join(run_dir, "redis_server.stderr.log")
        cli_out = os.path.join(run_dir, "redis_benchmark.stdout.log")
        cli_err = os.path.join(run_dir, "redis_benchmark.stderr.log")

        port = int(cfg.get("port", 16379))
        srv_cmd = [srv_bin, "--port", str(port), "--save", "", "--appendonly", "no"]

        with open(srv_out, "w") as so, open(srv_err, "w") as se:
            srv_proc = subprocess.Popen(srv_cmd, stdout=so, stderr=se, text=True)
            if place_fn is not None:
                place_fn([srv_proc.pid])
            time.sleep(0.4)

            cli_cmd = [
                "taskset", "-c", client_cpulist,
                cli_bin,
                "-p", str(port),
                "-n", str(cfg.get("requests", 50000)),
                "-c", str(cfg.get("clients", 16)),
                "-d", str(cfg.get("data_size", 256)),
                "-r", str(cfg.get("keyspace", 10000)),
                "-t", str(cfg.get("tests", "get,set")),
                "--csv",
            ]
            t0 = time.perf_counter()
            with open(cli_out, "w") as co, open(cli_err, "w") as ce:
                cli_proc = subprocess.run(cli_cmd, stdout=co, stderr=ce, text=True, check=False)
            t1 = time.perf_counter()

            srv_proc.terminate()
            srv_proc.wait(timeout=3.0)

        with open(cli_out, "r") as f:
            txt = f.read()
        rps_matches = re.findall(r'"[^"]+","([0-9.]+)"', txt)
        avg_rps = (sum(float(x) for x in rps_matches) / len(rps_matches)) if rps_matches else None

        return WorkloadRunResult(
            name=self.name,
            variant=cfg.get("variant", "redis"),
            split=cfg.get("split", "unknown"),
            wall_time_sec=(t1 - t0),
            throughput_metric=avg_rps,
            throughput_unit="requests/sec",
            exit_status=cli_proc.returncode,
            stdout_path=cli_out,
            stderr_path=cli_err,
            extra_metrics={"client_cpulist": client_cpulist},
        )

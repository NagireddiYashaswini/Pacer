"""Mixed multi-tenant workload: concurrent compute + memory + I/O-ish processes."""

import os
import re
import subprocess
import sys
import time
from typing import Callable, List, Optional, Tuple

from workloads.base import WorkloadRunResult, WorkloadWrapper


class MixedTenantWorkload(WorkloadWrapper):
    name = "mixed_tenant"

    def check(self) -> Tuple[bool, str]:
        for b in ("./bin/bench_compute", "./bin/bench_memory"):
            if not (os.path.isfile(b) and os.access(b, os.X_OK)):
                return False, f"Missing {b} (run 'make all')"
        return True, "bench_compute + bench_memory + python fsync I/O worker ready"

    def launch(
        self,
        run_dir: str,
        variant_cfg: Optional[dict] = None,
        place_fn: Optional[Callable[[List[int]], None]] = None,
        launch_cpus: Optional[set] = None,
        sample_fn: Optional[Callable[[List[int]], None]] = None,
    ) -> WorkloadRunResult:
        ok, reason = self.check()
        if not ok:
            raise FileNotFoundError(reason)
        cfg = variant_cfg or {}
        dur = float(cfg.get("duration_sec", 3.0))
        os.makedirs(run_dir, exist_ok=True)

        def _preexec() -> None:
            if launch_cpus:
                os.sched_setaffinity(0, launch_cpus)

        io_script = (
            "import os, time, tempfile\n"
            "t0 = time.monotonic()\n"
            "n = 0\n"
            "buf = b'x' * 4096\n"
            "f = tempfile.TemporaryFile()\n"
            f"while time.monotonic() - t0 < {dur}:\n"
            "    f.seek(0); f.write(buf); f.flush(); os.fsync(f.fileno()); n += 1\n"
            "print(f'IO_OPS={n} elapsed={time.monotonic()-t0:.4f}')\n"
        )

        cmds = {
            "compute": ["./bin/bench_compute", "--iterations", "0", "--duration", str(dur), "--matrix-size", "128"],
            "memory": ["./bin/bench_memory", "--iterations", "0", "--duration", str(dur), "--buffer-mb", "64"],
            "io": [sys.executable, "-c", io_script],
        }

        procs = {}
        logs = {}
        t0 = time.perf_counter()
        for role, cmd in cmds.items():
            out_p = os.path.join(run_dir, f"mixed_{role}.stdout.log")
            err_p = os.path.join(run_dir, f"mixed_{role}.stderr.log")
            out_f = open(out_p, "w")
            err_f = open(err_p, "w")
            p = subprocess.Popen(
                cmd, stdout=out_f, stderr=err_f, text=True,
                preexec_fn=_preexec if launch_cpus else None,
            )
            procs[role] = (p, out_f, err_f, out_p, err_p)

        all_pids = [p.pid for p, *_ in procs.values()]
        if place_fn is not None:
            place_fn(all_pids)

        while any(p.poll() is None for p, *_ in procs.values()):
            if sample_fn is not None:
                sample_fn(all_pids)
            time.sleep(0.25)

        rc_max = 0
        for role, (p, out_f, err_f, _, _) in procs.items():
            rc = p.wait()
            out_f.close()
            err_f.close()
            rc_max = max(rc_max, rc)
            logs[role] = procs[role][3]
        t1 = time.perf_counter()

        per_tenant = {}
        with open(logs["compute"]) as f:
            m = re.search(r"Throughput:\s*([0-9.]+)\s*GFLOPS", f.read())
            per_tenant["compute_gflops"] = float(m.group(1)) if m else None
        with open(logs["memory"]) as f:
            m = re.search(r"([0-9.]+)\s*M\s*traversals/sec", f.read(), re.IGNORECASE)
            per_tenant["memory_mtraversals_sec"] = float(m.group(1)) if m else None
        with open(logs["io"]) as f:
            m = re.search(r"IO_OPS=(\d+)\s+elapsed=([0-9.]+)", f.read())
            per_tenant["io_fsync_ops_sec"] = (
                float(m.group(1)) / float(m.group(2)) if (m and float(m.group(2)) > 0) else None
            )

        return WorkloadRunResult(
            name=self.name,
            variant=cfg.get("variant", self.name),
            split=cfg.get("split", "unknown"),
            wall_time_sec=(t1 - t0),
            throughput_metric=per_tenant.get("compute_gflops"),
            throughput_unit="per_tenant_metrics",
            exit_status=rc_max,
            stdout_path=logs["compute"],
            stderr_path=procs["compute"][4],
            extra_metrics=per_tenant,
        )

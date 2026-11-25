"""Wrappers for built-in C99 microbenchmarks (bench_compute, bench_memory, bench_dram_chase, bench_phase)."""

import os
import re
from typing import Callable, List, Optional, Tuple

from workloads.base import WorkloadRunResult, WorkloadWrapper


class _MicrobenchBase(WorkloadWrapper):
    bin_path: str = ""
    throughput_unit: str = "ops/sec"

    def check(self) -> Tuple[bool, str]:
        if os.path.isfile(self.bin_path) and os.access(self.bin_path, os.X_OK):
            return True, f"built binary found at {self.bin_path}"
        return False, f"binary missing at {self.bin_path} (run 'make all')"

    def _parse_throughput(self, stdout_text: str, wall_time: float) -> Optional[float]:
        return None

    def launch(
        self,
        run_dir: str,
        variant_cfg: Optional[dict] = None,
        place_fn: Optional[Callable[[List[int]], None]] = None,
    ) -> WorkloadRunResult:
        ok, reason = self.check()
        if not ok:
            raise FileNotFoundError(reason)
        cfg = variant_cfg or {}
        args = [str(a) for a in cfg.get("args", [])]
        cmd = [self.bin_path] + args
        rc, wall, out_p, err_p = self._run_subprocess(cmd, run_dir, self.name, place_fn)
        with open(out_p, "r") as f:
            txt = f.read()
        tp = self._parse_throughput(txt, wall)
        return WorkloadRunResult(
            name=self.name,
            variant=cfg.get("variant", self.name),
            split=cfg.get("split", "unknown"),
            wall_time_sec=wall,
            throughput_metric=tp,
            throughput_unit=self.throughput_unit,
            exit_status=rc,
            stdout_path=out_p,
            stderr_path=err_p,
        )


class BenchComputeWorkload(_MicrobenchBase):
    name = "bench_compute"
    bin_path = "./bin/bench_compute"
    throughput_unit = "GFLOPS"

    def _parse_throughput(self, stdout_text: str, wall_time: float) -> Optional[float]:
        m = re.search(r"Throughput:\s*([0-9.]+)\s*GFLOPS", stdout_text)
        return float(m.group(1)) if m else None


class BenchMemoryWorkload(_MicrobenchBase):
    name = "bench_memory"
    bin_path = "./bin/bench_memory"
    throughput_unit = "M_traversals/sec"

    def _parse_throughput(self, stdout_text: str, wall_time: float) -> Optional[float]:
        m = re.search(r"([0-9.]+)\s*M\s*traversals/sec", stdout_text, re.IGNORECASE)
        return float(m.group(1)) if m else None


class BenchDramChaseWorkload(_MicrobenchBase):
    name = "bench_dram_chase"
    bin_path = "./bin/bench_dram_chase"
    throughput_unit = "steps/sec"

    def _parse_throughput(self, stdout_text: str, wall_time: float) -> Optional[float]:
        m = re.search(r"steps=(\d+)\s+elapsed=([0-9.]+)", stdout_text)
        if m and float(m.group(2)) > 0:
            return float(m.group(1)) / float(m.group(2))
        return None


class BenchPhaseWorkload(_MicrobenchBase):
    name = "bench_phase"
    bin_path = "./bin/bench_phase"
    throughput_unit = "phases/sec"

    def _parse_throughput(self, stdout_text: str, wall_time: float) -> Optional[float]:
        return (1.0 / wall_time) if wall_time > 0 else None

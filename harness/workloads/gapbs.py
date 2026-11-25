"""GAP Benchmark Suite (bfs, pr) wrapper."""

import os
import re
from typing import Callable, List, Optional, Tuple

from workloads.base import WorkloadRunResult, WorkloadWrapper


class _GapbsBase(WorkloadWrapper):
    binary_name: str = ""

    def _find_binary(self) -> Optional[str]:
        gapbs_dir = os.environ.get("GAPBS_DIR", os.path.join("third_party", "gapbs"))
        cand = os.path.join(gapbs_dir, self.binary_name)
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
        return None

    def check(self) -> Tuple[bool, str]:
        b = self._find_binary()
        if b:
            return True, f"GAPBS {self.binary_name} found at {b}"
        return False, (
            f"GAPBS {self.binary_name} not found in $GAPBS_DIR or third_party/gapbs/ "
            f"(run scripts/fetch_workloads.sh)"
        )

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
        bin_path = self._find_binary()
        cfg = variant_cfg or {}
        scale = int(cfg.get("graph_scale", 18))
        degree = int(cfg.get("graph_degree", 16))
        trials = int(cfg.get("trials", 8))
        cmd = [bin_path, "-g", str(scale), "-k", str(degree), "-n", str(trials)]
        rc, wall, out_p, err_p = self._run_subprocess(
            cmd, run_dir, self.name, place_fn=place_fn,
            launch_cpus=launch_cpus, sample_fn=sample_fn,
        )
        with open(out_p, "r") as f:
            txt = f.read()
        avg_m = re.search(r"Average Time:\s*([0-9.]+)", txt)
        tp = (1.0 / float(avg_m.group(1))) if (avg_m and float(avg_m.group(1)) > 0) else None
        return WorkloadRunResult(
            name=self.name,
            variant=cfg.get("variant", self.name),
            split=cfg.get("split", "unknown"),
            wall_time_sec=wall,
            throughput_metric=tp,
            throughput_unit="trials/sec",
            exit_status=rc,
            stdout_path=out_p,
            stderr_path=err_p,
        )


class GapbsBfsWorkload(_GapbsBase):
    name = "gapbs_bfs"
    binary_name = "bfs"


class GapbsPrWorkload(_GapbsBase):
    name = "gapbs_pr"
    binary_name = "pr"

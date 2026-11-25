"""PARSEC and SPEC CPU detection wrappers (explicit SKIPPED if not installed/licensed)."""

import os
from typing import Callable, List, Optional, Tuple

from workloads.base import WorkloadRunResult, WorkloadWrapper


class ParsecWorkload(WorkloadWrapper):
    name = "parsec"

    def check(self) -> Tuple[bool, str]:
        parsec_dir = os.environ.get("PARSEC_DIR") or os.environ.get("PARSECDIR")
        if not parsec_dir or not os.path.isdir(parsec_dir):
            return False, "PARSEC_DIR not set or directory does not exist (not installed)"
        parsecmgmt = os.path.join(parsec_dir, "bin", "parsecmgmt")
        if not os.path.isfile(parsecmgmt):
            return False, f"parsecmgmt not found under {parsec_dir}/bin/"
        return True, f"PARSEC detected at {parsec_dir}"

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
            raise RuntimeError(f"SKIPPED parsec: {reason}")
        parsec_dir = os.environ.get("PARSEC_DIR") or os.environ.get("PARSECDIR")
        cfg = variant_cfg or {}
        pkg = cfg.get("package", "blackscholes")
        inp = cfg.get("input", "simmedium")
        cmd = [os.path.join(parsec_dir, "bin", "parsecmgmt"), "-a", "run", "-p", pkg, "-i", inp]
        rc, wall, out_p, err_p = self._run_subprocess(
            cmd, run_dir, f"parsec_{pkg}", place_fn=place_fn,
            launch_cpus=launch_cpus, sample_fn=sample_fn,
        )
        return WorkloadRunResult(
            name=self.name,
            variant=cfg.get("variant", pkg),
            split=cfg.get("split", "unknown"),
            wall_time_sec=wall,
            throughput_metric=(1.0 / wall if wall > 0 else None),
            throughput_unit="runs/sec",
            exit_status=rc,
            stdout_path=out_p,
            stderr_path=err_p,
        )


class SpecCpuWorkload(WorkloadWrapper):
    name = "spec_cpu"

    def check(self) -> Tuple[bool, str]:
        spec_dir = os.environ.get("SPEC") or os.environ.get("SPEC_DIR")
        if not spec_dir or not os.path.isdir(spec_dir):
            return False, "SPEC / SPEC_DIR not set (licensed SPEC CPU suite not installed)"
        runcpu = os.path.join(spec_dir, "bin", "runcpu")
        if not os.path.isfile(runcpu):
            return False, f"runcpu not found under {spec_dir}/bin/ (not licensed/installed)"
        return True, f"SPEC CPU detected at {spec_dir}"

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
            raise RuntimeError(f"SKIPPED spec_cpu: {reason}")
        spec_dir = os.environ.get("SPEC") or os.environ.get("SPEC_DIR")
        cfg = variant_cfg or {}
        bench = cfg.get("benchmark", "505.mcf_r")
        spec_cfg = cfg.get("config", "default.cfg")
        cmd = [os.path.join(spec_dir, "bin", "runcpu"), "--config", spec_cfg, bench]
        rc, wall, out_p, err_p = self._run_subprocess(
            cmd, run_dir, f"spec_{bench}", place_fn=place_fn,
            launch_cpus=launch_cpus, sample_fn=sample_fn,
        )
        return WorkloadRunResult(
            name=self.name,
            variant=cfg.get("variant", bench),
            split=cfg.get("split", "unknown"),
            wall_time_sec=wall,
            throughput_metric=(1.0 / wall if wall > 0 else None),
            throughput_unit="runs/sec",
            exit_status=rc,
            stdout_path=out_p,
            stderr_path=err_p,
        )

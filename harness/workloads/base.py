"""Base interface for workload wrappers and Part C split enforcement."""

import abc
import dataclasses
import os
import subprocess
import time
from typing import Callable, Dict, List, Optional, Tuple

import yaml

DEFAULT_SPLIT_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "config",
    "workload_split.yaml",
)


@dataclasses.dataclass
class WorkloadRunResult:
    name: str
    variant: str
    split: str
    wall_time_sec: float
    throughput_metric: Optional[float]
    throughput_unit: str
    exit_status: int
    stdout_path: str
    stderr_path: str
    extra_metrics: Dict[str, object] = dataclasses.field(default_factory=dict)


def load_workload_split(path: str = DEFAULT_SPLIT_PATH) -> Dict[str, dict]:
    if not os.path.exists(path):
        raise FileNotFoundError(f"Workload split config not found at {path}")
    with open(path, "r") as f:
        data = yaml.safe_load(f) or {}
    return data.get("workloads", {})


def assert_split_allows(variant_name: str, purpose: str, split_path: str = DEFAULT_SPLIT_PATH) -> dict:
    """
    Part C: refuse to use 'evaluation' variants for parameter selection or
    offline profiling/tuning.
    """
    variants = load_workload_split(split_path)
    if variant_name not in variants:
        raise KeyError(
            f"Unknown workload variant '{variant_name}' in {split_path}. "
            f"Available: {sorted(variants.keys())}"
        )
    cfg = dict(variants[variant_name])
    cfg["variant"] = variant_name
    split = cfg.get("split")
    if purpose in ("tuning", "parameter_selection", "offline_profile") and split == "evaluation":
        raise ValueError(
            f"REFUSED (Part C): variant '{variant_name}' belongs to split='evaluation' "
            f"and cannot be used for '{purpose}'."
        )
    return cfg


class WorkloadWrapper(abc.ABC):
    name: str = "base"

    @abc.abstractmethod
    def check(self) -> Tuple[bool, str]:
        """Return (is_available, human_readable_reason)."""
        ...

    def build(self) -> Tuple[bool, str]:
        """Build if needed, or return check() status."""
        return self.check()

    @abc.abstractmethod
    def launch(
        self,
        run_dir: str,
        variant_cfg: Optional[dict] = None,
        place_fn: Optional[Callable[[List[int]], None]] = None,
    ) -> WorkloadRunResult:
        ...

    def _run_subprocess(
        self,
        cmd: List[str],
        run_dir: str,
        tag: str,
        place_fn: Optional[Callable[[List[int]], None]] = None,
        launch_cpus: Optional[set] = None,
        sample_fn: Optional[Callable[[List[int]], None]] = None,
        sample_interval_sec: float = 0.25,
    ) -> Tuple[int, float, str, str]:
        os.makedirs(run_dir, exist_ok=True)
        stdout_path = os.path.join(run_dir, f"{tag}.stdout.log")
        stderr_path = os.path.join(run_dir, f"{tag}.stderr.log")

        def _preexec() -> None:
            if launch_cpus:
                os.sched_setaffinity(0, launch_cpus)

        with open(stdout_path, "w") as out_f, open(stderr_path, "w") as err_f:
            t0 = time.perf_counter()
            proc = subprocess.Popen(
                cmd,
                stdout=out_f,
                stderr=err_f,
                text=True,
                preexec_fn=_preexec if launch_cpus else None,
            )
            if place_fn is not None:
                place_fn([proc.pid])
            while proc.poll() is None:
                if sample_fn is not None:
                    sample_fn([proc.pid])
                time.sleep(sample_interval_sec)
            rc = proc.wait()
            t1 = time.perf_counter()
        return rc, (t1 - t0), stdout_path, stderr_path

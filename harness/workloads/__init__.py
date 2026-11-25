"""
Workload Wrappers & Tuning/Evaluation Split Enforcement (Part B & Part C).
"""

from workloads.base import (
    WorkloadRunResult,
    WorkloadWrapper,
    load_workload_split,
    assert_split_allows,
)
from workloads.microbenchmarks import (
    BenchComputeWorkload,
    BenchMemoryWorkload,
    BenchDramChaseWorkload,
    BenchPhaseWorkload,
)
from workloads.parsec_spec import ParsecWorkload, SpecCpuWorkload
from workloads.gapbs import GapbsBfsWorkload, GapbsPrWorkload
from workloads.redis_bench import RedisBenchmarkWorkload
from workloads.mixed_tenant import MixedTenantWorkload


ALL_WORKLOADS = {
    "bench_compute": BenchComputeWorkload,
    "bench_memory": BenchMemoryWorkload,
    "bench_dram_chase": BenchDramChaseWorkload,
    "bench_phase": BenchPhaseWorkload,
    "parsec": ParsecWorkload,
    "spec_cpu": SpecCpuWorkload,
    "gapbs_bfs": GapbsBfsWorkload,
    "gapbs_pr": GapbsPrWorkload,
    "redis": RedisBenchmarkWorkload,
    "mixed_tenant": MixedTenantWorkload,
}


def detect_all_workloads():
    """Return (available_dict, skipped_dict) without executing any workload."""
    available = {}
    skipped = {}
    for name, cls in ALL_WORKLOADS.items():
        w = cls()
        ok, reason = w.check()
        if ok:
            available[name] = reason
        else:
            skipped[name] = reason
    return available, skipped

#!/usr/bin/env python3
"""
Host Environment Guard for Pacer.

Refuses to start env_check.py or run_experiments.py unless:
  (a) PID 1 inside the PID namespace is the host init (systemd or init).
  (b) The process's Cpus_allowed_list is a subset of control_plane_cpus
      from config/pools.yaml.
  (c) /proc/1/cgroup shows the host root cgroup (/init.scope or /).
"""

from typing import List, Set

from pool_config import load_pools_config, parse_cpulist


def _read_stripped(path: str) -> str:
    with open(path, "r") as f:
        return f.read().strip()


def _parse_cpus_allowed_from_status(status_text: str) -> str:
    for line in status_text.splitlines():
        if line.startswith("Cpus_allowed_list:"):
            return line.split(":", 1)[1].strip()
    raise RuntimeError("Cpus_allowed_list not found in /proc/self/status")


def check_host_guard(
    pid1_comm: str = None,
    pid1_cgroup: str = None,
    cpus_allowed_str: str = None,
    control_plane_cpus: Set[int] = None,
) -> List[str]:
    """
    Validate all three host-guard conditions. Returns list of failure reasons
    (empty list means all checks passed). Parameters are injectable for unit testing.
    """
    failures: List[str] = []

    if pid1_comm is None:
        try:
            pid1_comm = _read_stripped("/proc/1/comm")
        except OSError as exc:
            pid1_comm = f"<unreadable: {exc}>"

    if pid1_cgroup is None:
        try:
            pid1_cgroup = _read_stripped("/proc/1/cgroup")
        except OSError as exc:
            pid1_cgroup = f"<unreadable: {exc}>"

    if cpus_allowed_str is None:
        try:
            cpus_allowed_str = _parse_cpus_allowed_from_status(_read_stripped("/proc/self/status"))
        except Exception as exc:
            cpus_allowed_str = ""
            failures.append(f"(b) Cpus_allowed_list unreadable: {exc}")

    if control_plane_cpus is None:
        pools = load_pools_config()
        control_plane_cpus = set(pools["control_plane_cpus"])

    # (a) PID 1 must be host init (systemd or init)
    if pid1_comm not in ("systemd", "init"):
        failures.append(
            f"(a) PID 1 check failed: /proc/1/comm is '{pid1_comm}' "
            f"(expected 'systemd' or 'init'; refusing to run inside a container/sandbox PID namespace)."
        )

    # (b) Cpus_allowed_list must be a subset of control_plane_cpus
    if cpus_allowed_str:
        allowed_set = set(parse_cpulist(cpus_allowed_str))
        if not allowed_set.issubset(control_plane_cpus):
            failures.append(
                f"(b) Cpus_allowed_list check failed: process Cpus_allowed_list='{cpus_allowed_str}' "
                f"is NOT a subset of control_plane_cpus={sorted(control_plane_cpus)} from config/pools.yaml. "
                f"Launch with: taskset -c {','.join(map(str, sorted(control_plane_cpus)))} ..."
            )

    # (c) /proc/1/cgroup must show host root cgroup (0::/init.scope or 0::/)
    cg_lines = [ln.strip() for ln in pid1_cgroup.splitlines() if ln.strip()]
    valid_cg = any(
        ln.endswith("::/init.scope") or ln.endswith("::/")
        for ln in cg_lines
    )
    if not valid_cg:
        failures.append(
            f"(c) /proc/1/cgroup check failed: '{pid1_cgroup}' does not show host root cgroup "
            f"('/init.scope' or '/'); refusing to run inside a nested sandbox/container cgroup."
        )

    return failures


def enforce_host_guard() -> None:
    failures = check_host_guard()
    if failures:
        msg = "\n".join(f"  - {f}" for f in failures)
        raise RuntimeError(
            f"[FATAL] Host guard refused to start:\n{msg}\n"
            f"Run from your host terminal under control-plane pinning, e.g.:\n"
            f"  taskset -c 17,35 python3 ..."
        )

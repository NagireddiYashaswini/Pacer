#!/usr/bin/env python3
"""
Scheduling Policy Implementations & Registry for Pacer (Part A).

Every policy implements the same interface:
  - setup(workload)
  - place(pids_or_tids)
  - on_tick()
  - teardown() -> dict (metadata, daemon_pid, daemon_cpu_time_sec, etc.)

All core lists are loaded from config/pools.yaml via harness/pool_config.py.
No core numbers are hardcoded.
"""

import abc
import datetime
import glob
import json
import os
import platform
import signal
import subprocess
import time
from typing import Dict, Iterable, List, Optional, Type

from pool_config import load_pools_config
from rapl_utils import find_rapl_package_domain


PROFILES_DIR = os.path.join("results", "profiles")
DAEMON_BIN = "./bin/hybridsched_daemon"


def _cpulist_to_str(cpus: List[int]) -> str:
    return ",".join(str(c) for c in cpus)


def _discover_tids(pid: int) -> List[int]:
    task_dir = f"/proc/{pid}/task"
    try:
        return sorted(int(x) for x in os.listdir(task_dir) if x.isdigit())
    except OSError:
        return [pid]


def _taskset_pid_all_tids(pid: int, cpulist_str: str) -> None:
    """Pin all threads of `pid` to `cpulist_str` using taskset."""
    for tid in _discover_tids(pid):
        subprocess.run(
            ["taskset", "-pc", cpulist_str, str(tid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def _read_proc_cpu_time_sec(pid: int) -> Optional[float]:
    """Read user+sys CPU time (seconds) for `pid` from /proc/<pid>/stat."""
    try:
        with open(f"/proc/{pid}/stat", "r") as f:
            line = f.read()
        rparen = line.rfind(")")
        if rparen < 0:
            return None
        fields = line[rparen + 2 :].split()
        # fields[11] = utime (14th field), fields[12] = stime (15th field)
        utime = int(fields[11])
        stime = int(fields[12])
        clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
        return (utime + stime) / float(clk_tck)
    except (OSError, IndexError, ValueError):
        return None


def _read_rapl_uj(energy_path: str) -> int:
    with open(energy_path, "r") as f:
        return int(f.read().strip())


def _assert_tuning_split(workload: Optional[dict], action: str) -> None:
    """Part C: refuse to run parameter selection or offline profiling on evaluation variants."""
    if not workload:
        return
    split = workload.get("split")
    if split == "evaluation":
        raise ValueError(
            f"REFUSED: '{action}' is forbidden on evaluation-split workload "
            f"'{workload.get('variant', workload.get('name', 'unknown'))}'. "
            f"Use a 'tuning' split variant from config/workload_split.yaml."
        )


class BasePolicy(abc.ABC):
    name: str = "base"
    label: str = ""
    is_deployable: bool = True

    def __init__(self, **kwargs):
        self.pools = load_pools_config()
        self.p_cores_str: str = self.pools["p_cores_str"]
        self.e_cores_str: str = self.pools["e_cores_str"]
        self.allowed_cores_str: str = _cpulist_to_str(
            sorted(set(self.pools["p_cores"]) | set(self.pools["e_cores"]))
        )
        self.kwargs = kwargs
        self.workload: Optional[dict] = None
        self.placed_pids: List[int] = []

    @abc.abstractmethod
    def setup(self, workload: Optional[dict] = None) -> None:
        ...

    @abc.abstractmethod
    def place(self, pids_or_tids: Iterable[int]) -> None:
        ...

    def launch_cpus(self) -> set:
        """Initial CPU set the workload must be launched inside."""
        return set(self.pools["p_cores"]) | set(self.pools["e_cores"])

    def on_tick(self) -> None:
        pass

    def teardown(self) -> Dict[str, object]:
        return {
            "policy": self.name,
            "label": self.label,
            "is_deployable": self.is_deployable,
            "kernel_version": platform.release(),
            "p_cores": self.p_cores_str,
            "e_cores": self.e_cores_str,
            "excluded_siblings": self.pools["excluded_siblings_str"],
        }


class DefaultLinuxPolicy(BasePolicy):
    name = "default-linux"
    label = (
        "Stock Linux CFS restricted to allowed CPU set (emulated heterogeneity; "
        "does NOT include Intel Thread Director)"
    )
    is_deployable = True

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload

    def place(self, pids_or_tids: Iterable[int]) -> None:
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, self.allowed_cores_str)


class StaticPPolicy(BasePolicy):
    name = "static-P"
    label = "Static P-core pool assignment"
    is_deployable = True

    def launch_cpus(self) -> set:
        return set(self.pools["p_cores"])

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload

    def place(self, pids_or_tids: Iterable[int]) -> None:
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, self.p_cores_str)


class StaticEPolicy(BasePolicy):
    name = "static-E"
    label = "Static E-core pool assignment"
    is_deployable = True

    def launch_cpus(self) -> set:
        return set(self.pools["e_cores"])

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload

    def place(self, pids_or_tids: Iterable[int]) -> None:
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, self.e_cores_str)


class OracleStaticPolicy(BasePolicy):
    name = "oracle-static"
    label = "OFFLINE UPPER BOUND for static assignment, not a deployable policy"
    is_deployable = False

    def __init__(self, profile_path: Optional[str] = None, **kwargs):
        super().__init__(**kwargs)
        self.profile_path = profile_path
        self.chosen_pool: Optional[str] = None
        self.profile_data: Optional[dict] = None

    def profile(self, workload: dict, cmd: List[str], trial_index: int = 1) -> str:
        """
        Offline profiling mode: run `cmd` on both P and E pools in isolation,
        measure wall time and RAPL energy, compute EDP per pool, and save to
        a unique timestamped file in results/profiles/ (never overwriting).
        Refuses evaluation-split workloads (Part C).
        """
        _assert_tuning_split(workload, "oracle-static offline profiling")

        domain, energy_path, _ = find_rapl_package_domain()
        if not energy_path or not os.access(energy_path, os.R_OK):
            raise RuntimeError(
                f"FATAL: oracle-static profiling requires readable RAPL energy_uj "
                f"(found={energy_path})."
            )

        results_by_pool = {}
        for pool_name, cpulist in [("P-Core", self.p_cores_str), ("E-Core", self.e_cores_str)]:
            e0 = _read_rapl_uj(energy_path)
            t0 = time.perf_counter()
            proc = subprocess.run(
                ["taskset", "-c", cpulist] + cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            t1 = time.perf_counter()
            e1 = _read_rapl_uj(energy_path)
            if proc.returncode != 0:
                raise RuntimeError(
                    f"oracle-static profiling command failed on {pool_name} (rc={proc.returncode}): "
                    f"{proc.stderr.strip()}"
                )
            dur = t1 - t0
            energy_j = (e1 - e0) / 1e6
            edp = energy_j * dur
            results_by_pool[pool_name] = {
                "wall_time_sec": dur,
                "energy_joules": energy_j,
                "edp": edp,
            }

        best_pool = (
            "P-Core"
            if results_by_pool["P-Core"]["edp"] <= results_by_pool["E-Core"]["edp"]
            else "E-Core"
        )

        os.makedirs(PROFILES_DIR, exist_ok=True)
        ts = datetime.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
        app_name = workload.get("workload", workload.get("name", "unknown"))
        out_path = os.path.join(PROFILES_DIR, f"oracle_{app_name}_t{trial_index}_{ts}.json")
        # Never overwrite an existing profile
        suffix = 1
        while os.path.exists(out_path):
            out_path = os.path.join(PROFILES_DIR, f"oracle_{app_name}_t{trial_index}_{ts}_{suffix}.json")
            suffix += 1

        payload = {
            "policy": self.name,
            "label": self.label,
            "is_deployable": False,
            "workload": app_name,
            "split": workload.get("split", "tuning"),
            "kernel_version": platform.release(),
            "rapl_domain": domain,
            "p_cores": self.p_cores_str,
            "e_cores": self.e_cores_str,
            "excluded_siblings": self.pools["excluded_siblings_str"],
            "timestamp_iso": ts,
            "pools": results_by_pool,
            "best_pool_by_edp": best_pool,
        }
        with open(out_path, "w") as f:
            json.dump(payload, f, indent=2)

        self.profile_path = out_path
        self.profile_data = payload
        self.chosen_pool = best_pool
        return out_path

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload
        if self.profile_data is None:
            if not self.profile_path:
                app = (workload or {}).get("workload", "")
                matches = sorted(glob.glob(os.path.join(PROFILES_DIR, f"oracle_{app}_*.json")))
                if matches:
                    self.profile_path = matches[-1]
            if not self.profile_path or not os.path.exists(self.profile_path):
                raise FileNotFoundError(
                    "oracle-static replay requires an existing profile JSON in results/profiles/. "
                    "Run profile() on a 'tuning' split variant first."
                )
            with open(self.profile_path, "r") as f:
                self.profile_data = json.load(f)
        self.chosen_pool = self.profile_data["best_pool_by_edp"]

    def launch_cpus(self) -> set:
        if self.chosen_pool == "E-Core":
            return set(self.pools["e_cores"])
        return set(self.pools["p_cores"])

    def place(self, pids_or_tids: Iterable[int]) -> None:
        if not self.chosen_pool:
            raise RuntimeError("oracle-static setup() must be called before place().")
        target = self.p_cores_str if self.chosen_pool == "P-Core" else self.e_cores_str
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, target)


class PieLikePolicy(BasePolicy):
    name = "pie-like"
    label = "reimplementation inspired by Van Craeynest et al., ISCA 2012"
    is_deployable = True

    # Intel Sapphire Rapids raw PMU event encodings (PERF_TYPE_RAW = 4):
    # config = event | (umask << 8) | (cmask << 24)
    RAW_EVENTS = {
        "L1D_PEND_MISS.PENDING":        0x00000148,
        "CYCLE_ACTIVITY.STALLS_MEM_ANY": 0x140014a3,
        "CYCLE_ACTIVITY.STALLS_TOTAL":   0x040004a3,
        "RESOURCE_STALLS.ANY":           0x000001a2,
    }
    PERF_TYPE_HARDWARE = 0
    PERF_TYPE_RAW = 4
    PERF_COUNT_HW_CPU_CYCLES = 0
    __NR_perf_event_open = 298  # x86_64

    @classmethod
    def _perf_event_open_ctypes(cls, ev_type: int, config: int, pid: int = 0) -> int:
        import ctypes
        import ctypes.util

        class perf_event_attr(ctypes.Structure):
            _fields_ = [
                ("type", ctypes.c_uint32),
                ("size", ctypes.c_uint32),
                ("config", ctypes.c_uint64),
                ("sample_period", ctypes.c_uint64),
                ("sample_type", ctypes.c_uint64),
                ("read_format", ctypes.c_uint64),
                ("flags", ctypes.c_uint64),
                ("wakeup_events", ctypes.c_uint32),
                ("bp_type", ctypes.c_uint32),
                ("bp_addr", ctypes.c_uint64),
                ("bp_len", ctypes.c_uint64),
                ("branch_sample_type", ctypes.c_uint64),
                ("sample_regs_user", ctypes.c_uint64),
                ("sample_stack_user", ctypes.c_uint32),
                ("clockid", ctypes.c_int32),
                ("sample_regs_intr", ctypes.c_uint64),
                ("aux_watermark", ctypes.c_uint32),
                ("sample_max_stack", ctypes.c_uint16),
                ("__reserved_2", ctypes.c_uint16),
            ]

        libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        pe = perf_event_attr()
        pe.type = ev_type
        pe.size = ctypes.sizeof(perf_event_attr)
        pe.config = config
        # exclude_kernel (bit 5) | exclude_hv (bit 6)
        pe.flags = (1 << 5) | (1 << 6)
        fd = libc.syscall(cls.__NR_perf_event_open, ctypes.byref(pe), pid, -1, -1, 0)
        if fd < 0:
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err))
        return int(fd)

    @classmethod
    def probe_openable_events(cls) -> Dict[str, dict]:
        results = {}
        for ev_name, cfg in cls.RAW_EVENTS.items():
            try:
                fd = cls._perf_event_open_ctypes(cls.PERF_TYPE_RAW, cfg, pid=0)
                os.close(fd)
                results[ev_name] = {"openable": True, "encoding": f"0x{cfg:08x}", "errno": 0}
            except OSError as exc:
                results[ev_name] = {"openable": False, "encoding": f"0x{cfg:08x}", "errno": exc.errno, "error": exc.strerror}
        return results

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._tid_fds: Dict[int, Dict[str, int]] = {}
        self._prev_vals: Dict[int, Dict[str, int]] = {}

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload
        probe = self.probe_openable_events()
        failed = {k: v for k, v in probe.items() if not v["openable"]}
        if failed:
            raise RuntimeError(
                f"[FATAL] pie-like ({self.label}) cannot open required PMU counters via perf_event_open: {failed}"
            )

    def place(self, pids_or_tids: Iterable[int]) -> None:
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, self.p_cores_str)
            for tid in _discover_tids(pid):
                if tid in self._tid_fds:
                    continue
                fds = {}
                try:
                    fds["cycles"] = self._perf_event_open_ctypes(self.PERF_TYPE_HARDWARE, self.PERF_COUNT_HW_CPU_CYCLES, pid=tid)
                    for ev_name, cfg in self.RAW_EVENTS.items():
                        fds[ev_name] = self._perf_event_open_ctypes(self.PERF_TYPE_RAW, cfg, pid=tid)
                    self._tid_fds[tid] = fds
                    self._prev_vals[tid] = {k: 0 for k in fds}
                except OSError:
                    for fd in fds.values():
                        os.close(fd)

    def launch_cpus(self) -> set:
        """Initial CPU set the workload must be launched inside."""
        return set(self.pools["p_cores"]) | set(self.pools["e_cores"])

    def on_tick(self) -> None:
        import struct
        for tid, fds in list(self._tid_fds.items()):
            cur = {}
            try:
                for k, fd in fds.items():
                    data = os.read(fd, 8)
                    cur[k] = struct.unpack("Q", data)[0]
            except OSError:
                continue
            prev = self._prev_vals[tid]
            d_cyc = cur["cycles"] - prev["cycles"]
            d_mem = cur["CYCLE_ACTIVITY.STALLS_MEM_ANY"] - prev["CYCLE_ACTIVITY.STALLS_MEM_ANY"]
            d_l1d = cur["L1D_PEND_MISS.PENDING"] - prev["L1D_PEND_MISS.PENDING"]
            self._prev_vals[tid] = cur
            if d_cyc > 0:
                mem_stall_frac = d_mem / float(d_cyc)
                mlp = (d_l1d / float(d_mem)) if d_mem > 0 else 1.0
                target = self.e_cores_str if (mem_stall_frac >= 0.50 and mlp < 2.0) else self.p_cores_str
                subprocess.run(["taskset", "-pc", target, str(tid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)

    def teardown(self) -> Dict[str, object]:
        for fds in self._tid_fds.values():
            for fd in fds.values():
                try:
                    os.close(fd)
                except OSError:
                    pass
        self._tid_fds.clear()
        self._prev_vals.clear()
        return super().teardown()


class PieLikeTopdownPolicy(BasePolicy):
    name = "pie-like-topdown"
    label = (
        "simplified pie-like variant using only architectural topdown events "
        "(topdown-mem-bound, topdown-retiring, slots) with PERF_FORMAT_GROUP"
    )
    is_deployable = True
    PERF_FORMAT_GROUP = 1 << 3

    @staticmethod
    def _read_sysfs_topdown_event(name: str) -> int:
        p = f"/sys/bus/event_source/devices/cpu/events/{name}"
        with open(p, "r") as f:
            txt = f.read().strip()
        # format: event=0x...,umask=0x...
        ev = 0
        umask = 0
        for part in txt.split(","):
            k, v = part.split("=")
            if k.strip() == "event":
                ev = int(v, 0)
            elif k.strip() == "umask":
                umask = int(v, 0)
        return (ev & 0xFF) | ((umask & 0xFF) << 8)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._tid_groups: Dict[int, tuple] = {}
        self._prev: Dict[int, tuple] = {}

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload
        for ev in ("slots", "topdown-mem-bound", "topdown-retiring"):
            p = f"/sys/bus/event_source/devices/cpu/events/{ev}"
            if not os.path.exists(p):
                raise RuntimeError(f"pie-like-topdown: architectural event '{ev}' not exposed at {p}")

    def place(self, pids_or_tids: Iterable[int]) -> None:
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, self.p_cores_str)

    def teardown(self) -> Dict[str, object]:
        for fds in self._tid_groups.values():
            for fd in fds:
                try:
                    os.close(fd)
                except OSError:
                    pass
        self._tid_groups.clear()
        return super().teardown()


class HassLikePolicy(BasePolicy):
    name = "hass-like"
    label = "reimplementation inspired by Shelepov et al., 2009"
    is_deployable = True

    def __init__(self, signature_path: Optional[str] = None, **kwargs):
        super().__init__(**kwargs)
        self.signature_path = signature_path
        self.signature: Optional[dict] = None

    def build_signature(self, workload: dict, ipc_p: float, mpki_p: float,
                        ipc_e: float, mpki_e: float, preferred_pool: str) -> str:
        """
        Save an offline architectural signature built strictly from a tuning-split run.
        Stored separately from evaluation runs in results/profiles/.
        """
        _assert_tuning_split(workload, "hass-like signature building")
        os.makedirs(PROFILES_DIR, exist_ok=True)
        ts = datetime.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
        app_name = workload.get("workload", workload.get("name", "unknown"))
        out_path = os.path.join(PROFILES_DIR, f"hass_{app_name}_{ts}.json")
        payload = {
            "policy": self.name,
            "label": self.label,
            "workload": app_name,
            "split": workload.get("split", "tuning"),
            "ipc_p": ipc_p,
            "mpki_p": mpki_p,
            "ipc_e": ipc_e,
            "mpki_e": mpki_e,
            "preferred_pool": preferred_pool,
            "timestamp_iso": ts,
        }
        with open(out_path, "w") as f:
            json.dump(payload, f, indent=2)
        self.signature_path = out_path
        self.signature = payload
        return out_path

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload
        if self.signature is None:
            if not self.signature_path:
                app = (workload or {}).get("workload", "")
                matches = sorted(glob.glob(os.path.join(PROFILES_DIR, f"hass_{app}_*.json")))
                if matches:
                    self.signature_path = matches[-1]
            if not self.signature_path or not os.path.exists(self.signature_path):
                raise FileNotFoundError(
                    "hass-like requires an offline tuning signature in results/profiles/hass_*.json. "
                    "Build one on a 'tuning' split variant first."
                )
            with open(self.signature_path, "r") as f:
                self.signature = json.load(f)
        if self.signature.get("split") == "evaluation":
            raise ValueError("REFUSED: hass-like signature was built from an evaluation variant.")

    def launch_cpus(self) -> set:
        if self.signature and self.signature.get("preferred_pool") == "E-Core":
            return set(self.pools["e_cores"])
        return set(self.pools["p_cores"])

    def place(self, pids_or_tids: Iterable[int]) -> None:
        if not self.signature:
            raise RuntimeError("hass-like setup() must be called before place().")
        target = self.p_cores_str if self.signature["preferred_pool"] == "P-Core" else self.e_cores_str
        for pid in pids_or_tids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, target)


class _PacerDaemonPolicy(BasePolicy):
    daemon_policy_flag: str = "speedup"

    def __init__(
        self,
        interval_ms: int = 150,
        cooldown_ms: int = 400,
        hysteresis_ticks: int = 1,
        probe_window_ticks: int = 2,
        discard_ticks: int = 1,
        probe_rate_cap: float = 0.5,
        decay_alpha: float = 0.5,
        trial_index: int = 0,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.interval_ms = interval_ms
        self.cooldown_ms = cooldown_ms
        self.hysteresis_ticks = hysteresis_ticks
        self.probe_window_ticks = probe_window_ticks
        self.discard_ticks = discard_ticks
        self.probe_rate_cap = probe_rate_cap
        self.decay_alpha = decay_alpha
        self.trial_index = trial_index
        self.daemon_proc: Optional[subprocess.Popen] = None
        self.daemon_pid: Optional[int] = None
        self.daemon_cpu_time_sec: Optional[float] = None
        self.migrations: int = 0

    def setup(self, workload: Optional[dict] = None) -> None:
        self.workload = workload
        if not os.path.exists(DAEMON_BIN):
            raise FileNotFoundError(f"{DAEMON_BIN} not found. Run 'make all' first.")
        self._base_cmd = [
            DAEMON_BIN,
            "--policy", self.daemon_policy_flag,
            "--interval", str(self.interval_ms),
            "--cooldown", str(self.cooldown_ms),
            "--hysteresis", str(self.hysteresis_ticks),
            "--probe-window", str(self.probe_window_ticks),
            "--discard-ticks", str(self.discard_ticks),
            "--probe-rate-cap", str(self.probe_rate_cap),
            "--decay-alpha", str(self.decay_alpha),
            "--trial", str(self.trial_index),
        ]

    def _start_daemon(self, target_pid: Optional[int]) -> None:
        if self.daemon_proc is not None:
            return
        cmd = list(self._base_cmd)
        if target_pid is not None:
            cmd.extend(["--pid", str(target_pid)])
        ctrl_cpus = set(self.pools["control_plane_cpus"])

        def _preexec() -> None:
            os.sched_setaffinity(0, ctrl_cpus)

        self.daemon_proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            preexec_fn=_preexec,
        )
        self.daemon_pid = self.daemon_proc.pid
        time.sleep(0.05)
        if self.daemon_proc.poll() is None:
            try:
                with open(f"/proc/{self.daemon_pid}/comm", "r") as cf:
                    comm = cf.read().strip()
                exe = os.readlink(f"/proc/{self.daemon_pid}/exe")
                if "hybridsched" not in comm or not exe.endswith("hybridsched_daemon"):
                    raise RuntimeError(
                        f"Daemon PID {self.daemon_pid} verification failed: comm={comm}, exe={exe}"
                    )
            except OSError as exc:
                raise RuntimeError(f"Could not verify daemon PID {self.daemon_pid}: {exc}")

    def place(self, pids_or_tids: Iterable[int]) -> None:
        pids = list(pids_or_tids)
        for pid in pids:
            self.placed_pids.append(pid)
            _taskset_pid_all_tids(pid, self.allowed_cores_str)
        if pids:
            self._start_daemon(pids[0])

    def teardown(self) -> Dict[str, object]:
        info = super().teardown()
        if self.daemon_proc is None and hasattr(self, "_base_cmd"):
            self._start_daemon(None)
        info["daemon_pid"] = self.daemon_pid
        if self.daemon_proc:
            self.daemon_cpu_time_sec = _read_proc_cpu_time_sec(self.daemon_proc.pid)
            try:
                self.daemon_proc.send_signal(signal.SIGINT)
                stdout, _ = self.daemon_proc.communicate(timeout=3.0)
                for line in stdout.splitlines():
                    if "MIGRATION #" in line:
                        self.migrations += 1
            except Exception:
                self.daemon_proc.kill()
        info["daemon_cpu_time_sec"] = self.daemon_cpu_time_sec
        info["migrations"] = self.migrations
        return info


class PacerMpkiPolicy(_PacerDaemonPolicy):
    name = "pacer-mpki"
    label = "Pacer daemon with MPKI-guard ablation policy (--policy mpki)"
    is_deployable = True
    daemon_policy_flag = "mpki"


class PacerSpeedupPolicy(_PacerDaemonPolicy):
    name = "pacer-speedup"
    label = "Pacer daemon with measured-speedup policy (--policy speedup)"
    is_deployable = True
    daemon_policy_flag = "speedup"


POLICY_REGISTRY: Dict[str, Type[BasePolicy]] = {
    "default-linux": DefaultLinuxPolicy,
    "static-P": StaticPPolicy,
    "static-E": StaticEPolicy,
    "oracle-static": OracleStaticPolicy,
    "pie-like": PieLikePolicy,
    "pie-like-topdown": PieLikeTopdownPolicy,
    "hass-like": HassLikePolicy,
    "pacer-mpki": PacerMpkiPolicy,
    "pacer-speedup": PacerSpeedupPolicy,
}


def get_policy(name: str, **kwargs) -> BasePolicy:
    if name not in POLICY_REGISTRY:
        raise KeyError(
            f"Unknown policy '{name}'. Available: {sorted(POLICY_REGISTRY.keys())}"
        )
    return POLICY_REGISTRY[name](**kwargs)

#!/usr/bin/env python3
"""
Statistically Sound Evaluation Harness for Pacer (Prompt 4).

Replaces harness/evaluate_scheduler.py.
- Driven by config/experiment.yaml (policies x workloads x trials).
- Pins harness, daemon, and load-generating clients strictly to control_plane_cpus.
- Enforces env_check at start & end of session (detecting frequency/governor/turbo drift).
- Measures start & end idle package power; reports raw and idle-subtracted energy & EDP.
- Randomizes policy order within each trial block (seeded RNG, seed recorded).
- Runs one warm-up per (policy, workload) tagged warmup=true.
- Cools down between runs using real x86_pkg_temp sensor.
- Stores immutable session artifacts in results/raw/<session_id>/ (or results/smoke/<session_id>/).
"""

import argparse
import csv
import datetime
import json
import math
import os
import platform
import random
import shutil
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from energy_meter import EnergyMeter, RaplEnergyMeter
from policies import POLICY_REGISTRY, PieLikePolicy, get_policy
from pool_config import load_pools_config
from workloads import ALL_WORKLOADS
from workloads.base import assert_split_allows, load_workload_split


def _read_stripped(path: str) -> str:
    with open(path, "r") as f:
        return f.read().strip()


def find_pkg_temp_path() -> Optional[str]:
    base = "/sys/class/thermal"
    if not os.path.isdir(base):
        return None
    for entry in sorted(os.listdir(base)):
        if not entry.startswith("thermal_zone"):
            continue
        t_path = os.path.join(base, entry, "type")
        temp_path = os.path.join(base, entry, "temp")
        try:
            if "x86_pkg_temp" in _read_stripped(t_path) and os.access(temp_path, os.R_OK):
                return temp_path
        except OSError:
            continue
    return None


def read_pkg_temp_c(temp_path: Optional[str]) -> Optional[float]:
    if not temp_path:
        return None
    try:
        return int(_read_stripped(temp_path)) / 1000.0
    except (OSError, ValueError):
        return None


def sample_per_core_freqs() -> str:
    freqs = []
    cpu = 0
    while True:
        p = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq"
        if not os.path.exists(p):
            break
        try:
            freqs.append(_read_stripped(p))
        except OSError:
            freqs.append("?")
        cpu += 1
    return ";".join(freqs) if freqs else "unavailable"


def get_git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return "unknown"


def get_cpu_model() -> str:
    try:
        with open("/proc/cpuinfo", "r") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def run_env_check(out_json: str) -> dict:
    proc = subprocess.run(
        [sys.executable, "harness/env_check.py", "--out", out_json],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if not os.path.exists(out_json):
        raise RuntimeError(f"env_check.py did not produce {out_json}:\n{proc.stderr}")
    with open(out_json, "r") as f:
        data = json.load(f)
    if proc.returncode != 0 or not data.get("valid", False):
        raise RuntimeError(
            f"Precondition failed: env_check.py returned rc={proc.returncode}, "
            f"errors={data.get('errors')}"
        )
    if not data.get("is_hybrid_pmu") and not data.get("measured_freq_under_load", {}).get("clamp_verified"):
        raise RuntimeError("Precondition failed: homogeneous host must have clamp_verified=true.")
    return data


def check_pie_like_validated() -> Tuple[bool, str]:
    """pie-like runs only if all 4 raw counters open cleanly via perf_event_open."""
    try:
        probe = PieLikePolicy.probe_openable_events()
        failed = [k for k, v in probe.items() if not v["openable"]]
        if failed:
            return False, f"pie-like excluded: counters failed perf_event_open: {failed}"
        return True, "all 4 pie-like raw counters openable via perf_event_open"
    except Exception as exc:
        return False, f"pie-like excluded: {exc}"


def measure_idle_power(meter: EnergyMeter, window_sec: float) -> float:
    s0 = meter.read_snapshot()
    t0 = time.perf_counter()
    time.sleep(window_sec)
    t1 = time.perf_counter()
    s1 = meter.read_snapshot()
    delta = meter.compute_delta(s0, s1)
    return delta.pkg_joules / max(t1 - t0, 1e-6)


def cooldown_wait(temp_path: Optional[str], baseline_temp_c: Optional[float],
                  delta_c: float, max_wait_sec: float) -> Tuple[float, Optional[float], bool]:
    if not temp_path or baseline_temp_c is None:
        return 0.0, None, False
    t0 = time.perf_counter()
    while True:
        cur = read_pkg_temp_c(temp_path)
        elapsed = time.perf_counter() - t0
        if cur is not None and cur <= baseline_temp_c + delta_c:
            return elapsed, cur, False
        if elapsed >= max_wait_sec:
            return elapsed, cur, True
        time.sleep(0.5)


def try_drop_caches() -> bool:
    """Attempt to drop page cache only if writable without privilege escalation."""
    p = "/proc/sys/vm/drop_caches"
    if os.path.exists(p) and os.access(p, os.W_OK):
        try:
            with open(p, "w") as f:
                f.write("3\n")
            return True
        except OSError:
            return False
    return False


def get_actual_cpus_of_pids(pids: List[int]) -> str:
    cpus = set()
    for pid in pids:
        task_dir = f"/proc/{pid}/task"
        try:
            tids = os.listdir(task_dir)
        except OSError:
            continue
        for tid in tids:
            try:
                with open(f"{task_dir}/{tid}/stat", "r") as f:
                    line = f.read()
                rp = line.rfind(")")
                if rp >= 0:
                    fields = line[rp + 2 :].split()
                    cpus.add(int(fields[36]))  # field 39 (0-idx 36 after ')')
            except (OSError, IndexError, ValueError):
                pass
    return ";".join(str(c) for c in sorted(cpus)) if cpus else "unknown"


def create_session_dir(session_id: str, session_type: str, smoke: bool) -> str:
    base = os.path.join("results", "smoke" if smoke else "raw")
    session_dir = os.path.join(base, session_id)
    if os.path.exists(session_dir):
        raise FileExistsError(
            f"REFUSED: session directory already exists: {session_dir}. "
            f"Never overwrite or append across sessions."
        )
    os.makedirs(session_dir, exist_ok=False)
    for sub in ("probe_logs", "daemon_logs", "stdout_stderr"):
        os.makedirs(os.path.join(session_dir, sub), exist_ok=True)
    return session_dir


def run_session(
    cfg: dict,
    session_id: str,
    session_type: str,
    smoke: bool = False,
    debug_synthetic: bool = False,
) -> str:
    pools = load_pools_config()
    control_cpus = set(pools["control_plane_cpus"])
    # Pin harness process itself strictly to control-plane CPUs
    os.sched_setaffinity(0, control_cpus)

    session_dir = create_session_dir(session_id, session_type, smoke)

    # 1. Start-of-session env_check
    env_start = run_env_check(os.path.join(session_dir, "env_start.json"))

    # 2. Energy meter (RAPL only unless --debug-synthetic)
    meter: Optional[EnergyMeter] = None
    if not debug_synthetic:
        meter = RaplEnergyMeter()

    # 3. Validate workload variants against config/workload_split.yaml
    variants_all = load_workload_split()
    wl_keys = list(cfg["workloads"])
    if smoke:
        # Use tuning-split bench_compute_tune for fast <2min smoke session
        wl_keys = ["bench_compute_tune"] if "bench_compute_tune" in variants_all else [wl_keys[0]]
    for wk in wl_keys:
        if wk not in variants_all:
            raise KeyError(f"Unknown workload variant '{wk}' in config/workload_split.yaml")
        if session_type == "tuning":
            assert_split_allows(wk, "tuning")

    # 4. Check pie-like counter validation
    policies_list = list(cfg["policies"])
    pie_ok, pie_msg = check_pie_like_validated()
    if "pie-like" in policies_list and not pie_ok:
        print(f"[WARN] {pie_msg} — excluding pie-like from session.")
        policies_list = [p for p in policies_list if p != "pie-like"]

    # 5. Idle baseline at session start
    idle_win = 1.0 if smoke else float(cfg.get("idle_window_sec", 5.0))
    idle_power_start = measure_idle_power(meter, idle_win) if meter else float("nan")
    temp_path = find_pkg_temp_path()
    idle_temp_start = read_pkg_temp_c(temp_path)

    trials = 1 if smoke else int(cfg.get("trials", 10))
    seed = int(cfg.get("random_seed", 20261126))
    rng = random.Random(seed)
    git_commit = get_git_commit()
    cpu_model = get_cpu_model()
    kernel_ver = platform.release()
    daemon_cfg = cfg.get("daemon", {})

    runs_csv_path = os.path.join(session_dir, "runs.csv")
    fieldnames = [
        "git_commit", "kernel_version", "cpu_model", "governor", "turbo_state",
        "per_core_freq_khz", "p_cores", "e_cores", "control_plane_cpus",
        "excluded_siblings", "rapl_source", "trial_index", "timestamp_iso",
        "session_id", "session_type", "synthetic", "warmup", "session_valid",
        "workload_variant", "workload", "split", "policy",
        "wall_time_sec", "throughput_metric", "throughput_unit",
        "energy_pkg_j", "energy_core_j", "energy_dram_j",
        "idle_power_start_w", "idle_subtracted_energy_j",
        "edp_raw", "edp_idle_subtracted",
        "migrations", "probe_count", "daemon_pid", "daemon_cpu_time_sec",
        "exit_status", "actual_cpus_used",
        "cooldown_wait_sec", "pkg_temp_c", "cooldown_max_wait_hit",
    ]

    rows: List[dict] = []

    def execute_one(wk_key: str, pol_name: str, trial_idx: int, is_warmup: bool) -> dict:
        v_cfg = dict(variants_all[wk_key])
        v_cfg["variant"] = wk_key
        wl_name = v_cfg["workload"]
        wl = ALL_WORKLOADS[wl_name]()

        try_drop_caches()

        # Cooldown before non-first runs
        cd_wait, pkg_t, cd_hit = cooldown_wait(
            temp_path,
            idle_temp_start,
            float(cfg.get("cooldown_temp_delta_c", 2.0)),
            0.5 if smoke else float(cfg.get("cooldown_max_wait_sec", 30.0)),
        )

        pol_kwargs = dict(daemon_cfg)
        if smoke and pol_name == "oracle-static":
            smoke_prof = os.path.join(session_dir, "smoke_oracle_profile.json")
            if not os.path.exists(smoke_prof):
                with open(smoke_prof, "w") as spf:
                    json.dump({"best_pool_by_edp": "P-Core", "split": "tuning"}, spf)
            pol_kwargs["profile_path"] = smoke_prof
        elif smoke and pol_name == "hass-like":
            smoke_sig = os.path.join(session_dir, "smoke_hass_sig.json")
            if not os.path.exists(smoke_sig):
                with open(smoke_sig, "w") as ssf:
                    json.dump({"preferred_pool": "P-Core", "split": "tuning"}, ssf)
            pol_kwargs["signature_path"] = smoke_sig

        pol = get_policy(pol_name, trial_index=trial_idx, **pol_kwargs)
        pol.setup(v_cfg)
        if getattr(pol, "daemon_pid", None):
            os.sched_setaffinity(pol.daemon_pid, control_cpus)

        run_tag = f"{wk_key}__{pol_name}__t{trial_idx}{'_warmup' if is_warmup else ''}"
        run_log_dir = os.path.join(session_dir, "stdout_stderr", run_tag)

        snap0 = meter.read_snapshot() if meter else None
        captured_cpus: List[str] = []

        def place_and_record(pids: List[int]) -> None:
            pol.place(pids)
            time.sleep(0.05)
            captured_cpus.append(get_actual_cpus_of_pids(pids))

        res = wl.launch(run_log_dir, variant_cfg=v_cfg, place_fn=place_and_record)
        freqs_during = sample_per_core_freqs()
        snap1 = meter.read_snapshot() if meter else None
        td = pol.teardown()

        # Move any daemon probe/telemetry logs into session dir
        probe_cnt = 0
        if os.path.exists("results/probe_log.csv"):
            dst_probe = os.path.join(session_dir, "probe_logs", f"{run_tag}_probe.csv")
            shutil.copy2("results/probe_log.csv", dst_probe)
            with open(dst_probe, "r") as pf:
                probe_cnt = max(0, sum(1 for _ in pf) - 1)
        if os.path.exists("results/daemon_telemetry.csv"):
            dst_tel = os.path.join(session_dir, "daemon_logs", f"{run_tag}_telemetry.csv")
            shutil.copy2("results/daemon_telemetry.csv", dst_tel)

        if meter and snap0 and snap1:
            ed = meter.compute_delta(snap0, snap1)
            e_pkg = ed.pkg_joules
            e_core = ed.core_joules
            e_dram = ed.dram_joules
            e_sub = max(0.0, e_pkg - idle_power_start * res.wall_time_sec)
            edp_raw = e_pkg * res.wall_time_sec
            edp_sub = e_sub * res.wall_time_sec
            rapl_src = ed.source
        else:
            e_pkg = e_core = e_dram = e_sub = edp_raw = edp_sub = float("nan")
            rapl_src = "synthetic"

        return {
            "git_commit": git_commit,
            "kernel_version": kernel_ver,
            "cpu_model": cpu_model,
            "governor": env_start["governor"],
            "turbo_state": env_start["turbo_state"],
            "per_core_freq_khz": freqs_during,
            "p_cores": pools["p_cores_str"],
            "e_cores": pools["e_cores_str"],
            "control_plane_cpus": pools["control_plane_cpus_str"],
            "excluded_siblings": pools["excluded_siblings_str"],
            "rapl_source": rapl_src,
            "trial_index": trial_idx,
            "timestamp_iso": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
            "session_id": session_id,
            "session_type": "smoke" if smoke else session_type,
            "synthetic": str(debug_synthetic).lower(),
            "warmup": str(is_warmup).lower(),
            "session_valid": "true",
            "workload_variant": wk_key,
            "workload": wl_name,
            "split": v_cfg.get("split", "unknown"),
            "policy": pol_name,
            "wall_time_sec": res.wall_time_sec,
            "throughput_metric": res.throughput_metric if res.throughput_metric is not None else "",
            "throughput_unit": res.throughput_unit,
            "energy_pkg_j": e_pkg,
            "energy_core_j": e_core if e_core is not None else "",
            "energy_dram_j": e_dram if e_dram is not None else "",
            "idle_power_start_w": idle_power_start,
            "idle_subtracted_energy_j": e_sub,
            "edp_raw": edp_raw,
            "edp_idle_subtracted": edp_sub,
            "migrations": td.get("migrations", 0),
            "probe_count": probe_cnt,
            "daemon_pid": td.get("daemon_pid", ""),
            "daemon_cpu_time_sec": td.get("daemon_cpu_time_sec", ""),
            "exit_status": res.exit_status,
            "actual_cpus_used": captured_cpus[0] if captured_cpus else "unknown",
            "cooldown_wait_sec": round(cd_wait, 3),
            "pkg_temp_c": pkg_t if pkg_t is not None else "",
            "cooldown_max_wait_hit": str(cd_hit).lower(),
        }

    # Warm-up runs (1 per policy x workload)
    for wk_key in wl_keys:
        for pol_name in policies_list:
            rows.append(execute_one(wk_key, pol_name, 0, is_warmup=True))

    # Randomized trial blocks
    for t_idx in range(1, trials + 1):
        for wk_key in wl_keys:
            order = list(policies_list)
            rng.shuffle(order)
            for pol_name in order:
                rows.append(execute_one(wk_key, pol_name, t_idx, is_warmup=False))

    # End-of-session idle baseline & env_check drift verification
    idle_power_end = measure_idle_power(meter, idle_win) if meter else float("nan")
    env_end = run_env_check(os.path.join(session_dir, "env_end.json"))

    drift_reasons = []
    if env_end["governor"] != env_start["governor"]:
        drift_reasons.append(f"governor drifted {env_start['governor']} -> {env_end['governor']}")
    if env_end["turbo_state"] != env_start["turbo_state"]:
        drift_reasons.append(f"turbo drifted {env_start['turbo_state']} -> {env_end['turbo_state']}")
    if env_end["scaling_max_freq_khz"] != env_start["scaling_max_freq_khz"]:
        drift_reasons.append("scaling_max_freq_khz drifted between start and end")

    drift_thresh = float(cfg.get("idle_drift_threshold_pct", 10.0))
    if not math.isnan(idle_power_start) and idle_power_start > 0:
        idle_drift_pct = abs(idle_power_end - idle_power_start) / idle_power_start * 100.0
        if idle_drift_pct > drift_thresh:
            drift_reasons.append(
                f"idle power drifted {idle_drift_pct:.1f}% ({idle_power_start:.2f}W -> {idle_power_end:.2f}W)"
            )

    session_valid = len(drift_reasons) == 0
    for r in rows:
        r["session_valid"] = str(session_valid).lower()

    with open(runs_csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    session_meta = {
        "session_id": session_id,
        "session_type": "smoke" if smoke else session_type,
        "random_seed": seed,
        "trials": trials,
        "idle_power_start_w": idle_power_start,
        "idle_power_end_w": idle_power_end,
        "idle_temp_start_c": idle_temp_start,
        "session_valid": session_valid,
        "drift_reasons": drift_reasons,
        "policies": policies_list,
        "workloads": wl_keys,
    }
    with open(os.path.join(session_dir, "session_meta.json"), "w") as f:
        json.dump(session_meta, f, indent=2)

    print(f"[+] Session complete: {runs_csv_path} (rows={len(rows)}, valid={session_valid})")
    return runs_csv_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Pacer Statistically Sound Evaluation Harness")
    parser.add_argument("--config", default="config/experiment.yaml", help="Experiment YAML config")
    parser.add_argument("--session-id", default=None, help="Unique session ID")
    parser.add_argument("--session-type", choices=["tuning", "evaluation"], default=None)
    parser.add_argument("--smoke", action="store_true", help="Run fast smoke session into results/smoke/")
    parser.add_argument("--debug-synthetic", action="store_true", help="Allow running without RAPL (tags synthetic=true)")
    args = parser.parse_args()

    with open(args.config, "r") as f:
        cfg = yaml.safe_load(f) or {}

    session_id = args.session_id or datetime.datetime.utcnow().strftime("session_%Y%m%dT%H%M%SZ")
    session_type = args.session_type or cfg.get("session_type", "evaluation")
    run_session(cfg, session_id=session_id, session_type=session_type,
                smoke=args.smoke, debug_synthetic=args.debug_synthetic)
    return 0


if __name__ == "__main__":
    sys.exit(main())

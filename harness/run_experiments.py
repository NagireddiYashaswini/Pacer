#!/usr/bin/env python3
"""
Statistically Sound Evaluation Harness for Pacer (Prompt 4).

- Driven by config/experiment.yaml (policies x workloads x trials).
- Pins harness, daemon, and load-generating clients strictly to control_plane_cpus
  from config/pools.yaml and asserts it at startup.
- Enforces env_check at start & end of session (detecting frequency/governor/turbo drift).
- Requires validated-counter record (results/pie_counter_validation.json) for pie-like.
- Measures start & end idle package power; reports raw and idle-subtracted energy & EDP.
- Samples per-CPU frequencies AND actual_cpus_used continuously during each run.
- Hard check: any workload thread observed on control-plane or excluded CPUs marks
  the row invalid (row_valid=false).
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
from typing import Dict, List, Optional, Set, Tuple

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from energy_meter import EnergyMeter, RaplEnergyMeter
from policies import get_policy
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


def read_target_cpus_freqs(target_cpus: List[int]) -> Dict[int, float]:
    out: Dict[int, float] = {}
    for c in target_cpus:
        p = f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq"
        val = int(_read_stripped(p))
        out[c] = float(val)
    return out


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


def check_pie_like_validated(
    validation_path: str = "results/pie_counter_validation.json",
) -> Tuple[bool, str]:
    """pie-like runs only if a validated-counter record exists for this kernel."""
    if not os.path.exists(validation_path):
        return False, f"pie-like excluded: validated-counter record missing at {validation_path}"
    try:
        with open(validation_path, "r") as f:
            rec = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        return False, f"pie-like excluded: unreadable {validation_path}: {exc}"

    if not rec.get("validated", False):
        return False, f"pie-like excluded: {validation_path} has validated=false"
    if rec.get("kernel_version") != platform.release():
        return False, (
            f"pie-like excluded: {validation_path} kernel_version={rec.get('kernel_version')} "
            f"!= current {platform.release()}"
        )
    return True, f"validated-counter record OK ({validation_path})"


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
    p = "/proc/sys/vm/drop_caches"
    if os.path.exists(p) and os.access(p, os.W_OK):
        try:
            with open(p, "w") as f:
                f.write("3\n")
            return True
        except OSError:
            return False
    return False


def sample_pids_cpus(pids: List[int]) -> Set[int]:
    cpus: Set[int] = set()
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
                    cpus.add(int(fields[36]))
            except (OSError, IndexError, ValueError):
                pass
    return cpus


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
    p_set = set(pools["p_cores"])
    e_set = set(pools["e_cores"])
    control_cpus = set(pools["control_plane_cpus"])
    excluded_cpus = set(pools["excluded_siblings"])
    monitored_cpus = sorted(p_set | e_set | control_cpus)

    # Pin harness process itself strictly to control-plane CPUs and assert it
    os.sched_setaffinity(0, control_cpus)
    actual_self_aff = os.sched_getaffinity(0)
    if actual_self_aff != control_cpus:
        raise RuntimeError(
            f"Control-plane confinement assertion failed: "
            f"expected={sorted(control_cpus)}, actual={sorted(actual_self_aff)}"
        )

    session_dir = create_session_dir(session_id, session_type, smoke)

    # 1. Start-of-session env_check
    env_start = run_env_check(os.path.join(session_dir, "env_start.json"))

    # 2. Energy meter (RAPL only unless --debug-synthetic)
    meter: Optional[RaplEnergyMeter] = None
    if not debug_synthetic:
        meter = RaplEnergyMeter()
        rapl_domains_status = (
            f"pkg={meter.pkg_status}; core={meter.core_status}; dram={meter.dram_status}"
        )
        print(f"[INFO] RAPL domains: {rapl_domains_status}")
    else:
        rapl_domains_status = "synthetic=true"

    # 3. Validate workload variants against config/workload_split.yaml
    variants_all = load_workload_split()
    wl_keys = list(cfg["workloads"])
    if smoke:
        wl_keys = ["bench_compute_smoke"]
        variants_all["bench_compute_smoke"] = {
            "workload": "bench_compute",
            "split": "tuning",
            "args": ["--iterations", "0", "--duration", "5.0", "--matrix-size", "128"],
        }
    else:
        for wk in wl_keys:
            if wk not in variants_all:
                raise KeyError(f"Unknown workload variant '{wk}' in config/workload_split.yaml")
            if session_type == "tuning":
                assert_split_allows(wk, "tuning")

    # 4. Check pie-like validated-counter record gate
    policies_list = list(cfg["policies"])
    pie_ok, pie_msg = check_pie_like_validated()
    print(f"[INFO] pie-like gate: {pie_msg}")
    if "pie-like" in policies_list and not pie_ok:
        print(f"[WARN] Excluding pie-like from session: {pie_msg}")
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
        "excluded_siblings", "rapl_source", "rapl_domains_status",
        "trial_index", "timestamp_iso",
        "session_id", "session_type", "synthetic", "warmup",
        "row_valid", "violation_reason", "session_valid", "env_recheck_passed",
        "workload_variant", "workload", "split", "policy",
        "wall_time_sec", "throughput_metric", "throughput_unit",
        "energy_pkg_j", "energy_core_j", "energy_dram_j",
        "idle_power_start_w", "idle_power_end_w", "idle_drift_pct", "idle_drift_flagged",
        "idle_subtracted_energy_j", "edp_raw", "edp_idle_subtracted",
        "migrations", "probe_count", "daemon_pid", "daemon_cpu_time_sec",
        "exit_status", "actual_cpus_used", "used_cpus_mean_freq_khz",
        "cooldown_wait_sec", "pkg_temp_c", "cooldown_max_wait_hit",
    ]

    rows: List[dict] = []

    def execute_one(wk_key: str, pol_name: str, trial_idx: int, is_warmup: bool) -> dict:
        v_cfg = dict(variants_all[wk_key])
        v_cfg["variant"] = wk_key
        wl_name = v_cfg["workload"]
        wl = ALL_WORKLOADS[wl_name]()

        try_drop_caches()

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
        launch_set = pol.launch_cpus()

        run_tag = f"{wk_key}__{pol_name}__t{trial_idx}{'_warmup' if is_warmup else ''}"
        run_log_dir = os.path.join(session_dir, "stdout_stderr", run_tag)

        freq_samples: Dict[int, List[float]] = {c: [] for c in monitored_cpus}
        observed_cpus: Set[int] = set()

        def sample_during_run(pids: List[int]) -> None:
            observed_cpus.update(sample_pids_cpus(pids))
            try:
                cur_f = read_target_cpus_freqs(monitored_cpus)
                for c, fval in cur_f.items():
                    freq_samples[c].append(fval)
            except OSError:
                pass
            pol.on_tick()

        snap0 = meter.read_snapshot() if meter else None
        res = wl.launch(
            run_log_dir,
            variant_cfg=v_cfg,
            place_fn=pol.place,
            launch_cpus=launch_set,
            sample_fn=sample_during_run,
        )
        snap1 = meter.read_snapshot() if meter else None
        td = pol.teardown()

        # Check all monitored CPUs had real frequency samples during the run
        row_valid = True
        violations: List[str] = []
        freq_pairs: List[str] = []
        mean_by_cpu: Dict[int, float] = {}
        for c in monitored_cpus:
            if not freq_samples[c]:
                row_valid = False
                violations.append(f"MISSING_FREQ_CPU_{c}")
            else:
                m = sum(freq_samples[c]) / len(freq_samples[c])
                mean_by_cpu[c] = m
                freq_pairs.append(f"{c}:{m:.0f}")
        if not row_valid and not freq_pairs:
            raise RuntimeError(f"Failed to sample per-CPU frequencies during run {run_tag}")
        freqs_during_str = ";".join(freq_pairs)

        # Hard check: workload thread observed on control-plane or excluded CPU
        bad_cpus = sorted(observed_cpus & (control_cpus | excluded_cpus))
        if bad_cpus:
            row_valid = False
            violations.append(f"WORKLOAD_ON_FORBIDDEN_CPU:{bad_cpus}")
            print(f"[VIOLATION] {run_tag}: workload observed on forbidden CPUs {bad_cpus}")

        # Daemon accounting check for pacer policies
        d_pid = td.get("daemon_pid")
        d_cpu_t = td.get("daemon_cpu_time_sec")
        if pol_name.startswith("pacer-"):
            if not d_pid or d_cpu_t is None or (res.wall_time_sec >= 1.0 and float(d_cpu_t) <= 0.0):
                row_valid = False
                violations.append(f"DAEMON_ACCOUNTING_ERROR(pid={d_pid},cpu_time={d_cpu_t})")

        used_cpus_sorted = sorted(observed_cpus)
        used_cpus_str = ";".join(str(c) for c in used_cpus_sorted) if used_cpus_sorted else "none"
        used_freqs = [mean_by_cpu[c] for c in used_cpus_sorted if c in mean_by_cpu]
        used_mean_freq = round(sum(used_freqs) / len(used_freqs), 1) if used_freqs else 0.0

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
            e_core = ed.core_joules if ed.core_joules is not None else meter.core_status
            e_dram = ed.dram_joules if ed.dram_joules is not None else meter.dram_status
            e_sub = max(0.0, e_pkg - idle_power_start * res.wall_time_sec)
            edp_raw = e_pkg * res.wall_time_sec
            edp_sub = e_sub * res.wall_time_sec
            rapl_src = ed.source
        else:
            e_pkg = e_sub = edp_raw = edp_sub = float("nan")
            e_core = e_dram = "NA (synthetic)"
            rapl_src = "synthetic"

        return {
            "git_commit": git_commit,
            "kernel_version": kernel_ver,
            "cpu_model": cpu_model,
            "governor": env_start["governor"],
            "turbo_state": env_start["turbo_state"],
            "per_core_freq_khz": freqs_during_str,
            "p_cores": pools["p_cores_str"],
            "e_cores": pools["e_cores_str"],
            "control_plane_cpus": pools["control_plane_cpus_str"],
            "excluded_siblings": pools["excluded_siblings_str"],
            "rapl_source": rapl_src,
            "rapl_domains_status": rapl_domains_status,
            "trial_index": trial_idx,
            "timestamp_iso": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "session_id": session_id,
            "session_type": "smoke" if smoke else session_type,
            "synthetic": str(debug_synthetic).lower(),
            "warmup": str(is_warmup).lower(),
            "row_valid": str(row_valid).lower(),
            "violation_reason": ";".join(violations) if violations else "none",
            "session_valid": "true",
            "env_recheck_passed": "true",
            "workload_variant": wk_key,
            "workload": wl_name,
            "split": v_cfg.get("split", "unknown"),
            "policy": pol_name,
            "wall_time_sec": round(res.wall_time_sec, 6),
            "throughput_metric": res.throughput_metric if res.throughput_metric is not None else "",
            "throughput_unit": res.throughput_unit,
            "energy_pkg_j": round(e_pkg, 6) if not math.isnan(e_pkg) else "nan",
            "energy_core_j": e_core,
            "energy_dram_j": e_dram,
            "idle_power_start_w": round(idle_power_start, 4) if not math.isnan(idle_power_start) else "nan",
            "idle_power_end_w": "",
            "idle_drift_pct": "",
            "idle_drift_flagged": "false",
            "idle_subtracted_energy_j": round(e_sub, 6) if not math.isnan(e_sub) else "nan",
            "edp_raw": round(edp_raw, 6) if not math.isnan(edp_raw) else "nan",
            "edp_idle_subtracted": round(edp_sub, 6) if not math.isnan(edp_sub) else "nan",
            "migrations": td.get("migrations", 0),
            "probe_count": probe_cnt,
            "daemon_pid": d_pid if d_pid is not None else "",
            "daemon_cpu_time_sec": d_cpu_t if d_cpu_t is not None else "",
            "exit_status": res.exit_status,
            "actual_cpus_used": used_cpus_str,
            "used_cpus_mean_freq_khz": used_mean_freq,
            "cooldown_wait_sec": round(cd_wait, 3),
            "pkg_temp_c": pkg_t if pkg_t is not None else "",
            "cooldown_max_wait_hit": str(cd_hit).lower(),
        }

    # In smoke mode, run 1 trial per policy without extra 5s warmups (keeps total < ~50s);
    # in full sessions, run 1 warmup + >=10 randomized trial blocks.
    if not smoke:
        for wk_key in wl_keys:
            for pol_name in policies_list:
                rows.append(execute_one(wk_key, pol_name, 0, is_warmup=True))

    for t_idx in range(1, trials + 1):
        for wk_key in wl_keys:
            order = list(policies_list)
            rng.shuffle(order)
            for pol_name in order:
                rows.append(execute_one(wk_key, pol_name, t_idx, is_warmup=False))

    # End-of-session idle baseline & env_check drift verification
    idle_power_end = measure_idle_power(meter, idle_win) if meter else float("nan")
    env_recheck_passed = True
    drift_reasons = []
    try:
        env_end = run_env_check(os.path.join(session_dir, "env_end.json"))
        if env_end["governor"] != env_start["governor"]:
            drift_reasons.append(f"governor drifted {env_start['governor']} -> {env_end['governor']}")
        if env_end["turbo_state"] != env_start["turbo_state"]:
            drift_reasons.append(f"turbo drifted {env_start['turbo_state']} -> {env_end['turbo_state']}")
        if env_end["scaling_max_freq_khz"] != env_start["scaling_max_freq_khz"]:
            drift_reasons.append("scaling_max_freq_khz drifted between start and end")
    except RuntimeError as exc:
        env_recheck_passed = False
        drift_reasons.append(f"env_end failed: {exc}")

    drift_thresh = float(cfg.get("idle_drift_threshold_pct", 10.0))
    idle_drift_pct = 0.0
    idle_drift_flagged = False
    if not math.isnan(idle_power_start) and idle_power_start > 0:
        idle_drift_pct = abs(idle_power_end - idle_power_start) / idle_power_start * 100.0
        if idle_drift_pct > drift_thresh:
            idle_drift_flagged = True
            drift_reasons.append(
                f"idle power drifted {idle_drift_pct:.1f}% ({idle_power_start:.2f}W -> {idle_power_end:.2f}W)"
            )

    session_valid = len(drift_reasons) == 0
    for r in rows:
        r["session_valid"] = str(session_valid).lower()
        r["env_recheck_passed"] = str(env_recheck_passed).lower()
        r["idle_power_end_w"] = round(idle_power_end, 4) if not math.isnan(idle_power_end) else "nan"
        r["idle_drift_pct"] = round(idle_drift_pct, 2)
        r["idle_drift_flagged"] = str(idle_drift_flagged).lower()

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
        "idle_drift_pct": idle_drift_pct,
        "idle_drift_flagged": idle_drift_flagged,
        "idle_temp_start_c": idle_temp_start,
        "env_recheck_passed": env_recheck_passed,
        "session_valid": session_valid,
        "drift_reasons": drift_reasons,
        "rapl_domains_status": rapl_domains_status,
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

    session_id = args.session_id or datetime.datetime.now(datetime.timezone.utc).strftime("session_%Y%m%dT%H%M%SZ")
    session_type = args.session_type or cfg.get("session_type", "evaluation")
    run_session(cfg, session_id=session_id, session_type=session_type,
                smoke=args.smoke, debug_synthetic=args.debug_synthetic)
    return 0


if __name__ == "__main__":
    sys.exit(main())

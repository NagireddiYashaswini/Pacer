#!/usr/bin/env bash
# ==============================================================================
# Setup Environment & Emulated E-Core Frequency Clamping for Hybrid Scheduling
# Target CPU: Intel Xeon w5-2565X (or any homogeneous x86_64 host)
# ==============================================================================

set -euo pipefail

E_CORE_CPUS=(8 9 10 11 12 13 14 15 16 17 26 27 28 29 30 31 32 33 34 35)
P_CORE_CPUS=(0 1 2 3 4 5 6 7 18 19 20 21 22 23 24 25)

E_CORE_FREQ_KHZ="2000000" # 2.0 GHz
P_CORE_FREQ_KHZ="4600000" # 4.6 GHz Max

usage() {
    echo "Usage: $0 {enable-emulation|restore-frequencies|setup-perf|status}"
    echo "  enable-emulation    : Clamp E-cores to 2.0 GHz and P-cores to max"
    echo "  restore-frequencies : Reset all cores to max frequency ceiling"
    echo "  setup-perf          : Configure perf_event_paranoid and RAPL permissions"
    echo "  status              : Show current core frequencies and perf paranoid"
    exit 1
}

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "[!] Sudo/root privileges required for modifying sysfs / kernel parameters."
        echo "    Run with: sudo $0 $1"
        exit 1
    fi
}

setup_perf() {
    check_root "setup-perf"
    echo "[+] Lowering perf_event_paranoid to 1 (enables unprivileged hardware counters)..."
    sysctl -w kernel.perf_event_paranoid=1

    echo "[+] Permitting read access to Intel RAPL energy counters..."
    if [ -f /sys/class/powercap/intel-rapl:0/energy_uj ]; then
        chmod o+r /sys/class/powercap/intel-rapl:0/energy_uj
        echo "[+] RAPL package energy counter is now readable."
    fi
    if [ -f /sys/class/powercap/intel-rapl:0:0/energy_uj ]; then
        chmod o+r /sys/class/powercap/intel-rapl:0:0/energy_uj
        echo "[+] RAPL core energy counter is now readable."
    fi
}

enable_emulation() {
    check_root "enable-emulation"
    echo "[+] Emulating E-Core topology on homogeneous CPU:"
    echo "    - Clamping E-core pool (CPUs ${E_CORE_CPUS[*]}) to ${E_CORE_FREQ_KHZ} kHz (2.0 GHz)..."
    for cpu in "${E_CORE_CPUS[@]}"; do
        freq_path="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_max_freq"
        if [ -w "$freq_path" ]; then
            echo "$E_CORE_FREQ_KHZ" > "$freq_path"
        fi
    done

    echo "    - Setting P-core pool (CPUs ${P_CORE_CPUS[*]}) to ${P_CORE_FREQ_KHZ} kHz (Max Performance)..."
    for cpu in "${P_CORE_CPUS[@]}"; do
        freq_path="/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_max_freq"
        if [ -w "$freq_path" ]; then
            echo "$P_CORE_FREQ_KHZ" > "$freq_path"
        fi
    done
    echo "[+] Hybrid frequency profile applied successfully."
}

restore_frequencies() {
    check_root "restore-frequencies"
    echo "[+] Restoring all CPUs to maximum scaling frequency ceiling..."
    for cpu_path in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq; do
        if [ -w "$cpu_path" ]; then
            echo "$P_CORE_FREQ_KHZ" > "$cpu_path"
        fi
    done
    echo "[+] All CPU frequencies restored."
}

show_status() {
    echo "================ CPU FREQUENCY & TELEMETRY STATUS ================"
    echo "Kernel perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 'N/A')"
    if [ -r /sys/class/powercap/intel-rapl:0/energy_uj ]; then
        echo "Intel RAPL energy_uj: Readable (Package = $(cat /sys/class/powercap/intel-rapl:0/energy_uj) uJ)"
    else
        echo "Intel RAPL energy_uj: Not readable without root"
    fi
    echo "--- Sample P-Core (CPU 0) ---"
    if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]; then
        echo "Cur Freq: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) kHz"
        echo "Max Freq: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) kHz"
    fi
    echo "--- Sample E-Core (CPU 8) ---"
    if [ -f /sys/devices/system/cpu/cpu8/cpufreq/scaling_cur_freq ]; then
        echo "Cur Freq: $(cat /sys/devices/system/cpu/cpu8/cpufreq/scaling_cur_freq) kHz"
        echo "Max Freq: $(cat /sys/devices/system/cpu/cpu8/cpufreq/scaling_max_freq) kHz"
    fi
    echo "=================================================================="
}

case "${1:-}" in
    enable-emulation)    enable_emulation ;;
    restore-frequencies) restore_frequencies ;;
    setup-perf)          setup_perf ;;
    status)              show_status ;;
    *)                   usage ;;
esac

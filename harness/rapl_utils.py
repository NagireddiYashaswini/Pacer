#!/usr/bin/env python3
"""
Shared RAPL powercap sysfs scanner for the Python harness.
Matches the domain-scanning logic in src/rapl_energy.c.
"""

import os
from typing import Optional, Tuple

POWERCAP_CANDIDATES = [
    "/sys/class/powercap/intel-rapl",
    "/sys/devices/virtual/powercap/intel-rapl",
]


def find_rapl_package_domain() -> Tuple[Optional[str], Optional[str], Optional[str]]:
    """
    Scan powercap sysfs for the Intel RAPL package domain (name contains 'package').
    Returns (domain_name, energy_uj_path, max_energy_range_uj_path) or (None, None, None).
    """
    for root in POWERCAP_CANDIDATES:
        if not os.path.isdir(root):
            continue
        try:
            entries = sorted(os.listdir(root))
        except OSError:
            continue
        for entry in entries:
            if not entry.startswith("intel-rapl:"):
                continue
            dom_dir = os.path.join(root, entry)
            name_file = os.path.join(dom_dir, "name")
            if not os.path.isfile(name_file):
                continue
            try:
                with open(name_file, "r") as f:
                    name = f.read().strip()
            except OSError:
                continue
            if "package" in name:
                energy_path = os.path.join(dom_dir, "energy_uj")
                max_range_path = os.path.join(dom_dir, "max_energy_range_uj")
                return entry, energy_path, (max_range_path if os.path.isfile(max_range_path) else None)
    return None, None, None

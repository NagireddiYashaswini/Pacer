#!/usr/bin/env python3
"""
Single-source-of-truth pool configuration loader for config/pools.yaml.
Fails loudly if config/pools.yaml is missing or malformed.
"""

import os
import sys
from typing import Dict, List

import yaml

DEFAULT_POOLS_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "config",
    "pools.yaml",
)


def parse_cpulist(cpulist_str: str) -> List[int]:
    cpus: List[int] = []
    for part in str(cpulist_str).split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            cpus.extend(range(int(a), int(b) + 1))
        else:
            cpus.append(int(part))
    return sorted(set(cpus))


def load_pools_config(path: str = DEFAULT_POOLS_PATH) -> Dict[str, object]:
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"Pool config not found at {path}. "
            f"Create config/pools.yaml with p_cores, e_cores, excluded_siblings."
        )
    with open(path, "r") as f:
        data = yaml.safe_load(f) or {}

    for key in ("p_cores", "e_cores", "excluded_siblings"):
        if key not in data or not str(data[key]).strip():
            raise ValueError(f"Missing required key '{key}' in {path}")

    p_str = str(data["p_cores"]).strip()
    e_str = str(data["e_cores"]).strip()
    excl_str = str(data["excluded_siblings"]).strip()

    return {
        "p_cores_str": p_str,
        "e_cores_str": e_str,
        "excluded_siblings_str": excl_str,
        "p_cores": parse_cpulist(p_str),
        "e_cores": parse_cpulist(e_str),
        "excluded_siblings": parse_cpulist(excl_str),
    }

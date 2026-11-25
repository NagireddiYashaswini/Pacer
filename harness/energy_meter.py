#!/usr/bin/env python3
"""
Pluggable Energy Meter Interface & Intel RAPL Implementation.

Allows an external power meter to be plugged in later while enforcing
real RAPL measurements (with max_energy_range_uj wraparound) by default.
"""

import abc
import dataclasses
import os
from typing import Optional

from rapl_utils import find_rapl_package_domain


@dataclasses.dataclass
class EnergySnapshot:
    pkg_uj: int
    core_uj: Optional[int] = None
    dram_uj: Optional[int] = None


@dataclasses.dataclass
class EnergyDelta:
    pkg_joules: float
    core_joules: Optional[float]
    dram_joules: Optional[float]
    source: str


def compute_rapl_delta_uj(prev_uj: int, cur_uj: int, max_range_uj: int) -> int:
    """Compute microjoule delta with counter wraparound at max_range_uj."""
    if cur_uj >= prev_uj:
        return cur_uj - prev_uj
    if max_range_uj <= 0 or cur_uj >= max_range_uj or prev_uj > max_range_uj:
        raise ValueError(
            f"RAPL counter wraparound (prev={prev_uj}, cur={cur_uj}) "
            f"cannot be corrected with max_range_uj={max_range_uj}"
        )
    return cur_uj + max_range_uj - prev_uj


class EnergyMeter(abc.ABC):
    source_name: str = "unknown"

    @abc.abstractmethod
    def read_snapshot(self) -> EnergySnapshot:
        ...

    @abc.abstractmethod
    def compute_delta(self, start: EnergySnapshot, end: EnergySnapshot) -> EnergyDelta:
        ...


class RaplEnergyMeter(EnergyMeter):
    def __init__(self):
        domain, pkg_path, max_range_path = find_rapl_package_domain()
        if not pkg_path or not os.access(pkg_path, os.R_OK):
            raise RuntimeError(
                f"FATAL: Intel RAPL package energy_uj not readable (found={pkg_path}). "
                f"Real energy measurement is required."
            )
        self.source_name = domain or "intel-rapl:0"
        self.pkg_path = pkg_path
        self.max_range_uj = 0
        if max_range_path and os.access(max_range_path, os.R_OK):
            with open(max_range_path, "r") as f:
                self.max_range_uj = int(f.read().strip())

        pkg_dir = os.path.dirname(pkg_path)
        self.core_path, self.core_status = self._find_subdomain_energy(pkg_dir, "core")
        self.dram_path, self.dram_status = self._find_subdomain_energy(pkg_dir, "dram")
        self.pkg_status = f"readable ({self.pkg_path})"

    @staticmethod
    def _find_subdomain_energy(pkg_dir: str, substr: str):
        try:
            for entry in sorted(os.listdir(pkg_dir)):
                if not entry.startswith("intel-rapl:"):
                    continue
                sub_dir = os.path.join(pkg_dir, entry)
                name_p = os.path.join(sub_dir, "name")
                en_p = os.path.join(sub_dir, "energy_uj")
                if os.path.isfile(name_p):
                    with open(name_p, "r") as f:
                        nm = f.read().strip().lower()
                    if substr in nm:
                        if os.access(en_p, os.R_OK):
                            return en_p, f"readable ({en_p})"
                        return None, f"NA (unreadable {en_p})"
        except OSError as exc:
            return None, f"NA ({exc})"
        return None, f"NA (no {substr} domain exposed in {pkg_dir})"

    @staticmethod
    def _read_uj(path: str) -> int:
        with open(path, "r") as f:
            return int(f.read().strip())

    def read_snapshot(self) -> EnergySnapshot:
        pkg = self._read_uj(self.pkg_path)
        core = self._read_uj(self.core_path) if self.core_path else None
        dram = self._read_uj(self.dram_path) if self.dram_path else None
        return EnergySnapshot(pkg_uj=pkg, core_uj=core, dram_uj=dram)

    def compute_delta(self, start: EnergySnapshot, end: EnergySnapshot) -> EnergyDelta:
        d_pkg = compute_rapl_delta_uj(start.pkg_uj, end.pkg_uj, self.max_range_uj) / 1e6
        d_core = (
            compute_rapl_delta_uj(start.core_uj, end.core_uj, self.max_range_uj) / 1e6
            if (start.core_uj is not None and end.core_uj is not None)
            else None
        )
        d_dram = (
            compute_rapl_delta_uj(start.dram_uj, end.dram_uj, self.max_range_uj) / 1e6
            if (start.dram_uj is not None and end.dram_uj is not None)
            else None
        )
        return EnergyDelta(
            pkg_joules=d_pkg,
            core_joules=d_core,
            dram_joules=d_dram,
            source=self.source_name,
        )

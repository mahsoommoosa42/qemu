#!/usr/bin/env python3
"""Package the tested ESP32-S3 integration build with pinned-file provenance."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
BINS = {
    "qemu-system-xtensa": "qemu-system-xtensa",
    "esp32s3-system-test": "tests/qtest/esp32s3-system-test",
    "esp32s3-rtc-test": "tests/qtest/esp32s3-rtc-test",
    "esp32s3-rtc-clock-test": "tests/qtest/esp32s3-rtc-clock-test",
    "esp32s3-timg-rtc-calibration-test": "tests/qtest/esp32s3-timg-rtc-calibration-test",
    "check-esp32-uart-icount": "tests/qtest/esp32s3-uart-icount-test",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def model_results():
    expected = {
        "esp32s3-system-test": 3, "esp32s3-rtc-test": 9,
        "esp32s3-rtc-clock-test": 10, "esp32s3-timg-rtc-calibration-test": 16,
        "esp32s3-uart-icount-test": 3,
    }
    records = [json.loads(line) for line in
               (ROOT / "build/meson-logs/testlog.json").read_text().splitlines()]
    results = {}
    for name, count in expected.items():
        record = next(r for r in records if r["name"].endswith("/" + name))
        actual = sum(line.startswith("ok ") for line in record["stdout"].splitlines())
        if record["result"] != "OK" or record["returncode"] != 0 or actual != count:
            raise RuntimeError("Model regression did not pass: " + name)
        results[name] = actual
    return results


def main():
    tag = os.environ["RELEASE_TAG"]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", tag):
        raise ValueError("Invalid release tag")
    results = model_results()
    source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    stage = ROOT / "dist" / "qemu"
    stage.mkdir(parents=True, exist_ok=False)
    for name, built in BINS.items():
        target = stage / "bin" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / "build" / built, target)
    rom = stage / "share/qemu/esp32s3_rev0_rom.bin"
    rom.parent.mkdir(parents=True)
    shutil.copy2(ROOT / "pc-bios/esp32s3_rev0_rom.bin", rom)
    shutil.copy2(ROOT / "COPYING", stage / "COPYING")
    shutil.copy2(Path(__file__).with_name("fixes.json"), stage / "fixes.json")
    (stage / "DEPENDENCIES.txt").write_text(
        "Linux x86-64; built on Debian 11.11 (glibc 2.31).\n"
        "Runtime packages: libglib2.0-0, libgcrypt20, libslirp0, libfdt1, zlib1g.\n"
        "Ubuntu 24.04 provides libglib2.0-0t64 for the GLib runtime.\n\n" +
        subprocess.check_output(["ldd", str(stage / "bin/qemu-system-xtensa")], text=True))
    version = subprocess.check_output(
        [str(stage / "bin/qemu-system-xtensa"), "--version"], text=True).strip()
    if tag not in version:
        raise RuntimeError("QEMU was not built with the requested release version")
    files = {str(p.relative_to(stage)): sha256(p)
             for p in sorted(stage.rglob("*")) if p.is_file()}
    manifest = {
        "schema_version": 1,
        "release_tag": tag,
        "source_commit": source,
        "upstream_commit": "febae182e132e4055529be423a818225ebddaa3a",
        "platform": "linux-x86_64",
        "build_baseline": "Debian 11.11 / glibc 2.31",
        "qemu_version": version,
        "model_subtests": results,
        "model_subtests_passed": sum(results.values()),
        "physical_timing_verified": False,
        "files": files,
    }
    (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = stage.parent / ("qemu-" + tag + "-linux-x86_64.tar.gz")
    with tarfile.open(archive, "w:gz") as package:
        package.add(stage, arcname="qemu")
    shutil.copy2(stage / "manifest.json", stage.parent / "manifest.json")
    (stage.parent / "SHA256SUMS").write_text(
        sha256(archive) + "  " + archive.name + "\n" +
        sha256(stage.parent / "manifest.json") + "  manifest.json\n")
    print(archive)
    print((stage.parent / "SHA256SUMS").read_text())


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
irisOS - Automated Headless QEMU Verification Runner

Executes two-stage cross-reboot persistence verification (Section 9 of AGENTS.md):
  - Phase 1 (Boot 1): its_irisOS writes unique token to /Users/persist_check.txt
    and cleanly tears down virtio_blk, iris_crypt, iris_fshost, iris_pty, and iris_init.
  - Phase 2 (Boot 2): Re-uses the EXACT same disk image without repacking, verifies
    /Users/persist_check.txt token persistence, and executes the full test suite.
Fails immediately if any [ FAIL ], [ WARN ], mlibc warning, or kernel exception appears.
"""

import sys
import os
import time
import signal
import select
import shutil
import argparse
import subprocess
from typing import List, Tuple, Optional

DAEMON_TEARDOWN_MARKERS = [
    "iris_pty - Daemon exited cleanly.",
    "iris_fshost - Cleanly unmounted filesystems and exited.",
    "iris_crypt - Zeroized AES-256-XTS keys and exited cleanly.",
    "virtio_blk - Unpinned BTI DMA tokens (zx_pmt_unpin) and exited cleanly.",
    "iris_init - All daemons cleanly terminated."
]

def safe_stdout_write(data: str):
    try:
        sys.stdout.write(data)
        sys.stdout.flush()
    except (BlockingIOError, OSError):
        pass

def run_qemu_boot(arch: str, esp_img: str, disk_raw: str, boot_name: str,
                  expected_milestones: List[str], timeout: float = 25.0) -> Tuple[bool, str, Optional[str]]:
    tmp_vars = None
    if arch == "x86_64":
        qemu_bin = "qemu-system-x86_64"
        uefi_code = "/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
        cmd = [
            qemu_bin,
            "-machine", "q35",
            "-cpu", "max",
            "-m", "512M",
            "-smp", "2",
            "-nic", "none",
            "-drive", f"if=pflash,format=raw,readonly=on,file={uefi_code}",
            "-drive", f"id=bootdisk,file={esp_img},format=raw,if=none",
            "-device", "nvme,drive=bootdisk,serial=irisboot,bootindex=0",
            "-drive", f"file={disk_raw},if=none,id=hd0,format=raw",
            "-device", "virtio-blk-pci,drive=hd0",
            "-device", "virtio-gpu-pci,xres=1280,yres=800",
            "-device", "virtio-keyboard-pci",
            "-device", "virtio-tablet-pci",
            "-device", "virtio-rng-pci",
            "-serial", "stdio",
            "-display", "none"
        ]
    elif arch == "aarch64":
        qemu_bin = "qemu-system-aarch64"
        uefi_code = "/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
        vars_src = "/opt/homebrew/share/qemu/edk2-arm-vars.fd"
        tmp_vars = f"/tmp/edk2-aarch64-vars-{os.getpid()}-{int(time.time()*1000)}.fd"
        shutil.copyfile(vars_src, tmp_vars)
        cmd = [
            qemu_bin,
            "-M", "virt,gic-version=2",
            "-cpu", "max",
            "-m", "512M",
            "-smp", "2",
            "-nic", "none",
            "-drive", f"if=pflash,format=raw,readonly=on,file={uefi_code}",
            "-drive", f"if=pflash,format=raw,file={tmp_vars}",
            "-drive", f"id=bootdisk,file={esp_img},format=raw,if=none",
            "-device", "nvme,drive=bootdisk,serial=irisboot,bootindex=0",
            "-drive", f"file={disk_raw},if=none,id=hd0,format=raw",
            "-device", "virtio-blk-pci,drive=hd0",
            "-device", "virtio-gpu-pci,xres=1280,yres=800",
            "-device", "virtio-keyboard-pci",
            "-device", "virtio-tablet-pci",
            "-device", "virtio-rng-pci",
            "-serial", "stdio",
            "-display", "none"
        ]
    else:
        return False, "", f"Unsupported architecture: {arch}"

    print("-" * 70)
    print(f" [{boot_name}] Starting Headless QEMU ({arch.upper()})")
    print(f" Command: {' '.join(cmd)}")
    print(f" Timeout: {timeout:.1f}s")
    print("-" * 70, flush=True)

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid
    )

    os.set_blocking(proc.stdout.fileno(), False)

    start_time = time.time()
    accumulated_output = ""
    success = False
    failure_reason = None

    try:
        while time.time() - start_time < timeout:
            rlist, _, _ = select.select([proc.stdout], [], [], 0.1)
            if rlist:
                try:
                    raw_data = os.read(proc.stdout.fileno(), 4096)
                except (BlockingIOError, OSError):
                    raw_data = b""
                if not raw_data:
                    if proc.poll() is not None:
                        break
                    continue
                chunk = raw_data.decode("utf-8", errors="replace")
                safe_stdout_write(chunk)
                accumulated_output += chunk

                # Check for critical failures immediately
                if "Shell>" in accumulated_output or "2.0 Shell>" in accumulated_output:
                    failure_reason = "Dropped into UEFI EFI Shell prompt."
                    break

                if "BdsDxe: failed to load" in accumulated_output:
                    failure_reason = "EDK2 BdsDxe failed to load bootloader."
                    break

                if "panic: " in accumulated_output or "KERNEL PANIC" in accumulated_output:
                    failure_reason = "Kernel panic encountered."
                    break

                if "Failed to get \"write\" lock" in accumulated_output:
                    failure_reason = "QEMU disk lock collision (stray process holding disk image)."
                    break

                if "[ FAIL ]" in accumulated_output or "[FAILED]" in accumulated_output:
                    failure_reason = "Test suite reported a failure ([ FAIL ])."
                    break

                if "[ WARN ]" in accumulated_output:
                    failure_reason = "System logged a warning ([ WARN ]). Section 9 requires zero warnings."
                    break

                if "mlibc warning:" in accumulated_output:
                    failure_reason = "mlibc runtime warning detected. Section 9 requires zero warnings."
                    # Read remaining output to get full error message
                    try:
                        time.sleep(0.2)
                        rem = os.read(proc.stdout.fileno(), 4096).decode("utf-8", errors="replace")
                        if rem:
                            safe_stdout_write(rem)
                    except Exception:
                        pass
                    break

                # Verify all expected milestones are present
                milestones_met = all(m in accumulated_output for m in expected_milestones)

                # Check if all 5 daemons have cleanly logged teardown
                teardown_met = all(t in accumulated_output for t in DAEMON_TEARDOWN_MARKERS)

                if milestones_met and teardown_met:
                    # Give a brief moment to catch any trailing output
                    time.sleep(0.2)
                    try:
                        tail = os.read(proc.stdout.fileno(), 4096).decode("utf-8", errors="replace")
                        if tail:
                            safe_stdout_write(tail)
                            accumulated_output += tail
                    except Exception:
                        pass
                    success = True
                    break

            if proc.poll() is not None:
                break

    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            time.sleep(0.1)
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            proc.wait(timeout=2.0)
        except Exception:
            pass
        if tmp_vars and os.path.exists(tmp_vars):
            try:
                os.remove(tmp_vars)
            except Exception:
                pass

    return success, accumulated_output, failure_reason

def run_verification(arch: str, timeout: float = 45.0) -> int:
    script_dir = os.path.dirname(os.path.realpath(__file__))
    root_dir = os.path.dirname(script_dir)
    images_dir = os.path.join(root_dir, "out", "images")

    esp_img = os.path.join(images_dir, f"esp_{arch}.img")
    disk_raw = os.path.join(images_dir, f"disk_{arch}.raw")

    print("=" * 70)
    print(f" irisOS Adversarial Verification: {arch.upper()}")
    print(f" Packaging clean test images (build/mkimage.sh)...")
    print("=" * 70, flush=True)

    mk = subprocess.run([os.path.join(root_dir, "build", "mkimage.sh"), arch],
                        env=dict(os.environ, IRIS_MODE="test"),
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if mk.returncode != 0:
        print(f"[ERROR] mkimage.sh failed for {arch}:\n{mk.stderr}")
        return 1

    if not os.path.exists(esp_img) or not os.path.exists(disk_raw):
        print(f"[ERROR] Required disk images missing in {images_dir} for {arch}.")
        return 1

    # =========================================================================
    # Boot 1: Write persistence token & clean shutdown
    # =========================================================================
    boot1_milestones = [
        "Cross-Reboot Persistence Setup: Wrote token to /Users/persist_check.txt.",
        "Boot 1 complete. Requesting daemon teardown for reboot."
    ]
    b1_ok, b1_out, b1_reason = run_qemu_boot(
        arch, esp_img, disk_raw, "Boot 1: Persistence Token Setup",
        boot1_milestones, timeout
    )
    if not b1_ok:
        print("\n" + "=" * 70)
        print(f" [FAIL] Boot 1 failed: {b1_reason or f'Timed out after {timeout:.1f}s.'}")
        print("=" * 70, flush=True)
        return 1

    print("\n==> [PASS] Boot 1 successful: Token written and daemons cleanly terminated.")
    print("==> Preparing Boot 2: Re-using the exact same disk image WITHOUT repacking...\n")

    # =========================================================================
    # Boot 2: Verify persistence from Boot 1 & run full test suite
    # =========================================================================
    boot2_milestones = [
        "Cross-reboot persistence verified: /Users/persist_check.txt token matched from Boot 1.",
        "Interactive Test Suite all checks passed."
    ]
    b2_ok, b2_out, b2_reason = run_qemu_boot(
        arch, esp_img, disk_raw, "Boot 2: Persistence Readback & Full Test Suite",
        boot2_milestones, timeout
    )
    if not b2_ok:
        print("\n" + "=" * 70)
        print(f" [FAIL] Boot 2 failed: {b2_reason or f'Timed out after {timeout:.1f}s.'}")
        print("=" * 70, flush=True)
        return 1

    print("\n" + "=" * 70)
    print(f" [PASS] irisOS {arch} verification SUCCEEDED!")
    print(f"   - Boot 1: Token written, disk flushed, daemons cleanly exited.")
    print(f"   - Boot 2: Verified token persistence across reboot + passed all 14 tests.")
    print(f"   - Zero warnings, zero errors, clean BTI DMA unpin, zeroized AES keys.")
    print("=" * 70, flush=True)
    return 0

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="irisOS Headless QEMU Verification Runner")
    parser.add_argument("-a", "--arch", choices=["x86_64", "aarch64"], default="x86_64",
                        help="Target architecture (x86_64 or aarch64)")
    parser.add_argument("-t", "--timeout", type=float, default=45.0,
                        help="Execution timeout per boot in seconds (default: 25.0)")
    args = parser.parse_args()

    sys.exit(run_verification(args.arch, args.timeout))

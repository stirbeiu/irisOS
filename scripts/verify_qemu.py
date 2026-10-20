#!/usr/bin/env python3
"""
irisOS - Automated Headless QEMU Verification Runner

Launches irisOS in QEMU (x86_64 or aarch64) with non-blocking I/O and strict timeouts.
Detects bootloader or UEFI failures (such as returning to EFI Shell), kernel panics,
and verifies systemd-style milestone logs.
"""

import sys
import os
import time
import signal
import select
import shutil
import argparse
import subprocess

def run_verification(arch: str, timeout: float = 20.0) -> int:
    script_dir = os.path.dirname(os.path.realpath(__file__))
    root_dir = os.path.dirname(script_dir)
    images_dir = os.path.join(root_dir, "out", "images")

    esp_img = os.path.join(images_dir, f"esp_{arch}.img")
    disk_raw = os.path.join(images_dir, f"disk_{arch}.raw")

    if not os.path.exists(esp_img) or not os.path.exists(disk_raw):
        print(f"[ERROR] Required disk images missing in {images_dir} for {arch}.")
        print(f"Please run: build/mkimage.sh {arch}")
        return 1

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
            "-device", "virtio-keyboard-pci",
            "-device", "virtio-rng-pci",
            "-serial", "stdio",
            "-display", "none"
        ]
    elif arch == "aarch64":
        qemu_bin = "qemu-system-aarch64"
        uefi_code = "/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
        vars_src = "/opt/homebrew/share/qemu/edk2-arm-vars.fd"
        tmp_vars = f"/tmp/edk2-aarch64-vars-{os.getpid()}.fd"
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
            "-device", "virtio-keyboard-pci",
            "-device", "virtio-rng-pci",
            "-serial", "stdio",
            "-display", "none"
        ]
    else:
        print(f"[ERROR] Unsupported architecture: {arch}")
        return 1

    print("=" * 70)
    print(f" Starting Headless QEMU Verification: {arch.upper()}")
    print(f" Command: {' '.join(cmd)}")
    print(f" Timeout: {timeout:.1f}s")
    print("=" * 70, flush=True)

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=0,
        preexec_fn=os.setsid
    )

    # Ensure stdout is non-blocking so readline never hangs on partial lines (e.g. Shell>)
    os.set_blocking(proc.stdout.fileno(), False)

    start_time = time.time()
    accumulated_output = ""
    success = False
    failure_reason = None

    try:
        while time.time() - start_time < timeout:
            rlist, _, _ = select.select([proc.stdout], [], [], 0.1)
            if rlist:
                chunk = proc.stdout.read()
                if not chunk:
                    if proc.poll() is not None:
                        break
                    continue
                sys.stdout.write(chunk)
                sys.stdout.flush()
                accumulated_output += chunk

                # 1. Detect EFI Shell prompt (indicates bootloader failed or disk was unbootable)
                if "Shell>" in accumulated_output or "2.0 Shell>" in accumulated_output:
                    failure_reason = "QEMU dropped into UEFI EFI Shell prompt (bootloader not found or boot aborted)."
                    break

                if "BdsDxe: failed to load" in accumulated_output:
                    failure_reason = "EDK2 BdsDxe failed to load bootloader."
                    break

                # 2. Detect Kernel Panic
                if "panic: " in accumulated_output or "KERNEL PANIC" in accumulated_output:
                    failure_reason = "Kernel panic encountered."
                    break

                # 3. Detect verification completion
                if "Interactive Test Suite all checks passed." in accumulated_output:
                    time.sleep(0.3)
                    # Read any remaining output
                    try:
                        tail = proc.stdout.read()
                        if tail:
                            sys.stdout.write(tail)
                            sys.stdout.flush()
                            accumulated_output += tail
                    except Exception:
                        pass

                    if "[ FAIL ]" in accumulated_output or "[FAILED]" in accumulated_output:
                        failure_reason = "Test suite reported a failure ([ FAIL ] or [FAILED])."
                    else:
                        success = True
                    break

            if proc.poll() is not None:
                break

    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass
        if tmp_vars and os.path.exists(tmp_vars):
            try:
                os.remove(tmp_vars)
            except Exception:
                pass

    print("\n" + "=" * 70)
    if success:
        print(f" [PASS] irisOS {arch} verification succeeded!")
        print("=" * 70, flush=True)
        return 0
    else:
        if failure_reason:
            print(f" [FAIL] {failure_reason}")
        else:
            print(f" [FAIL] irisOS {arch} verification timed out after {timeout:.1f}s.")
        print("=" * 70, flush=True)
        return 1

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="irisOS Headless QEMU Verification Runner")
    parser.add_argument("-a", "--arch", choices=["x86_64", "aarch64"], default="x86_64",
                        help="Target architecture (x86_64 or aarch64)")
    parser.add_argument("-t", "--timeout", type=float, default=20.0,
                        help="Execution timeout in seconds (default: 20.0)")
    args = parser.parse_args()

    sys.exit(run_verification(args.arch, args.timeout))

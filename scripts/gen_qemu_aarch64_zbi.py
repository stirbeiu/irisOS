#!/usr/bin/env python3
# ==============================================================================
# irisOS - QEMU AArch64 Board Configuration ZBI Generator
#
# Generates a ZBI containing board metadata for QEMU 'virt' machine:
#  - PLATFORM_ID (QEMU)
#  - CPU_CONFIG (2 CPUs)
#  - MEM_CONFIG (512MB RAM @ 0x40000000, 1GB Peripherals @ 0x00000000)
#  - KERNEL_DRIVER: PL011 UART (MMIO 0x09000000, IRQ 33)
#  - KERNEL_DRIVER: GICv2 (MMIO 0x08000000, MSI 0x08020000, IPI 12)
#  - KERNEL_DRIVER: PSCI (HVC enabled)
#  - KERNEL_DRIVER: ARM Generic Timer (Phys IRQ 30, Virt IRQ 27)
# ==============================================================================

import struct
import sys

ZBI_TYPE_CONTAINER   = 0x544F4F42 # 'BOOT'
ZBI_CONTAINER_MAGIC  = 0x868cf7e6
ZBI_ITEM_MAGIC       = 0xb5781729
ZBI_ITEM_NO_CRC32    = 0x4a87e8d6
ZBI_FLAG_VERSION     = 0x00010000

def make_item(itype, extra, payload):
    pad = (8 - (len(payload) % 8)) % 8
    hdr = struct.pack('<IIIIIIII', itype, len(payload), extra, ZBI_FLAG_VERSION, 0, 0, ZBI_ITEM_MAGIC, ZBI_ITEM_NO_CRC32)
    return hdr + payload + (b'\x00' * pad)

def generate_qemu_arm64_zbi(output_path):
    # 1. PLATFORM_ID (0x44494C50)
    platform_id = struct.pack('<II32s', 0, 0, b'qemu')
    item_plat = make_item(0x44494C50, 0, platform_id)

    # 2. CPU_CONFIG (0x43555043)
    cpu_config = struct.pack('<IIIII', 1, 2, 0, 0, 0)
    item_cpu = make_item(0x43555043, 0, cpu_config)

    # 3. MEM_CONFIG (0x434D454D)
    # RAM: paddr=0x40000000, len=0x20000000 (512MB), type=1 (RAM)
    # PERIPHERAL: paddr=0x00000000, len=0x40000000 (1GB), type=2 (PERIPH)
    mem_config = struct.pack('<QQIIQQII',
        0x40000000, 0x20000000, 1, 0,
        0x00000000, 0x40000000, 2, 0
    )
    item_mem = make_item(0x434D454D, 0, mem_config)

    # 4. KERNEL_DRIVER (0x5652444B) - KDRV_PL011_UART (0x55304C50)
    pl011 = struct.pack('<QII', 0x09000000, 33, 0)
    item_uart = make_item(0x5652444B, 0x55304C50, pl011)

    # 5. KERNEL_DRIVER - KDRV_ARM_GIC_V2 (0x32434947)
    gicv2 = struct.pack('<QQQQQQIBBH',
        0x08000000, 0x08020000, 0x00000, 0x10000, 0x20000, 0x30000, 12, 1, 1, 0
    )
    item_gic = make_item(0x5652444B, 0x32434947, gicv2)

    # 6. KERNEL_DRIVER - KDRV_ARM_PSCI (0x49435350)
    psci = struct.pack('<B7x12Q', 1, *([0]*12))
    item_psci = make_item(0x5652444B, 0x49435350, psci)

    # 7. KERNEL_DRIVER - KDRV_ARM_GENERIC_TIMER (0x4D495441)
    timer = struct.pack('<IIII', 30, 27, 0, 0)
    item_timer = make_item(0x5652444B, 0x4D495441, timer)

    payload = item_plat + item_cpu + item_mem + item_uart + item_gic + item_psci + item_timer
    container = struct.pack('<IIIIIIII', ZBI_TYPE_CONTAINER, len(payload), ZBI_CONTAINER_MAGIC, ZBI_FLAG_VERSION, 0, 0, ZBI_ITEM_MAGIC, ZBI_ITEM_NO_CRC32) + payload

    with open(output_path, 'wb') as f:
        f.write(container)
    print(f"[gen_qemu_arm64_zbi] Wrote {len(container)} bytes to {output_path}")

if __name__ == '__main__':
    out_file = sys.argv[1] if len(sys.argv) > 1 else 'qemu_board.zbi'
    generate_qemu_arm64_zbi(out_file)

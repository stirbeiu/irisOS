# irisOS Subsystem Guide: Unified PCIe Discovery & Ring 3 VirtIO Block Driver

## 1. Architectural Overview & Philosophy

In accordance with irisOS's microkernel philosophy, device drivers do **not** run in kernel mode (Ring 0 on x86_64, EL1 on AArch64). All hardware drivers execute strictly as unprivileged user-space server processes (**Ring 3 / EL0**).

To avoid architecture-specific driver fragmentation (e.g. legacy x86 I/O ports vs ARM MMIO), irisOS standardizes on **PCIe Enhanced Configuration Access Mechanism (ECAM)** across both target architectures:
- **x86_64 (QEMU `-M q35` with EDK2/OVMF):** Physical ECAM base at `0xE0000000ULL` (mapped via `zx_vmo_create_physical()`).
- **AArch64 (QEMU `-M virt` with EDK2):** Physical ECAM base at `0x4010000000ULL` (mapped via `zx_vmo_create_physical()`).

```text
+-------------------------------------------------------------------------+
|                              iris_init                                  |
|   1. Maps PCIe ECAM configuration space via zx_vmo_create_physical()     |
|   2. Scans buses 0..15 for VirtIO Block device (Vendor 0x1AF4)           |
|   3. Creates IPC channel pair (client_ch <===> server_ch)               |
|   4. Spawns Ring 3 daemon: bin/virtio_blk (passing server_ch & Root Rsrc)|
+-------------------------------------------------------------------------+
                                    │
                                    │ Zircon IPC Channel (iris_msg_hdr_t)
                                    ▼
+-------------------------------------------------------------------------+
|                  bin/virtio_blk (Ring 3 / EL0 Daemon)                   |
|   1. Extracts Root Resource and Server Channel from Startup Handles     |
|   2. Scans PCIe ECAM, parses BARs & VirtIO 1.0+ capabilities            |
|   3. Allocates Virtqueue 0 & DMA buffer via Zircon BTI (zx_bti_pin)     |
|   4. Negotiates VIRTIO_F_VERSION_1 and enables Virtqueue 0              |
|   5. Serves BLK_GET_INFO, BLK_READ, BLK_WRITE over IPC                  |
+-------------------------------------------------------------------------+
```

---

## 2. Unified PCIe (ECAM) Configuration Scanner

The PCIe scanner is defined in `sdk/libiris/include/iris/pci.h` and implemented in `servers/drivers/pci/pci.c`.

### 2.1 ECAM Register Addressing Formula
The flat virtual address for any PCI register in an ECAM mapping is calculated via:
```c
uintptr_t reg_vaddr = ecam_base_vaddr +
                      (((uintptr_t)bus << 20) |
                       ((uintptr_t)dev << 15) |
                       ((uintptr_t)func << 12) |
                       ((uintptr_t)reg_offset & 0xFFFu));
```

### 2.2 VirtIO 1.0+ Capability Parsing
Modern VirtIO devices expose capabilities via the standard PCI Capability Linked List (starting at config offset `0x34`). Each capability uses `cap_vndr == 0x09` (`PCI_CAP_ID_VNDR`) and contains a `cfg_type`:
1. `VIRTIO_PCI_CAP_COMMON_CFG` (`1`): Points to `virtio_pci_common_cfg_t` (device status, feature selectors, queue selection, queue sizes, queue addresses).
2. `VIRTIO_PCI_CAP_NOTIFY_CFG` (`2`): Points to the doorbell register used to alert the device of new requests.
3. `VIRTIO_PCI_CAP_ISR_CFG` (`3`): Points to the interrupt status register.
4. `VIRTIO_PCI_CAP_DEVICE_CFG` (`4`): Points to device-specific configuration (e.g. 64-bit disk capacity in 512-byte sectors).

Each capability specifies the target BAR index (`bar`), byte offset (`offset`), and length (`length`). The driver maps the corresponding physical BAR into its virtual address space using `zx_vmo_create_physical()` and `zx_vmar_map()`.

---

## 3. Safe User-Space DMA via Zircon BTI

User-space device drivers cannot directly access arbitrary physical memory addresses without violating memory safety. irisOS utilizes Zircon's **Bus Transaction Initiator (BTI)** primitives to safely allocate and pin DMA buffers:

```c
/* 1. Create a dummy IOMMU handle from the Root Resource */
zx_iommu_desc_dummy_t dummy_desc = {0};
zx_handle_t iommu = ZX_HANDLE_INVALID;
zx_iommu_create(root_resource, ZX_IOMMU_TYPE_DUMMY, &dummy_desc, sizeof(dummy_desc), &iommu);

/* 2. Create BTI handle */
zx_handle_t bti = ZX_HANDLE_INVALID;
zx_bti_create(iommu, 0, 0, &bti);

/* 3. Allocate physically contiguous memory for the Virtqueue descriptor ring */
zx_handle_t vq_vmo = ZX_HANDLE_INVALID;
zx_vmo_create_contiguous(bti, 16384u, 0, &vq_vmo);

/* 4. Pin memory and obtain physical address (requires ZX_BTI_CONTIGUOUS) */
zx_paddr_t vq_paddr = 0;
zx_handle_t vq_pmt = ZX_HANDLE_INVALID;
zx_bti_pin(bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
           vq_vmo, 0, 16384u, &vq_paddr, 1, &vq_pmt);

/* 5. Map into driver virtual address space */
zx_vaddr_t vq_vaddr = 0;
zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vq_vmo, 0, 16384u, &vq_vaddr);
```

> [!IMPORTANT]
> When calling `zx_bti_pin()` with a single address entry (`addrs_count == 1`) on a contiguous VMO, the `ZX_BTI_CONTIGUOUS` option flag **must** be specified. Omitting `ZX_BTI_CONTIGUOUS` causes Zircon's `sys_bti_pin` to expect an array of size `size / PAGE_SIZE`, failing with `ZX_ERR_INVALID_ARGS (-10)`.

---

## 4. Block Driver IPC Protocol Wire Specification

Communication between filesystem/storage clients and `bin/virtio_blk` uses raw Zircon channels and the standard 24-byte `iris_msg_hdr_t` wire protocol (`IRIS_PROTO_DRIVER`).

### 4.1 Protocol Opcodes
| Opcode Name | Value | Description |
|:---|:---:|:---|
| `IRIS_BLK_OP_GET_INFO` | `0x0050` | Query total capacity and sector size |
| `IRIS_BLK_OP_READ` | `0x0051` | Read $N$ contiguous sectors into response buffer |
| `IRIS_BLK_OP_WRITE` | `0x0052` | Write $N$ contiguous sectors from request payload |
| `IRIS_BLK_OP_FLUSH` | `0x0053` | Flush non-volatile disk write cache |

### 4.2 Data Structures
```c
typedef struct iris_blk_info {
    uint64_t sector_count; /**< Total disk capacity in sectors */
    uint32_t sector_size;  /**< Sector size in bytes (512) */
    uint32_t flags;        /**< Status flags (read-only, removable, etc.) */
} iris_blk_info_t;

typedef struct iris_blk_rw_req {
    uint64_t start_sector; /**< Starting sector LBA */
    uint32_t num_sectors;  /**< Number of sectors to transfer */
    uint32_t flags;        /**< Transfer flags */
} iris_blk_rw_req_t;
```

---

## 5. End-to-End Verification Proof

During system boot, `iris_init` discovers the PCIe VirtIO block controller, spawns `bin/virtio_blk`, queries disk capacity, writes a distinctive 512-byte test pattern to Sector 100, reads it back, and validates byte-for-byte consistency.

### Verification Output (x86_64 - QEMU q35)
```text
[iris_init] Phase 4 Step 4.1: Unified PCIe Discovery & Block Driver
[iris_init] ========================================================
[iris_init] Mapping ECAM at phys 0x00000000e0000000...
[iris_init] Discovered VirtIO Block Device at PCI 0x00:0x03 (Vendor: 0x1af4, Device: 0x1001)
[iris_init] Spawning bin/virtio_blk driver daemon...
[virtio_blk] irisOS VirtIO Block Driver Daemon starting...
[virtio_blk] Mapping PCIe ECAM configuration space...
[virtio_blk] Scanning PCIe buses for VirtIO block device...
[virtio_blk] Discovered VirtIO Block device at PCI 00:03.0 (Vendor: 0x1af4, Device: 0x1001)
[virtio_blk] Mapped BAR 4 (phys: 0xc000000000) to vaddr: 0x178a1539b000
[virtio_blk] Configuring Virtqueue 0 (size: 256 descriptors)...
[virtio_blk] VirtIO block device active! Capacity: 131072 sectors (64 MB)
[virtio_blk] Entering IPC server loop on channel...
[iris_init] Sending BLK_GET_INFO query over IPC channel...
[iris_init] VirtIO Block query successful! Capacity: 0x0000000000020000 sectors, Sector Size: 0x0000000000000200 bytes
[iris_init] Preparing 512-byte verification pattern for Sector 100...
[iris_init] Writing Sector 100 via IPC request...
[iris_init] Sector 100 write confirmed OK. Reading back over IPC...
[iris_init] Sector 100 read/write data verification: MATCHED! (512/512 bytes OK)
[iris_init] VirtIO Block read/write test on Sector 100: PASSED!
[iris_init] Phase 4 Step 4.1 verification COMPLETE!
```

### Verification Output (AArch64 - QEMU virt)
```text
[  OK  ] Mapped PCIe ECAM configuration space at phys 0x0000004010000000.
[  OK  ] Discovered VirtIO Block Device at PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
[  OK  ] Located bin/virtio_blk in BOOTFS (2367736 bytes).
[  OK  ] Started process virtio_blk (bin/virtio_blk).
[  OK  ] virtio_blk: Bound to PCI 00:02.0 (Vendor: 0x1af4, Device: 0x1001).
[  OK  ] virtio_blk: Mapped BAR 4 (phys 0x8000000000 -> vaddr 0xbe4abaaa9000).
[  OK  ] virtio_blk: Virtqueue 0 (256 descriptors) & BTI DMA initialized (262144 sectors, 128 MB).
[  OK  ] virtio_blk: IPC server listening on control channel.
[  OK  ] VirtIO Block IPC query (BLK_GET_INFO): 262144 sectors (512 bytes/sector).
[  OK  ] VirtIO Block IPC write to Sector 100 confirmed (512 bytes).
[  OK  ] VirtIO Block IPC read from Sector 100 verified (512/512 bytes matched).
[  OK  ] Phase 4 Step 4.1 PCIe & VirtIO Block verification complete.
```

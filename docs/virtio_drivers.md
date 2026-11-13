# Unified PCIe Discovery & Ring 3 Hardware Drivers

## 1. Unified PCIe (ECAM) Architecture
All hardware drivers in `irisOS` execute as isolated Ring 3 (`x86_64`) / EL0 (`aarch64`) user-space processes. Both architectures share a single **PCIe Enhanced Configuration Access Mechanism (ECAM)** scanner in [`servers/drivers/pci/pci.c`](../servers/drivers/pci/pci.c):
* **`x86_64` (QEMU `-M q35`):** Physical ECAM base `0xE0000000ULL`.
* **`aarch64` (QEMU `-M virt`):** Physical ECAM base `0x4010000000ULL`.
* **Register Offset Formula:**
  ```math
  \text{vaddr} = \text{ecam\_base} + ((\text{bus} \ll 20) \mid (\text{dev} \ll 15) \mid (\text{func} \ll 12) \mid (\text{offset} \mathbin{\&} \text{0xFFF}))
  ```
* **VirtIO 1.0+ Capabilities (`PCI_CAP_ID_VNDR = 0x09`):** Parses `COMMON_CFG` (`1`), `NOTIFY_CFG` (`2`), `ISR_CFG` (`3`), and `DEVICE_CFG` (`4`), mapping the target PCI BARs into driver virtual memory via `zx_vmo_create_physical()` and `zx_vmar_map()`.

---

## 2. Safe User-Space DMA via Zircon BTI
Ring 3 drivers allocate and pin physically contiguous DMA buffers using Zircon's **Bus Transaction Initiator (`BTI`)**:
1. Create dummy IOMMU (`zx_iommu_create`) and BTI (`zx_bti_create`) from the Root Resource (`PA_RESOURCE`).
2. Allocate contiguous VMO (`zx_vmo_create_contiguous(bti, size, 0, &vmo)`).
3. Pin physical address with `ZX_BTI_CONTIGUOUS`:
   ```c
   zx_bti_pin(bti, ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
              vmo, 0, size, &paddr, 1, &pmt);
   ```
4. On driver exit, unpin all Pinned Memory Tokens via `zx_pmt_unpin(pmt)` before closing the BTI.

---

## 3. Ring 3 Driver Inventory

### 3.1 `bin/virtio_blk` ([`servers/drivers/virtio_blk/`](../servers/drivers/virtio_blk/src/main.c))
* **PCI ID:** Vendor `0x1AF4`, Device `0x1001` / `0x1042` (`VIRTIO_DEV_ID_BLK`).
* **Operation:** Configures split Virtqueue 0 (`256` descriptors) and a BTI-pinned DMA buffer supporting up to `120` sectors (`60 KB`) per transfer.
* **IPC Interface (`IRIS_PROTO_DRIVER`):** Serves `IRIS_BLK_OP_GET_INFO`, `IRIS_BLK_OP_READ`, `IRIS_BLK_OP_WRITE`, and `IRIS_BLK_OP_FLUSH` to `iris_fshost` and `iris_crypt`.

### 3.2 `bin/virtio_gpu` ([`servers/drivers/virtio_gpu/`](../servers/drivers/virtio_gpu/src/main.c)) & `bin/gop_fb` ([`servers/drivers/gop_fb/`](../servers/drivers/gop_fb/src/main.c))
* **PCI ID:** Vendor `0x1AF4`, Device `0x1050` (`VIRTIO_DEV_ID_GPU`).
* **2D Scanout Handshake (`ctrlq`, Queue 0):**
  1. `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` -> Queries preferred scanout geometry (`1280x800` `B8G8R8X8`).
  2. `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` -> Allocates host 2D Resource ID `1`.
  3. `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING` -> Attaches BTI-pinned guest framebuffer VMO.
  4. `VIRTIO_GPU_CMD_SET_SCANOUT` -> Binds Scanout `0` to Resource `1`.
* **Damage Flush (`IRIS_GPU_OP_FLUSH`):** Issues `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` for the damaged sub-rectangle `(x, y, width, height)` followed by `VIRTIO_GPU_CMD_RESOURCE_FLUSH`.
* **UEFI GOP Fallback (`bin/gop_fb`):** Exposes the identical `IRIS_GPU_OP_GET_INFO` / `IRIS_GPU_OP_FLUSH` IPC contract over the firmware GOP physical framebuffer (`zx_framebuffer_get_info`) when no VirtIO GPU is present.

### 3.3 `virtio_input` ([`servers/drivers/virtio_input/`](../servers/drivers/virtio_input/src/virtio_input.c))
* **PCI ID:** Vendor `0x1AF4`, Device `0x1052` (`virtio-keyboard-pci` and `virtio-tablet-pci`).
* **Operation:** Populates Virtqueue 0 (`eventq`) with 64 pre-posted 8-byte `virtio_input_event_t` (`type`, `code`, `value`) DMA descriptors.
* **Consumers:**
  * `iris_pty`: Translates `EV_KEY` scancodes (including Shift/Ctrl modifiers) into ASCII/ANSI sequences for terminal line discipline.
  * `iris_compositor`: Processes `EV_KEY` keystrokes and `EV_ABS` (`ABS_X`, `ABS_Y` in `0..32767`) / `EV_REL` mouse tablet events at 250 Hz (`4ms` frame sub-ticks).

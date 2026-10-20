# Storage Architecture, Block Encryption (`iris_crypt`) & VFS Routing (`iris_fshost`)

This document specifies the storage stack, 4-partition GPT disk layout, block-level AES-256-XTS encryption proxy (`iris_crypt`), FreeBSD UFS2 integration (`libufs`), and Virtual Filesystem (`iris_fshost`) routing architecture of **irisOS**.

---

## 1. Ring 3 Storage Pipeline Architecture

In accordance with the irisOS microkernel philosophy (Section 2 & Section 4.7 of [`AGENTS.md`](../AGENTS.md)), no storage drivers, cryptographic transforms, or filesystem parsers reside in the Zircon kernel. All layers execute as isolated, unprivileged Ring 3 (`x86_64`) / EL0 (`arm64`) user-space server processes communicating over raw Zircon channels (`zx_channel_*`) using the 24-byte [`iris_msg_hdr_t`](../sdk/libiris/include/iris/ipc.h) wire protocol:

```text
+-------------------------------------------------------------------+
|  User Applications & POSIX Clients (libc.so / mlibc-sysdeps)      |
+-------------------------------------------------------------------+
                                 |
                                 | IRIS_PROTO_FS (OPEN, READ, WRITE, CLOSE, STAT, SEEK)
                                 v
+-------------------------------------------------------------------+
|  VFS Router, GPT Parser & UFS2 Server (bin/iris_fshost + libufs.a)|
+-------------------------------------------------------------------+
         |                                               |
         | Read-Only (/System -> System_A)               | Read-Write (/Users, /Applications -> Data)
         | IRIS_PROTO_DRIVER (LBA 67584..133119)         | IRIS_PROTO_DRIVER (Plaintext Logical Sectors)
         |                                               v
         |                               +----------------------------------+
         |                               |  Block Encryption Proxy          |
         |                               |  (bin/iris_crypt)                |
         |                               |  AES-256-XTS + Argon2id (MbedTLS)|
         |                               +----------------------------------+
         |                                               |
         |                                               | IRIS_PROTO_DRIVER (Ciphertext, LBA 198656+8+L)
         v                                               v
+-------------------------------------------------------------------+
|  PCIe VirtIO Block Hardware Driver (bin/virtio_blk)               |
|  Zircon BTI DMA (zx_bti_pin) + Split Virtqueue 0                  |
+-------------------------------------------------------------------+
```

---

## 2. 128 MB 4-Partition GPT Disk Layout (`build/mkimage.sh` & `scripts/mk_gpt_disk.py`)

`build/mkimage.sh` and `scripts/mk_gpt_disk.py` generate a 128 MB (`262,144` sectors $\times$ `512` bytes = `134,217,728` bytes) raw GPT disk image (`out/images/disk_<arch>.raw`, symlinked at `out/images/disk.img`) with a valid Protective MBR at LBA `0`, Primary GPT Header & Partition Table at LBA `1..33`, and Backup GPT Partition Table & Header at LBA `262111..262143`:

| Partition # | Label | LBA Range | Size | Filesystem | Encryption | Mount Point / Role |
|---|---|---|---|---|---|---|
| `1` | `ESP` | `2048 .. 67583` (`65,536` sectors) | `32 MB` | FAT32 (`mformat` / `FatFs`) | Unencrypted | UEFI System Partition containing Gigaboot (`BOOTX64.EFI` / `BOOTAA64.EFI`), `iris_a.zbi`, and `iris_b.zbi` |
| `2` | `System_A` | `67584 .. 133119` (`65,536` sectors) | `32 MB` | FreeBSD UFS2 (`libufs`) | Unencrypted (Immutable RO) | Active read-only `/System` image (`Priority=15, Tries=7, Successful=1`, attr `0x00FF000000000000`) |
| `3` | `System_B` | `133120 .. 198655` (`65,536` sectors) | `32 MB` | FreeBSD UFS2 (`libufs`) | Unencrypted (Immutable RO) | Inactive read-only `/System` slot for A/B rollback (`Priority=1, Tries=0, Successful=0`) |
| `4` | `Data` | `198656 .. 260095` (`61,440` sectors) | `30 MB` | FreeBSD UFS2 (`libufs`) | **AES-256-XTS (`iris_crypt`)** | Persistent read-write storage mounted at `/Users` and `/Applications` (Sectors `0..7` reserved for 4 KB `iris_crypt` header; `61,432` usable logical sectors) |

---

## 3. Block-Level Encryption Proxy (`servers/iris_crypt`)

[`bin/iris_crypt`](../servers/iris_crypt/src/main.c) operates as a transparent block-level encryption filter (`IRIS_PROTO_DRIVER`) backed by a freestanding build of **Mbed-TLS 3.6 (`libmbedcrypto.a`, Apache 2.0)** and **Argon2id**.

### 3.1 Capability & Channel Endpoints
`iris_crypt` requires zero hardware MMIO or Root Resource privileges:
* **`PA_HND(PA_USER0, 0..3)`**: Upstream server channels listening for `IRIS_PROTO_DRIVER` block requests from `iris_fshost` and `iris_init`.
* **`PA_HND(PA_USER1, 0)`**: Downstream client channel connected to `bin/virtio_blk`.

### 3.2 GPT `Data` Partition Discovery & Sector 0 Metadata Header (4096 Bytes)
At startup, `iris_crypt` inspects LBA `1` for `"EFI PART"` and locates Partition 4 (`"Data"`, `first_lba = 198656`, `61,440` physical sectors). Physical sectors `0..7` of the `Data` partition (`LBA 198656..198663`, `4096` bytes) are reserved for the `iris_crypt_header_t` metadata structure defined in [`servers/iris_crypt/include/iris_crypt.h`](../servers/iris_crypt/include/iris_crypt.h):

| Byte Offset | Size (Bytes) | Field | Description |
|---|---|---|---|
| `0x000` | `8` | `magic` | `0x5450595243534952` (`"IRISCRYPT"` in little-endian ASCII) |
| `0x008` | `4` | `version` | Header format version (`1`) |
| `0x00C` | `4` | `flags` | Feature flags (`IRIS_CRYPT_FLAG_DEV_KEY = 0x1`) |
| `0x010` | `4` | `argon2_t_cost` | Argon2id iteration count (time cost, default `3`) |
| `0x014` | `4` | `argon2_m_cost` | Argon2id memory pool size in KiB (default `64`) |
| `0x018` | `4` | `argon2_lanes` | Argon2id degree of parallelism (default `1`) |
| `0x01C` | `4` | `sector_offset` | Physical sector offset to logical Sector 0 (`8` sectors = 4096 B) |
| `0x020` | `8` | `logical_sectors` | Usable logical 512-byte sectors (`physical_sectors - 8 = 61,432`) |
| `0x028` | `32` | `argon2_salt` | 256-bit cryptographic salt for Argon2id KDF |
| `0x048` | `64` | `encrypted_master_key` | 512-bit AES-256-XTS Master Key wrapped by the Argon2id KEK |
| `0x088` | `32` | `key_verification_tag` | SHA-256 digest of the unwrapped 512-bit Master Key |
| `0x0A8` | `32` | `header_checksum` | SHA-256 integrity hash over header bytes `[0x000..0x0A7]` |
| `0x0C8` | `3896` | `reserved` | Zero-padded to `4096` bytes (`_Static_assert(sizeof == 4096)`) |

### 3.3 Sector Offset Translation & Per-Sector XTS Tweak
* **Logical-to-Physical Translation**:
  $$\text{Physical LBA } P = \text{part\_start\_lba} + 8 + \text{Logical Sector } L$$
* **Per-Sector AES-256-XTS Encryption/Decryption**:
  Each 512-byte sector is encrypted/decrypted as an independent XTS data unit (`mbedtls_aes_crypt_xts`), where the 16-byte `data_unit` tweak encodes the 64-bit logical sector number $L + i$ in little-endian byte order.

---

## 4. FreeBSD UFS2 (`libufs.a`) & Block-Device IPC Adapter (`ufs_io.c`)

`vendor/libufs` (BSD 2-Clause, Zone 1 permissive) is cross-compiled for both `x86_64` and `aarch64` into `libufs.a` using Homebrew LLVM:
* **Unmodified Upstream Tree**: All block I/O inside `vendor/libufs/libufs/{block.c,sblock.c,cgroup.c,inode.c}` funnels through `pread(disk->d_fd, ...)` and `pwrite(disk->d_fd, ...)`.
* **IPC Block Adapter ([`servers/iris_fshost/src/ufs_io.c`](../servers/iris_fshost/src/ufs_io.c))**:
  - Compiling `libufs.a` with `-D_LIBUFS -Dpread=iris_ufs_pread -Dpwrite=iris_ufs_pwrite` routes all UFS2 superblock (`sbread`/`sbwrite`), cylinder group (`cgread1`/`cgwrite1`), inode (`getinode`/`putinode`), and data block (`bread`/`bwrite`) transfers over `IRIS_PROTO_DRIVER` (`IRIS_BLK_OP_READ` / `IRIS_BLK_OP_WRITE`).
  - Device ID `1` maps to Partition 2 (`System_A`, LBA `67584..133119`, strictly read-only) on `virtio_blk`.
  - Device ID `2` maps to Logical Sector `0..61431` of decrypted Partition 4 (`Data`, read-write) on `iris_crypt`.
  - Provides Castagnoli `calculate_crc32c()` (polynomial `0x82F63B78`) for UFS2 cylinder group and dinode check-hashes.

---

## 5. Central VFS Router (`servers/iris_fshost`) & `mlibc-sysdeps` Integration

[`bin/iris_fshost`](../servers/iris_fshost/src/main.c) exposes the macOS-style root directory hierarchy without legacy `/usr` clutter:

```text
/
├── System/                   # Mounted Read-Only from active GPT slot (System_A)
│   ├── Bin/                  # System binaries and utilities
│   ├── Libraries/            # Shared libraries (ld-iris.so, libc.so, libzircon.so, libiris.so)
│   ├── Servers/              # System servers and Ring 3 drivers
│   └── Resources/            # System release metadata, fonts, icons, default configs
├── Users/                    # Mounted Read-Write from AES-256-XTS encrypted Data partition
│   └── admin/                # User home directory
├── Applications/             # Mounted Read-Write from encrypted Data partition
├── bin -> /System/Bin        # Virtual symlink in RAM for POSIX script compatibility (#!/bin/sh)
└── lib -> /System/Libraries  # Virtual symlink in RAM for ELF dynamic loader compatibility
```

### 5.1 `IRIS_PROTO_FS` Wire Opcodes & Read-Only Enforcement
`sdk/mlibc-sysdeps/iris/generic/generic.cpp` routes POSIX file syscalls (`sys_open`, `sys_read`, `sys_write`, `sys_close`, `sys_stat`, `sys_fstat`, `sys_seek`) over the inherited `PA_HND(PA_NS_DIR, 0)` IPC channel to `iris_fshost`:
* `IRIS_FS_OP_OPEN` (`0x0010`): Expands virtual symlinks (`/bin`, `/lib`), resolves UFS2 directory inodes, creates new files on `/Users` or `/Applications` when `O_CREAT` is set, and rejects any write/create/truncate attempt on `/System` with `IRIS_ERR_READ_ONLY` (`-56`), which `mlibc-sysdeps` translates to `EROFS`.
* `IRIS_FS_OP_CLOSE` (`0x0011`): Releases the server-side open file slot.
* `IRIS_FS_OP_READ` (`0x0012`): Reads file data blocks via `libufs` `bread()` and advances the file offset.
* `IRIS_FS_OP_WRITE` (`0x0013`): Allocates 4 KB UFS2 blocks from Cylinder Group 0 on `Data`, writes file data via `libufs` `bwrite()`, updates the UFS2 dinode via `putinode()`, and enforces `IRIS_ERR_READ_ONLY` (`EROFS`) on read-only descriptors.
* `IRIS_FS_OP_SEEK` (`0x0014`): Repositions the file offset (`SEEK_SET`, `SEEK_CUR`, `SEEK_END`).
* `IRIS_FS_OP_STAT` (`0x0015`): Returns UFS2 inode metadata (`st_ino`, `st_size`, `st_mode`, `st_nlink`, `st_mtime`) for `stat()` and `fstat()`.

---

## 6. Automated QEMU Verification Runner (`scripts/verify_qemu.py` & `./iris test`)

To ensure robust CI and developer testing without manual intervention or hanging runs, [`scripts/verify_qemu.py`](../scripts/verify_qemu.py) executes headless QEMU instances with:
* Non-blocking stdio polling via `select.select()` and `os.set_blocking(False)`.
* Active detection of bootloader failures and UEFI fallback prompts (`Shell>`, `2.0 Shell>`, `BdsDxe: failed to load`, kernel panics).
* Strict execution timeouts (default 20 seconds) with process group termination (`SIGKILL`).
* Full validation of the complete Phase 4 storage and VFS milestone on both `x86_64` and `aarch64`:
  ```bash
  ./iris test all       # Runs automated headless tests on x86_64 and aarch64 sequentially
  ./iris test x86_64    # Runs x86_64 test suite
  ./iris test aarch64   # Runs aarch64 test suite
  ```

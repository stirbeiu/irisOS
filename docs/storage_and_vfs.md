# Storage Architecture, Block Encryption (`iris_crypt`) & VFS Routing

This document specifies the storage stack, block-level AES-256-XTS encryption proxy (`iris_crypt`), GPT partition layout, and Virtual Filesystem (`iris_fshost`) routing architecture of **irisOS**.

---

## 1. Ring 3 Storage Pipeline Architecture

In accordance with the irisOS microkernel philosophy (Section 2 & Section 4.7 of [`AGENTS.md`](../AGENTS.md)), no storage drivers, cryptographic transforms, or filesystem parsers reside in the Zircon kernel. All layers execute as isolated, unprivileged Ring 3 (`x86_64`) / EL0 (`arm64`) user-space server processes communicating over raw Zircon channels (`zx_channel_*`) using the 24-byte [`iris_msg_hdr_t`](../sdk/libiris/include/iris/ipc.h) wire protocol:

```text
+-------------------------------------------------------------------+
|  User Applications & POSIX Clients (libc.so / mlibc-sysdeps)      |
+-------------------------------------------------------------------+
                                 |
                                 | IRIS_PROTO_FS (open, read, write)
                                 v
+-------------------------------------------------------------------+
|  VFS Router & UFS2 Filesystem Server (bin/iris_fshost)            |
+-------------------------------------------------------------------+
         |                                               |
         | Read-Only (/System)                           | Read-Write (/Users, /Applications)
         | IRIS_PROTO_DRIVER                             | IRIS_PROTO_DRIVER (Plaintext Sectors)
         |                                               v
         |                               +----------------------------------+
         |                               |  Block Encryption Proxy          |
         |                               |  (bin/iris_crypt)                |
         |                               |  AES-256-XTS + Argon2id (MbedTLS)|
         |                               +----------------------------------+
         |                                               |
         |                                               | IRIS_PROTO_DRIVER (Ciphertext, Sector + 8)
         v                                               v
+-------------------------------------------------------------------+
|  PCIe VirtIO Block Hardware Driver (bin/virtio_blk)               |
|  Zircon BTI DMA (zx_bti_pin) + Split Virtqueue 0                  |
+-------------------------------------------------------------------+
```

---

## 2. Block-Level Encryption Proxy (`servers/iris_crypt`)

[`bin/iris_crypt`](../servers/iris_crypt/src/main.c) operates as a transparent block-level encryption filter (`IRIS_PROTO_DRIVER`) backed by a freestanding build of **Mbed-TLS 3.6 (`libmbedcrypto.a`, Apache 2.0)** and **Argon2id**.

### 2.1 Capability & Channel Endpoints
`iris_crypt` requires zero hardware MMIO or Root Resource privileges:
* **`PA_HND(PA_USER0, 0)`**: Upstream server channel listening for `IRIS_PROTO_DRIVER` block requests from `iris_fshost` or `iris_init`.
* **`PA_HND(PA_USER1, 0)`**: Downstream client channel connected to `bin/virtio_blk`.

### 2.2 Sector 0 Metadata Header (4096 Bytes / Physical Sectors 0..7)
Physical sectors `0..7` (the first `4096` bytes) of the encrypted storage volume are reserved for the `iris_crypt_header_t` metadata structure defined in [`servers/iris_crypt/include/iris_crypt.h`](../servers/iris_crypt/include/iris_crypt.h).

| Byte Offset | Size (Bytes) | Field | Description |
|---|---|---|---|
| `0x000` | `8` | `magic` | `0x5450595243534952` (`"IRISCRYPT"` in little-endian ASCII) |
| `0x008` | `4` | `version` | Header format version (`1`) |
| `0x00C` | `4` | `flags` | Feature flags (`IRIS_CRYPT_FLAG_DEV_KEY = 0x1`) |
| `0x010` | `4` | `argon2_t_cost` | Argon2id iteration count (time cost, default `3`) |
| `0x014` | `4` | `argon2_m_cost` | Argon2id memory pool size in KiB (default `64`) |
| `0x018` | `4` | `argon2_lanes` | Argon2id degree of parallelism (default `1`) |
| `0x01C` | `4` | `sector_offset` | Physical sector offset to logical Sector 0 (`8` sectors = 4096 B) |
| `0x020` | `8` | `logical_sectors` | Usable logical 512-byte sectors (`physical_sectors - 8`) |
| `0x028` | `32` | `argon2_salt` | 256-bit cryptographic salt for Argon2id KDF |
| `0x048` | `64` | `encrypted_master_key` | 512-bit AES-256-XTS Master Key wrapped by the Argon2id KEK |
| `0x088` | `32` | `key_verification_tag` | SHA-256 digest of the unwrapped 512-bit Master Key |
| `0x0A8` | `32` | `header_checksum` | SHA-256 integrity hash over header bytes `[0x000..0x0A7]` |
| `0x0C8` | `3896` | `reserved` | Zero-padded to `4096` bytes (`_Static_assert(sizeof == 4096)`) |

### 2.3 Key Hierarchy: Argon2id + AES-256-XTS
1. **Key Derivation (`iris_argon2id_derive`)**:
   * The user passphrase (or Phase 4 deterministic development key fallback `IRIS_CRYPT_DEV_PASSPHRASE`) and the 32-byte `argon2_salt` are processed through `t_cost` memory-hard BlaMka G-round mixing passes across `m_cost` 1 KiB blocks (`servers/iris_crypt/src/argon2.c`).
   * Produces a 512-bit (`64`-byte) **Key Encryption Key (KEK)**.
2. **Master Key Wrapping & Verification**:
   * The volume is encrypted with a 512-bit (`64`-byte) **Master Key** split into two 256-bit AES keys as required by IEEE P1619 / NIST SP 800-38E AES-256-XTS:
     - `Key1` (bits `0..255`): 256-bit AES block cipher key.
     - `Key2` (bits `256..511`): 256-bit AES tweak encryption key.
   * On disk, the Master Key is stored encrypted (`encrypted_master_key`) under the KEK using AES-256-XTS.
   * Upon unlocking, `iris_crypt` unwraps the Master Key, verifies its SHA-256 digest against `key_verification_tag`, and wipes the KEK from memory via `mbedtls_platform_zeroize()`.
3. **Phase 4 Deterministic Test Key Fallback**:
   * Prior to Phase 5's interactive passphrase utility (`apps/sysutils/passphrase`), `iris_crypt` automatically initializes unformatted Sector 0 headers and unlocks existing headers using a deterministic development passphrase and salt so automated QEMU tests execute without blocking on user input.

### 2.4 Sector Offset Translation & Per-Sector XTS Tweak
* **Logical-to-Physical Translation**:
  $$\text{Physical Sector } P = \text{Logical Sector } L + 8$$
  Upstream filesystem Logical Sector `0` maps directly to Physical Sector `8` on `virtio_blk` (immediately following the 4 KB metadata header in Physical Sectors `0..7`).
* **Capacity Adjustment (`IRIS_BLK_OP_GET_INFO`)**:
  `iris_crypt` queries downstream `virtio_blk` and subtracts `8` sectors (`IRIS_CRYPT_HEADER_SECTORS`) from the reported `sector_count` (e.g. `131,072` physical sectors $\rightarrow$ `131,064` logical sectors).
* **Per-Sector AES-256-XTS Encryption/Decryption**:
  Each 512-byte sector is encrypted/decrypted as an independent XTS data unit (`mbedtls_aes_crypt_xts`), where the 16-byte `data_unit` tweak contains the 64-bit logical sector number $L + i$ encoded in little-endian format padded with zeros.

---

## 3. GPT Partition Layout & A/B Update Slots

`irisOS` uses a 4-partition GUID Partition Table (GPT) layout with seamless A/B rollback protection:

| Partition # | Label | Filesystem | Encryption | Mount Point / Role |
|---|---|---|---|---|
| `1` | `ESP` | FAT32 (`FatFs`) | Unencrypted | UEFI System Partition containing Gigaboot (`BOOTX64.EFI` / `BOOTAA64.EFI`), `iris_a.zbi`, and `iris_b.zbi` |
| `2` | `System_A` | UFS2 (`libufs`) | Unencrypted (Immutable RO) | Active read-only `/System` image for Slot A |
| `3` | `System_B` | UFS2 (`libufs`) | Unencrypted (Immutable RO) | Inactive read-only `/System` image for Slot B updates |
| `4` | `Data` | UFS2 (`libufs`) | **AES-256-XTS (`iris_crypt`)** | Persistent read-write storage mounted at `/Users` and `/Applications` |

---

## 4. Runtime VFS Hierarchy (`iris_fshost`)

`iris_fshost` exposes a clean macOS-style root directory hierarchy without legacy `/usr` clutter:

```text
/
├── System/                   # Mounted Read-Only from active GPT slot (System_A or System_B)
│   ├── Bin/                  # toybox binary and command symlinks (sh, ls, cp, cat, etc.)
│   ├── Libraries/            # ld-iris.so, libc.so, libzircon.so, libiris.so, libpixman.so
│   ├── Servers/              # iris_compositor, non-boot drivers, background daemons
│   └── Resources/            # System fonts (.ttf), icons, default configs
├── Users/                    # Mounted Read-Write from AES-256-XTS encrypted Data partition
│   └── admin/                # User home directory
├── Applications/             # Mounted Read-Write from encrypted Data partition
├── bin -> /System/Bin        # Virtual symlink in RAM for POSIX script compatibility (#!/bin/sh)
└── lib -> /System/Libraries  # Virtual symlink in RAM for ELF dynamic loader compatibility
```

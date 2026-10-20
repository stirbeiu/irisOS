/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_fshost/src/main.c
 * Purpose: Central Ring 3 VFS Mount Router, GPT Partition Parser & UFS2 Server.
 *
 * Licensing: MIT / BSD 2-Clause (Zone 1 Permissive).
 */

/**
 * @file main.c
 * @brief Central VFS Daemon, GPT Partition Parser & FreeBSD UFS2 Server (`bin/iris_fshost`).
 *
 * MODULE LIFECYCLE, CHANNEL ENDPOINTS & CAPABILITY DEPENDENCIES:
 *  1. Process Isolation & Capabilities:
 *     - Runs as an isolated Ring 3 (x86_64) / EL0 (arm64) user-space server.
 *     - `PA_HND(PA_USER0, 0..3)`: Upstream VFS IPC server channel endpoints
 *       receiving `IRIS_PROTO_FS` requests from user-space processes (`mlibc-sysdeps`).
 *     - `PA_HND(PA_USER1, 0)`: Downstream IPC client channel connected to `bin/virtio_blk`
 *       for reading the GPT partition table and mounting `System_A` (UFS2 read-only).
 *     - `PA_HND(PA_USER2, 0)`: Downstream IPC client channel connected to `bin/iris_crypt`
 *       for mounting the AES-256-XTS decrypted `Data` partition (UFS2 read-write).
 *
 *  2. GPT Partition Table Discovery & A/B Slot Selection:
 *     - Reads LBA 0 (Protective MBR) and LBA 1..5 (Primary GPT Header & Partition Entries)
 *       from `virtio_blk`.
 *     - Discovers the 4-partition irisOS GPT layout:
 *       * Partition 1 (`ESP`):      FAT32 EFI System Partition (Gigaboot + dual ZBIs).
 *       * Partition 2 (`System_A`): Active UFS2 system root (mounted Read-Only at `/System`).
 *       * Partition 3 (`System_B`): Inactive UFS2 A/B rollback slot.
 *       * Partition 4 (`Data`):     AES-256-XTS encrypted UFS2 partition (mounted Read-Write
 *                                   at `/Users` and `/Applications` via `iris_crypt`).
 *
 *  3. Runtime Filesystem Hierarchy & Symlink Resolution:
 *     - Exposes the macOS-style root hierarchy (`/System`, `/Users`, `/Applications`).
 *     - Resolves virtual in-memory symlinks (`/bin -> /System/Bin`, `/lib -> /System/Libraries`).
 *     - Strictly enforces `IRIS_ERR_READ_ONLY` (`EROFS`) on any write/create/truncate
 *       attempt targeting `/System` (or `/bin`, `/lib`).
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/disk.h>
#include <sys/disklabel.h>
#include <sys/stat.h>

#include <ufs/ufs/extattr.h>
#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/dinode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ffs/fs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iris/ipc.h>
#include <iris/init.h>

#include <libufs.h>
#include "ufs_io.h"

typedef ino_t ufs_ino_t;

/* ==============================================================================
 * Constants & Structures
 * ============================================================================== */

#define MAX_FSHOST_CLIENTS 8
#define MAX_OPEN_FILES     64

#define DEV_ID_SYSTEM_A    1
#define DEV_ID_DATA_CRYPT  2

/* POSIX open flag compatibility constants (matching mlibc abi-bits/fcntl.h) */
#define VFS_O_ACCMODE  0x0003u
#define VFS_O_RDONLY   0x0000u
#define VFS_O_WRONLY   0x0001u
#define VFS_O_RDWR     0x0002u
#define VFS_O_CREAT    0x0008u
#define VFS_O_TRUNC    0x0200u
#define VFS_O_APPEND   0x0004u

/* Also accept Linux/BSD style O_CREAT (0x40 / 0x200) if passed by raw callers */
#define VFS_ANY_WRITE_MASK (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | 0x0040u | 0x0200u | 0x0400u)

#pragma pack(push, 1)
typedef struct gpt_header {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

typedef struct gpt_part_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} gpt_part_entry_t;
#pragma pack(pop)

typedef struct gpt_part_info {
    bool     present;
    char     name[36];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t sector_count;
    uint64_t attributes;
} gpt_part_info_t;

typedef enum vfs_mount_id {
    VFS_MOUNT_NONE = 0,
    VFS_MOUNT_SYSTEM_RO,
    VFS_MOUNT_DATA_RW,
} vfs_mount_id_t;

typedef struct vfs_open_file {
    bool           in_use;
    vfs_mount_id_t mount_id;
    bool           writable;
    ufs_ino_t      ino;
    uint32_t       flags;
    uint64_t       offset;
    uint64_t       size;
    bool           is_dir;
} vfs_open_file_t;

/* Global state */
static zx_handle_t     g_client_channels[MAX_FSHOST_CLIENTS];
static uint32_t        g_num_clients   = 0;
static zx_handle_t     g_blk_channel   = ZX_HANDLE_INVALID;
static zx_handle_t     g_crypt_channel = ZX_HANDLE_INVALID;

static gpt_part_info_t g_part_esp;
static gpt_part_info_t g_part_sys_a;
static gpt_part_info_t g_part_sys_b;
static gpt_part_info_t g_part_data;

static struct uufsd    g_ufs_system;
static struct uufsd    g_ufs_data;
static bool            g_system_mounted = false;
static bool            g_data_mounted   = false;

static vfs_open_file_t g_open_files[MAX_OPEN_FILES];

/* ==============================================================================
 * Logging & Downstream Block Helpers
 * ============================================================================== */

static void fshost_log(const char* fmt, ...) {
    char line_buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line_buf, sizeof(line_buf), fmt, args);
    va_end(args);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line_buf) ? (size_t)n : sizeof(line_buf) - 1;
        write(1, line_buf, len);
    }
}

static zx_status_t blk_get_info(zx_handle_t ch, iris_blk_info_t* out_info) {
    static uint32_t txid = 500;
    iris_msg_hdr_t req;
    iris_msg_hdr_init(&req, IRIS_PROTO_DRIVER, IRIS_BLK_OP_GET_INFO, txid++, 0);

    zx_status_t st = zx_channel_write(ch, 0, &req, sizeof(req), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) return ZX_ERR_PEER_CLOSED;

    uint8_t rep_buf[128] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t) + sizeof(iris_blk_info_t)) {
        return ZX_ERR_IO;
    }
    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    if (rep_hdr->status != ZX_OK) return rep_hdr->status;

    memcpy(out_info, rep_buf + sizeof(iris_msg_hdr_t), sizeof(iris_blk_info_t));
    return ZX_OK;
}

static zx_status_t blk_read_sectors(zx_handle_t ch, uint64_t lba, uint32_t count, void* out_buf) {
    static uint32_t txid = 600;
    uint8_t req_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t)] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
    iris_blk_rw_req_t* rw = (iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_READ, txid++, sizeof(iris_blk_rw_req_t));
    rw->start_sector = lba;
    rw->num_sectors  = count;
    rw->flags        = 0;

    zx_status_t st = zx_channel_write(ch, 0, req_buf, sizeof(req_buf), NULL, 0);
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) return ZX_ERR_PEER_CLOSED;

    static uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK) return st;

    uint32_t expected = count * 512u;
    if (act_bytes < sizeof(iris_msg_hdr_t) + expected) return ZX_ERR_IO;
    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    if (rep_hdr->status != ZX_OK) return rep_hdr->status;

    memcpy(out_buf, rep_buf + sizeof(iris_msg_hdr_t), expected);
    return ZX_OK;
}

/* ==============================================================================
 * GPT Partition Table Parser
 * ============================================================================== */

static void utf16le_to_ascii(const uint16_t* u16, size_t max_chars, char* out) {
    size_t i = 0;
    for (; i < max_chars - 1 && u16[i] != 0; ++i) {
        out[i] = (char)(u16[i] & 0x7Fu);
    }
    out[i] = '\0';
}

static zx_status_t parse_gpt_partitions(void) {
    uint8_t sec_buf[512 * 6] __attribute__((aligned(8)));
    zx_status_t st = blk_read_sectors(g_blk_channel, 0, 6, sec_buf);
    if (st != ZX_OK) {
        fshost_log("[FAILED] iris_fshost: Failed to read LBA 0..5 from virtio_blk (%d).\n", st);
        return st;
    }

    /* 1. Verify Protective MBR at LBA 0 */
    if (sec_buf[510] != 0x55 || sec_buf[511] != 0xAA || sec_buf[446 + 4] != 0xEE) {
        fshost_log("[FAILED] iris_fshost: Invalid Protective MBR at LBA 0.\n");
        return ZX_ERR_IO_DATA_INTEGRITY;
    }

    /* 2. Verify Primary GPT Header at LBA 1 */
    const gpt_header_t* gpt = (const gpt_header_t*)(sec_buf + 512);
    if (memcmp(gpt->signature, "EFI PART", 8) != 0 || gpt->revision != 0x00010000u) {
        fshost_log("[FAILED] iris_fshost: Invalid GPT Header signature at LBA 1.\n");
        return ZX_ERR_IO_DATA_INTEGRITY;
    }

    /* 3. Parse Partition Entries starting at LBA 2 */
    const gpt_part_entry_t* entries = (const gpt_part_entry_t*)(sec_buf + 1024);
    uint32_t found = 0;

    for (uint32_t i = 0; i < 16; ++i) {
        const gpt_part_entry_t* e = &entries[i];
        if (e->first_lba == 0 || e->last_lba < e->first_lba) {
            continue;
        }

        gpt_part_info_t info;
        memset(&info, 0, sizeof(info));
        info.present      = true;
        info.first_lba    = e->first_lba;
        info.last_lba     = e->last_lba;
        info.sector_count = (e->last_lba - e->first_lba) + 1ULL;
        info.attributes   = e->attributes;
        utf16le_to_ascii(e->name, sizeof(info.name), info.name);

        if (strcmp(info.name, "ESP") == 0) {
            g_part_esp = info;
            found++;
        } else if (strcmp(info.name, "System_A") == 0) {
            g_part_sys_a = info;
            found++;
        } else if (strcmp(info.name, "System_B") == 0) {
            g_part_sys_b = info;
            found++;
        } else if (strcmp(info.name, "Data") == 0) {
            g_part_data = info;
            found++;
        }
    }

    if (!g_part_esp.present || !g_part_sys_a.present || !g_part_sys_b.present || !g_part_data.present) {
        fshost_log("[FAILED] iris_fshost: Missing required GPT partitions (found %u/4).\n", found);
        return ZX_ERR_NOT_FOUND;
    }

    uint32_t slot_a_prio = (uint32_t)((g_part_sys_a.attributes >> 48) & 0x0Fu);
    uint32_t slot_a_ok   = (uint32_t)((g_part_sys_a.attributes >> 55) & 0x01u);

    fshost_log("[  OK  ] iris_fshost: Parsed GPT (ESP=%llu..%llu, System_A=%llu..%llu [prio=%u,ok=%u], System_B=%llu..%llu, Data=%llu..%llu).\n",
               (unsigned long long)g_part_esp.first_lba,   (unsigned long long)g_part_esp.last_lba,
               (unsigned long long)g_part_sys_a.first_lba, (unsigned long long)g_part_sys_a.last_lba,
               slot_a_prio, slot_a_ok,
               (unsigned long long)g_part_sys_b.first_lba, (unsigned long long)g_part_sys_b.last_lba,
               (unsigned long long)g_part_data.first_lba,  (unsigned long long)g_part_data.last_lba);

    return ZX_OK;
}

/* ==============================================================================
 * FreeBSD UFS2 Mount & Inode / Block Operations via vendor/libufs
 * ============================================================================== */

static void init_uufsd_struct(struct uufsd* disk, int dev_id, bool writable) {
    memset(disk, 0, sizeof(*disk));
    disk->d_name      = "iris_ufs2";
    disk->d_ufs       = 2;
    disk->d_fd        = dev_id;
    disk->d_bsize     = 512;
    disk->d_sblockloc = UFS_STDSB;
    disk->d_mine      = writable ? 0x02 : 0x00; /* MINE_WRITE = 0x02 */
}

static zx_status_t mount_filesystems(void) {
    /* 1. Register & mount System_A (Read-Only UFS2) at /System */
    zx_status_t st = iris_ufs_register_device(
        DEV_ID_SYSTEM_A,
        g_blk_channel,
        g_part_sys_a.first_lba,
        g_part_sys_a.sector_count,
        true /* strictly read-only */
    );
    if (st != ZX_OK) return st;

    init_uufsd_struct(&g_ufs_system, DEV_ID_SYSTEM_A, false);
    if (sbread(&g_ufs_system) == -1 || g_ufs_system.d_fs.fs_magic != FS_UFS2_MAGIC) {
        fshost_log("[FAILED] iris_fshost: sbread() failed on System_A (%s).\n",
                   g_ufs_system.d_error ? g_ufs_system.d_error : "invalid magic");
        return ZX_ERR_IO_DATA_INTEGRITY;
    }
    g_system_mounted = true;
    fshost_log("[  OK  ] iris_fshost: Mounted System_A (UFS2 magic 0x%08x) at /System [Read-Only].\n",
               (unsigned int)g_ufs_system.d_fs.fs_magic);

    /* 2. Query iris_crypt & mount decrypted Data (Read-Write UFS2) at /Users and /Applications */
    iris_blk_info_t crypt_info;
    st = blk_get_info(g_crypt_channel, &crypt_info);
    if (st != ZX_OK) {
        fshost_log("[FAILED] iris_fshost: Failed to query iris_crypt block geometry (%d).\n", st);
        return st;
    }

    st = iris_ufs_register_device(
        DEV_ID_DATA_CRYPT,
        g_crypt_channel,
        0,
        crypt_info.sector_count,
        false /* read-write (read_only = false) */
    );
    if (st != ZX_OK) return st;

    init_uufsd_struct(&g_ufs_data, DEV_ID_DATA_CRYPT, true);
    if (sbread(&g_ufs_data) == -1 || g_ufs_data.d_fs.fs_magic != FS_UFS2_MAGIC) {
        fshost_log("[FAILED] iris_fshost: sbread() failed on decrypted Data (%s).\n",
                   g_ufs_data.d_error ? g_ufs_data.d_error : "invalid magic");
        return ZX_ERR_IO_DATA_INTEGRITY;
    }
    g_data_mounted = true;
    fshost_log("[  OK  ] iris_fshost: Mounted encrypted Data (UFS2 magic 0x%08x) at /Users & /Applications [Read-Write].\n",
               (unsigned int)g_ufs_data.d_fs.fs_magic);

    fshost_log("[  OK  ] iris_fshost: Active virtual symlinks: /bin -> /System/Bin, /lib -> /System/Libraries.\n");
    return ZX_OK;
}

/**
 * @brief Look up a single path component inside a UFS2 directory inode.
 */
static zx_status_t ufs2_dir_lookup(struct uufsd* disk, ufs_ino_t dir_ino,
                                   const char* name, ufs_ino_t* out_ino) {
    union dinodep dp;
    if (getinode(disk, &dp, dir_ino) == -1) {
        return ZX_ERR_IO;
    }
    struct ufs2_dinode dir_di = *dp.dp2;
    if ((dir_di.di_mode & IFMT) != IFDIR) {
        return ZX_ERR_NOT_DIR;
    }

    struct fs* fs = &disk->d_fs;
    uint8_t blk_buf[4096] __attribute__((aligned(8)));
    size_t name_len = strlen(name);

    for (int b = 0; b < UFS_NDADDR; ++b) {
        ufs2_daddr_t frag = dir_di.di_db[b];
        if (frag == 0) {
            continue;
        }
        if (bread(disk, fsbtodb(fs, frag), blk_buf, fs->fs_bsize) == -1) {
            return ZX_ERR_IO;
        }

        for (uint32_t chunk = 0; chunk < (uint32_t)fs->fs_bsize; chunk += DIRBLKSIZ) {
            uint32_t off = 0;
            while (off < DIRBLKSIZ) {
                const struct direct* de = (const struct direct*)(blk_buf + chunk + off);
                if (de->d_reclen == 0) {
                    break;
                }
                if (de->d_ino != 0 && de->d_namlen == name_len &&
                    memcmp(de->d_name, name, name_len) == 0) {
                    *out_ino = (ufs_ino_t)de->d_ino;
                    return ZX_OK;
                }
                off += de->d_reclen;
            }
        }
    }

    return ZX_ERR_NOT_FOUND;
}

/**
 * @brief Allocate a 4096-byte (8-fragment) block from Cylinder Group 0.
 */
static zx_status_t ufs2_alloc_block(struct uufsd* disk, ufs2_daddr_t* out_frag) {
    struct fs* fs = &disk->d_fs;
    if (cgread1(disk, 0) <= 0) {
        return ZX_ERR_IO;
    }
    struct cg* cg = &disk->d_cg;
    uint8_t* blksfree = cg_blksfree(cg);

    for (int32_t frag = 64; frag + 8 <= cg->cg_ndblk; frag += 8) {
        bool all_free = true;
        for (int32_t f = 0; f < 8; ++f) {
            if (isclr(blksfree, frag + f)) {
                all_free = false;
                break;
            }
        }
        if (all_free) {
            for (int32_t f = 0; f < 8; ++f) {
                clrbit(blksfree, frag + f);
            }
            cg->cg_cs.cs_nbfree--;
            fs->fs_cstotal.cs_nbfree--;

            if (cgwrite1(disk, 0) == -1 || sbwrite(disk, 0) == -1) {
                return ZX_ERR_IO;
            }

            uint8_t zero_blk[4096];
            memset(zero_blk, 0, sizeof(zero_blk));
            if (bwrite(disk, fsbtodb(fs, frag), zero_blk, sizeof(zero_blk)) == -1) {
                return ZX_ERR_IO;
            }

            *out_frag = (ufs2_daddr_t)frag;
            return ZX_OK;
        }
    }

    return ZX_ERR_NO_SPACE;
}

/**
 * @brief Create a new regular file in `dir_ino` on a writable UFS2 volume.
 */
static zx_status_t ufs2_create_file(struct uufsd* disk, ufs_ino_t dir_ino,
                                    const char* name, uint32_t mode, ufs_ino_t* out_ino) {
    struct fs* fs = &disk->d_fs;
    if (cgread1(disk, 0) <= 0) {
        return ZX_ERR_IO;
    }
    struct cg* cg = &disk->d_cg;
    uint8_t* inosused = cg_inosused(cg);

    ufs_ino_t new_ino = 0;
    for (uint32_t ino = 10; ino < (uint32_t)fs->fs_ipg; ++ino) {
        if (isclr(inosused, ino)) {
            setbit(inosused, ino);
            cg->cg_cs.cs_nifree--;
            fs->fs_cstotal.cs_nifree--;
            new_ino = (ufs_ino_t)ino;
            break;
        }
    }
    if (new_ino == 0) {
        return ZX_ERR_NO_SPACE;
    }

    if (cgwrite1(disk, 0) == -1 || sbwrite(disk, 0) == -1) {
        return ZX_ERR_IO;
    }

    /* Initialize the newly allocated UFS2 dinode */
    union dinodep dp;
    if (getinode(disk, &dp, new_ino) == -1) {
        return ZX_ERR_IO;
    }
    memset(dp.dp2, 0, sizeof(struct ufs2_dinode));
    dp.dp2->di_mode  = (uint16_t)(IFREG | (mode ? (mode & 0777u) : 0644u));
    dp.dp2->di_nlink = 1;
    dp.dp2->di_size  = 0;
    if (putinode(disk) == -1) {
        return ZX_ERR_IO;
    }

    /* Insert directory entry into dir_ino */
    if (getinode(disk, &dp, dir_ino) == -1) {
        return ZX_ERR_IO;
    }
    ufs2_daddr_t dir_frag = dp.dp2->di_db[0];
    if (dir_frag == 0) {
        return ZX_ERR_IO;
    }

    uint8_t blk_buf[4096] __attribute__((aligned(8)));
    if (bread(disk, fsbtodb(fs, dir_frag), blk_buf, sizeof(blk_buf)) == -1) {
        return ZX_ERR_IO;
    }

    size_t name_len = strlen(name);
    uint16_t needed_len = (uint16_t)DIRECTSIZ(name_len);
    bool inserted = false;

    for (uint32_t chunk = 0; chunk < sizeof(blk_buf) && !inserted; chunk += DIRBLKSIZ) {
        uint32_t off = 0;
        while (off < DIRBLKSIZ) {
            struct direct* de = (struct direct*)(blk_buf + chunk + off);
            if (de->d_reclen == 0) {
                break;
            }
            uint16_t actual_len = (de->d_ino == 0) ? 0 : (uint16_t)DIRECTSIZ(de->d_namlen);
            if (de->d_reclen - actual_len >= needed_len) {
                if (de->d_ino == 0) {
                    de->d_ino    = (uint32_t)new_ino;
                    de->d_type   = DT_REG;
                    de->d_namlen = (uint8_t)name_len;
                    memcpy(de->d_name, name, name_len + 1);
                } else {
                    uint16_t rem_len = de->d_reclen - actual_len;
                    de->d_reclen = actual_len;
                    struct direct* next_de = (struct direct*)((uint8_t*)de + actual_len);
                    next_de->d_ino    = (uint32_t)new_ino;
                    next_de->d_reclen = rem_len;
                    next_de->d_type   = DT_REG;
                    next_de->d_namlen = (uint8_t)name_len;
                    memcpy(next_de->d_name, name, name_len + 1);
                }
                inserted = true;
                break;
            }
            off += de->d_reclen;
        }
    }

    if (!inserted) {
        return ZX_ERR_NO_SPACE;
    }

    if (bwrite(disk, fsbtodb(fs, dir_frag), blk_buf, sizeof(blk_buf)) == -1) {
        return ZX_ERR_IO;
    }

    *out_ino = new_ino;
    return ZX_OK;
}

/* ==============================================================================
 * VFS Symlink Expansion & Path Resolution
 * ============================================================================== */

/**
 * @brief Expand virtual in-memory symlinks (/bin -> /System/Bin, /lib -> /System/Libraries).
 */
static void expand_virtual_symlinks(const char* in_path, char* out_path, size_t out_max) {
    if (strcmp(in_path, "/bin") == 0) {
        snprintf(out_path, out_max, "/System/Bin");
    } else if (strncmp(in_path, "/bin/", 5) == 0) {
        snprintf(out_path, out_max, "/System/Bin/%s", in_path + 5);
    } else if (strcmp(in_path, "/lib") == 0) {
        snprintf(out_path, out_max, "/System/Libraries");
    } else if (strncmp(in_path, "/lib/", 5) == 0) {
        snprintf(out_path, out_max, "/System/Libraries/%s", in_path + 5);
    } else {
        snprintf(out_path, out_max, "%s", in_path);
    }
}

/**
 * @brief Resolve an absolute VFS path to its mounted UFS2 volume, parent directory inode,
 *        and final component name.
 */
static zx_status_t resolve_vfs_path(const char* raw_path,
                                    vfs_mount_id_t* out_mount,
                                    struct uufsd** out_disk,
                                    ufs_ino_t* out_parent_ino,
                                    char* out_leaf,
                                    size_t leaf_max,
                                    ufs_ino_t* out_target_ino) {
    char path[256];
    expand_virtual_symlinks(raw_path, path, sizeof(path));

    if (path[0] != '/') {
        return ZX_ERR_INVALID_ARGS;
    }

    struct uufsd* disk = NULL;
    vfs_mount_id_t mount_id = VFS_MOUNT_NONE;
    const char* rel = NULL;
    ufs_ino_t cur_ino = UFS_ROOTINO;

    if (strcmp(path, "/System") == 0 || strncmp(path, "/System/", 8) == 0) {
        if (!g_system_mounted) return ZX_ERR_UNAVAILABLE;
        mount_id = VFS_MOUNT_SYSTEM_RO;
        disk     = &g_ufs_system;
        rel      = (path[7] == '/') ? (path + 8) : "";
        cur_ino  = UFS_ROOTINO;
    } else if (strcmp(path, "/Users") == 0 || strncmp(path, "/Users/", 7) == 0) {
        if (!g_data_mounted) return ZX_ERR_UNAVAILABLE;
        mount_id = VFS_MOUNT_DATA_RW;
        disk     = &g_ufs_data;
        rel      = (path[6] == '/') ? (path + 7) : "";
        /* Look up /Users directory on Data UFS2 (or fall back to rootino if flat) */
        ufs_ino_t users_ino = UFS_ROOTINO;
        if (ufs2_dir_lookup(disk, UFS_ROOTINO, "Users", &users_ino) == ZX_OK) {
            cur_ino = users_ino;
        }
    } else if (strcmp(path, "/Applications") == 0 || strncmp(path, "/Applications/", 14) == 0) {
        if (!g_data_mounted) return ZX_ERR_UNAVAILABLE;
        mount_id = VFS_MOUNT_DATA_RW;
        disk     = &g_ufs_data;
        rel      = (path[13] == '/') ? (path + 14) : "";
        ufs_ino_t apps_ino = UFS_ROOTINO;
        if (ufs2_dir_lookup(disk, UFS_ROOTINO, "Applications", &apps_ino) == ZX_OK) {
            cur_ino = apps_ino;
        }
    } else {
        return ZX_ERR_NOT_FOUND;
    }

    *out_mount = mount_id;
    *out_disk  = disk;

    /* Strip leading slashes in rel */
    while (*rel == '/') rel++;
    if (*rel == '\0') {
        *out_parent_ino = cur_ino;
        out_leaf[0]     = '\0';
        *out_target_ino = cur_ino;
        return ZX_OK;
    }

    /* Walk intermediate directory components */
    char rel_copy[256];
    snprintf(rel_copy, sizeof(rel_copy), "%s", rel);

    char* p = rel_copy;
    while (*p != '\0') {
        char* slash = strchr(p, '/');
        if (slash != NULL) {
            *slash = '\0';
            while (*(slash + 1) == '/') slash++;
            if (*(slash + 1) != '\0') {
                ufs_ino_t next_ino = 0;
                zx_status_t st = ufs2_dir_lookup(disk, cur_ino, p, &next_ino);
                if (st != ZX_OK) return st;
                cur_ino = next_ino;
                p = slash + 1;
                continue;
            }
        }
        /* Final leaf component */
        *out_parent_ino = cur_ino;
        snprintf(out_leaf, leaf_max, "%s", p);
        return ufs2_dir_lookup(disk, cur_ino, p, out_target_ino);
    }

    return ZX_ERR_NOT_FOUND;
}

/* ==============================================================================
 * VFS IPC Request Handlers (IRIS_PROTO_FS)
 * ============================================================================== */

static void handle_fs_open(const iris_msg_hdr_t* req_hdr, const uint8_t* payload,
                           iris_msg_hdr_t* rep_hdr, uint8_t* rep_payload, uint32_t* out_rep_len) {
    if (req_hdr->payload_len < sizeof(iris_fs_open_req_t)) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    const iris_fs_open_req_t* req = (const iris_fs_open_req_t*)payload;
    char safe_path[256];
    memcpy(safe_path, req->path, sizeof(safe_path) - 1);
    safe_path[sizeof(safe_path) - 1] = '\0';

    vfs_mount_id_t mount_id = VFS_MOUNT_NONE;
    struct uufsd* disk = NULL;
    ufs_ino_t parent_ino = 0;
    ufs_ino_t target_ino = 0;
    char leaf[128] = {0};

    zx_status_t st = resolve_vfs_path(safe_path, &mount_id, &disk, &parent_ino,
                                      leaf, sizeof(leaf), &target_ino);

    bool wants_write = (req->flags & VFS_ANY_WRITE_MASK) != 0;

    /* Enforce EROFS (IRIS_ERR_READ_ONLY) on /System write/create/truncate attempts */
    if (mount_id == VFS_MOUNT_SYSTEM_RO && wants_write) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, IRIS_ERR_READ_ONLY, 0);
        return;
    }

    if (st == ZX_ERR_NOT_FOUND && (req->flags & (VFS_O_CREAT | 0x0040u | 0x0200u)) != 0) {
        if (mount_id != VFS_MOUNT_DATA_RW || leaf[0] == '\0') {
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, IRIS_ERR_READ_ONLY, 0);
            return;
        }
        st = ufs2_create_file(disk, parent_ino, leaf, req->mode, &target_ino);
    }

    if (st != ZX_OK) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, st, 0);
        return;
    }

    union dinodep dp;
    if (getinode(disk, &dp, target_ino) == -1) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
        return;
    }

    bool is_dir = ((dp.dp2->di_mode & IFMT) == IFDIR);
    if (is_dir && wants_write) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NOT_FILE, 0);
        return;
    }

    /* Handle O_TRUNC on writable files */
    if (!is_dir && mount_id == VFS_MOUNT_DATA_RW && (req->flags & (VFS_O_TRUNC | 0x0400u)) != 0) {
        dp.dp2->di_size = 0;
        putinode(disk);
    }

    uint64_t file_size = dp.dp2->di_size;

    /* Allocate open file table slot (1-based handle) */
    uint32_t slot = 0;
    for (uint32_t i = 1; i < MAX_OPEN_FILES; ++i) {
        if (!g_open_files[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot == 0) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NO_RESOURCES, 0);
        return;
    }

    g_open_files[slot].in_use   = true;
    g_open_files[slot].mount_id = mount_id;
    g_open_files[slot].writable = wants_write && (mount_id == VFS_MOUNT_DATA_RW);
    g_open_files[slot].ino      = target_ino;
    g_open_files[slot].flags    = req->flags;
    g_open_files[slot].offset   = (req->flags & VFS_O_APPEND) ? file_size : 0;
    g_open_files[slot].size     = file_size;
    g_open_files[slot].is_dir   = is_dir;

    iris_fs_open_resp_t* resp = (iris_fs_open_resp_t*)rep_payload;
    resp->vfs_handle = slot;
    resp->is_dir     = is_dir ? 1u : 0u;
    resp->size       = file_size;

    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, sizeof(iris_fs_open_resp_t));
    *out_rep_len = sizeof(iris_msg_hdr_t) + sizeof(iris_fs_open_resp_t);
}

static void handle_fs_close(const iris_msg_hdr_t* req_hdr, const uint8_t* payload,
                            iris_msg_hdr_t* rep_hdr) {
    if (req_hdr->payload_len < sizeof(iris_fs_close_req_t)) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }
    const iris_fs_close_req_t* req = (const iris_fs_close_req_t*)payload;
    if (req->vfs_handle == 0 || req->vfs_handle >= MAX_OPEN_FILES ||
        !g_open_files[req->vfs_handle].in_use) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BAD_HANDLE, 0);
        return;
    }
    memset(&g_open_files[req->vfs_handle], 0, sizeof(vfs_open_file_t));
    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, 0);
}

static void handle_fs_read(const iris_msg_hdr_t* req_hdr, const uint8_t* payload,
                           iris_msg_hdr_t* rep_hdr, uint8_t* rep_payload, uint32_t* out_rep_len) {
    if (req_hdr->payload_len < sizeof(iris_fs_read_req_t)) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }
    const iris_fs_read_req_t* req = (const iris_fs_read_req_t*)payload;
    if (req->vfs_handle == 0 || req->vfs_handle >= MAX_OPEN_FILES ||
        !g_open_files[req->vfs_handle].in_use) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BAD_HANDLE, 0);
        return;
    }

    vfs_open_file_t* f = &g_open_files[req->vfs_handle];
    struct uufsd* disk = (f->mount_id == VFS_MOUNT_SYSTEM_RO) ? &g_ufs_system : &g_ufs_data;
    struct fs* fs = &disk->d_fs;

    union dinodep dp;
    if (getinode(disk, &dp, f->ino) == -1) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
        return;
    }
    struct ufs2_dinode di = *dp.dp2;
    f->size = di.di_size;

    if (f->offset >= di.di_size || req->count == 0) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, 0);
        return;
    }

    uint32_t to_read = req->count;
    if (to_read > IRIS_IPC_MAX_PAYLOAD) {
        to_read = IRIS_IPC_MAX_PAYLOAD;
    }
    if (f->offset + to_read > di.di_size) {
        to_read = (uint32_t)(di.di_size - f->offset);
    }

    uint8_t blk_buf[4096] __attribute__((aligned(8)));
    uint32_t bytes_copied = 0;

    while (bytes_copied < to_read) {
        uint64_t cur_off = f->offset + bytes_copied;
        uint32_t lbn     = (uint32_t)(cur_off / 4096u);
        uint32_t blk_off = (uint32_t)(cur_off % 4096u);
        uint32_t chunk   = 4096u - blk_off;
        if (chunk > to_read - bytes_copied) {
            chunk = to_read - bytes_copied;
        }

        if (lbn >= UFS_NDADDR) {
            break;
        }

        ufs2_daddr_t frag = di.di_db[lbn];
        if (frag == 0) {
            memset(rep_payload + bytes_copied, 0, chunk);
        } else {
            if (bread(disk, fsbtodb(fs, frag), blk_buf, 4096) == -1) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
                return;
            }
            memcpy(rep_payload + bytes_copied, blk_buf + blk_off, chunk);
        }
        bytes_copied += chunk;
    }

    f->offset += bytes_copied;
    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, bytes_copied);
    *out_rep_len = sizeof(iris_msg_hdr_t) + bytes_copied;
}

static void handle_fs_write(const iris_msg_hdr_t* req_hdr, const uint8_t* payload,
                            iris_msg_hdr_t* rep_hdr, uint8_t* rep_payload, uint32_t* out_rep_len) {
    if (req_hdr->payload_len < sizeof(iris_fs_write_req_t)) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }
    const iris_fs_write_req_t* req = (const iris_fs_write_req_t*)payload;
    if (req->vfs_handle == 0 || req->vfs_handle >= MAX_OPEN_FILES ||
        !g_open_files[req->vfs_handle].in_use) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BAD_HANDLE, 0);
        return;
    }

    vfs_open_file_t* f = &g_open_files[req->vfs_handle];
    if (f->mount_id == VFS_MOUNT_SYSTEM_RO || !f->writable) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, IRIS_ERR_READ_ONLY, 0);
        return;
    }

    if (req_hdr->payload_len < sizeof(iris_fs_write_req_t) + req->count) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BUFFER_TOO_SMALL, 0);
        return;
    }

    const uint8_t* src_data = payload + sizeof(iris_fs_write_req_t);
    struct uufsd* disk = &g_ufs_data;
    struct fs* fs = &disk->d_fs;

    union dinodep dp;
    if (getinode(disk, &dp, f->ino) == -1) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
        return;
    }
    struct ufs2_dinode di = *dp.dp2;

    uint8_t blk_buf[4096] __attribute__((aligned(8)));
    uint32_t bytes_written = 0;

    while (bytes_written < req->count) {
        uint64_t cur_off = f->offset + bytes_written;
        uint32_t lbn     = (uint32_t)(cur_off / 4096u);
        uint32_t blk_off = (uint32_t)(cur_off % 4096u);
        uint32_t chunk   = 4096u - blk_off;
        if (chunk > req->count - bytes_written) {
            chunk = req->count - bytes_written;
        }

        if (lbn >= UFS_NDADDR) {
            break;
        }

        ufs2_daddr_t frag = di.di_db[lbn];
        if (frag == 0) {
            zx_status_t st = ufs2_alloc_block(disk, &frag);
            if (st != ZX_OK) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, st, 0);
                return;
            }
            di.di_db[lbn]  = frag;
            di.di_blocks  += btodb(4096);
            memset(blk_buf, 0, sizeof(blk_buf));
        } else if (blk_off != 0 || chunk < 4096u) {
            if (bread(disk, fsbtodb(fs, frag), blk_buf, 4096) == -1) {
                iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
                return;
            }
        }

        memcpy(blk_buf + blk_off, src_data + bytes_written, chunk);
        if (bwrite(disk, fsbtodb(fs, frag), blk_buf, 4096) == -1) {
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
            return;
        }

        bytes_written += chunk;
    }

    f->offset += bytes_written;
    if (f->offset > di.di_size) {
        di.di_size = f->offset;
    }
    f->size = di.di_size;

    /* Reload and persist updated dinode */
    if (getinode(disk, &dp, f->ino) == -1) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
        return;
    }
    *dp.dp2 = di;
    if (putinode(disk) == -1) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
        return;
    }

    iris_fs_write_resp_t* resp = (iris_fs_write_resp_t*)rep_payload;
    resp->bytes_written = bytes_written;
    resp->reserved      = 0;

    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, sizeof(iris_fs_write_resp_t));
    *out_rep_len = sizeof(iris_msg_hdr_t) + sizeof(iris_fs_write_resp_t);
}

static void handle_fs_seek(const iris_msg_hdr_t* req_hdr, const uint8_t* payload,
                           iris_msg_hdr_t* rep_hdr, uint8_t* rep_payload, uint32_t* out_rep_len) {
    if (req_hdr->payload_len < sizeof(iris_fs_seek_req_t)) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }
    const iris_fs_seek_req_t* req = (const iris_fs_seek_req_t*)payload;
    if (req->vfs_handle == 0 || req->vfs_handle >= MAX_OPEN_FILES ||
        !g_open_files[req->vfs_handle].in_use) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BAD_HANDLE, 0);
        return;
    }

    vfs_open_file_t* f = &g_open_files[req->vfs_handle];
    int64_t base = 0;
    if (req->whence == 0 /* SEEK_SET */) {
        base = 0;
    } else if (req->whence == 1 /* SEEK_CUR */) {
        base = (int64_t)f->offset;
    } else if (req->whence == 2 /* SEEK_END */) {
        base = (int64_t)f->size;
    } else {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    int64_t new_off = base + req->offset;
    if (new_off < 0) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    f->offset = (uint64_t)new_off;
    iris_fs_seek_resp_t* resp = (iris_fs_seek_resp_t*)rep_payload;
    resp->new_offset = new_off;

    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, sizeof(iris_fs_seek_resp_t));
    *out_rep_len = sizeof(iris_msg_hdr_t) + sizeof(iris_fs_seek_resp_t);
}

static void handle_fs_stat(const iris_msg_hdr_t* req_hdr, const uint8_t* payload,
                           iris_msg_hdr_t* rep_hdr, uint8_t* rep_payload, uint32_t* out_rep_len) {
    if (req_hdr->payload_len < sizeof(iris_fs_stat_req_t)) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_INVALID_ARGS, 0);
        return;
    }
    const iris_fs_stat_req_t* req = (const iris_fs_stat_req_t*)payload;
    struct uufsd* disk = NULL;
    ufs_ino_t target_ino = 0;

    if (req->use_handle) {
        if (req->vfs_handle == 0 || req->vfs_handle >= MAX_OPEN_FILES ||
            !g_open_files[req->vfs_handle].in_use) {
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_BAD_HANDLE, 0);
            return;
        }
        vfs_open_file_t* f = &g_open_files[req->vfs_handle];
        disk       = (f->mount_id == VFS_MOUNT_SYSTEM_RO) ? &g_ufs_system : &g_ufs_data;
        target_ino = f->ino;
    } else {
        char safe_path[256];
        memcpy(safe_path, req->path, sizeof(safe_path) - 1);
        safe_path[sizeof(safe_path) - 1] = '\0';

        vfs_mount_id_t mount_id = VFS_MOUNT_NONE;
        ufs_ino_t parent_ino = 0;
        char leaf[128] = {0};
        zx_status_t st = resolve_vfs_path(safe_path, &mount_id, &disk, &parent_ino,
                                          leaf, sizeof(leaf), &target_ino);
        if (st != ZX_OK) {
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, st, 0);
            return;
        }
    }

    union dinodep dp;
    if (getinode(disk, &dp, target_ino) == -1) {
        iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_IO, 0);
        return;
    }

    iris_fs_stat_resp_t* resp = (iris_fs_stat_resp_t*)rep_payload;
    resp->ino       = (uint64_t)target_ino;
    resp->size      = dp.dp2->di_size;
    resp->mode      = dp.dp2->di_mode;
    resp->nlink     = dp.dp2->di_nlink;
    resp->mtime_sec = (uint64_t)dp.dp2->di_mtime;

    iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_OK, sizeof(iris_fs_stat_resp_t));
    *out_rep_len = sizeof(iris_msg_hdr_t) + sizeof(iris_fs_stat_resp_t);
}

/* ==============================================================================
 * VFS IPC Server Loop
 * ============================================================================== */

static void dispatch_fs_message(zx_handle_t ch, const uint8_t* req_buf, uint32_t act_bytes) {
    static uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));

    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        return;
    }

    const iris_msg_hdr_t* req_hdr = (const iris_msg_hdr_t*)req_buf;
    if (!iris_msg_hdr_is_valid(req_hdr, IRIS_PROTO_FS)) {
        return;
    }

    const uint8_t* req_payload = req_buf + sizeof(iris_msg_hdr_t);
    iris_msg_hdr_t* rep_hdr    = (iris_msg_hdr_t*)rep_buf;
    uint8_t* rep_payload       = rep_buf + sizeof(iris_msg_hdr_t);
    uint32_t rep_total_len     = sizeof(iris_msg_hdr_t);

    switch (req_hdr->opcode) {
        case IRIS_FS_OP_OPEN:
            handle_fs_open(req_hdr, req_payload, rep_hdr, rep_payload, &rep_total_len);
            break;
        case IRIS_FS_OP_CLOSE:
            handle_fs_close(req_hdr, req_payload, rep_hdr);
            break;
        case IRIS_FS_OP_READ:
            handle_fs_read(req_hdr, req_payload, rep_hdr, rep_payload, &rep_total_len);
            break;
        case IRIS_FS_OP_WRITE:
            handle_fs_write(req_hdr, req_payload, rep_hdr, rep_payload, &rep_total_len);
            break;
        case IRIS_FS_OP_SEEK:
            handle_fs_seek(req_hdr, req_payload, rep_hdr, rep_payload, &rep_total_len);
            break;
        case IRIS_FS_OP_STAT:
            handle_fs_stat(req_hdr, req_payload, rep_hdr, rep_payload, &rep_total_len);
            break;
        default:
            iris_msg_hdr_init_reply(rep_hdr, req_hdr, ZX_ERR_NOT_SUPPORTED, 0);
            break;
    }

    zx_channel_write(ch, 0, rep_buf, rep_total_len, NULL, 0);
}

static void run_vfs_server(void) {
    static uint8_t req_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t handles[IRIS_IPC_MAX_HANDLES];

    for (;;) {
        zx_wait_item_t items[MAX_FSHOST_CLIENTS];
        uint32_t active_count = 0;

        for (uint32_t i = 0; i < g_num_clients; ++i) {
            if (g_client_channels[i] != ZX_HANDLE_INVALID) {
                items[active_count].handle  = g_client_channels[i];
                items[active_count].waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
                items[active_count].pending = 0;
                active_count++;
            }
        }
        if (active_count == 0) {
            break;
        }

        zx_status_t st = zx_object_wait_many(items, active_count, ZX_TIME_INFINITE);
        if (st != ZX_OK) {
            break;
        }

        for (uint32_t i = 0; i < active_count; ++i) {
            zx_handle_t ch = items[i].handle;
            if (items[i].pending & ZX_CHANNEL_READABLE) {
                for (;;) {
                    uint32_t act_bytes = 0, act_handles = 0;
                    zx_status_t rd_st = zx_channel_read(ch, 0, req_buf, handles,
                                                        sizeof(req_buf), IRIS_IPC_MAX_HANDLES,
                                                        &act_bytes, &act_handles);
                    if (rd_st != ZX_OK) {
                        break;
                    }
                    dispatch_fs_message(ch, req_buf, act_bytes);
                }
            }
            if (items[i].pending & ZX_CHANNEL_PEER_CLOSED) {
                for (uint32_t c = 0; c < g_num_clients; ++c) {
                    if (g_client_channels[c] == ch) {
                        zx_handle_close(ch);
                        g_client_channels[c] = ZX_HANDLE_INVALID;
                    }
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    /* 1. Extract upstream VFS client channels (PA_USER0, 0..7) and downstream storage channels */
    for (uint32_t i = 0; i < MAX_FSHOST_CLIENTS; ++i) {
        zx_handle_t h = zx_take_startup_handle(PA_HND(PA_USER0, i));
        if (h != ZX_HANDLE_INVALID) {
            g_client_channels[g_num_clients++] = h;
        }
    }
    g_blk_channel   = zx_take_startup_handle(PA_HND(PA_USER1, 0));
    g_crypt_channel = zx_take_startup_handle(PA_HND(PA_USER2, 0));

    if (g_num_clients == 0 || g_blk_channel == ZX_HANDLE_INVALID || g_crypt_channel == ZX_HANDLE_INVALID) {
        fshost_log("[FAILED] iris_fshost: Missing required IPC channels (PA_USER0 / PA_USER1 / PA_USER2).\n");
        return 1;
    }

    /* 2. Parse GPT partition table from virtio_blk */
    if (parse_gpt_partitions() != ZX_OK) {
        return 1;
    }

    /* 3. Mount System_A (UFS2 RO at /System) and decrypted Data (UFS2 RW at /Users) */
    if (mount_filesystems() != ZX_OK) {
        return 1;
    }

    /* 4. Serve IRIS_PROTO_FS requests */
    run_vfs_server();

    return 0;
}

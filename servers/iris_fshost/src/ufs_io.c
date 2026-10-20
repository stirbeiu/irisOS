/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_fshost/src/ufs_io.c
 * Purpose: Block-device I/O adapter connecting FreeBSD libufs read/write hooks
 *          to irisOS IPC block channels (IRIS_PROTO_DRIVER / iris_msg_hdr_t).
 *
 * Licensing: BSD 2-Clause / MIT (Zone 1 Permissive).
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
#include <ufs/ffs/fs.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libufs.h>
#include "servers/iris_fshost/include/ufs_io.h"

#ifndef IRIS_HOST_BUILD
#include <zircon/types.h>
#include <zircon/syscalls.h>
#include <iris/ipc.h>

typedef struct iris_ufs_blk_dev {
    bool        in_use;
    bool        read_only;
    zx_handle_t channel;
    uint64_t    start_lba;
    uint64_t    sector_count;
} iris_ufs_blk_dev_t;

static iris_ufs_blk_dev_t g_ufs_devs[IRIS_UFS_MAX_DEVS];
static uint32_t g_ufs_txid = 5000u;

int iris_ufs_register_device(int dev_id,
                             zx_handle_t ipc_channel,
                             uint64_t start_lba,
                             uint64_t sector_count,
                             bool read_only) {
    if (dev_id < 0 || dev_id >= IRIS_UFS_MAX_DEVS || ipc_channel == ZX_HANDLE_INVALID) {
        return -1;
    }
    g_ufs_devs[dev_id].in_use       = true;
    g_ufs_devs[dev_id].read_only    = read_only;
    g_ufs_devs[dev_id].channel      = ipc_channel;
    g_ufs_devs[dev_id].start_lba    = start_lba;
    g_ufs_devs[dev_id].sector_count = sector_count;
    return 0;
}

/**
 * @brief Issue a synchronous IRIS_BLK_OP_READ over IPC for up to 64 sectors.
 */
static zx_status_t ipc_read_sectors(zx_handle_t ch, uint64_t lba, uint32_t count, void* out_buf) {
    uint8_t req_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t)] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
    iris_blk_rw_req_t* rw = (iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_READ, g_ufs_txid++, sizeof(iris_blk_rw_req_t));
    rw->start_sector = lba;
    rw->num_sectors  = count;
    rw->flags        = 0;

    zx_status_t st = zx_channel_write(ch, 0, req_buf, sizeof(req_buf), NULL, 0);
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    static uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK) {
        return st;
    }

    uint32_t expected_bytes = count * IRIS_UFS_SECTOR_SZ;
    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }
    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    if (rep_hdr->status != ZX_OK) {
        return rep_hdr->status;
    }
    if (act_bytes < sizeof(iris_msg_hdr_t) + expected_bytes) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out_buf, rep_buf + sizeof(iris_msg_hdr_t), expected_bytes);
    return ZX_OK;
}

/**
 * @brief Issue a synchronous IRIS_BLK_OP_WRITE over IPC for up to 64 sectors.
 */
static zx_status_t ipc_write_sectors(zx_handle_t ch, uint64_t lba, uint32_t count, const void* in_buf) {
    uint32_t data_bytes = count * IRIS_UFS_SECTOR_SZ;
    uint32_t total_len = (uint32_t)(sizeof(iris_msg_hdr_t) + sizeof(iris_blk_rw_req_t) + data_bytes);
    if (total_len > IRIS_IPC_MAX_MSG_BYTES) {
        return ZX_ERR_INVALID_ARGS;
    }

    static uint8_t req_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
    iris_blk_rw_req_t* rw = (iris_blk_rw_req_t*)(req_buf + sizeof(iris_msg_hdr_t));

    iris_msg_hdr_init(hdr, IRIS_PROTO_DRIVER, IRIS_BLK_OP_WRITE,
                      g_ufs_txid++, (uint32_t)sizeof(iris_blk_rw_req_t) + data_bytes);
    rw->start_sector = lba;
    rw->num_sectors  = count;
    rw->flags        = 0;
    memcpy(rw + 1, in_buf, data_bytes);

    zx_status_t st = zx_channel_write(ch, 0, req_buf, total_len, NULL, 0);
    if (st != ZX_OK) {
        return st;
    }

    zx_signals_t obs = 0;
    st = zx_object_wait_one(ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED, ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) {
        return ZX_ERR_PEER_CLOSED;
    }

    uint8_t rep_buf[128] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t)) {
        return ZX_ERR_IO;
    }
    const iris_msg_hdr_t* rep_hdr = (const iris_msg_hdr_t*)rep_buf;
    return rep_hdr->status;
}

ssize_t iris_ufs_pread(int dev_id, void* buf, size_t size, off_t offset) {
    if (dev_id < 0 || dev_id >= IRIS_UFS_MAX_DEVS || !g_ufs_devs[dev_id].in_use || offset < 0) {
        errno = EBADF;
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    const iris_ufs_blk_dev_t* dev = &g_ufs_devs[dev_id];
    uint8_t* dst = (uint8_t*)buf;
    size_t remaining = size;
    uint64_t cur_off = (uint64_t)offset;

    uint8_t sector_bounce[IRIS_UFS_SECTOR_SZ];

    while (remaining > 0) {
        uint64_t rel_sec = cur_off / IRIS_UFS_SECTOR_SZ;
        uint32_t sec_off = (uint32_t)(cur_off % IRIS_UFS_SECTOR_SZ);

        if (rel_sec >= dev->sector_count) {
            errno = EIO;
            return -1;
        }

        if (sec_off != 0 || remaining < IRIS_UFS_SECTOR_SZ) {
            if (ipc_read_sectors(dev->channel, dev->start_lba + rel_sec, 1, sector_bounce) != ZX_OK) {
                errno = EIO;
                return -1;
            }
            size_t chunk = IRIS_UFS_SECTOR_SZ - sec_off;
            if (chunk > remaining) {
                chunk = remaining;
            }
            memcpy(dst, sector_bounce + sec_off, chunk);
            dst       += chunk;
            cur_off   += chunk;
            remaining -= chunk;
        } else {
            uint32_t num_secs = (uint32_t)(remaining / IRIS_UFS_SECTOR_SZ);
            if (num_secs > 64u) {
                num_secs = 64u;
            }
            if (rel_sec + num_secs > dev->sector_count) {
                num_secs = (uint32_t)(dev->sector_count - rel_sec);
            }
            if (ipc_read_sectors(dev->channel, dev->start_lba + rel_sec, num_secs, dst) != ZX_OK) {
                errno = EIO;
                return -1;
            }
            size_t bytes = (size_t)num_secs * IRIS_UFS_SECTOR_SZ;
            dst       += bytes;
            cur_off   += bytes;
            remaining -= bytes;
        }
    }

    return (ssize_t)size;
}

ssize_t iris_ufs_pwrite(int dev_id, const void* buf, size_t size, off_t offset) {
    if (dev_id < 0 || dev_id >= IRIS_UFS_MAX_DEVS || !g_ufs_devs[dev_id].in_use || offset < 0) {
        errno = EBADF;
        return -1;
    }
    const iris_ufs_blk_dev_t* dev = &g_ufs_devs[dev_id];
    if (dev->read_only) {
        errno = EROFS;
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    const uint8_t* src = (const uint8_t*)buf;
    size_t remaining = size;
    uint64_t cur_off = (uint64_t)offset;

    uint8_t sector_bounce[IRIS_UFS_SECTOR_SZ];

    while (remaining > 0) {
        uint64_t rel_sec = cur_off / IRIS_UFS_SECTOR_SZ;
        uint32_t sec_off = (uint32_t)(cur_off % IRIS_UFS_SECTOR_SZ);

        if (rel_sec >= dev->sector_count) {
            errno = ENOSPC;
            return -1;
        }

        if (sec_off != 0 || remaining < IRIS_UFS_SECTOR_SZ) {
            if (ipc_read_sectors(dev->channel, dev->start_lba + rel_sec, 1, sector_bounce) != ZX_OK) {
                errno = EIO;
                return -1;
            }
            size_t chunk = IRIS_UFS_SECTOR_SZ - sec_off;
            if (chunk > remaining) {
                chunk = remaining;
            }
            memcpy(sector_bounce + sec_off, src, chunk);
            if (ipc_write_sectors(dev->channel, dev->start_lba + rel_sec, 1, sector_bounce) != ZX_OK) {
                errno = EIO;
                return -1;
            }
            src       += chunk;
            cur_off   += chunk;
            remaining -= chunk;
        } else {
            uint32_t num_secs = (uint32_t)(remaining / IRIS_UFS_SECTOR_SZ);
            if (num_secs > 64u) {
                num_secs = 64u;
            }
            if (rel_sec + num_secs > dev->sector_count) {
                num_secs = (uint32_t)(dev->sector_count - rel_sec);
            }
            if (ipc_write_sectors(dev->channel, dev->start_lba + rel_sec, num_secs, src) != ZX_OK) {
                errno = EIO;
                return -1;
            }
            size_t bytes = (size_t)num_secs * IRIS_UFS_SECTOR_SZ;
            src       += bytes;
            cur_off   += bytes;
            remaining -= bytes;
        }
    }

    return (ssize_t)size;
}

#endif /* !IRIS_HOST_BUILD */

/* ==============================================================================
 * libufs Disk Lifecycle Hooks & Castagnoli CRC-32C
 * ============================================================================== */

#define MINE_NAME  0x01
#define MINE_WRITE 0x02

int ufs_disk_write(struct uufsd* disk) {
    if (!disk) {
        return -1;
    }
    if (disk->d_mine & MINE_WRITE) {
        return 0;
    }
    errno = EROFS;
    return -1;
}

int ufs_disk_close(struct uufsd* disk) {
    if (!disk) {
        return -1;
    }
    if (disk->d_inoblock != NULL) {
        free(disk->d_inoblock);
        disk->d_inoblock = NULL;
    }
    if (disk->d_si != NULL) {
        free(disk->d_si->si_csp);
        free(disk->d_si);
        disk->d_si = NULL;
    }
    disk->d_fd = -1;
    return 0;
}

/**
 * @brief Castagnoli CRC-32C (polynomial 0x82F63B78) matching FreeBSD sys/libkern/gsb_crc32.c.
 */
uint32_t calculate_crc32c(uint32_t crc32c, const void* buffer, size_t length) {
    const uint8_t* p = (const uint8_t*)buffer;
    uint32_t crc = crc32c;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0x82F63B78u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

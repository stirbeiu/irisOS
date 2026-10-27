/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_fshost/include/ufs_io.h
 * Purpose: Block-device I/O adapter connecting FreeBSD libufs to irisOS
 *          IPC block protocol (IRIS_PROTO_DRIVER / iris_msg_hdr_t).
 *
 * Licensing: BSD 2-Clause / MIT (Zone 1 Permissive).
 */

#ifndef IRIS_FSHOST_UFS_IO_H_
#define IRIS_FSHOST_UFS_IO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#ifndef IRIS_HOST_BUILD
#include <zircon/types.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_UFS_MAX_DEVS   4
#define IRIS_UFS_SECTOR_SZ  512u

#ifndef IRIS_HOST_BUILD
/**
 * @brief Register an IPC block channel slot for use with libufs (d_fd).
 *
 * @param dev_id       Slot index (0 <= dev_id < IRIS_UFS_MAX_DEVS).
 * @param ipc_channel  Zircon channel connected to virtio_blk or iris_crypt.
 * @param start_lba    Starting 512-byte sector offset of the partition on the channel.
 * @param sector_count Number of 512-byte sectors in the partition.
 * @param read_only    True if writes must be rejected with EROFS.
 * @return 0 on success, -1 on invalid arguments.
 */
int iris_ufs_register_device(int dev_id,
                             zx_handle_t ipc_channel,
                             uint64_t start_lba,
                             uint64_t sector_count,
                             bool read_only);
#endif

/**
 * @brief Hook invoked by libufs (via -Dpread=iris_ufs_pread) to read bytes from a block device.
 */
ssize_t iris_ufs_pread(int dev_id, void* buf, size_t size, off_t offset);

/**
 * @brief Hook invoked by libufs (via -Dpwrite=iris_ufs_pwrite) to write bytes to a block device.
 */
ssize_t iris_ufs_pwrite(int dev_id, const void* buf, size_t size, off_t offset);

#ifndef IRIS_HOST_BUILD
/**
 * @brief Synchronously flush dirty cache buffers of an underlying block device to disk.
 *
 * @param dev_id Slot index of the block device.
 * @return ZX_OK on success, or Zircon error code on failure.
 */
zx_status_t iris_ufs_flush_device(int dev_id);
#endif

/**
 * @brief Castagnoli CRC-32C calculation required by FreeBSD UFS2 metadata check-hashes.
 */
uint32_t calculate_crc32c(uint32_t crc32c, const void* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_FSHOST_UFS_IO_H_ */

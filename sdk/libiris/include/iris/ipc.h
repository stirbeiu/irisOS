/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: sdk/libiris/include/iris/ipc.h
 * Purpose: 24-byte C-ABI IPC wire protocol header and protocol constants.
 *
 * Licensing: MIT License. Permissive Zone 1 SDK header.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef IRIS_IPC_H_
#define IRIS_IPC_H_

/**
 * @file ipc.h
 * @brief 24-byte C-ABI IPC Wire Protocol for irisOS.
 *
 * ARCHITECTURAL DESIGN & SUBSYSTEM COMMUNICATION:
 *  Per Section 4.5 of AGENTS.md, all inter-process communication across Ring 3
 *  processes in irisOS (servers, client libraries, drivers, and user applications)
 *  is conducted over raw Zircon channels (`zx_channel_*`) structured by a standard
 *  24-byte C-ABI header (`iris_msg_hdr_t`).
 *
 *  Upper-layer Google Fuchsia frameworks (FIDL, Scenic, Component Manager) are
 *  strictly excluded. Instead, this wire protocol provides lightweight, deterministic,
 *  zero-copy-capable message passing across user-space boundaries.
 *
 * WIRE FORMAT SPECIFICATION:
 *  +-------------------------------------------------------------------------+
 *  | Offset | Field        | Type     | Description                          |
 *  +-------------------------------------------------------------------------+
 *  | 0x00   | magic        | uint32_t | Must be IRIS_IPC_MAGIC ('IRIS')       |
 *  | 0x04   | protocol     | uint32_t | Subsystem protocol ID (IRIS_PROTO_*) |
 *  | 0x08   | opcode       | uint32_t | Command or event opcode              |
 *  | 0x0C   | txid         | uint32_t | Transaction ID (request/reply match) |
 *  | 0x10   | status       | int32_t  | zx_status_t (ZX_OK or ZX_ERR_*)      |
 *  | 0x14   | payload_len  | uint32_t | Trailing payload size in bytes       |
 *  +-------------------------------------------------------------------------+
 *  Total Header Size: 24 bytes (strictly enforced via _Static_assert).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zircon/types.h>
#include <zircon/errors.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==============================================================================
 * Magic Number & Buffer Limits
 * ============================================================================== */

/**
 * @brief Wire protocol magic identifier: ASCII 'IRIS' (0x49524953).
 */
#define IRIS_IPC_MAGIC 0x49524953u

/**
 * @brief Maximum message payload permitted on a standard Zircon channel (64 KB).
 */
#define IRIS_IPC_MAX_MSG_BYTES 65536u

/**
 * @brief Maximum capability handles transferred per IPC transaction.
 */
#define IRIS_IPC_MAX_HANDLES 64u

/**
 * @brief Maximum payload length trailing the 24-byte header in a single message.
 */
#define IRIS_IPC_MAX_PAYLOAD (IRIS_IPC_MAX_MSG_BYTES - sizeof(iris_msg_hdr_t))

/* ==============================================================================
 * 24-Byte Packed C-ABI Wire Header
 * ============================================================================== */

#pragma pack(push, 4)
/**
 * @brief Standard 24-byte IPC wire protocol header for all irisOS messages.
 */
typedef struct iris_msg_hdr {
    uint32_t magic;       /**< Magic identifier; must equal IRIS_IPC_MAGIC. */
    uint32_t protocol;    /**< Subsystem protocol identifier (IRIS_PROTO_*). */
    uint32_t opcode;      /**< Protocol-specific command or event opcode. */
    uint32_t txid;        /**< Transaction ID for pairing requests with responses. */
    int32_t  status;      /**< Return status (zx_status_t: ZX_OK or negative error). */
    uint32_t payload_len; /**< Byte length of the trailing payload data. */
} iris_msg_hdr_t;
#pragma pack(pop)

/* Compile-time verification of header ABI guarantee */
_Static_assert(sizeof(iris_msg_hdr_t) == 24, "iris_msg_hdr_t ABI broken: must be exactly 24 bytes");

/* ==============================================================================
 * Subsystem Protocol Identifiers (protocol)
 * ============================================================================== */

#define IRIS_PROTO_SYSTEM     0x0001u /**< Core lifecycle, ping/pong, heartbeat, shutdown */
#define IRIS_PROTO_FS         0x0002u /**< VFS mount router & filesystem operations (iris_fshost) */
#define IRIS_PROTO_CRYPT      0x0003u /**< Block-level AES-256-XTS encryption proxy (iris_crypt) */
#define IRIS_PROTO_COMPOSITOR 0x0004u /**< Zero-copy VMO window compositor (iris_compositor) */
#define IRIS_PROTO_PTY        0x0005u /**< Pseudo-terminal server & signal routing (iris_pty) */
#define IRIS_PROTO_DRIVER     0x0006u /**< Hardware driver control & device enumeration */

/* ==============================================================================
 * Standard Subsystem Opcodes (opcode)
 * ============================================================================== */

/**
 * @brief System Protocol Opcodes (IRIS_PROTO_SYSTEM)
 */
#define IRIS_SYS_OP_PING         0x0001u /**< Ping echo request; server replies with PONG */
#define IRIS_SYS_OP_PONG         0x0002u /**< Pong response containing server timestamp */
#define IRIS_SYS_OP_SHUTDOWN     0x0003u /**< Request clean system poweroff */
#define IRIS_SYS_OP_REBOOT       0x0004u /**< Request system restart */
#define IRIS_SYS_OP_GET_VERSION  0x0005u /**< Query irisOS kernel & server version info */
#define IRIS_SYS_OP_HEARTBEAT    0x0006u /**< Watchdog heartbeat ping */

/**
 * @brief VFS Router Protocol Opcodes (IRIS_PROTO_FS)
 */
#define IRIS_FS_OP_OPEN          0x0010u /**< Open file or directory path */
#define IRIS_FS_OP_CLOSE         0x0011u /**< Close open file descriptor */
#define IRIS_FS_OP_READ          0x0012u /**< Read data from open file */
#define IRIS_FS_OP_WRITE         0x0013u /**< Write data to open file */
#define IRIS_FS_OP_SEEK          0x0014u /**< Reposition read/write file offset */
#define IRIS_FS_OP_STAT          0x0015u /**< Query file attributes and metadata */
#define IRIS_FS_OP_READDIR       0x0016u /**< Read directory entries */
#define IRIS_FS_OP_MKDIR         0x0017u /**< Create directory node */
#define IRIS_FS_OP_UNLINK        0x0018u /**< Delete file or directory entry */
#define IRIS_FS_OP_RENAME        0x0019u /**< Rename or move file path */
#define IRIS_FS_OP_TRUNCATE      0x001Au /**< Truncate file to specified size */
#define IRIS_FS_OP_SYNC          0x001Bu /**< Flush cached dirty buffers to storage */

#pragma pack(push, 4)
/**
 * @brief Request payload for IRIS_FS_OP_OPEN.
 */
typedef struct iris_fs_open_req {
    uint32_t flags;      /**< POSIX open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND) */
    uint32_t mode;       /**< Permission mode bits (e.g. 0644) */
    char     path[256];  /**< Null-terminated absolute VFS path */
} iris_fs_open_req_t;

/**
 * @brief Reply payload for IRIS_FS_OP_OPEN.
 */
typedef struct iris_fs_open_resp {
    uint32_t vfs_handle; /**< Server-side open file table slot ID */
    uint32_t is_dir;     /**< 1 if directory, 0 if regular file */
    uint64_t size;       /**< Current file size in bytes */
} iris_fs_open_resp_t;

/**
 * @brief Request payload for IRIS_FS_OP_CLOSE.
 */
typedef struct iris_fs_close_req {
    uint32_t vfs_handle; /**< Server-side open file table slot ID */
    uint32_t reserved;
} iris_fs_close_req_t;

/**
 * @brief Request payload for IRIS_FS_OP_READ.
 */
typedef struct iris_fs_read_req {
    uint32_t vfs_handle; /**< Server-side open file table slot ID */
    uint32_t count;      /**< Maximum bytes to read */
} iris_fs_read_req_t;

/**
 * @brief Request payload for IRIS_FS_OP_WRITE (data bytes immediately follow this struct).
 */
typedef struct iris_fs_write_req {
    uint32_t vfs_handle; /**< Server-side open file table slot ID */
    uint32_t count;      /**< Number of bytes trailing this struct */
} iris_fs_write_req_t;

/**
 * @brief Reply payload for IRIS_FS_OP_WRITE.
 */
typedef struct iris_fs_write_resp {
    uint32_t bytes_written; /**< Actual bytes written */
    uint32_t reserved;
} iris_fs_write_resp_t;

/**
 * @brief Request payload for IRIS_FS_OP_SEEK.
 */
typedef struct iris_fs_seek_req {
    uint32_t vfs_handle; /**< Server-side open file table slot ID */
    int32_t  whence;     /**< SEEK_SET (0), SEEK_CUR (1), or SEEK_END (2) */
    int64_t  offset;     /**< Byte offset */
} iris_fs_seek_req_t;

/**
 * @brief Reply payload for IRIS_FS_OP_SEEK.
 */
typedef struct iris_fs_seek_resp {
    int64_t new_offset;  /**< Updated file offset in bytes */
} iris_fs_seek_resp_t;

/**
 * @brief Request payload for IRIS_FS_OP_STAT.
 */
typedef struct iris_fs_stat_req {
    uint32_t vfs_handle; /**< Server-side open file table slot ID (if use_handle == 1) */
    uint32_t use_handle; /**< 1 to stat vfs_handle (fstat), 0 to stat path */
    char     path[256];  /**< Null-terminated VFS path (if use_handle == 0) */
} iris_fs_stat_req_t;

/**
 * @brief Reply payload for IRIS_FS_OP_STAT.
 */
typedef struct iris_fs_stat_resp {
    uint64_t ino;        /**< Inode number */
    uint64_t size;       /**< File size in bytes */
    uint32_t mode;       /**< POSIX st_mode (S_IFREG | 0644, S_IFDIR | 0755, etc.) */
    uint32_t nlink;      /**< Hard link count */
    uint64_t mtime_sec;  /**< Modification time in seconds since epoch */
} iris_fs_stat_resp_t;
#pragma pack(pop)

/**
 * @brief Cryptographic Storage Protocol Opcodes (IRIS_PROTO_CRYPT)
 */
#define IRIS_CRYPT_OP_STATUS     0x0020u /**< Query partition lock status and salt metadata */
#define IRIS_CRYPT_OP_UNLOCK     0x0021u /**< Unlock Data partition with Argon2 key derivation */
#define IRIS_CRYPT_OP_LOCK       0x0022u /**< Lock Data partition and wipe master key from RAM */
#define IRIS_CRYPT_OP_READ       0x0023u /**< Read and decrypt sector range from Data partition */
#define IRIS_CRYPT_OP_WRITE      0x0024u /**< Encrypt and write sector range to Data partition */

/**
 * @brief Window Compositor Protocol Opcodes (IRIS_PROTO_COMPOSITOR)
 */
#define IRIS_COMP_OP_CREATE      0x0030u /**< Create new client window surface */
#define IRIS_COMP_OP_DESTROY     0x0031u /**< Destroy client window surface */
#define IRIS_COMP_OP_ATTACH_VMO  0x0032u /**< Attach shared pixel VMO to window surface */
#define IRIS_COMP_OP_DAMAGE      0x0033u /**< Post rectangle damage region notification */
#define IRIS_COMP_OP_PRESENT     0x0034u /**< Present completed frame at next 60 Hz vblank */
#define IRIS_COMP_OP_POLL_EVENT  0x0035u /**< Poll pending keyboard or mouse input events */

/**
 * @brief Pseudo-Terminal Protocol Opcodes (IRIS_PROTO_PTY)
 */
#define IRIS_PTY_OP_READ         0x0040u /**< Read line-buffered character stream from PTY */
#define IRIS_PTY_OP_WRITE        0x0041u /**< Write character stream to PTY console */
#define IRIS_PTY_OP_SET_WINDOW   0x0042u /**< Update terminal window row and column dimensions */
#define IRIS_PTY_OP_GET_WINDOW   0x0043u /**< Query terminal window dimensions */
#define IRIS_PTY_OP_DRAIN        0x0044u /**< Flush and drain pending terminal buffers */
#define IRIS_PTY_OP_SIGNAL       0x0045u /**< Send or query POSIX signal (SIGINT, SIGWINCH, etc.) */
#define IRIS_PTY_OP_SET_MODE     0x0046u /**< Set canonical / raw / echo terminal line mode */
#define IRIS_PTY_OP_GET_MODE     0x0047u /**< Query current terminal line mode flags */
#define IRIS_PTY_OP_INJECT       0x0048u /**< Inject raw/synthetic input bytes into PTY stream */
#define IRIS_PTY_OP_OPEN_CLIENT  0x0049u /**< Request new PTY client session channel endpoint */

/* PTY Line Mode Flags */
#define IRIS_PTY_MODE_CANONICAL  (1u << 0) /**< Canonical (cooked) mode: line buffering & backspace editing */
#define IRIS_PTY_MODE_ECHO       (1u << 1) /**< Echo typed characters back to output display */
#define IRIS_PTY_MODE_RAW        0u        /**< Raw mode: immediate character pass-through, no echo */

#pragma pack(push, 4)
/**
 * @brief Request payload for IRIS_PTY_OP_READ.
 */
typedef struct iris_pty_read_req {
    uint32_t count; /**< Maximum number of bytes requested */
} iris_pty_read_req_t;

/**
 * @brief Reply payload for IRIS_PTY_OP_READ (bytes immediately follow this struct).
 */
typedef struct iris_pty_read_resp {
    uint32_t bytes_read; /**< Actual number of bytes returned */
} iris_pty_read_resp_t;

/**
 * @brief Request payload for IRIS_PTY_OP_WRITE (bytes immediately follow this struct).
 */
typedef struct iris_pty_write_req {
    uint32_t count; /**< Number of bytes trailing this struct to write */
} iris_pty_write_req_t;

/**
 * @brief Reply payload for IRIS_PTY_OP_WRITE.
 */
typedef struct iris_pty_write_resp {
    uint32_t bytes_written; /**< Number of bytes successfully written/echoed */
} iris_pty_write_resp_t;

/**
 * @brief Window size payload for IRIS_PTY_OP_SET_WINDOW / IRIS_PTY_OP_GET_WINDOW.
 */
typedef struct iris_pty_window {
    uint16_t rows;   /**< Terminal rows (default: 24) */
    uint16_t cols;   /**< Terminal columns (default: 80) */
    uint16_t xpixel; /**< Horizontal pixels (default: 0) */
    uint16_t ypixel; /**< Vertical pixels (default: 0) */
} iris_pty_window_t;

/**
 * @brief Mode payload for IRIS_PTY_OP_SET_MODE / IRIS_PTY_OP_GET_MODE.
 */
typedef struct iris_pty_mode {
    uint32_t flags; /**< Bitwise OR of IRIS_PTY_MODE_* */
} iris_pty_mode_t;

/**
 * @brief Synthetic input injection payload for IRIS_PTY_OP_INJECT (bytes immediately follow).
 */
typedef struct iris_pty_inject_req {
    uint32_t count; /**< Number of raw/synthetic input bytes trailing this struct */
} iris_pty_inject_req_t;

/**
 * @brief Reply payload for IRIS_PTY_OP_INJECT.
 */
typedef struct iris_pty_inject_resp {
    uint32_t bytes_injected; /**< Number of bytes accepted into line discipline */
} iris_pty_inject_resp_t;

/**
 * @brief Signal payload for IRIS_PTY_OP_SIGNAL.
 */
typedef struct iris_pty_signal_req {
    uint32_t signo; /**< POSIX signal number (e.g. 2 for SIGINT) */
} iris_pty_signal_req_t;
#pragma pack(pop)

/**
 * @brief Hardware Driver / Block Storage Protocol Opcodes (IRIS_PROTO_DRIVER)
 */
#define IRIS_BLK_OP_GET_INFO     0x0050u /**< Query block device capacity and sector geometry */
#define IRIS_BLK_OP_READ         0x0051u /**< Read sector range from block device into memory */
#define IRIS_BLK_OP_WRITE        0x0052u /**< Write sector range from memory to block device */
#define IRIS_BLK_OP_FLUSH        0x0053u /**< Flush dirty cache buffers to non-volatile storage */

#pragma pack(push, 4)
/**
 * @brief Block device geometry info returned by IRIS_BLK_OP_GET_INFO reply.
 */
typedef struct iris_blk_info {
    uint64_t sector_count; /**< Total 512-byte sectors on disk */
    uint32_t sector_size;  /**< Sector size in bytes (normally 512) */
    uint32_t max_transfer; /**< Maximum transfer sectors per request */
} iris_blk_info_t;

/**
 * @brief Block read/write request payload.
 */
typedef struct iris_blk_rw_req {
    uint64_t start_sector; /**< Starting 512-byte sector number */
    uint32_t num_sectors;  /**< Number of sectors to read or write */
    uint32_t flags;        /**< Reserved or operation flags */
    /* For WRITE: data bytes (num_sectors * sector_size) immediately follow this struct */
} iris_blk_rw_req_t;
#pragma pack(pop)

/* ==============================================================================
 * Status Codes & Aliases (status)
 * ============================================================================== */

#define IRIS_OK                   ZX_OK
#define IRIS_ERR_INTERNAL         ZX_ERR_INTERNAL
#define IRIS_ERR_NOT_SUPPORTED    ZX_ERR_NOT_SUPPORTED
#define IRIS_ERR_NO_RESOURCES     ZX_ERR_NO_RESOURCES
#define IRIS_ERR_NO_MEMORY        ZX_ERR_NO_MEMORY
#define IRIS_ERR_INVALID_ARGS     ZX_ERR_INVALID_ARGS
#define IRIS_ERR_BAD_HANDLE       ZX_ERR_BAD_HANDLE
#define IRIS_ERR_WRONG_TYPE       ZX_ERR_WRONG_TYPE
#define IRIS_ERR_ACCESS_DENIED    ZX_ERR_ACCESS_DENIED
#define IRIS_ERR_UNAVAILABLE      ZX_ERR_UNAVAILABLE
#define IRIS_ERR_TIMED_OUT        ZX_ERR_TIMED_OUT
#define IRIS_ERR_NOT_FOUND        ZX_ERR_NOT_FOUND
#define IRIS_ERR_ALREADY_EXISTS   ZX_ERR_ALREADY_EXISTS
#define IRIS_ERR_ALREADY_BOUND    ZX_ERR_ALREADY_BOUND
#define IRIS_ERR_PEER_CLOSED      ZX_ERR_PEER_CLOSED
#define IRIS_ERR_BUFFER_TOO_SMALL ZX_ERR_BUFFER_TOO_SMALL
#define IRIS_ERR_READ_ONLY        (-56) /**< Read-only filesystem (maps to POSIX EROFS) */

/* ==============================================================================
 * Inline Helper Functions
 * ============================================================================== */

/**
 * @brief Initialize an IPC message header for an outgoing request.
 *
 * @param hdr Pointer to the header structure to initialize.
 * @param protocol Target subsystem ID (IRIS_PROTO_*).
 * @param opcode Command opcode.
 * @param txid Caller-allocated transaction ID.
 * @param payload_len Length of data payload trailing the header.
 */
static inline void iris_msg_hdr_init(iris_msg_hdr_t* hdr,
                                     uint32_t protocol,
                                     uint32_t opcode,
                                     uint32_t txid,
                                     uint32_t payload_len) {
    if (!hdr) {
        return;
    }
    hdr->magic = IRIS_IPC_MAGIC;
    hdr->protocol = protocol;
    hdr->opcode = opcode;
    hdr->txid = txid;
    hdr->status = IRIS_OK;
    hdr->payload_len = payload_len;
}

/**
 * @brief Initialize an IPC message header for a reply matching a previous request.
 *
 * @param reply_hdr Pointer to the reply header to populate.
 * @param req_hdr Pointer to the original request header (supplies protocol, opcode, txid).
 * @param status Return status (ZX_OK or negative error code).
 * @param payload_len Length of reply payload in bytes.
 */
static inline void iris_msg_hdr_init_reply(iris_msg_hdr_t* reply_hdr,
                                          const iris_msg_hdr_t* req_hdr,
                                          int32_t status,
                                          uint32_t payload_len) {
    if (!reply_hdr) {
        return;
    }
    reply_hdr->magic = IRIS_IPC_MAGIC;
    reply_hdr->protocol = req_hdr ? req_hdr->protocol : 0;
    reply_hdr->opcode = req_hdr ? req_hdr->opcode : 0;
    reply_hdr->txid = req_hdr ? req_hdr->txid : 0;
    reply_hdr->status = status;
    reply_hdr->payload_len = payload_len;
}

/**
 * @brief Validate header integrity and verify matching protocol expectations.
 *
 * @param hdr Pointer to header to validate.
 * @param expected_protocol Subsystem ID expected (or 0 to accept any protocol).
 * @return true if header magic is valid, protocol matches, and payload is within bounds.
 * @return false otherwise.
 */
static inline bool iris_msg_hdr_is_valid(const iris_msg_hdr_t* hdr, uint32_t expected_protocol) {
    if (!hdr) {
        return false;
    }
    if (hdr->magic != IRIS_IPC_MAGIC) {
        return false;
    }
    if (expected_protocol != 0 && hdr->protocol != expected_protocol) {
        return false;
    }
    if (hdr->payload_len > IRIS_IPC_MAX_PAYLOAD) {
        return false;
    }
    return true;
}

/**
 * @brief Validate whether a received reply header corresponds to an active request.
 *
 * @param reply_hdr The incoming reply header.
 * @param req_hdr The outgoing request header.
 * @return true if magic, protocol, opcode, and txid match.
 * @return false otherwise.
 */
static inline bool iris_msg_hdr_is_reply(const iris_msg_hdr_t* reply_hdr,
                                        const iris_msg_hdr_t* req_hdr) {
    if (!reply_hdr || !req_hdr) {
        return false;
    }
    if (reply_hdr->magic != IRIS_IPC_MAGIC) {
        return false;
    }
    if (reply_hdr->protocol != req_hdr->protocol) {
        return false;
    }
    if (reply_hdr->opcode != req_hdr->opcode) {
        return false;
    }
    if (reply_hdr->txid != req_hdr->txid) {
        return false;
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* IRIS_IPC_H_ */

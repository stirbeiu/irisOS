#ifndef IRIS_FSHOST_USER_SESSION_H
#define IRIS_FSHOST_USER_SESSION_H

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

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <zircon/types.h>
#include <iris/ipc.h>

#include <libufs.h>

typedef ino_t ufs_ino_t;

#define DEV_ID_USER_VAULT 3

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_user_session_state {
    bool          session_active;
    bool          is_guest;
    char          active_user[32];
    uint32_t      crypt_slot_id;
    struct uufsd  ufs_user;
} iris_user_session_state_t;

/**
 * @brief Initialize the in-memory RAMFS (/tmp and /Users/guest).
 */
void ramfs_init(void);

/**
 * @brief Reset ephemeral /Users/guest RAMFS contents (called on Guest login/logout).
 */
void ramfs_reset_guest(void);

/**
 * @brief Check if RAMFS handles a given path (either /tmp... or /Users/guest... when Guest is active).
 */
bool ramfs_handles_path(const char* path, bool is_guest_active, bool* out_is_tmp, const char** out_rel);

zx_status_t ramfs_open(const char* rel_path, bool is_tmp, uint32_t flags, uint32_t mode,
                       uint32_t* out_ino, bool* out_is_dir, uint64_t* out_size);
zx_status_t ramfs_read(uint32_t ino, uint64_t offset, uint32_t count,
                       uint8_t* out_buf, uint32_t* out_bytes);
zx_status_t ramfs_write(uint32_t ino, uint64_t offset, uint32_t count,
                        const uint8_t* data, uint32_t* out_bytes);
zx_status_t ramfs_stat(uint32_t ino, iris_fs_stat_resp_t* out_resp);
zx_status_t ramfs_stat_path(const char* rel_path, bool is_tmp, iris_fs_stat_resp_t* out_resp);
zx_status_t ramfs_readdir(uint32_t ino, uint64_t* io_offset, uint32_t max_bytes,
                          uint8_t* out_buf, uint32_t* out_bytes);
zx_status_t ramfs_mkdir(const char* rel_path, bool is_tmp, uint32_t mode);
zx_status_t ramfs_unlink(const char* rel_path, bool is_tmp);
zx_status_t ramfs_rename(const char* old_rel, const char* new_rel, bool is_tmp);
zx_status_t ramfs_truncate(uint32_t ino, uint64_t new_size);

/**
 * @brief Parse /Users/.accounts from Data partition into iris_fs_user_list_resp_t.
 */
zx_status_t user_session_list_users(struct uufsd* data_disk,
                                    iris_fs_user_list_resp_t* out_resp);

/**
 * @brief Authenticate user passphrase and mount /Users/<user> vault (or Guest RAMFS).
 */
zx_status_t user_session_login(struct uufsd* data_disk,
                               zx_handle_t crypt_ch,
                               const iris_fs_user_login_req_t* req,
                               iris_user_session_state_t* state);

/**
 * @brief Sync & unmount active user space and zeroize per-user key slot.
 */
zx_status_t user_session_logout(struct uufsd* data_disk,
                                zx_handle_t crypt_ch,
                                iris_user_session_state_t* state);

/**
 * @brief Get pointer to mounted user UFS2 disk (if persistent user is logged in).
 */
struct uufsd* user_session_get_disk(iris_user_session_state_t* state);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_FSHOST_USER_SESSION_H */

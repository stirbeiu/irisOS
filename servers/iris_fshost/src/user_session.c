/**
 * @file user_session.c
 * @brief Per-user encrypted UFS2 vault mounting and ephemeral Guest/tmp RAMFS.
 */

#include "user_session.h"
#include "ufs_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <zircon/syscalls.h>

/* ==============================================================================
 * VFS Open Flags (matching servers/iris_fshost/src/main.c)
 * ============================================================================== */

#define US_O_ACCMODE  0x0003u
#define US_O_RDONLY   0x0000u
#define US_O_WRONLY   0x0001u
#define US_O_RDWR     0x0002u
#define US_O_APPEND   0x0004u
#define US_O_CREAT    0x0008u
#define US_O_TRUNC    0x0200u
#define US_CREAT_MASK (US_O_CREAT | 0x0040u)
#define US_TRUNC_MASK (US_O_TRUNC | 0x0200u | 0x0400u)

/* ==============================================================================
 * In-Memory Ephemeral RAMFS (/tmp and /Users/guest)
 * ============================================================================== */

#define RAMFS_MAX_NODES   128u
#define RAMFS_MAX_NAME    64u
#define RAMFS_TMP_ROOT    1u
#define RAMFS_GUEST_ROOT  2u

typedef struct ramfs_node {
    bool     in_use;
    bool     is_dir;
    uint32_t ino;
    uint32_t parent_ino;
    uint32_t mode;
    char     name[RAMFS_MAX_NAME];
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
} ramfs_node_t;

static ramfs_node_t g_ramfs[RAMFS_MAX_NODES];
static bool         g_ramfs_initialized = false;

static void ramfs_clear_subtree(uint32_t root_ino) {
    for (uint32_t i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!g_ramfs[i].in_use) continue;
        if (i == RAMFS_TMP_ROOT || i == RAMFS_GUEST_ROOT) continue;

        /* Walk up parent chain to check if node belongs to root_ino */
        uint32_t cur = g_ramfs[i].parent_ino;
        uint32_t depth = 0;
        bool match = false;
        while (cur != 0 && depth < RAMFS_MAX_NODES) {
            if (cur == root_ino) {
                match = true;
                break;
            }
            if (cur >= RAMFS_MAX_NODES || !g_ramfs[cur].in_use) break;
            if (cur == g_ramfs[cur].parent_ino) break;
            cur = g_ramfs[cur].parent_ino;
            depth++;
        }
        if (match) {
            if (g_ramfs[i].data) {
                memset(g_ramfs[i].data, 0, (size_t)g_ramfs[i].capacity);
                free(g_ramfs[i].data);
            }
            memset(&g_ramfs[i], 0, sizeof(g_ramfs[i]));
        }
    }
}

void ramfs_init(void) {
    if (g_ramfs_initialized) return;
    memset(g_ramfs, 0, sizeof(g_ramfs));

    g_ramfs[RAMFS_TMP_ROOT].in_use     = true;
    g_ramfs[RAMFS_TMP_ROOT].is_dir     = true;
    g_ramfs[RAMFS_TMP_ROOT].ino        = RAMFS_TMP_ROOT;
    g_ramfs[RAMFS_TMP_ROOT].parent_ino = RAMFS_TMP_ROOT;
    g_ramfs[RAMFS_TMP_ROOT].mode       = 0040777u;
    snprintf(g_ramfs[RAMFS_TMP_ROOT].name, RAMFS_MAX_NAME, "tmp");

    g_ramfs[RAMFS_GUEST_ROOT].in_use     = true;
    g_ramfs[RAMFS_GUEST_ROOT].is_dir     = true;
    g_ramfs[RAMFS_GUEST_ROOT].ino        = RAMFS_GUEST_ROOT;
    g_ramfs[RAMFS_GUEST_ROOT].parent_ino = RAMFS_GUEST_ROOT;
    g_ramfs[RAMFS_GUEST_ROOT].mode       = 0040755u;
    snprintf(g_ramfs[RAMFS_GUEST_ROOT].name, RAMFS_MAX_NAME, "guest");

    g_ramfs_initialized = true;
}

void ramfs_reset_guest(void) {
    ramfs_init();
    ramfs_clear_subtree(RAMFS_GUEST_ROOT);
}

bool ramfs_handles_path(const char* path, bool is_guest_active, bool* out_is_tmp, const char** out_rel) {
    if (!path) return false;
    if (strcmp(path, "/tmp") == 0) {
        if (out_is_tmp) *out_is_tmp = true;
        if (out_rel) *out_rel = "";
        return true;
    }
    if (strncmp(path, "/tmp/", 5) == 0) {
        if (out_is_tmp) *out_is_tmp = true;
        if (out_rel) *out_rel = path + 5;
        return true;
    }
    if (is_guest_active) {
        if (strcmp(path, "/Users/guest") == 0) {
            if (out_is_tmp) *out_is_tmp = false;
            if (out_rel) *out_rel = "";
            return true;
        }
        if (strncmp(path, "/Users/guest/", 13) == 0) {
            if (out_is_tmp) *out_is_tmp = false;
            if (out_rel) *out_rel = path + 13;
            return true;
        }
    }
    return false;
}

static uint32_t ramfs_find_child(uint32_t parent_ino, const char* name) {
    for (uint32_t i = 1; i < RAMFS_MAX_NODES; i++) {
        if (g_ramfs[i].in_use && i != parent_ino &&
            g_ramfs[i].parent_ino == parent_ino &&
            strcmp(g_ramfs[i].name, name) == 0) {
            return i;
        }
    }
    return 0;
}

static uint32_t ramfs_alloc_node(uint32_t parent_ino, const char* name, bool is_dir, uint32_t mode) {
    for (uint32_t i = 3; i < RAMFS_MAX_NODES; i++) {
        if (!g_ramfs[i].in_use) {
            memset(&g_ramfs[i], 0, sizeof(g_ramfs[i]));
            g_ramfs[i].in_use     = true;
            g_ramfs[i].is_dir     = is_dir;
            g_ramfs[i].ino        = i;
            g_ramfs[i].parent_ino = parent_ino;
            g_ramfs[i].mode       = is_dir ? (0040000u | (mode & 0777u)) : (0100000u | (mode & 0777u));
            snprintf(g_ramfs[i].name, RAMFS_MAX_NAME, "%s", name);
            return i;
        }
    }
    return 0;
}

static zx_status_t ramfs_resolve(const char* rel_path, bool is_tmp,
                                 uint32_t* out_parent, char* out_leaf, size_t leaf_sz,
                                 uint32_t* out_ino) {
    ramfs_init();
    uint32_t root = is_tmp ? RAMFS_TMP_ROOT : RAMFS_GUEST_ROOT;
    while (*rel_path == '/') rel_path++;

    if (*rel_path == '\0') {
        if (out_parent) *out_parent = root;
        if (out_leaf && leaf_sz > 0) out_leaf[0] = '\0';
        if (out_ino) *out_ino = root;
        return ZX_OK;
    }

    char copy[256];
    snprintf(copy, sizeof(copy), "%s", rel_path);

    uint32_t cur = root;
    char* p = copy;
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;
        char* slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
            if (strcmp(p, ".") == 0) {
                p = slash + 1;
                continue;
            }
            if (strcmp(p, "..") == 0) {
                cur = g_ramfs[cur].parent_ino;
                p = slash + 1;
                continue;
            }
            uint32_t next = ramfs_find_child(cur, p);
            if (next == 0) return ZX_ERR_NOT_FOUND;
            if (!g_ramfs[next].is_dir) return ZX_ERR_NOT_DIR;
            cur = next;
            p = slash + 1;
        } else {
            if (strcmp(p, ".") == 0) {
                if (out_parent) *out_parent = g_ramfs[cur].parent_ino;
                if (out_leaf && leaf_sz > 0) snprintf(out_leaf, leaf_sz, "%s", g_ramfs[cur].name);
                if (out_ino) *out_ino = cur;
                return ZX_OK;
            }
            if (strcmp(p, "..") == 0) {
                uint32_t par = g_ramfs[cur].parent_ino;
                if (out_parent) *out_parent = g_ramfs[par].parent_ino;
                if (out_leaf && leaf_sz > 0) snprintf(out_leaf, leaf_sz, "%s", g_ramfs[par].name);
                if (out_ino) *out_ino = par;
                return ZX_OK;
            }
            if (out_parent) *out_parent = cur;
            if (out_leaf && leaf_sz > 0) snprintf(out_leaf, leaf_sz, "%s", p);
            uint32_t found = ramfs_find_child(cur, p);
            if (out_ino) *out_ino = found;
            return (found != 0) ? ZX_OK : ZX_ERR_NOT_FOUND;
        }
    }

    if (out_parent) *out_parent = root;
    if (out_leaf && leaf_sz > 0) out_leaf[0] = '\0';
    if (out_ino) *out_ino = cur;
    return ZX_OK;
}

zx_status_t ramfs_open(const char* rel_path, bool is_tmp, uint32_t flags, uint32_t mode,
                       uint32_t* out_ino, bool* out_is_dir, uint64_t* out_size) {
    uint32_t parent_ino = 0, target_ino = 0;
    char leaf[RAMFS_MAX_NAME] = {0};
    zx_status_t st = ramfs_resolve(rel_path, is_tmp, &parent_ino, leaf, sizeof(leaf), &target_ino);

    if (st == ZX_ERR_NOT_FOUND) {
        if ((flags & US_CREAT_MASK) && leaf[0] != '\0' && parent_ino != 0) {
            target_ino = ramfs_alloc_node(parent_ino, leaf, false, mode ? mode : 0644u);
            if (target_ino == 0) return ZX_ERR_NO_MEMORY;
        } else {
            return ZX_ERR_NOT_FOUND;
        }
    } else if (st != ZX_OK) {
        return st;
    } else {
        if (!g_ramfs[target_ino].is_dir && (flags & US_TRUNC_MASK)) {
            if ((flags & US_O_ACCMODE) != US_O_RDONLY) {
                g_ramfs[target_ino].size = 0;
            }
        }
    }

    if (out_ino) *out_ino = target_ino;
    if (out_is_dir) *out_is_dir = g_ramfs[target_ino].is_dir;
    if (out_size) *out_size = g_ramfs[target_ino].size;
    return ZX_OK;
}

zx_status_t ramfs_read(uint32_t ino, uint64_t offset, uint32_t count,
                       uint8_t* out_buf, uint32_t* out_bytes) {
    if (ino == 0 || ino >= RAMFS_MAX_NODES || !g_ramfs[ino].in_use) return ZX_ERR_BAD_HANDLE;
    ramfs_node_t* n = &g_ramfs[ino];
    if (n->is_dir) return ZX_ERR_NOT_FILE;
    if (offset >= n->size || count == 0) {
        *out_bytes = 0;
        return ZX_OK;
    }
    uint64_t avail = n->size - offset;
    uint32_t to_read = (count < avail) ? count : (uint32_t)avail;
    if (to_read > IRIS_IPC_MAX_PAYLOAD) to_read = IRIS_IPC_MAX_PAYLOAD;
    memcpy(out_buf, n->data + offset, to_read);
    *out_bytes = to_read;
    return ZX_OK;
}

zx_status_t ramfs_write(uint32_t ino, uint64_t offset, uint32_t count,
                        const uint8_t* data, uint32_t* out_bytes) {
    if (ino == 0 || ino >= RAMFS_MAX_NODES || !g_ramfs[ino].in_use) return ZX_ERR_BAD_HANDLE;
    ramfs_node_t* n = &g_ramfs[ino];
    if (n->is_dir) return ZX_ERR_NOT_FILE;

    uint64_t needed = offset + count;
    if (needed > 4u * 1024u * 1024u) return ZX_ERR_NO_SPACE;
    if (needed > n->capacity) {
        uint64_t new_cap = (n->capacity == 0) ? 4096u : n->capacity;
        while (new_cap < needed) new_cap *= 2u;
        uint8_t* nb = (uint8_t*)realloc(n->data, (size_t)new_cap);
        if (!nb) return ZX_ERR_NO_MEMORY;
        memset(nb + n->capacity, 0, (size_t)(new_cap - n->capacity));
        n->data = nb;
        n->capacity = new_cap;
    }
    if (count > 0) {
        memcpy(n->data + offset, data, count);
    }
    if (needed > n->size) n->size = needed;
    *out_bytes = count;
    return ZX_OK;
}

zx_status_t ramfs_stat(uint32_t ino, iris_fs_stat_resp_t* out_resp) {
    if (ino == 0 || ino >= RAMFS_MAX_NODES || !g_ramfs[ino].in_use) return ZX_ERR_BAD_HANDLE;
    ramfs_node_t* n = &g_ramfs[ino];
    out_resp->ino       = n->ino;
    out_resp->size      = n->is_dir ? 4096u : n->size;
    out_resp->mode      = n->mode;
    out_resp->nlink     = 1;
    out_resp->mtime_sec = 0;
    return ZX_OK;
}

zx_status_t ramfs_stat_path(const char* rel_path, bool is_tmp, iris_fs_stat_resp_t* out_resp) {
    uint32_t ino = 0;
    zx_status_t st = ramfs_resolve(rel_path, is_tmp, NULL, NULL, 0, &ino);
    if (st != ZX_OK) return st;
    return ramfs_stat(ino, out_resp);
}

zx_status_t ramfs_readdir(uint32_t ino, uint64_t* io_offset, uint32_t max_bytes,
                          uint8_t* out_buf, uint32_t* out_bytes) {
    if (ino == 0 || ino >= RAMFS_MAX_NODES || !g_ramfs[ino].in_use) return ZX_ERR_BAD_HANDLE;
    if (!g_ramfs[ino].is_dir) return ZX_ERR_NOT_DIR;
    if (max_bytes > IRIS_IPC_MAX_PAYLOAD) max_bytes = IRIS_IPC_MAX_PAYLOAD;

    uint32_t packed = 0;
    uint64_t idx = 0;

    /* Synthetic . and .. plus children */
    for (int special = 0; special < 2; special++) {
        const char* nm = (special == 0) ? "." : "..";
        uint64_t eino = (special == 0) ? ino : g_ramfs[ino].parent_ino;
        if (idx >= *io_offset) {
            size_t nlen = strlen(nm);
            uint16_t reclen = (uint16_t)((19u + nlen + 1u + 7u) & ~7u);
            if (packed + reclen > max_bytes) {
                *out_bytes = packed;
                return ZX_OK;
            }
            iris_dirent_t* d = (iris_dirent_t*)(out_buf + packed);
            d->d_ino    = eino;
            d->d_off    = idx + 1;
            d->d_reclen = reclen;
            d->d_type   = DT_DIR;
            memcpy(d->d_name, nm, nlen + 1);
            packed += reclen;
            *io_offset = idx + 1;
        }
        idx++;
    }

    for (uint32_t i = 3; i < RAMFS_MAX_NODES; i++) {
        if (!g_ramfs[i].in_use || g_ramfs[i].parent_ino != ino) continue;
        if (idx >= *io_offset) {
            size_t nlen = strlen(g_ramfs[i].name);
            uint16_t reclen = (uint16_t)((19u + nlen + 1u + 7u) & ~7u);
            if (packed + reclen > max_bytes) {
                *out_bytes = packed;
                return ZX_OK;
            }
            iris_dirent_t* d = (iris_dirent_t*)(out_buf + packed);
            d->d_ino    = g_ramfs[i].ino;
            d->d_off    = idx + 1;
            d->d_reclen = reclen;
            d->d_type   = g_ramfs[i].is_dir ? DT_DIR : DT_REG;
            memcpy(d->d_name, g_ramfs[i].name, nlen + 1);
            packed += reclen;
            *io_offset = idx + 1;
        }
        idx++;
    }

    *out_bytes = packed;
    return ZX_OK;
}

zx_status_t ramfs_mkdir(const char* rel_path, bool is_tmp, uint32_t mode) {
    uint32_t parent_ino = 0, target_ino = 0;
    char leaf[RAMFS_MAX_NAME] = {0};
    zx_status_t st = ramfs_resolve(rel_path, is_tmp, &parent_ino, leaf, sizeof(leaf), &target_ino);
    if (st == ZX_OK) return ZX_ERR_ALREADY_EXISTS;
    if (st != ZX_ERR_NOT_FOUND || leaf[0] == '\0' || parent_ino == 0) return st;
    if (ramfs_alloc_node(parent_ino, leaf, true, mode ? mode : 0755u) == 0) return ZX_ERR_NO_MEMORY;
    return ZX_OK;
}

zx_status_t ramfs_unlink(const char* rel_path, bool is_tmp) {
    uint32_t parent_ino = 0, target_ino = 0;
    char leaf[RAMFS_MAX_NAME] = {0};
    zx_status_t st = ramfs_resolve(rel_path, is_tmp, &parent_ino, leaf, sizeof(leaf), &target_ino);
    if (st != ZX_OK) return st;
    if (target_ino == RAMFS_TMP_ROOT || target_ino == RAMFS_GUEST_ROOT) return ZX_ERR_ACCESS_DENIED;
    if (g_ramfs[target_ino].is_dir) {
        for (uint32_t i = 3; i < RAMFS_MAX_NODES; i++) {
            if (g_ramfs[i].in_use && g_ramfs[i].parent_ino == target_ino) {
                return ZX_ERR_NOT_EMPTY;
            }
        }
    }
    if (g_ramfs[target_ino].data) {
        memset(g_ramfs[target_ino].data, 0, (size_t)g_ramfs[target_ino].capacity);
        free(g_ramfs[target_ino].data);
    }
    memset(&g_ramfs[target_ino], 0, sizeof(g_ramfs[target_ino]));
    return ZX_OK;
}

zx_status_t ramfs_rename(const char* old_rel, const char* new_rel, bool is_tmp) {
    uint32_t old_parent = 0, old_ino = 0;
    char old_leaf[RAMFS_MAX_NAME] = {0};
    zx_status_t st = ramfs_resolve(old_rel, is_tmp, &old_parent, old_leaf, sizeof(old_leaf), &old_ino);
    if (st != ZX_OK) return st;
    if (old_ino == RAMFS_TMP_ROOT || old_ino == RAMFS_GUEST_ROOT) return ZX_ERR_ACCESS_DENIED;

    uint32_t new_parent = 0, new_ino = 0;
    char new_leaf[RAMFS_MAX_NAME] = {0};
    st = ramfs_resolve(new_rel, is_tmp, &new_parent, new_leaf, sizeof(new_leaf), &new_ino);
    if (st == ZX_OK) {
        if (new_ino == old_ino) return ZX_OK;
        if (g_ramfs[new_ino].is_dir) return ZX_ERR_NOT_FILE;
        if (g_ramfs[new_ino].data) free(g_ramfs[new_ino].data);
        memset(&g_ramfs[new_ino], 0, sizeof(g_ramfs[new_ino]));
    } else if (st != ZX_ERR_NOT_FOUND || new_leaf[0] == '\0' || new_parent == 0) {
        return st;
    }

    g_ramfs[old_ino].parent_ino = new_parent;
    snprintf(g_ramfs[old_ino].name, RAMFS_MAX_NAME, "%s", new_leaf);
    return ZX_OK;
}

zx_status_t ramfs_truncate(uint32_t ino, uint64_t new_size) {
    if (ino == 0 || ino >= RAMFS_MAX_NODES || !g_ramfs[ino].in_use) return ZX_ERR_BAD_HANDLE;
    ramfs_node_t* n = &g_ramfs[ino];
    if (n->is_dir) return ZX_ERR_NOT_FILE;
    if (new_size > 4u * 1024u * 1024u) return ZX_ERR_NO_SPACE;
    if (new_size > n->capacity) {
        uint8_t* nb = (uint8_t*)realloc(n->data, (size_t)new_size);
        if (!nb) return ZX_ERR_NO_MEMORY;
        memset(nb + n->capacity, 0, (size_t)(new_size - n->capacity));
        n->data = nb;
        n->capacity = new_size;
    } else if (new_size > n->size && n->data) {
        memset(n->data + n->size, 0, (size_t)(new_size - n->size));
    }
    n->size = new_size;
    return ZX_OK;
}

/* ==============================================================================
 * UFS2 Vault Backing & iris_crypt Slot IPC
 * ============================================================================== */

#define MAX_VAULT_BLOCKS 2048u /* Up to 8 MB vault image at 4 KB blocks */

static struct uufsd* g_vault_backing_disk = NULL;
static zx_handle_t   g_vault_crypt_ch     = ZX_HANDLE_INVALID;
static uint32_t      g_vault_slot_id      = 0;
static uint64_t      g_vault_file_size    = 0;
static uint32_t      g_vault_num_blocks   = 0;
static ufs2_daddr_t  g_vault_lbn_frags[MAX_VAULT_BLOCKS];

static zx_status_t ufs2_lookup_dir_entry(struct uufsd* disk, ufs_ino_t dir_ino,
                                         const char* name, ufs_ino_t* out_ino) {
    union dinodep dp;
    if (getinode(disk, &dp, dir_ino) == -1) return ZX_ERR_IO;
    struct ufs2_dinode di = *dp.dp2;
    if ((di.di_mode & IFMT) != IFDIR) return ZX_ERR_NOT_DIR;

    struct fs* fs = &disk->d_fs;
    uint8_t blk[4096] __attribute__((aligned(8)));
    uint64_t off = 0;
    while (off < di.di_size) {
        uint32_t lbn = (uint32_t)(off / 4096u);
        if (lbn >= UFS_NDADDR || di.di_db[lbn] == 0) break;
        if (bread(disk, fsbtodb(fs, di.di_db[lbn]), blk, 4096) == -1) return ZX_ERR_IO;

        uint32_t pos = 0;
        while (pos < 4096u && off + pos < di.di_size) {
            struct direct* de = (struct direct*)(blk + pos);
            if (de->d_reclen == 0) break;
            if (de->d_ino != 0 && de->d_namlen == strlen(name) &&
                memcmp(de->d_name, name, de->d_namlen) == 0) {
                *out_ino = de->d_ino;
                return ZX_OK;
            }
            pos += de->d_reclen;
        }
        off += 4096u;
    }
    return ZX_ERR_NOT_FOUND;
}

static zx_status_t ufs2_lookup_abs(struct uufsd* disk, const char* path, ufs_ino_t* out_ino) {
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", path);
    ufs_ino_t cur = UFS_ROOTINO;
    char* p = copy;
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;
        char* slash = strchr(p, '/');
        if (slash) *slash = '\0';
        zx_status_t st = ufs2_lookup_dir_entry(disk, cur, p, &cur);
        if (st != ZX_OK) return st;
        if (!slash) break;
        p = slash + 1;
    }
    *out_ino = cur;
    return ZX_OK;
}

static zx_status_t ufs2_cache_file_blocks(struct uufsd* disk, ufs_ino_t ino) {
    union dinodep dp;
    if (getinode(disk, &dp, ino) == -1) return ZX_ERR_IO;
    struct ufs2_dinode di = *dp.dp2;
    struct fs* fs = &disk->d_fs;

    g_vault_file_size  = di.di_size;
    g_vault_num_blocks = (uint32_t)((di.di_size + 4095u) / 4096u);
    if (g_vault_num_blocks > MAX_VAULT_BLOCKS) return ZX_ERR_OUT_OF_RANGE;
    memset(g_vault_lbn_frags, 0, sizeof(g_vault_lbn_frags));

    uint32_t nindir = (uint32_t)(fs->fs_bsize / sizeof(ufs2_daddr_t));
    if (nindir == 0 || nindir > 512u) return ZX_ERR_INVALID_ARGS;
    static ufs2_daddr_t ib0[512] __attribute__((aligned(8)));
    static ufs2_daddr_t ib1_l1[512] __attribute__((aligned(8)));
    static ufs2_daddr_t ib1_l2[512] __attribute__((aligned(8)));
    bool ib0_loaded = false;
    bool ib1_l1_loaded = false;
    uint32_t ib1_l2_idx = UINT32_MAX;

    for (uint32_t lbn = 0; lbn < g_vault_num_blocks; lbn++) {
        if (lbn < UFS_NDADDR) {
            g_vault_lbn_frags[lbn] = di.di_db[lbn];
        } else {
            uint32_t rel = lbn - UFS_NDADDR;
            if (rel < nindir) {
                if (di.di_ib[0] == 0) return ZX_ERR_IO_DATA_INTEGRITY;
                if (!ib0_loaded) {
                    if (bread(disk, fsbtodb(fs, di.di_ib[0]), ib0, 4096) == -1) return ZX_ERR_IO;
                    ib0_loaded = true;
                }
                g_vault_lbn_frags[lbn] = ib0[rel];
            } else {
                uint32_t drel = rel - nindir;
                uint32_t l1_idx = drel / nindir;
                uint32_t l2_idx = drel % nindir;
                if (l1_idx >= nindir) return ZX_ERR_OUT_OF_RANGE;
                if (di.di_ib[1] == 0) return ZX_ERR_IO_DATA_INTEGRITY;
                if (!ib1_l1_loaded) {
                    if (bread(disk, fsbtodb(fs, di.di_ib[1]), ib1_l1, 4096) == -1) return ZX_ERR_IO;
                    ib1_l1_loaded = true;
                }
                if (ib1_l1[l1_idx] == 0) return ZX_ERR_IO_DATA_INTEGRITY;
                if (ib1_l2_idx != l1_idx) {
                    if (bread(disk, fsbtodb(fs, ib1_l1[l1_idx]), ib1_l2, 4096) == -1) return ZX_ERR_IO;
                    ib1_l2_idx = l1_idx;
                }
                g_vault_lbn_frags[lbn] = ib1_l2[l2_idx];
            }
        }
    }
    return ZX_OK;
}

static zx_status_t vault_crypt_ipc(uint32_t encrypt, uint64_t start_lba, uint32_t count,
                                   const uint8_t* in_data, uint8_t* out_data) {
    static uint32_t txid = 9100;
    static uint8_t req_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    static uint8_t rep_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));

    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > 64u) chunk = 64u;
        uint32_t bytes = chunk * 512u;

        iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
        iris_crypt_sectors_req_t* req = (iris_crypt_sectors_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
        iris_msg_hdr_init(hdr, IRIS_PROTO_CRYPT, IRIS_CRYPT_OP_CRYPT_SECTORS, txid++,
                          (uint32_t)(sizeof(iris_crypt_sectors_req_t) + bytes));
        req->slot_id      = g_vault_slot_id;
        req->encrypt      = encrypt;
        req->start_lba    = start_lba + done;
        req->sector_count = chunk;
        memcpy(req_buf + sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_sectors_req_t),
               in_data + done * 512u, bytes);

        zx_status_t st = zx_channel_write(
            g_vault_crypt_ch, 0, req_buf,
            (uint32_t)(sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_sectors_req_t) + bytes),
            NULL, 0);
        if (st != ZX_OK) return st;

        zx_signals_t obs = 0;
        st = zx_object_wait_one(g_vault_crypt_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                ZX_TIME_INFINITE, &obs);
        if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) return ZX_ERR_PEER_CLOSED;

        uint32_t act_bytes = 0, act_handles = 0;
        st = zx_channel_read(g_vault_crypt_ch, 0, rep_buf, NULL, sizeof(rep_buf), 0,
                             &act_bytes, &act_handles);
        if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t)) return ZX_ERR_IO;
        const iris_msg_hdr_t* rh = (const iris_msg_hdr_t*)rep_buf;
        if (rh->status != ZX_OK) return rh->status;
        if (act_bytes < sizeof(iris_msg_hdr_t) + bytes) return ZX_ERR_IO;
        memcpy(out_data + done * 512u, rep_buf + sizeof(iris_msg_hdr_t), bytes);
        done += chunk;
    }
    return ZX_OK;
}

static zx_status_t vault_read_raw_sectors(uint64_t phys_lba, uint32_t count, void* out_buf) {
    struct fs* fs = &g_vault_backing_disk->d_fs;
    uint8_t blk[4096] __attribute__((aligned(8)));
    uint8_t* dst = (uint8_t*)out_buf;

    uint32_t done = 0;
    while (done < count) {
        uint64_t cur_lba = phys_lba + done;
        uint32_t lbn     = (uint32_t)(cur_lba / 8u);
        uint32_t sec_off = (uint32_t)(cur_lba % 8u);
        uint32_t chunk   = 8u - sec_off;
        if (chunk > count - done) chunk = count - done;
        if (lbn >= g_vault_num_blocks || g_vault_lbn_frags[lbn] == 0) return ZX_ERR_OUT_OF_RANGE;

        if (bread(g_vault_backing_disk, fsbtodb(fs, g_vault_lbn_frags[lbn]), blk, 4096) == -1) {
            return ZX_ERR_IO;
        }
        memcpy(dst + done * 512u, blk + sec_off * 512u, chunk * 512u);
        done += chunk;
    }
    return ZX_OK;
}

static zx_status_t vault_write_raw_sectors(uint64_t phys_lba, uint32_t count, const void* in_buf) {
    struct fs* fs = &g_vault_backing_disk->d_fs;
    uint8_t blk[4096] __attribute__((aligned(8)));
    const uint8_t* src = (const uint8_t*)in_buf;

    uint32_t done = 0;
    while (done < count) {
        uint64_t cur_lba = phys_lba + done;
        uint32_t lbn     = (uint32_t)(cur_lba / 8u);
        uint32_t sec_off = (uint32_t)(cur_lba % 8u);
        uint32_t chunk   = 8u - sec_off;
        if (chunk > count - done) chunk = count - done;
        if (lbn >= g_vault_num_blocks || g_vault_lbn_frags[lbn] == 0) return ZX_ERR_OUT_OF_RANGE;

        if (sec_off != 0 || chunk < 8u) {
            if (bread(g_vault_backing_disk, fsbtodb(fs, g_vault_lbn_frags[lbn]), blk, 4096) == -1) {
                return ZX_ERR_IO;
            }
        }
        memcpy(blk + sec_off * 512u, src + done * 512u, chunk * 512u);
        if (bwrite(g_vault_backing_disk, fsbtodb(fs, g_vault_lbn_frags[lbn]), blk, 4096) == -1) {
            return ZX_ERR_IO;
        }
        done += chunk;
    }
    return ZX_OK;
}

static zx_status_t vault_custom_read(uint64_t logical_lba, uint32_t count, void* out_buf) {
    static uint8_t cipher_buf[64u * 512u] __attribute__((aligned(8)));
    uint8_t* dst = (uint8_t*)out_buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > 64u) chunk = 64u;
        zx_status_t st = vault_read_raw_sectors(logical_lba + 8u + done, chunk, cipher_buf);
        if (st != ZX_OK) return st;
        st = vault_crypt_ipc(0, logical_lba + done, chunk, cipher_buf, dst + done * 512u);
        if (st != ZX_OK) return st;
        done += chunk;
    }
    return ZX_OK;
}

static zx_status_t vault_custom_write(uint64_t logical_lba, uint32_t count, const void* in_buf) {
    static uint8_t cipher_buf[64u * 512u] __attribute__((aligned(8)));
    const uint8_t* src = (const uint8_t*)in_buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > 64u) chunk = 64u;
        zx_status_t st = vault_crypt_ipc(1, logical_lba + done, chunk, src + done * 512u, cipher_buf);
        if (st != ZX_OK) return st;
        st = vault_write_raw_sectors(logical_lba + 8u + done, chunk, cipher_buf);
        if (st != ZX_OK) return st;
        done += chunk;
    }
    return ZX_OK;
}

static zx_status_t vault_custom_flush(void) {
    return ZX_OK;
}

/* ==============================================================================
 * User Accounts Parser (/Users/.accounts) & Session Lifecycle
 * ============================================================================== */

static void trim_cr(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

zx_status_t user_session_list_users(struct uufsd* data_disk,
                                    iris_fs_user_list_resp_t* out_resp) {
    if (!data_disk || !out_resp) return ZX_ERR_INVALID_ARGS;
    memset(out_resp, 0, sizeof(*out_resp));

    ufs_ino_t ino = 0;
    zx_status_t st = ufs2_lookup_abs(data_disk, "/.accounts", &ino);
    if (st != ZX_OK) {
        st = ufs2_lookup_abs(data_disk, "/Users/.accounts", &ino);
    }
    if (st == ZX_OK) {
        union dinodep dp;
        if (getinode(data_disk, &dp, ino) == -1) return ZX_ERR_IO;
        struct ufs2_dinode di = *dp.dp2;
        if (di.di_size > 0 && di.di_size < 4096u && di.di_db[0] != 0) {
            uint8_t buf[4096] __attribute__((aligned(8)));
            memset(buf, 0, sizeof(buf));
            if (bread(data_disk, fsbtodb(&data_disk->d_fs, di.di_db[0]), buf, 4096) == -1) {
                return ZX_ERR_IO;
            }
            buf[di.di_size] = '\0';

            char* line = (char*)buf;
            while (line && *line && out_resp->count < IRIS_MAX_USER_PROFILES) {
                char* nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                trim_cr(line);
                if (line[0] != '\0' && line[0] != '#') {
                    iris_user_profile_info_t* u = &out_resp->users[out_resp->count];
                    char* fields[6] = {0};
                    char* cur = line;
                    for (int f = 0; f < 6; f++) {
                        fields[f] = cur;
                        char* bar = strchr(cur, '|');
                        if (bar) {
                            *bar = '\0';
                            cur = bar + 1;
                        } else {
                            cur = (char*)"";
                        }
                    }
                    snprintf(u->username, sizeof(u->username), "%s", fields[0]);
                    snprintf(u->display_name, sizeof(u->display_name), "%s", fields[1]);
                    snprintf(u->avatar_path, sizeof(u->avatar_path), "%s", fields[2]);
                    snprintf(u->wallpaper_path, sizeof(u->wallpaper_path), "%s", fields[3]);
                    snprintf(u->vault_path, sizeof(u->vault_path), "%s", fields[4]);
                    u->is_guest = (uint32_t)atoi(fields[5]);
                    if (u->username[0] != '\0') {
                        out_resp->count++;
                    }
                }
                line = nl ? (nl + 1) : NULL;
            }
        }
    }

    if (out_resp->count == 0) {
        /* Fallback defaults if .accounts was not present */
        iris_user_profile_info_t* a = &out_resp->users[0];
        snprintf(a->username, sizeof(a->username), "admin");
        snprintf(a->display_name, sizeof(a->display_name), "Administrator");
        snprintf(a->avatar_path, sizeof(a->avatar_path), "/System/Resources/avatar_admin.qoi");
        snprintf(a->wallpaper_path, sizeof(a->wallpaper_path), "/System/Resources/wallpaper_admin.qoi");
        snprintf(a->vault_path, sizeof(a->vault_path), "/Vaults/admin.img");
        a->is_guest = 0;

        iris_user_profile_info_t* g = &out_resp->users[1];
        snprintf(g->username, sizeof(g->username), "guest");
        snprintf(g->display_name, sizeof(g->display_name), "Guest");
        g->avatar_path[0] = '\0';
        snprintf(g->wallpaper_path, sizeof(g->wallpaper_path), "/System/Resources/wallpaper_guest.qoi");
        g->vault_path[0] = '\0';
        g->is_guest = 1;
        out_resp->count = 2;
    }

    return ZX_OK;
}

zx_status_t user_session_login(struct uufsd* data_disk,
                               zx_handle_t crypt_ch,
                               const iris_fs_user_login_req_t* req,
                               iris_user_session_state_t* state) {
    if (!data_disk || !req || !state) return ZX_ERR_INVALID_ARGS;

    if (state->session_active) {
        user_session_logout(data_disk, crypt_ch, state);
    }

    if (req->is_guest || strcmp(req->username, "guest") == 0) {
        ramfs_reset_guest();
        state->session_active = true;
        state->is_guest       = true;
        state->crypt_slot_id  = 0;
        snprintf(state->active_user, sizeof(state->active_user), "guest");
        return ZX_OK;
    }

    iris_fs_user_list_resp_t list;
    zx_status_t st = user_session_list_users(data_disk, &list);
    if (st != ZX_OK) return st;

    const iris_user_profile_info_t* profile = NULL;
    for (uint32_t i = 0; i < list.count; i++) {
        if (strcmp(list.users[i].username, req->username) == 0) {
            profile = &list.users[i];
            break;
        }
    }
    if (!profile || profile->vault_path[0] == '\0') return ZX_ERR_NOT_FOUND;

    ufs_ino_t vault_ino = 0;
    st = ufs2_lookup_abs(data_disk, profile->vault_path, &vault_ino);
    if (st != ZX_OK) return st;

    st = ufs2_cache_file_blocks(data_disk, vault_ino);
    if (st != ZX_OK) return st;
    if (g_vault_file_size <= 4096u) return ZX_ERR_IO_DATA_INTEGRITY;

    g_vault_backing_disk = data_disk;
    g_vault_crypt_ch     = crypt_ch;

    /* Read 4 KB vault header (physical sectors 0..7) */
    static uint8_t hdr_sec[4096] __attribute__((aligned(8)));
    st = vault_read_raw_sectors(0, 8u, hdr_sec);
    if (st != ZX_OK) return st;

    /* Send IRIS_CRYPT_OP_UNLOCK_USER with passphrase + 4 KB header */
    static uint32_t txid = 9000;
    static uint8_t req_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_unlock_user_req_t) + 4096u] __attribute__((aligned(8)));
    iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
    iris_crypt_unlock_user_req_t* ureq = (iris_crypt_unlock_user_req_t*)(req_buf + sizeof(iris_msg_hdr_t));
    iris_msg_hdr_init(hdr, IRIS_PROTO_CRYPT, IRIS_CRYPT_OP_UNLOCK_USER, txid++,
                      (uint32_t)(sizeof(iris_crypt_unlock_user_req_t) + 4096u));
    memset(ureq, 0, sizeof(*ureq));
    uint32_t plen = req->passphrase_len;
    if (plen >= sizeof(ureq->passphrase)) plen = sizeof(ureq->passphrase) - 1u;
    ureq->passphrase_len = plen;
    if (plen > 0) memcpy(ureq->passphrase, req->passphrase, plen);
    memcpy(req_buf + sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_unlock_user_req_t), hdr_sec, 4096u);

    st = zx_channel_write(crypt_ch, 0, req_buf, sizeof(req_buf), NULL, 0);
    memset(req_buf, 0, sizeof(req_buf));
    if (st != ZX_OK) return st;

    zx_signals_t obs = 0;
    st = zx_object_wait_one(crypt_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                            ZX_TIME_INFINITE, &obs);
    if (st != ZX_OK || (obs & ZX_CHANNEL_PEER_CLOSED)) return ZX_ERR_PEER_CLOSED;

    uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_user_slot_msg_t)] __attribute__((aligned(8)));
    uint32_t act_bytes = 0, act_handles = 0;
    st = zx_channel_read(crypt_ch, 0, rep_buf, NULL, sizeof(rep_buf), 0, &act_bytes, &act_handles);
    if (st != ZX_OK || act_bytes < sizeof(iris_msg_hdr_t)) return ZX_ERR_IO;

    const iris_msg_hdr_t* rh = (const iris_msg_hdr_t*)rep_buf;
    if (rh->status != ZX_OK) return rh->status;
    if (act_bytes < sizeof(rep_buf)) return ZX_ERR_IO;

    const iris_crypt_user_slot_msg_t* smsg = (const iris_crypt_user_slot_msg_t*)(rep_buf + sizeof(iris_msg_hdr_t));
    g_vault_slot_id = smsg->slot_id;

    uint64_t logical_sectors = (g_vault_file_size / 512u) - 8u;
    st = iris_ufs_register_custom(DEV_ID_USER_VAULT, logical_sectors, false,
                                  vault_custom_read, vault_custom_write, vault_custom_flush);
    if (st != ZX_OK) {
        user_session_logout(data_disk, crypt_ch, state);
        return st;
    }

    memset(&state->ufs_user, 0, sizeof(state->ufs_user));
    state->ufs_user.d_name      = "iris_user_ufs2";
    state->ufs_user.d_ufs       = 2;
    state->ufs_user.d_fd        = DEV_ID_USER_VAULT;
    state->ufs_user.d_bsize     = 512;
    state->ufs_user.d_sblockloc = UFS_STDSB;
    state->ufs_user.d_mine      = 0x02;

    if (sbread(&state->ufs_user) == -1 || state->ufs_user.d_fs.fs_magic != FS_UFS2_MAGIC) {
        iris_ufs_unregister_device(DEV_ID_USER_VAULT);
        state->session_active = true;
        state->is_guest       = false;
        state->crypt_slot_id  = g_vault_slot_id;
        user_session_logout(data_disk, crypt_ch, state);
        return ZX_ERR_ACCESS_DENIED;
    }

    state->session_active = true;
    state->is_guest       = false;
    state->crypt_slot_id  = g_vault_slot_id;
    snprintf(state->active_user, sizeof(state->active_user), "%s", profile->username);
    return ZX_OK;
}

zx_status_t user_session_logout(struct uufsd* data_disk,
                                zx_handle_t crypt_ch,
                                iris_user_session_state_t* state) {
    (void)data_disk;
    if (!state || !state->session_active) return ZX_OK;

    if (state->is_guest) {
        ramfs_reset_guest();
    } else {
        if (state->ufs_user.d_fs.fs_magic == FS_UFS2_MAGIC) {
            cgwrite1(&state->ufs_user, 0);
            sbwrite(&state->ufs_user, 0);
        }
        iris_ufs_unregister_device(DEV_ID_USER_VAULT);

        if (crypt_ch != ZX_HANDLE_INVALID) {
            static uint32_t txid = 9500;
            uint8_t req_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_crypt_user_slot_msg_t)] __attribute__((aligned(8)));
            iris_msg_hdr_t* hdr = (iris_msg_hdr_t*)req_buf;
            iris_crypt_user_slot_msg_t* smsg = (iris_crypt_user_slot_msg_t*)(req_buf + sizeof(iris_msg_hdr_t));
            iris_msg_hdr_init(hdr, IRIS_PROTO_CRYPT, IRIS_CRYPT_OP_LOCK_USER, txid++,
                              sizeof(iris_crypt_user_slot_msg_t));
            smsg->slot_id = state->crypt_slot_id;
            if (zx_channel_write(crypt_ch, 0, req_buf, sizeof(req_buf), NULL, 0) == ZX_OK) {
                zx_signals_t obs = 0;
                if (zx_object_wait_one(crypt_ch, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                       ZX_TIME_INFINITE, &obs) == ZX_OK &&
                    (obs & ZX_CHANNEL_READABLE)) {
                    uint8_t rep[64];
                    uint32_t ab = 0, ah = 0;
                    zx_channel_read(crypt_ch, 0, rep, NULL, sizeof(rep), 0, &ab, &ah);
                }
            }
        }
        memset(&state->ufs_user, 0, sizeof(state->ufs_user));
        memset(g_vault_lbn_frags, 0, sizeof(g_vault_lbn_frags));
        g_vault_num_blocks = 0;
        g_vault_file_size  = 0;
    }

    state->session_active = false;
    state->is_guest       = false;
    state->crypt_slot_id  = 0;
    state->active_user[0] = '\0';
    return ZX_OK;
}

struct uufsd* user_session_get_disk(iris_user_session_state_t* state) {
    if (!state || !state->session_active || state->is_guest) return NULL;
    return &state->ufs_user;
}

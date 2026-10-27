/*
 * irisOS - Host UFS2 Filesystem & Encrypted Partition Formatter (mkufs2)
 *
 * File: build/mkufs2_host.c
 * Purpose: Formats FreeBSD UFS2 images (System_A, System_B, Data) using
 *          vendor/libufs and optionally wraps/encrypts the Data image with
 *          the 4 KB iris_crypt Sector 0 header + AES-256-XTS + Argon2id.
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
#include <ufs/ufs/dir.h>
#include <ufs/ffs/fs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libufs.h>
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "servers/iris_crypt/include/iris_crypt.h"

uint32_t ffs_calc_sbhash(struct fs *);

int ufs_disk_write(struct uufsd* disk) {
    if (!disk) return -1;
    disk->d_mine |= 0x02;
    return 0;
}

int ufs_disk_close(struct uufsd* disk) {
    if (!disk) return 0;
    if (disk->d_inoblock != NULL) {
        free(disk->d_inoblock);
        disk->d_inoblock = NULL;
    }
    if (disk->d_si != NULL) {
        free(disk->d_si->si_csp);
        free(disk->d_si);
        disk->d_si = NULL;
    }
    return 0;
}

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

/**
 * @brief Write a directory block containing '.' and '..' plus optional child entries.
 */
typedef struct dir_item {
    uint32_t    ino;
    uint8_t     type;
    const char* name;
} dir_item_t;

static void build_dir_block(uint8_t block[4096], uint32_t self_ino, uint32_t parent_ino,
                            const dir_item_t* items, size_t num_items) {
    memset(block, 0, 4096);

    /* First 512-byte DIRBLKSIZ chunk holds '.', '..', and items */
    uint8_t* chunk = block;
    size_t offset = 0;

    dir_item_t all_items[32];
    size_t total = 0;
    all_items[total++] = (dir_item_t){ self_ino, DT_DIR, "." };
    all_items[total++] = (dir_item_t){ parent_ino, DT_DIR, ".." };
    for (size_t i = 0; i < num_items && total < 32; ++i) {
        all_items[total++] = items[i];
    }

    for (size_t i = 0; i < total; ++i) {
        size_t namelen = strlen(all_items[i].name);
        uint16_t reclen = (uint16_t)DIRECTSIZ(namelen);
        if (i + 1 == total) {
            reclen = (uint16_t)(DIRBLKSIZ - offset);
        }
        struct direct* dp = (struct direct*)(chunk + offset);
        dp->d_ino    = all_items[i].ino;
        dp->d_reclen = reclen;
        dp->d_type   = all_items[i].type;
        dp->d_namlen = (uint8_t)namelen;
        memcpy(dp->d_name, all_items[i].name, namelen + 1);
        offset += reclen;
    }

    /* Remaining 512-byte chunks in the 4096-byte block have a single empty record */
    for (size_t c = 1; c < 4096 / DIRBLKSIZ; ++c) {
        struct direct* dp = (struct direct*)(block + c * DIRBLKSIZ);
        dp->d_ino    = 0;
        dp->d_reclen = DIRBLKSIZ;
        dp->d_type   = DT_UNKNOWN;
        dp->d_namlen = 0;
    }
}

static void write_inode_entry(int fd, struct fs* sblock, uint32_t ino, const struct ufs2_dinode* di) {
    struct ufs2_dinode copy = *di;
    ffs_update_dinode_ckhash(sblock, &copy);
    off_t ino_off = (off_t)sblock->fs_iblkno * sblock->fs_bsize + (off_t)ino * sizeof(struct ufs2_dinode);
    if (pwrite(fd, &copy, sizeof(copy), ino_off) != (ssize_t)sizeof(copy)) {
        perror("pwrite inode");
        exit(1);
    }
}

static int format_ufs2_file(int fd, uint64_t total_sectors, const char* volname, bool is_system, const char* toybox_path) {
    const int32_t bsize = 4096;
    const int32_t fsize = 4096;
    const int32_t frag  = 1;
    const uint32_t ipg  = 256;
    const int32_t total_blocks = (int32_t)((total_sectors * 512ULL) / (uint64_t)bsize);

    if (total_blocks < 128) {
        fprintf(stderr, "mkufs2: volume too small (%d blocks)\n", total_blocks);
        return -1;
    }

    /* Zero image file */
    if (ftruncate(fd, (off_t)total_sectors * 512) != 0) {
        perror("ftruncate");
        return -1;
    }

    /* 1. Construct UFS2 Superblock (struct fs) */
    uint8_t sb_buf[SBLOCKSIZE];
    memset(sb_buf, 0, sizeof(sb_buf));
    struct fs* sblock = (struct fs*)sb_buf;

    sblock->fs_magic           = FS_UFS2_MAGIC;
    sblock->fs_sblockloc       = SBLOCK_UFS2;
    sblock->fs_sblockactualloc = SBLOCK_UFS2;
    sblock->fs_bsize           = bsize;
    sblock->fs_maxbsize        = bsize;
    sblock->fs_fsize           = fsize;
    sblock->fs_frag            = frag;
    sblock->fs_bmask           = ~(bsize - 1);
    sblock->fs_fmask           = ~(fsize - 1);
    sblock->fs_qbmask          = ~sblock->fs_bmask;
    sblock->fs_qfmask          = ~sblock->fs_fmask;
    sblock->fs_bshift          = 12; /* log2(4096) */
    sblock->fs_fshift          = 12; /* log2(4096) */
    sblock->fs_fragshift       = 0;  /* log2(1) */
    sblock->fs_fsbtodb         = 3;  /* log2(4096 / 512) */
    sblock->fs_sbsize          = SBLOCKSIZE;
    sblock->fs_nindir          = bsize / (int32_t)sizeof(ufs2_daddr_t);       /* 512 */
    sblock->fs_inopb           = (uint32_t)(bsize / sizeof(struct ufs2_dinode)); /* 16 */
    sblock->fs_maxsymlinklen   = (int32_t)((UFS_NDADDR + UFS_NIADDR) * sizeof(ufs2_daddr_t)); /* 120 */

    sblock->fs_ncg             = 1;
    sblock->fs_ipg             = ipg;
    sblock->fs_fpg             = total_blocks;
    sblock->fs_size            = total_blocks;
    sblock->fs_providersize    = total_blocks;

    /* Layout offsets in 4 KB blocks:
     *  0..15: Boot area (64 KB)
     *  16..17: Primary Superblock (SBLOCK_UFS2 = 65536)
     *  18..19: CG 0 Superblock backup (fs_sblkno = 18)
     *  20:     CG 0 descriptor block (fs_cblkno = 20)
     *  21..36: Inode blocks (256 inodes / 16 per block = 16 blocks)
     *  37:     CG summary info block (fs_dblkno = 37, fs_csaddr = 37)
     *  38+:    Data blocks
     */
    sblock->fs_sblkno          = 18;
    sblock->fs_cblkno          = 20;
    sblock->fs_iblkno          = 21;
    sblock->fs_dblkno          = 37;
    sblock->fs_csaddr          = 37;
    sblock->fs_cssize          = fragroundup(sblock, sblock->fs_ncg * (int32_t)sizeof(struct csum));
    sblock->fs_cgsize          = fragroundup(sblock, CGSIZE(sblock));
    sblock->fs_dsize           = total_blocks - 38;
    sblock->fs_metaspace       = 0;
    sblock->fs_minfree         = MINFREE;
    sblock->fs_optim           = FS_OPTTIME;
    sblock->fs_maxcontig       = 0;
    sblock->fs_contigsumsize   = 0;
    sblock->fs_maxbpg          = total_blocks;
    sblock->fs_avgfilesize     = AVFILESIZ;
    sblock->fs_avgfpdir        = AFPDIR;
    sblock->fs_clean           = 1;
    sblock->fs_flags           = FS_METACKHASH | FS_FLAGS_UPDATED;
    sblock->fs_metackhash      = CK_SUPERBLOCK | CK_CYLGRP | CK_INODE;
    sblock->fs_time            = (ufs_time_t)time(NULL);
    sblock->fs_id[0]           = 0x49524953;
    sblock->fs_id[1]           = is_system ? 0x53595341 : 0x44415441;

    uint64_t maxfilesize = (uint64_t)bsize * UFS_NDADDR - 1;
    uint64_t sizepb = (uint64_t)bsize;
    for (int i = 0; i < UFS_NIADDR; ++i) {
        sizepb *= (uint64_t)sblock->fs_nindir;
        maxfilesize += sizepb;
    }
    sblock->fs_maxfilesize = maxfilesize;

    strncpy((char*)sblock->fs_volname, volname, MAXVOLLEN - 1);
    strncpy((char*)sblock->fs_fsmnt, is_system ? "/System" : "/Users", MAXMNTLEN - 1);

    /* 2. Construct Cylinder Group 0 (struct cg) */
    uint8_t cg_buf[4096];
    memset(cg_buf, 0, sizeof(cg_buf));
    struct cg* cgp = (struct cg*)cg_buf;

    cgp->cg_magic       = CG_MAGIC;
    cgp->cg_cgx         = 0;
    cgp->cg_ndblk       = (uint32_t)total_blocks;
    cgp->cg_niblk       = ipg;
    cgp->cg_initediblk  = ipg;
    cgp->cg_iusedoff    = sizeof(struct cg);
    cgp->cg_freeoff     = cgp->cg_iusedoff + howmany(ipg, NBBY);
    cgp->cg_nextfreeoff = cgp->cg_freeoff + howmany(total_blocks, NBBY);
    cgp->cg_time        = sblock->fs_time;

    uint8_t* inosused = cg_inosused(cgp);
    uint8_t* blksfree = cg_blksfree(cgp);

    /* Mark all data blocks [38 .. total_blocks-1] free initially */
    for (int32_t b = 38; b < total_blocks; ++b) {
        ffs_setblock(sblock, blksfree, b);
    }

    /* Reserve inodes 0 and 1 */
    setbit(inosused, 0);
    setbit(inosused, 1);

    uint32_t used_dirs   = 0;
    uint32_t used_inodes = 2; /* 0 and 1 */
    uint32_t next_blk    = 38;

    if (is_system) {
        /* System_A / System_B hierarchy:
         *  ino 2: / (root dir) -> Bin (3), Libraries (4), Servers (5), Resources (6)
         *  ino 3: /Bin         -> hello.txt (7), toybox (9), and 15 symlinks (10..24)
         *  ino 4: /Libraries
         *  ino 5: /Servers
         *  ino 6: /Resources   -> release.txt (8)
         *  ino 7: /Bin/hello.txt (regular file)
         *  ino 8: /Resources/release.txt (regular file)
         *  ino 9: /Bin/toybox (regular executable)
         *  ino 10..24: symlinks to toybox (sh, ls, cat, echo, pwd, uname, clear, mkdir, cp, rm, touch, wc, grep, sleep, kill)
         */
        static const char* const toybox_tools[] = {
            "sh", "ls", "cat", "echo", "pwd", "uname", "clear",
            "mkdir", "cp", "rm", "touch", "wc", "grep", "sleep", "kill",
            "reboot", "poweroff"
        };
        const size_t num_tools = sizeof(toybox_tools) / sizeof(toybox_tools[0]);

        int tb_fd = -1;
        off_t tb_size = 0;
        uint32_t num_tb_blks = 0;
        uint32_t blk_tb_indirect = 0;
        uint32_t blk_tb_data_start = 0;

        if (toybox_path != NULL) {
            tb_fd = open(toybox_path, O_RDONLY);
            if (tb_fd >= 0) {
                tb_size = lseek(tb_fd, 0, SEEK_END);
                lseek(tb_fd, 0, SEEK_SET);
                num_tb_blks = (uint32_t)((tb_size + 4095) / 4096);
            } else {
                fprintf(stderr, "mkufs2: warning: cannot open toybox '%s': %s\n", toybox_path, strerror(errno));
            }
        }

        uint32_t max_ino = 8;
        if (tb_fd >= 0) {
            max_ino = 8 + 1 + (uint32_t)num_tools; // ino 24
        }
        for (uint32_t ino = 2; ino <= max_ino; ++ino) {
            setbit(inosused, ino);
            used_inodes++;
        }
        used_dirs = 5; /* /, /Bin, /Libraries, /Servers, /Resources */

        uint32_t blk_root    = next_blk++;
        uint32_t blk_bin     = next_blk++;
        uint32_t blk_lib     = next_blk++;
        uint32_t blk_srv     = next_blk++;
        uint32_t blk_res     = next_blk++;
        uint32_t blk_hello   = next_blk++;
        uint32_t blk_release = next_blk++;

        if (tb_fd >= 0) {
            if (num_tb_blks > 12) {
                blk_tb_indirect = next_blk++;
            }
            blk_tb_data_start = next_blk;
            next_blk += num_tb_blks;
        }

        for (uint32_t b = 38; b < next_blk; ++b) {
            ffs_clrblock(sblock, blksfree, (int32_t)b);
        }

        uint8_t dir_blk[4096];
        dir_item_t root_items[] = {
            { 3, DT_DIR, "Bin" },
            { 4, DT_DIR, "Libraries" },
            { 5, DT_DIR, "Servers" },
            { 6, DT_DIR, "Resources" },
        };
        build_dir_block(dir_blk, 2, 2, root_items, 4);
        pwrite(fd, dir_blk, 4096, (off_t)blk_root * 4096);

        dir_item_t bin_items[32];
        size_t num_bin_items = 0;
        bin_items[num_bin_items++] = (dir_item_t){ 7, DT_REG, "hello.txt" };
        if (tb_fd >= 0) {
            bin_items[num_bin_items++] = (dir_item_t){ 9, DT_REG, "toybox" };
            for (size_t i = 0; i < num_tools && num_bin_items < 32; ++i) {
                bin_items[num_bin_items++] = (dir_item_t){ 10 + (uint32_t)i, DT_LNK, toybox_tools[i] };
            }
        }
        build_dir_block(dir_blk, 3, 2, bin_items, num_bin_items);
        pwrite(fd, dir_blk, 4096, (off_t)blk_bin * 4096);

        build_dir_block(dir_blk, 4, 2, NULL, 0);
        pwrite(fd, dir_blk, 4096, (off_t)blk_lib * 4096);

        build_dir_block(dir_blk, 5, 2, NULL, 0);
        pwrite(fd, dir_blk, 4096, (off_t)blk_srv * 4096);

        dir_item_t res_items[] = {
            { 8, DT_REG, "release.txt" },
        };
        build_dir_block(dir_blk, 6, 2, res_items, 1);
        pwrite(fd, dir_blk, 4096, (off_t)blk_res * 4096);

        const char* hello_str = "irisOS Immutable System Partition (UFS2)\n";
        uint8_t file_blk[4096];
        memset(file_blk, 0, sizeof(file_blk));
        memcpy(file_blk, hello_str, strlen(hello_str));
        pwrite(fd, file_blk, 4096, (off_t)blk_hello * 4096);

        const char* release_str = "irisOS release 0.1.0\n";
        memset(file_blk, 0, sizeof(file_blk));
        memcpy(file_blk, release_str, strlen(release_str));
        pwrite(fd, file_blk, 4096, (off_t)blk_release * 4096);

        /* Write inodes 2..6 (dirs) */
        struct ufs2_dinode di;
        uint32_t dir_blks[5] = { blk_root, blk_bin, blk_lib, blk_srv, blk_res };
        uint16_t dir_nlinks[5] = { 6, 2, 2, 2, 2 };
        for (int i = 0; i < 5; ++i) {
            memset(&di, 0, sizeof(di));
            di.di_mode    = IFDIR | 0755;
            di.di_nlink   = dir_nlinks[i];
            di.di_size    = DIRBLKSIZ;
            di.di_blocks  = 8; /* 4096 / 512 */
            di.di_blksize = 4096;
            di.di_gen     = 1;
            di.di_mtime   = sblock->fs_time;
            di.di_atime   = sblock->fs_time;
            di.di_ctime   = sblock->fs_time;
            di.di_db[0]   = dir_blks[i];
            write_inode_entry(fd, sblock, (uint32_t)(2 + i), &di);
        }

        /* Write inode 7 (Bin/hello.txt) */
        memset(&di, 0, sizeof(di));
        di.di_mode    = IFREG | 0644;
        di.di_nlink   = 1;
        di.di_size    = strlen(hello_str);
        di.di_blocks  = 8;
        di.di_blksize = 4096;
        di.di_gen     = 1;
        di.di_mtime   = sblock->fs_time;
        di.di_atime   = sblock->fs_time;
        di.di_ctime   = sblock->fs_time;
        di.di_db[0]   = blk_hello;
        write_inode_entry(fd, sblock, 7, &di);

        /* Write inode 8 (Resources/release.txt) */
        memset(&di, 0, sizeof(di));
        di.di_mode    = IFREG | 0644;
        di.di_nlink   = 1;
        di.di_size    = strlen(release_str);
        di.di_blocks  = 8;
        di.di_blksize = 4096;
        di.di_gen     = 1;
        di.di_mtime   = sblock->fs_time;
        di.di_atime   = sblock->fs_time;
        di.di_ctime   = sblock->fs_time;
        di.di_db[0]   = blk_release;
        write_inode_entry(fd, sblock, 8, &di);

        if (tb_fd >= 0) {
            uint8_t tbuf[4096];
            for (uint32_t i = 0; i < num_tb_blks; ++i) {
                memset(tbuf, 0, sizeof(tbuf));
                ssize_t rd = read(tb_fd, tbuf, sizeof(tbuf));
                (void)rd;
                pwrite(fd, tbuf, 4096, (off_t)(blk_tb_data_start + i) * 4096);
            }
            if (num_tb_blks > 12) {
                ufs2_daddr_t iblk[512];
                memset(iblk, 0, sizeof(iblk));
                for (uint32_t i = 12; i < num_tb_blks && (i - 12) < 512; ++i) {
                    iblk[i - 12] = blk_tb_data_start + i;
                }
                pwrite(fd, iblk, 4096, (off_t)blk_tb_indirect * 4096);
            }
            close(tb_fd);

            /* Write inode 9 (Bin/toybox) */
            memset(&di, 0, sizeof(di));
            di.di_mode    = IFREG | 0755;
            di.di_nlink   = 1;
            di.di_size    = tb_size;
            di.di_blocks  = (num_tb_blks + (num_tb_blks > 12 ? 1 : 0)) * 8;
            di.di_blksize = 4096;
            di.di_gen     = 1;
            di.di_mtime   = sblock->fs_time;
            di.di_atime   = sblock->fs_time;
            di.di_ctime   = sblock->fs_time;
            for (uint32_t i = 0; i < 12 && i < num_tb_blks; ++i) {
                di.di_db[i] = blk_tb_data_start + i;
            }
            if (num_tb_blks > 12) {
                di.di_ib[0] = blk_tb_indirect;
            }
            write_inode_entry(fd, sblock, 9, &di);

            /* Write inodes 10..24 (symlinks to toybox) */
            for (size_t i = 0; i < num_tools; ++i) {
                uint32_t sym_ino = 10 + (uint32_t)i;
                memset(&di, 0, sizeof(di));
                di.di_mode    = IFLNK | 0777;
                di.di_nlink   = 1;
                di.di_size    = strlen("toybox");
                di.di_blocks  = 0;
                di.di_blksize = 4096;
                di.di_gen     = 1;
                di.di_mtime   = sblock->fs_time;
                di.di_atime   = sblock->fs_time;
                di.di_ctime   = sblock->fs_time;
                memcpy(di.di_db, "toybox", strlen("toybox") + 1);
                write_inode_entry(fd, sblock, sym_ino, &di);
            }
        }
    } else {
        /* Encrypted Data partition hierarchy:
         *  ino 2: / (root of Data volume, mounted at /Users) -> admin (3), Applications (4)
         *  ino 3: /admin
         *  ino 4: /Applications
         */
        for (uint32_t ino = 2; ino <= 4; ++ino) {
            setbit(inosused, ino);
            used_inodes++;
        }
        used_dirs = 3;

        uint32_t blk_root  = next_blk++;
        uint32_t blk_admin = next_blk++;
        uint32_t blk_apps  = next_blk++;

        for (uint32_t b = 38; b < next_blk; ++b) {
            ffs_clrblock(sblock, blksfree, (int32_t)b);
        }

        uint8_t dir_blk[4096];
        dir_item_t root_items[] = {
            { 3, DT_DIR, "admin" },
            { 4, DT_DIR, "Applications" },
        };
        build_dir_block(dir_blk, 2, 2, root_items, 2);
        pwrite(fd, dir_blk, 4096, (off_t)blk_root * 4096);

        build_dir_block(dir_blk, 3, 2, NULL, 0);
        pwrite(fd, dir_blk, 4096, (off_t)blk_admin * 4096);

        build_dir_block(dir_blk, 4, 2, NULL, 0);
        pwrite(fd, dir_blk, 4096, (off_t)blk_apps * 4096);

        struct ufs2_dinode di;
        uint32_t dir_blks[3] = { blk_root, blk_admin, blk_apps };
        uint16_t dir_nlinks[3] = { 4, 2, 2 };
        for (int i = 0; i < 3; ++i) {
            memset(&di, 0, sizeof(di));
            di.di_mode    = IFDIR | 0755;
            di.di_nlink   = dir_nlinks[i];
            di.di_size    = DIRBLKSIZ;
            di.di_blocks  = 8;
            di.di_blksize = 4096;
            di.di_gen     = 1;
            di.di_mtime   = sblock->fs_time;
            di.di_atime   = sblock->fs_time;
            di.di_ctime   = sblock->fs_time;
            di.di_db[0]   = dir_blks[i];
            write_inode_entry(fd, sblock, (uint32_t)(2 + i), &di);
        }
    }

    int32_t free_blocks = total_blocks - (int32_t)next_blk;
    int32_t free_inodes = (int32_t)ipg - (int32_t)used_inodes;

    cgp->cg_cs.cs_ndir   = (int32_t)used_dirs;
    cgp->cg_cs.cs_nbfree = free_blocks;
    cgp->cg_cs.cs_nifree = free_inodes;
    cgp->cg_cs.cs_nffree = 0;

    sblock->fs_cstotal.cs_ndir   = used_dirs;
    sblock->fs_cstotal.cs_nbfree = free_blocks;
    sblock->fs_cstotal.cs_nifree = free_inodes;
    sblock->fs_cstotal.cs_nffree = 0;

    /* Write Cylinder Group Summary block at block 37 (fs_csaddr) */
    uint8_t csum_blk[4096];
    memset(csum_blk, 0, sizeof(csum_blk));
    memcpy(csum_blk, &cgp->cg_cs, sizeof(struct csum));
    pwrite(fd, csum_blk, 4096, (off_t)sblock->fs_csaddr * 4096);

    /* Compute CG 0 check-hash and write CG 0 at block 20 (fs_cblkno) */
    cgp->cg_ckhash = 0;
    cgp->cg_ckhash = calculate_crc32c(~0u, cgp, (size_t)sblock->fs_cgsize);
    pwrite(fd, cgp, 4096, (off_t)sblock->fs_cblkno * 4096);

    /* Compute Superblock check-hash and write at SBLOCK_UFS2 (65536) and backup (block 18) */
    sblock->fs_ckhash = ffs_calc_sbhash(sblock);
    pwrite(fd, sb_buf, SBLOCKSIZE, SBLOCK_UFS2);
    pwrite(fd, sb_buf, SBLOCKSIZE, (off_t)sblock->fs_sblkno * 4096);

    /* Verify with libufs sbread() + getinode()! */
    struct uufsd verify_disk;
    memset(&verify_disk, 0, sizeof(verify_disk));
    verify_disk.d_fd          = fd;
    verify_disk.d_bsize       = 512;
    verify_disk.d_sblockloc   = SBLOCK_UFS2;
    verify_disk.d_lookupflags = 0;
    verify_disk.d_name        = volname;

    if (sbread(&verify_disk) != 0) {
        fprintf(stderr, "mkufs2: libufs sbread verification failed: %s\n",
                verify_disk.d_error ? verify_disk.d_error : "unknown");
        return -1;
    }

    union dinodep dp;
    if (getinode(&verify_disk, &dp, UFS_ROOTINO) != 0) {
        fprintf(stderr, "mkufs2: libufs getinode(UFS_ROOTINO) verification failed\n");
        return -1;
    }
    ufs_disk_close(&verify_disk);
    return 0;
}

static int encrypt_data_partition(int plain_fd, const char* out_path, uint64_t total_phys_sectors) {
    uint64_t logical_sectors = total_phys_sectors - IRIS_CRYPT_HEADER_SECTORS;

    iris_crypt_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic           = IRIS_CRYPT_HEADER_MAGIC;
    hdr.version         = IRIS_CRYPT_HEADER_VERSION;
    hdr.flags           = IRIS_CRYPT_FLAG_DEV_KEY;
    hdr.argon2_t_cost   = IRIS_CRYPT_DEFAULT_T_COST;
    hdr.argon2_m_cost   = IRIS_CRYPT_DEFAULT_M_COST;
    hdr.argon2_lanes    = IRIS_CRYPT_DEFAULT_LANES;
    hdr.sector_offset   = IRIS_CRYPT_HEADER_SECTORS;
    hdr.logical_sectors = logical_sectors;

    static const uint8_t k_dev_salt[IRIS_CRYPT_SALT_LEN] = {
        0x49, 0x52, 0x49, 0x53, 0x4F, 0x53, 0x5F, 0x41,
        0x52, 0x47, 0x4F, 0x4E, 0x32, 0x5F, 0x53, 0x41,
        0x4C, 0x54, 0x5F, 0x56, 0x31, 0x5F, 0x32, 0x30,
        0x32, 0x36, 0x5F, 0x42, 0x4C, 0x4F, 0x43, 0x4B
    };
    memcpy(hdr.argon2_salt, k_dev_salt, IRIS_CRYPT_SALT_LEN);

    const uint8_t* dev_pw = (const uint8_t*)IRIS_CRYPT_DEV_PASSPHRASE;
    size_t dev_pw_len = strlen(IRIS_CRYPT_DEV_PASSPHRASE);

    uint8_t kek[IRIS_CRYPT_MASTER_KEY_LEN];
    if (iris_argon2id_derive(dev_pw, dev_pw_len,
                             hdr.argon2_salt, IRIS_CRYPT_SALT_LEN,
                             hdr.argon2_t_cost, hdr.argon2_m_cost, hdr.argon2_lanes,
                             kek, sizeof(kek)) != 0) {
        fprintf(stderr, "mkufs2: Argon2id KEK derivation failed\n");
        return -1;
    }

    uint8_t master_key[IRIS_CRYPT_MASTER_KEY_LEN];
    mbedtls_sha256((const uint8_t*)"irisOS-AES256-XTS-MasterKey-Part1-Data", 38, master_key, 0);
    mbedtls_sha256((const uint8_t*)"irisOS-AES256-XTS-MasterKey-Part2-Tweak", 39, master_key + 32, 0);
    mbedtls_sha256(master_key, IRIS_CRYPT_MASTER_KEY_LEN, hdr.key_verification_tag, 0);

    mbedtls_aes_xts_context kek_xts;
    mbedtls_aes_xts_init(&kek_xts);
    mbedtls_aes_xts_setkey_enc(&kek_xts, kek, 512);
    uint8_t zero_tweak[16] = {0};
    mbedtls_aes_crypt_xts(&kek_xts, MBEDTLS_AES_ENCRYPT, IRIS_CRYPT_MASTER_KEY_LEN,
                          zero_tweak, master_key, hdr.encrypted_master_key);
    mbedtls_aes_xts_free(&kek_xts);

    size_t csum_len = offsetof(iris_crypt_header_t, header_checksum);
    mbedtls_sha256((const uint8_t*)&hdr, csum_len, hdr.header_checksum, 0);

    int out_fd = open(out_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (out_fd < 0) {
        perror("open encrypted output");
        return -1;
    }

    /* Write 4 KB Sector 0 metadata header (Physical Sectors 0..7) */
    if (pwrite(out_fd, &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) {
        perror("pwrite header");
        close(out_fd);
        return -1;
    }

    /* Encrypt all logical sectors with AES-256-XTS */
    mbedtls_aes_xts_context xts_enc;
    mbedtls_aes_xts_init(&xts_enc);
    mbedtls_aes_xts_setkey_enc(&xts_enc, master_key, 512);

    uint8_t plain_buf[65536];
    uint8_t cipher_buf[65536];
    uint64_t lba = 0;

    while (lba < logical_sectors) {
        uint64_t chunk_secs = logical_sectors - lba;
        if (chunk_secs > 128) chunk_secs = 128;
        size_t chunk_bytes = (size_t)chunk_secs * 512u;

        if (pread(plain_fd, plain_buf, chunk_bytes, (off_t)(lba * 512u)) != (ssize_t)chunk_bytes) {
            perror("pread plain ufs");
            close(out_fd);
            return -1;
        }

        for (uint64_t i = 0; i < chunk_secs; ++i) {
            uint8_t data_unit[16] = {0};
            uint64_t cur_lba = lba + i;
            for (unsigned b = 0; b < 8; ++b) {
                data_unit[b] = (uint8_t)((cur_lba >> (b * 8u)) & 0xFFu);
            }
            mbedtls_aes_crypt_xts(&xts_enc, MBEDTLS_AES_ENCRYPT, 512u,
                                  data_unit,
                                  plain_buf + i * 512u,
                                  cipher_buf + i * 512u);
        }

        off_t phys_off = (off_t)((lba + IRIS_CRYPT_HEADER_SECTORS) * 512u);
        if (pwrite(out_fd, cipher_buf, chunk_bytes, phys_off) != (ssize_t)chunk_bytes) {
            perror("pwrite encrypted ufs");
            close(out_fd);
            return -1;
        }
        lba += chunk_secs;
    }

    mbedtls_aes_xts_free(&xts_enc);
    close(out_fd);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <output.img> <sectors> <system|data-encrypted> [volname]\n", argv[0]);
        return 1;
    }

    const char* out_path = argv[1];
    uint64_t total_sectors = (uint64_t)strtoull(argv[2], NULL, 10);
    const char* mode = argv[3];
    const char* volname = (argc >= 5) ? argv[4] : "irisOS";
    const char* toybox_bin = (argc >= 6) ? argv[5] : NULL;

    if (strcmp(mode, "system") == 0) {
        int fd = open(out_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fd < 0) {
            perror("open");
            return 1;
        }
        if (format_ufs2_file(fd, total_sectors, volname, true, toybox_bin) != 0) {
            close(fd);
            return 1;
        }
        close(fd);
    } else if (strcmp(mode, "data-encrypted") == 0) {
        char tmp_plain[512];
        snprintf(tmp_plain, sizeof(tmp_plain), "%s.plain.tmp", out_path);
        int fd = open(tmp_plain, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fd < 0) {
            perror("open tmp");
            return 1;
        }
        uint64_t logical_sectors = total_sectors - IRIS_CRYPT_HEADER_SECTORS;
        if (format_ufs2_file(fd, logical_sectors, volname, false, NULL) != 0) {
            close(fd);
            unlink(tmp_plain);
            return 1;
        }
        if (encrypt_data_partition(fd, out_path, total_sectors) != 0) {
            close(fd);
            unlink(tmp_plain);
            return 1;
        }
        close(fd);
        unlink(tmp_plain);
    } else {
        fprintf(stderr, "Unknown mode '%s'\n", mode);
        return 1;
    }

    return 0;
}

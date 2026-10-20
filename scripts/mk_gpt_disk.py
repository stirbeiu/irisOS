#!/usr/bin/env python3
"""
irisOS - 4-Partition GPT Disk Image Generator

Generates a 128 MB (262,144 sectors) GPT raw disk image with valid Protective MBR
and Primary + Backup GPT headers and partition tables:
  1. ESP      (LBA   2048 ..  67583, 32 MB FAT32: Gigaboot + iris_a.zbi + iris_b.zbi)
  2. System_A (LBA  67584 .. 133119, 32 MB UFS2: active immutable system root)
  3. System_B (LBA 133120 .. 198655, 32 MB UFS2: inactive A/B rollback slot)
  4. Data     (LBA 198656 .. 260095, 30 MB Encrypted UFS2 via iris_crypt)
"""

import os
import struct
import sys
import uuid
import zlib

SECTOR_SIZE = 512
TOTAL_SECTORS = 262144  # 128 MB

GUID_ESP  = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
GUID_UFS2 = uuid.UUID("516E7CB6-6ECF-11D6-8FF8-00022D09712B")
GUID_DISK = uuid.UUID("49524953-0000-4000-8000-000000000000")

PART_SPECS = [
    {
        "name": "ESP",
        "type_guid": GUID_ESP,
        "part_guid": uuid.UUID("49524953-0001-4000-8000-000000000001"),
        "first_lba": 2048,
        "last_lba": 67583,
        "attrs": 0x0000000000000000,
    },
    {
        "name": "System_A",
        "type_guid": GUID_UFS2,
        "part_guid": uuid.UUID("49524953-0002-4000-8000-000000000002"),
        "first_lba": 67584,
        "last_lba": 133119,
        # Active A/B slot attributes: Successful=1 (bit 55), Tries=7 (bits 52..54), Priority=15 (bits 48..51)
        "attrs": 0x00FF000000000000,
    },
    {
        "name": "System_B",
        "type_guid": GUID_UFS2,
        "part_guid": uuid.UUID("49524953-0003-4000-8000-000000000003"),
        "first_lba": 133120,
        "last_lba": 198655,
        # Inactive A/B slot attributes: Successful=0, Tries=0, Priority=1
        "attrs": 0x0001000000000000,
    },
    {
        "name": "Data",
        "type_guid": GUID_UFS2,
        "part_guid": uuid.UUID("49524953-0004-4000-8000-000000000004"),
        "first_lba": 198656,
        "last_lba": 260095,
        "attrs": 0x0000000000000000,
    },
]


def build_protective_mbr(total_sectors: int) -> bytes:
    mbr = bytearray(SECTOR_SIZE)
    # Partition 1 entry at offset 446 (16 bytes)
    # Status=0x00, CHS First=(0,0,2), Type=0xEE (GPT Protective), CHS Last=(0xFF,0xFF,0xFF)
    lba_start = 1
    lba_size = min(total_sectors - 1, 0xFFFFFFFF)
    entry = struct.pack(
        "<BBBBBBBBII",
        0x00,
        0x00, 0x02, 0x00,
        0xEE,
        0xFF, 0xFF, 0xFF,
        lba_start,
        lba_size,
    )
    mbr[446:462] = entry
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def build_partition_entries() -> bytes:
    entries = bytearray(128 * 128)
    for idx, p in enumerate(PART_SPECS):
        name_utf16 = p["name"].encode("utf-16le")[:72]
        name_padded = name_utf16 + b"\x00" * (72 - len(name_utf16))
        entry = struct.pack(
            "<16s16sQQQ72s",
            p["type_guid"].bytes_le,
            p["part_guid"].bytes_le,
            p["first_lba"],
            p["last_lba"],
            p["attrs"],
            name_padded,
        )
        entries[idx * 128 : (idx + 1) * 128] = entry
    return bytes(entries)


def build_gpt_header(
    my_lba: int,
    alt_lba: int,
    first_usable_lba: int,
    last_usable_lba: int,
    part_entry_lba: int,
    entries_crc32: int,
) -> bytes:
    hdr_without_crc = struct.pack(
        "<8sIIIIQQQQ16sQIII",
        b"EFI PART",
        0x00010000,          # Revision 1.0
        92,                  # HeaderSize
        0,                   # HeaderCRC32 placeholder
        0,                   # Reserved
        my_lba,
        alt_lba,
        first_usable_lba,
        last_usable_lba,
        GUID_DISK.bytes_le,
        part_entry_lba,
        128,                 # NumberOfPartitionEntries
        128,                 # SizeOfPartitionEntry
        entries_crc32,
    )
    crc = zlib.crc32(hdr_without_crc) & 0xFFFFFFFF
    hdr = bytearray(SECTOR_SIZE)
    hdr[0:92] = hdr_without_crc
    struct.pack_into("<I", hdr, 16, crc)
    return bytes(hdr)


def write_file_at_lba(disk_fd: int, img_path: str, first_lba: int, max_sectors: int) -> None:
    size = os.path.getsize(img_path)
    if size > max_sectors * SECTOR_SIZE:
        raise ValueError(f"{img_path} ({size} bytes) exceeds partition size ({max_sectors * SECTOR_SIZE} bytes)")
    with open(img_path, "rb") as src:
        os.lseek(disk_fd, first_lba * SECTOR_SIZE, os.SEEK_SET)
        while True:
            chunk = src.read(65536)
            if not chunk:
                break
            os.write(disk_fd, chunk)


def main() -> int:
    if len(sys.argv) != 6:
        print(
            f"Usage: {sys.argv[0]} <out_disk.raw> <esp.img> <sys_a.img> <sys_b.img> <data.img>",
            file=sys.stderr,
        )
        return 1

    out_disk, esp_img, sys_a_img, sys_b_img, data_img = sys.argv[1:6]

    entries_bytes = build_partition_entries()
    entries_crc = zlib.crc32(entries_bytes) & 0xFFFFFFFF

    first_usable_lba = 2048
    last_usable_lba = TOTAL_SECTORS - 34  # 262110
    backup_entries_lba = TOTAL_SECTORS - 33  # 262111
    backup_header_lba = TOTAL_SECTORS - 1    # 262143

    primary_hdr = build_gpt_header(
        my_lba=1,
        alt_lba=backup_header_lba,
        first_usable_lba=first_usable_lba,
        last_usable_lba=last_usable_lba,
        part_entry_lba=2,
        entries_crc32=entries_crc,
    )
    backup_hdr = build_gpt_header(
        my_lba=backup_header_lba,
        alt_lba=1,
        first_usable_lba=first_usable_lba,
        last_usable_lba=last_usable_lba,
        part_entry_lba=backup_entries_lba,
        entries_crc32=entries_crc,
    )
    pmbr = build_protective_mbr(TOTAL_SECTORS)

    fd = os.open(out_disk, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o644)
    try:
        os.ftruncate(fd, TOTAL_SECTORS * SECTOR_SIZE)

        # LBA 0: Protective MBR
        os.lseek(fd, 0, os.SEEK_SET)
        os.write(fd, pmbr)

        # LBA 1: Primary GPT Header
        os.lseek(fd, 1 * SECTOR_SIZE, os.SEEK_SET)
        os.write(fd, primary_hdr)

        # LBA 2..33: Primary Partition Entries
        os.lseek(fd, 2 * SECTOR_SIZE, os.SEEK_SET)
        os.write(fd, entries_bytes)

        # Partition 1..4 payloads
        imgs = [esp_img, sys_a_img, sys_b_img, data_img]
        for spec, img_path in zip(PART_SPECS, imgs):
            max_secs = spec["last_lba"] - spec["first_lba"] + 1
            write_file_at_lba(fd, img_path, spec["first_lba"], max_secs)

        # LBA 262111..262142: Backup Partition Entries
        os.lseek(fd, backup_entries_lba * SECTOR_SIZE, os.SEEK_SET)
        os.write(fd, entries_bytes)

        # LBA 262143: Backup GPT Header
        os.lseek(fd, backup_header_lba * SECTOR_SIZE, os.SEEK_SET)
        os.write(fd, backup_hdr)
    finally:
        os.close(fd)

    return 0


if __name__ == "__main__":
    sys.exit(main())

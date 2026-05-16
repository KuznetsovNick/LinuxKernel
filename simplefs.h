#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SIMPLEFS_MAGIC              0x51E5F500u
#define SIMPLEFS_VERSION            1u
#define SIMPLEFS_BLOCK_SIZE         512u
#define SIMPLEFS_NAME_MAX           64u
#define SIMPLEFS_FIRST_FILE_INO     100u

struct simplefs_super_block {
    __le32 magic;
    __le32 version;
    __le32 block_size;
    __le32 max_name_len;
    __le32 max_file_sectors;
    __le32 num_files;
    __le64 sb_first_offset;
    __le64 sb_second_offset;
    __le64 total_sectors;
    __le32 crc32;
    __u8   padding[SIMPLEFS_BLOCK_SIZE - 52];
} __attribute__((packed));

#define SIMPLEFS_IOC_MAGIC 'S'

#define SIMPLEFS_IOC_ZERO_ALL    _IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOC_ERASE_FS    _IO(SIMPLEFS_IOC_MAGIC, 2)

struct simplefs_file_meta {
    char    name[SIMPLEFS_NAME_MAX];
    __u64   offset_sector;
    __u64   size_bytes;
    __u32   content_hash;
};

struct simplefs_meta_list {
    __u32 max_count;
    __u32 count;
    __u64 entries_ptr;
};
#define SIMPLEFS_IOC_GET_META    _IOWR(SIMPLEFS_IOC_MAGIC, 3, struct simplefs_meta_list)

struct simplefs_file_mapping {
    char    name[SIMPLEFS_NAME_MAX];
    __u64   start_sector;
    __u64   sector_count;
    __u64   size_bytes;
};
#define SIMPLEFS_IOC_GET_MAPPING _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_file_mapping)

#endif
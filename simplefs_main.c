// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/string.h>
#include <linux/statfs.h>
#include <linux/blkdev.h>
#include <linux/uaccess.h>
#include <linux/mount.h>
#include <linux/pagemap.h>

#include "simplefs.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SimpleFS");
MODULE_VERSION("0.1");

static char        *device_name         = "";
static ulong        sb_first_offset     = 0;
static ulong        sb_second_offset    = 64;
static uint         max_name_len_param  = SIMPLEFS_NAME_MAX;
static uint         max_file_sectors    = 8;

module_param(device_name, charp, 0444);
MODULE_PARM_DESC(device_name, "Block device name (informational)");
module_param(sb_first_offset, ulong, 0444);
MODULE_PARM_DESC(sb_first_offset, "Primary superblock sector");
module_param(sb_second_offset, ulong, 0444);
MODULE_PARM_DESC(sb_second_offset, "Backup superblock sector");
module_param(max_name_len_param, uint, 0444);
MODULE_PARM_DESC(max_name_len_param, "Max file name length");
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Max file size in sectors (M)");

struct simplefs_sb_info {
    u32 version;
    u32 block_size;
    u32 max_name_len;
    u32 max_file_sectors;
    u32 num_files;
    u64 sb_first_offset;
    u64 sb_second_offset;
    u64 total_sectors;
};

struct simplefs_inode_info {
    sector_t start_sector;
    u64      file_size_bytes;
    u32      file_index;
    struct inode vfs_inode;
};

static struct kmem_cache *simplefs_inode_cachep;

static inline struct simplefs_inode_info *SIMPLEFS_I(struct inode *inode)
{
    return container_of(inode, struct simplefs_inode_info, vfs_inode);
}

static u32 simplefs_sb_crc(const struct simplefs_super_block *dsb)
{
    struct simplefs_super_block tmp;
    memcpy(&tmp, dsb, sizeof(tmp));
    tmp.crc32 = 0;
    return crc32(0, (const void *)&tmp, sizeof(tmp));
}

static sector_t simplefs_file_start(const struct simplefs_sb_info *sbi, u32 index)
{
    u64 first_chunk = (sbi->sb_second_offset - sbi->sb_first_offset - 1) /
                       sbi->max_file_sectors;
    if (index < first_chunk)
        return sbi->sb_first_offset + 1 + (u64)index * sbi->max_file_sectors;
    else
        return sbi->sb_second_offset + 1 +
               (u64)(index - first_chunk) * sbi->max_file_sectors;
}

static u32 simplefs_total_files(const struct simplefs_sb_info *sbi)
{
    u64 first_chunk, second_chunk = 0;
    if (sbi->sb_second_offset <= sbi->sb_first_offset + 1)
        return 0;
    first_chunk = (sbi->sb_second_offset - sbi->sb_first_offset - 1) /
                   sbi->max_file_sectors;
    if (sbi->total_sectors > sbi->sb_second_offset + 1)
        second_chunk = (sbi->total_sectors - sbi->sb_second_offset - 1) /
                        sbi->max_file_sectors;
    return (u32)(first_chunk + second_chunk);
}

static int simplefs_read_sb_at(struct super_block *sb, sector_t where,
                               struct simplefs_super_block *out)
{
    struct buffer_head *bh = sb_bread(sb, where);
    if (!bh)
        return -EIO;
    memcpy(out, bh->b_data, sizeof(*out));
    brelse(bh);
    return 0;
}

static int simplefs_write_sb_at(struct super_block *sb, sector_t where,
                                const struct simplefs_super_block *in)
{
    struct buffer_head *bh = sb_bread(sb, where);
    if (!bh)
        return -EIO;
    memcpy(bh->b_data, in, sizeof(*in));
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

static void simplefs_build_disk_sb(const struct simplefs_sb_info *sbi,
                                   struct simplefs_super_block *dsb)
{
    memset(dsb, 0, sizeof(*dsb));
    dsb->magic            = cpu_to_le32(SIMPLEFS_MAGIC);
    dsb->version          = cpu_to_le32(sbi->version);
    dsb->block_size       = cpu_to_le32(sbi->block_size);
    dsb->max_name_len     = cpu_to_le32(sbi->max_name_len);
    dsb->max_file_sectors = cpu_to_le32(sbi->max_file_sectors);
    dsb->num_files        = cpu_to_le32(sbi->num_files);
    dsb->sb_first_offset  = cpu_to_le64(sbi->sb_first_offset);
    dsb->sb_second_offset = cpu_to_le64(sbi->sb_second_offset);
    dsb->total_sectors    = cpu_to_le64(sbi->total_sectors);
    dsb->crc32            = 0;
    dsb->crc32            = cpu_to_le32(simplefs_sb_crc(dsb));
}

static bool simplefs_sb_valid(const struct simplefs_super_block *dsb)
{
    u32 expected;
    if (le32_to_cpu(dsb->magic) != SIMPLEFS_MAGIC)
        return false;
    expected = simplefs_sb_crc(dsb);
    return expected == le32_to_cpu(dsb->crc32);
}

static void simplefs_fill_info_from_disk(struct simplefs_sb_info *sbi,
                                         const struct simplefs_super_block *dsb)
{
    sbi->version          = le32_to_cpu(dsb->version);
    sbi->block_size       = le32_to_cpu(dsb->block_size);
    sbi->max_name_len     = le32_to_cpu(dsb->max_name_len);
    sbi->max_file_sectors = le32_to_cpu(dsb->max_file_sectors);
    sbi->num_files        = le32_to_cpu(dsb->num_files);
    sbi->sb_first_offset  = le64_to_cpu(dsb->sb_first_offset);
    sbi->sb_second_offset = le64_to_cpu(dsb->sb_second_offset);
    sbi->total_sectors    = le64_to_cpu(dsb->total_sectors);
}

static struct inode *simplefs_alloc_inode(struct super_block *sb)
{
    struct simplefs_inode_info *ei = kmem_cache_alloc(simplefs_inode_cachep, GFP_KERNEL);
    if (!ei)
        return NULL;
    inode_init_once(&ei->vfs_inode);
    return &ei->vfs_inode;
}

static void simplefs_free_inode(struct inode *inode)
{
    kmem_cache_free(simplefs_inode_cachep, SIMPLEFS_I(inode));
}

static ssize_t simplefs_read(struct file *filp, char __user *buf,
                             size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct simplefs_inode_info *ei = SIMPLEFS_I(inode);
    struct super_block *sb = inode->i_sb;
    loff_t pos = *ppos;
    size_t copied = 0;

    if (pos < 0)
        return -EINVAL;
    if (pos >= (loff_t)ei->file_size_bytes)
        return 0;
    if (pos + len > ei->file_size_bytes)
        len = ei->file_size_bytes - pos;

    while (len > 0) {
        sector_t sec = ei->start_sector + (pos / SIMPLEFS_BLOCK_SIZE);
        size_t off   = pos % SIMPLEFS_BLOCK_SIZE;
        size_t chunk = min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - off);
        struct buffer_head *bh = sb_bread(sb, sec);
        if (!bh)
            return copied ? copied : -EIO;
        if (copy_to_user(buf + copied, bh->b_data + off, chunk)) {
            brelse(bh);
            return copied ? copied : -EFAULT;
        }
        brelse(bh);
        copied += chunk;
        pos    += chunk;
        len    -= chunk;
    }
    *ppos = pos;
    return copied;
}

static ssize_t simplefs_write(struct file *filp, const char __user *buf,
                              size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct simplefs_inode_info *ei = SIMPLEFS_I(inode);
    struct super_block *sb = inode->i_sb;
    loff_t pos = *ppos;
    size_t written = 0;

    if (pos < 0)
        return -EINVAL;
    if (pos >= (loff_t)ei->file_size_bytes)
        return -ENOSPC;
    if (pos + len > ei->file_size_bytes)
        len = ei->file_size_bytes - pos;

    while (len > 0) {
        sector_t sec = ei->start_sector + (pos / SIMPLEFS_BLOCK_SIZE);
        size_t off   = pos % SIMPLEFS_BLOCK_SIZE;
        size_t chunk = min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - off);
        struct buffer_head *bh = sb_bread(sb, sec);
        if (!bh)
            return written ? written : -EIO;
        if (copy_from_user(bh->b_data + off, buf + written, chunk)) {
            brelse(bh);
            return written ? written : -EFAULT;
        }
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);
        written += chunk;
        pos     += chunk;
        len     -= chunk;
    }
    *ppos = pos;
    inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
    return written;
}

static loff_t simplefs_llseek(struct file *filp, loff_t off, int whence)
{
    struct inode *inode = file_inode(filp);
    return generic_file_llseek_size(filp, off, whence, MAX_LFS_FILESIZE,
                                    inode->i_size);
}

static int simplefs_ioc_zero_all(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    u32 i;
    for (i = 0; i < sbi->num_files; i++) {
        sector_t start = simplefs_file_start(sbi, i);
        u32 s;
        for (s = 0; s < sbi->max_file_sectors; s++) {
            struct buffer_head *bh = sb_bread(sb, start + s);
            if (!bh)
                return -EIO;
            memset(bh->b_data, 0, SIMPLEFS_BLOCK_SIZE);
            mark_buffer_dirty(bh);
            sync_dirty_buffer(bh);
            brelse(bh);
        }
    }
    return 0;
}

static int simplefs_ioc_erase_fs(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    struct buffer_head *bh;
    int ret;

    ret = simplefs_ioc_zero_all(sb);
    if (ret)
        return ret;

    bh = sb_bread(sb, sbi->sb_first_offset);
    if (bh) {
        memset(bh->b_data, 0, SIMPLEFS_BLOCK_SIZE);
        mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh);
    }
    bh = sb_bread(sb, sbi->sb_second_offset);
    if (bh) {
        memset(bh->b_data, 0, SIMPLEFS_BLOCK_SIZE);
        mark_buffer_dirty(bh); sync_dirty_buffer(bh); brelse(bh);
    }
    return 0;
}

static int simplefs_file_hash(struct super_block *sb,
                              const struct simplefs_sb_info *sbi,
                              u32 idx, u32 *out)
{
    sector_t start = simplefs_file_start(sbi, idx);
    u32 s, crc = 0;
    for (s = 0; s < sbi->max_file_sectors; s++) {
        struct buffer_head *bh = sb_bread(sb, start + s);
        if (!bh)
            return -EIO;
        crc = crc32(crc, bh->b_data, SIMPLEFS_BLOCK_SIZE);
        brelse(bh);
    }
    *out = crc;
    return 0;
}

static int simplefs_ioc_get_meta(struct super_block *sb, void __user *arg)
{
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    struct simplefs_meta_list hdr;
    struct simplefs_file_meta __user *uentries;
    u32 i, n;

    if (copy_from_user(&hdr, arg, sizeof(hdr)))
        return -EFAULT;
    uentries = (struct simplefs_file_meta __user *)(uintptr_t)hdr.entries_ptr;
    n = min(hdr.max_count, sbi->num_files);

    for (i = 0; i < n; i++) {
        struct simplefs_file_meta m;
        int ret;
        memset(&m, 0, sizeof(m));
        snprintf(m.name, sizeof(m.name), "file_%u", i);
        m.offset_sector = simplefs_file_start(sbi, i);
        m.size_bytes    = (u64)sbi->max_file_sectors * SIMPLEFS_BLOCK_SIZE;
        ret = simplefs_file_hash(sb, sbi, i, &m.content_hash);
        if (ret)
            return ret;
        if (copy_to_user(&uentries[i], &m, sizeof(m)))
            return -EFAULT;
    }
    hdr.count = n;
    if (copy_to_user(arg, &hdr, sizeof(hdr)))
        return -EFAULT;
    return 0;
}

static int simplefs_ioc_get_mapping(struct super_block *sb, void __user *arg)
{
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    struct simplefs_file_mapping m;
    u32 idx;

    if (copy_from_user(&m, arg, sizeof(m)))
        return -EFAULT;
    m.name[sizeof(m.name) - 1] = 0;
    if (strncmp(m.name, "file_", 5) != 0)
        return -ENOENT;
    if (kstrtou32(m.name + 5, 10, &idx))
        return -ENOENT;
    if (idx >= sbi->num_files)
        return -ENOENT;

    m.start_sector = simplefs_file_start(sbi, idx);
    m.sector_count = sbi->max_file_sectors;
    m.size_bytes   = (u64)sbi->max_file_sectors * SIMPLEFS_BLOCK_SIZE;
    if (copy_to_user(arg, &m, sizeof(m)))
        return -EFAULT;
    return 0;
}

static long simplefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct super_block *sb = file_inode(filp)->i_sb;
    switch (cmd) {
    case SIMPLEFS_IOC_ZERO_ALL:
        return simplefs_ioc_zero_all(sb);
    case SIMPLEFS_IOC_ERASE_FS:
        return simplefs_ioc_erase_fs(sb);
    case SIMPLEFS_IOC_GET_META:
        return simplefs_ioc_get_meta(sb, (void __user *)arg);
    case SIMPLEFS_IOC_GET_MAPPING:
        return simplefs_ioc_get_mapping(sb, (void __user *)arg);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations simplefs_file_ops = {
    .owner          = THIS_MODULE,
    .llseek         = simplefs_llseek,
    .read           = simplefs_read,
    .write          = simplefs_write,
    .unlocked_ioctl = simplefs_ioctl,
    .compat_ioctl   = simplefs_ioctl,
};

static int simplefs_setattr(struct mnt_idmap *idmap,
                            struct dentry *dentry, struct iattr *iattr)
{
    struct inode *inode = d_inode(dentry);
    int ret;

    iattr->ia_valid &= ~ATTR_SIZE;

    ret = setattr_prepare(idmap, dentry, iattr);
    if (ret)
        return ret;

    setattr_copy(idmap, inode, iattr);
    mark_inode_dirty(inode);
    return 0;
}

static const struct inode_operations simplefs_file_iops = {
    .setattr = simplefs_setattr,
};

static struct inode *simplefs_get_file_inode(struct super_block *sb, u32 idx);

static int simplefs_iterate(struct file *file, struct dir_context *ctx)
{
    struct inode *dir = file_inode(file);
    struct simplefs_sb_info *sbi = dir->i_sb->s_fs_info;

    if (!dir_emit_dots(file, ctx))
        return 0;
    while (ctx->pos - 2 < sbi->num_files) {
        u32 idx = (u32)(ctx->pos - 2);
        char name[48];
        int len = snprintf(name, sizeof(name), "file_%u", idx);
        if (!dir_emit(ctx, name, len,
                      SIMPLEFS_FIRST_FILE_INO + idx, DT_REG))
            return 0;
        ctx->pos++;
    }
    return 0;
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
                                      unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    const char *name = dentry->d_name.name;
    struct inode *inode = NULL;
    u32 idx;

    if (dentry->d_name.len >= SIMPLEFS_NAME_MAX)
        return ERR_PTR(-ENAMETOOLONG);

    if (strncmp(name, "file_", 5) == 0 &&
        kstrtou32(name + 5, 10, &idx) == 0 &&
        idx < sbi->num_files) {
        inode = simplefs_get_file_inode(sb, idx);
        if (IS_ERR(inode))
            return ERR_CAST(inode);
    }
    return d_splice_alias(inode, dentry);
}

static const struct file_operations simplefs_dir_ops = {
    .owner          = THIS_MODULE,
    .read           = generic_read_dir,
    .iterate_shared = simplefs_iterate,
    .llseek         = generic_file_llseek,
    .unlocked_ioctl = simplefs_ioctl,
};

static const struct inode_operations simplefs_dir_iops = {
    .lookup = simplefs_lookup,
};

static struct inode *simplefs_get_file_inode(struct super_block *sb, u32 idx)
{
    struct simplefs_sb_info *sbi = sb->s_fs_info;
    unsigned long ino = SIMPLEFS_FIRST_FILE_INO + idx;
    struct inode *inode = iget_locked(sb, ino);
    struct simplefs_inode_info *ei;

    if (!inode)
        return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW))
        return inode;

    ei = SIMPLEFS_I(inode);
    ei->file_index      = idx;
    ei->start_sector    = simplefs_file_start(sbi, idx);
    ei->file_size_bytes = (u64)sbi->max_file_sectors * SIMPLEFS_BLOCK_SIZE;

    inode->i_mode = S_IFREG | 0644;
    i_uid_write(inode, 0);
    i_gid_write(inode, 0);
    inode->i_size  = ei->file_size_bytes;
    inode->i_blocks = sbi->max_file_sectors;
    inode->i_op    = &simplefs_file_iops;
    inode->i_fop   = &simplefs_file_ops;
    set_nlink(inode, 1);
    simple_inode_init_ts(inode);

    unlock_new_inode(inode);
    return inode;
}

static struct inode *simplefs_get_root(struct super_block *sb)
{
    struct inode *inode = iget_locked(sb, 1);
    if (!inode) return ERR_PTR(-ENOMEM);
    if (!(inode->i_state & I_NEW)) return inode;

    inode->i_mode = S_IFDIR | 0755;
    inode->i_op   = &simplefs_dir_iops;
    inode->i_fop  = &simplefs_dir_ops;
    set_nlink(inode, 2);
    simple_inode_init_ts(inode);
    unlock_new_inode(inode);
    return inode;
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct simplefs_sb_info *sbi = dentry->d_sb->s_fs_info;
    buf->f_type    = SIMPLEFS_MAGIC;
    buf->f_bsize   = SIMPLEFS_BLOCK_SIZE;
    buf->f_blocks  = sbi->total_sectors;
    buf->f_bfree   = 0;
    buf->f_bavail  = 0;
    buf->f_files   = sbi->num_files;
    buf->f_ffree   = 0;
    buf->f_namelen = sbi->max_name_len;
    return 0;
}

static void simplefs_put_super(struct super_block *sb)
{
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct super_operations simplefs_sops = {
    .alloc_inode   = simplefs_alloc_inode,
    .free_inode    = simplefs_free_inode,
    .statfs        = simplefs_statfs,
    .put_super     = simplefs_put_super,
    .drop_inode    = generic_delete_inode,
};

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct simplefs_sb_info *sbi;
    struct simplefs_super_block *dsb_primary = NULL;
    struct simplefs_super_block *dsb_backup  = NULL;
    struct simplefs_super_block *dsb_new     = NULL;
    struct inode *root;
    u64 total_sectors;
    int ret;
    bool primary_ok, backup_ok;

    if (!sb_set_blocksize(sb, SIMPLEFS_BLOCK_SIZE)) {
        pr_err("simplefs: failed to set blocksize\n");
        return -EINVAL;
    }
    sb->s_magic      = SIMPLEFS_MAGIC;
    sb->s_op         = &simplefs_sops;
    sb->s_maxbytes   = MAX_LFS_FILESIZE;
    sb->s_time_gran  = 1;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;
    sb->s_fs_info = sbi;

    dsb_primary = kmalloc(sizeof(*dsb_primary), GFP_KERNEL);
    dsb_backup  = kmalloc(sizeof(*dsb_backup),  GFP_KERNEL);
    dsb_new     = kmalloc(sizeof(*dsb_new),     GFP_KERNEL);
    if (!dsb_primary || !dsb_backup || !dsb_new) {
        ret = -ENOMEM;
        goto err;
    }

    total_sectors = bdev_nr_sectors(sb->s_bdev);

    ret = simplefs_read_sb_at(sb, sb_first_offset, dsb_primary);
    if (ret) goto err;
    ret = simplefs_read_sb_at(sb, sb_second_offset, dsb_backup);
    if (ret) goto err;
    primary_ok = simplefs_sb_valid(dsb_primary);
    backup_ok  = simplefs_sb_valid(dsb_backup);

    if (primary_ok) {
        simplefs_fill_info_from_disk(sbi, dsb_primary);
        pr_info("simplefs: primary SB OK\n");
        if (!backup_ok) {
            pr_warn("simplefs: backup SB broken, restoring\n");
            simplefs_build_disk_sb(sbi, dsb_new);
            simplefs_write_sb_at(sb, sbi->sb_second_offset, dsb_new);
        }
    } else if (backup_ok) {
        simplefs_fill_info_from_disk(sbi, dsb_backup);
        pr_warn("simplefs: primary SB broken, using backup and restoring primary\n");
        simplefs_build_disk_sb(sbi, dsb_new);
        simplefs_write_sb_at(sb, sbi->sb_first_offset, dsb_new);
    } else {
        pr_info("simplefs: no valid SB found, initializing new FS\n");
        if (sb_second_offset <= sb_first_offset + 1 ||
            sb_second_offset + 1 >= total_sectors) {
            pr_err("simplefs: bad sb offsets (first=%lu second=%lu total=%llu)\n",
                   sb_first_offset, sb_second_offset, total_sectors);
            ret = -EINVAL;
            goto err;
        }
        sbi->version          = SIMPLEFS_VERSION;
        sbi->block_size       = SIMPLEFS_BLOCK_SIZE;
        sbi->max_name_len     = min_t(u32, max_name_len_param, SIMPLEFS_NAME_MAX);
        sbi->max_file_sectors = max_file_sectors;
        sbi->sb_first_offset  = sb_first_offset;
        sbi->sb_second_offset = sb_second_offset;
        sbi->total_sectors    = total_sectors;
        sbi->num_files        = simplefs_total_files(sbi);

        simplefs_build_disk_sb(sbi, dsb_new);
        simplefs_write_sb_at(sb, sbi->sb_first_offset, dsb_new);
        simplefs_write_sb_at(sb, sbi->sb_second_offset, dsb_new);
        pr_info("simplefs: initialized %u files (M=%u)\n",
                sbi->num_files, sbi->max_file_sectors);
    }

    root = simplefs_get_root(sb);
    if (IS_ERR(root)) { ret = PTR_ERR(root); goto err; }
    sb->s_root = d_make_root(root);
    if (!sb->s_root) { ret = -ENOMEM; goto err; }

    pr_info("simplefs: mounted; files=%u M=%u sb1=%llu sb2=%llu\n",
            sbi->num_files, sbi->max_file_sectors,
            sbi->sb_first_offset, sbi->sb_second_offset);

    kfree(dsb_primary);
    kfree(dsb_backup);
    kfree(dsb_new);
    return 0;

err:
    kfree(dsb_primary);
    kfree(dsb_backup);
    kfree(dsb_new);
    kfree(sbi);
    sb->s_fs_info = NULL;
    return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
                                     int flags, const char *dev_name, void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static struct file_system_type simplefs_type = {
    .owner      = THIS_MODULE,
    .name       = "simplefs",
    .mount      = simplefs_mount,
    .kill_sb    = kill_block_super,
    .fs_flags   = FS_REQUIRES_DEV,
};

static void simplefs_inode_once(void *p)
{
    struct simplefs_inode_info *ei = p;
    inode_init_once(&ei->vfs_inode);
}

static int __init simplefs_init(void)
{
    int ret;
    simplefs_inode_cachep = kmem_cache_create("simplefs_inode_cache",
        sizeof(struct simplefs_inode_info), 0,
        SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT, simplefs_inode_once);
    if (!simplefs_inode_cachep) return -ENOMEM;

    ret = register_filesystem(&simplefs_type);
    if (ret) { kmem_cache_destroy(simplefs_inode_cachep); return ret; }
    pr_info("simplefs: registered\n");
    return 0;
}

static void __exit simplefs_exit(void)
{
    unregister_filesystem(&simplefs_type);
    rcu_barrier();
    kmem_cache_destroy(simplefs_inode_cachep);
    pr_info("simplefs: unregistered\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);
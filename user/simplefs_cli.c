#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "../simplefs.h"
#include <stdint.h>

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s demo <mountpoint>\n"
        "  %s zero <mountpoint>\n"
        "  %s erase <mountpoint>\n"
        "  %s meta <mountpoint>\n"
        "  %s mapping <mountpoint> <filename>\n", p, p, p, p, p);
    exit(1);
}

static int open_any_file(const char *mnt, char *fullpath, size_t sz)
{
    DIR *d = opendir(mnt);
    struct dirent *de;
    if (!d) { perror("opendir"); return -1; }
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "file_", 5) == 0) {
            snprintf(fullpath, sz, "%s/%s", mnt, de->d_name);
            closedir(d);
            return open(fullpath, O_RDWR);
        }
    }
    closedir(d);
    return -1;
}

static int cmd_demo(const char *mnt)
{
    DIR *d = opendir(mnt);
    struct dirent *de;
    if (!d) { perror("opendir"); return 1; }

    srand(time(NULL));
    int ok = 0, fail = 0;

    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "file_", 5) != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", mnt, de->d_name);

        int fd = open(path, O_RDWR);
        if (fd < 0) { perror(path); fail++; continue; }

        unsigned long long wr = ((unsigned long long)rand() << 32) | rand();
        unsigned long long rd = 0;

        if (pwrite(fd, &wr, sizeof(wr), 0) != sizeof(wr)) {
            perror("pwrite"); close(fd); fail++; continue;
        }
        fsync(fd);
        if (pread(fd, &rd, sizeof(rd), 0) != sizeof(rd)) {
            perror("pread"); close(fd); fail++; continue;
        }
        close(fd);

        if (wr == rd) {
            printf("[OK]   %s  0x%016llx\n", de->d_name, wr);
            ok++;
        } else {
            printf("[FAIL] %s  wrote=0x%016llx  read=0x%016llx\n", de->d_name, wr, rd);
            fail++;
        }
    }
    closedir(d);
    printf("\nTotal: ok=%d fail=%d\n", ok, fail);
    return fail ? 1 : 0;
}

static int cmd_zero(const char *mnt)
{
    char path[512]; int fd = open_any_file(mnt, path, sizeof(path));
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, SIMPLEFS_IOC_ZERO_ALL) < 0) { perror("ioctl ZERO_ALL"); close(fd); return 1; }
    close(fd);
    printf("All files zeroed.\n");
    return 0;
}

static int cmd_erase(const char *mnt)
{
    char path[512]; int fd = open_any_file(mnt, path, sizeof(path));
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, SIMPLEFS_IOC_ERASE_FS) < 0) { perror("ioctl ERASE_FS"); close(fd); return 1; }
    close(fd);
    printf("FS erased (unmount to complete).\n");
    return 0;
}

static int cmd_meta(const char *mnt)
{
    char path[512]; int fd = open_any_file(mnt, path, sizeof(path));
    if (fd < 0) { perror("open"); return 1; }

    struct simplefs_meta_list hdr;
    struct simplefs_file_meta *arr;
    unsigned max = 1024;

    arr = calloc(max, sizeof(*arr));
    if (!arr) { close(fd); return 1; }

    memset(&hdr, 0, sizeof(hdr));
    hdr.max_count   = max;
    hdr.entries_ptr = (unsigned long long)(uintptr_t)arr;

    if (ioctl(fd, SIMPLEFS_IOC_GET_META, &hdr) < 0) {
        perror("ioctl GET_META"); close(fd); free(arr); return 1;
    }
    close(fd);

    printf("%-20s %12s %12s  %s\n", "NAME", "OFFSET_SEC", "SIZE", "CRC32");
    for (unsigned i = 0; i < hdr.count; i++) {
        printf("%-20s %12llu %12llu  0x%08x\n",
            arr[i].name,
            (unsigned long long)arr[i].offset_sector,
            (unsigned long long)arr[i].size_bytes,
            arr[i].content_hash);
    }
    free(arr);
    return 0;
}

static int cmd_mapping(const char *mnt, const char *fname)
{
    char path[512]; int fd = open_any_file(mnt, path, sizeof(path));
    if (fd < 0) { perror("open"); return 1; }

    struct simplefs_file_mapping m;
    memset(&m, 0, sizeof(m));
    strncpy(m.name, fname, sizeof(m.name) - 1);

    if (ioctl(fd, SIMPLEFS_IOC_GET_MAPPING, &m) < 0) {
        perror("ioctl GET_MAPPING"); close(fd); return 1;
    }
    close(fd);

    printf("name:          %s\n", m.name);
    printf("start_sector:  %llu\n", (unsigned long long)m.start_sector);
    printf("sector_count:  %llu\n", (unsigned long long)m.sector_count);
    printf("size_bytes:    %llu\n", (unsigned long long)m.size_bytes);
    printf("sectors:       [%llu .. %llu]\n",
           (unsigned long long)m.start_sector,
           (unsigned long long)(m.start_sector + m.sector_count - 1));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) usage(argv[0]);
    const char *cmd = argv[1];
    const char *mnt = argv[2];

    if (!strcmp(cmd, "demo"))    return cmd_demo(mnt);
    if (!strcmp(cmd, "zero"))    return cmd_zero(mnt);
    if (!strcmp(cmd, "erase"))   return cmd_erase(mnt);
    if (!strcmp(cmd, "meta"))    return cmd_meta(mnt);
    if (!strcmp(cmd, "mapping")) {
        if (argc < 4) usage(argv[0]);
        return cmd_mapping(mnt, argv[3]);
    }
    usage(argv[0]);
    return 1;
}
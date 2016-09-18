// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <magenta/types.h>
#include <mxio/io.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include <magenta/device/block.h>
#include <magenta/device/device.h>
#include <hexdump/hexdump.h>

#define DEV_BLOCK "/dev/class/block"

static char* size_to_cstring(char* str, size_t maxlen, uint64_t size) {
    const char* unit;
    uint64_t div;
    if (size < 1024) {
        unit = "";
        div = 1;
    } else if (size >= 1024 && size < 1024 * 1024) {
        unit = "K";
        div = 1024;
    } else if (size >= 1024 * 1024 && size < 1024 * 1024 * 1024) {
        unit = "M";
        div = 1024 * 1024;
    } else if (size >= 1024 * 1024 * 1024 && size < 1024llu * 1024 * 1024 * 1024) {
        unit = "G";
        div = 1024 * 1024 * 1024;
    } else {
        unit = "T";
        div = 1024llu * 1024 * 1024 * 1024;
    }
    snprintf(str, maxlen, "%llu%s", size / div, unit);
    return str;
}

static const char* guid_to_type(char* guid) {
    if (!strcmp("FE3A2A5D-4F32-41A7-B725-ACCC3285A309", guid)) {
        return "cros kernel";
    } else if (!strcmp("3CB8E202-3B7E-47DD-8A3C-7FF2A13CFCEC", guid)) {
        return "cros rootfs";
    } else if (!strcmp("2E0A753D-9E48-43B0-8337-B15192CB1B5E", guid)) {
        return "cros reserved";
    } else if (!strcmp("CAB6E88E-ABF3-4102-A07A-D4BB9BE3C1D3", guid)) {
        return "cros firmware";
    } else if (!strcmp("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", guid)) {
        return "efi system";
    } else if (!strcmp("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", guid)) {
        return "data";
    } else {
        return "unknown";
    }
}

typedef struct blkinfo {
    char path[128];
    char devname[128];
    char drvname[128];
    char guid[40];
    char label[40];
    char sizestr[6];
} blkinfo_t;

static int cmd_list_blk(void) {
    struct dirent* de;
    DIR* dir = opendir(DEV_BLOCK);
    if (!dir) {
        printf("Error opening %s\n", DEV_BLOCK);
        return -1;
    }
    blkinfo_t info;
    const char* type;
    uint64_t size;
    int fd;
    printf("%-3s %-8s %-8s %-4s %-14s %s\n", "ID", "DEV", "DRV", "SIZE", "TYPE", "LABEL");
    while ((de = readdir(dir)) != NULL) {
        memset(&info, 0, sizeof(blkinfo_t));
        type = NULL;
        snprintf(info.path, sizeof(info.path), "%s/%s", DEV_BLOCK, de->d_name);
        fd = open(info.path, O_RDONLY);
        if (fd < 0) {
            printf("Error opening %s\n", info.path);
            goto devdone;
        }
        mxio_ioctl(fd, IOCTL_DEVICE_GET_DEVICE_NAME, NULL, 0, info.devname, sizeof(info.devname));
        mxio_ioctl(fd, IOCTL_DEVICE_GET_DRIVER_NAME, NULL, 0, info.drvname, sizeof(info.drvname));
        if (mxio_ioctl(fd, IOCTL_BLOCK_GET_SIZE, NULL, 0, &size, sizeof(size)) > 0) {
            size_to_cstring(info.sizestr, sizeof(info.sizestr), size);
        }
        if (mxio_ioctl(fd, IOCTL_BLOCK_GET_GUID, NULL, 0, info.guid, sizeof(info.guid)) == NO_ERROR) {
            type = guid_to_type(info.guid);
        }
        mxio_ioctl(fd, IOCTL_BLOCK_GET_NAME, NULL, 0, info.label, sizeof(info.label));
devdone:
        close(fd);
        printf("%-3s %-8s %-8s %4s %-14s %s\n", de->d_name, info.devname, info.drvname, info.sizestr, type ? type : "", info.label);
    }
out:
    closedir(dir);
    return 0;
}

static int cmd_read_blk(const char* dev, off_t offset, size_t count) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        printf("Error opening %s\n", dev);
        return fd;
    }

    // check that count and offset are aligned to block size
    uint64_t blksize;
    int rc = mxio_ioctl(fd, IOCTL_BLOCK_GET_BLOCKSIZE, NULL, 0, &blksize, sizeof(blksize));
    if (rc < 0) {
        printf("Error getting block size for %s\n", dev);
        close(fd);
        goto out;
    }
    if (count % blksize) {
        printf("Bytes read must be a multiple of blksize=%llu\n", blksize);
        rc = -1;
        goto out;
    }
    if (offset % blksize) {
        printf("Offset must be a multiple of blksize=%llu\n", blksize);
        rc = -1;
        goto out;
    }

    // read the data
    void* buf = malloc(count);
    if (offset) {
        rc = lseek(fd, offset, SEEK_SET);
        if (rc < 0) {
            printf("Error %d seeking to offset %lld\n", rc, offset);
            goto out2;
        }
    }
    ssize_t c = read(fd, buf, count);
    if (c < 0) {
        printf("Error %zd in read()\n", c);
        rc = c;
        goto out2;
    }

    hexdump8_ex(buf, c, offset);

out2:
    free(buf);
out:
    close(fd);
    return 0;
}

int main(int argc, const char** argv) {
    int rc = 0;
    const char *cmd = argc > 1 ? argv[1] : NULL;
    if (cmd) {
        if (!strcmp(cmd, "help")) {
            goto usage;
        } else if (!strcmp(cmd, "read")) {
            if (argc < 5) goto usage;
            rc = cmd_read_blk(argv[2], strtoul(argv[3], NULL, 10), strtoull(argv[4], NULL, 10));
        } else {
            printf("Unrecognized command %s!\n", cmd);
            goto usage;
        }
    } else {
        rc = cmd_list_blk();
    }
    return rc;
usage:
    printf("Usage:\n");
    printf("%s\n", argv[0]);
    printf("%s read <blkdev> <offset> <count>\n", argv[0]);
    return 0;
}

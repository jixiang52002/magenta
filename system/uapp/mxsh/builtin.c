// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mxsh.h"

#include <hexdump/hexdump.h>
#include <launchpad/launchpad.h>
#include <magenta/syscalls.h>
#include <mxio/vfs.h>
#include <magenta/listnode.h>

static int mxc_dump(int argc, char** argv) {
    int fd;
    ssize_t len;
    off_t off;
    char buf[4096];

    if (argc != 2) {
        fprintf(stderr, "usage: dump <filename>\n");
        return -1;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        return -1;
    }
    off = 0;
    for (;;) {
        len = read(fd, buf, sizeof(buf));
        if (len <= 0) {
            if (len)
                fprintf(stderr, "error: io\n");
            break;
        }
        hexdump8_ex(buf, len, off);
        off += len;
    }
    close(fd);
    return len;
}

static int mxc_echo(int argc, char** argv) {
    argc--;
    argv++;
    while (argc > 0) {
        write(1, argv[0], strlen(argv[0]));
        argc--;
        argv++;
        if (argc)
            write(1, " ", 1);
    }
    write(1, "\n", 1);
    return 0;
}

static int mxc_msleep(int argc, char** argv) {
    if (argc == 2) {
        mx_nanosleep(MX_MSEC(atoi(argv[1])));
    }
    return 0;
}

static int mxc_cd(int argc, char** argv) {
    if (argc < 2) {
        return 0;
    }
    if (chdir(argv[1])) {
        fprintf(stderr, "error: cannot change directory to '%s'\n", argv[1]);
        return -1;
    }
    return 0;
}

static const char* modestr(uint32_t mode) {
    switch (mode & S_IFMT) {
    case S_IFREG:
        return "-";
    case S_IFCHR:
        return "c";
    case S_IFBLK:
        return "b";
    case S_IFDIR:
        return "d";
    default:
        return "?";
    }
}

static int mxc_ls(int argc, char** argv) {
    const char* dirn;
    struct stat s;
    char tmp[2048];
    size_t dirln;
    struct dirent* de;
    DIR* dir;

    if ((argc > 1) && !strcmp(argv[1], "-l")) {
        argc--;
        argv++;
    }
    if (argc < 2) {
        dirn = ".";
    } else {
        dirn = argv[1];
    }
    dirln = strlen(dirn);

    if (argc > 2) {
        fprintf(stderr, "usage: ls [ <directory> ]\n");
        return -1;
    }
    if ((dir = opendir(dirn)) == NULL) {
        fprintf(stderr, "error: cannot open '%s'\n", dirn);
        return -1;
    }
    while((de = readdir(dir)) != NULL) {
        memset(&s, 0, sizeof(struct stat));
        if ((strlen(de->d_name) + dirln + 2) <= sizeof(tmp)) {
            snprintf(tmp, sizeof(tmp), "%s/%s", dirn, de->d_name);
            stat(tmp, &s);
        }
        printf("%s %8llu %s\n", modestr(s.st_mode), s.st_size, de->d_name);
    }
    closedir(dir);
    return 0;
}

static int mxc_list(int argc, char** argv) {
    char line[1024];
    FILE* fp;
    int num = 1;

    if (argc != 2) {
        printf("usage: list <filename>\n");
        return -1;
    }

    fp = fopen(argv[1], "r");
    if (fp == NULL) {
        printf("error: cannot open '%s'\n", argv[1]);
        return -1;
    }
    while (fgets(line, 1024, fp) != NULL) {
        printf("%5d | %s", num, line);
        num++;
    }
    fclose(fp);
    return 0;
}

static int mxc_cp(int argc, char** argv) {
    char data[4096];
    int fdi = -1, fdo = -1;
    int r, wr;
    int count = 0;
    if (argc != 3) {
        fprintf(stderr, "usage: cp <srcfile> <dstfile>\n");
        return -1;
    }
    if ((fdi = open(argv[1], O_RDONLY)) < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        return -1;
    }
    if ((fdo = open(argv[2], O_WRONLY | O_CREAT)) < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[2]);
        r = fdo;
        goto done;
    }
    for (;;) {
        if ((r = read(fdi, data, sizeof(data))) < 0) {
            fprintf(stderr, "error: failed reading from '%s'\n", argv[1]);
            break;
        }
        if (r == 0) {
            break;
        }
        if ((wr = write(fdo, data, r)) != r) {
            fprintf(stderr, "error: failed writing to '%s'\n", argv[2]);
            r = wr;
            break;
        }
        count += r;
    }
    fprintf(stderr, "[copied %d bytes]\n", count);
done:
    close(fdi);
    close(fdo);
    return r;
}

static int mxc_mkdir(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir <path>\n");
        return -1;
    }
    while (argc > 1) {
        argc--;
        argv++;
        if (mkdir(argv[0], 0755)) {
            fprintf(stderr, "error: failed to make directory '%s'\n", argv[0]);
        }
    }
    return 0;
}

static int mxc_mv(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mv <old path> <new path>\n");
        return -1;
    }
    if (rename(argv[1], argv[2])) {
        fprintf(stderr, "error: failed to rename '%s' to '%s'\n", argv[1], argv[2]);
    }
    return 0;
}

static int mxc_rm(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rm <filename>\n");
        return -1;
    }
    while (argc > 1) {
        argc--;
        argv++;
        if (unlink(argv[0])) {
            fprintf(stderr, "error: failed to delete '%s'\n", argv[0]);
        }
    }
    return 0;
}

typedef struct failure {
    list_node_t node;
    int cause;
    int rc;
    char name[0];
} failure_t;

static void mxc_fail_test(list_node_t* failures, const char* name, int cause, int rc) {
    size_t name_len = strlen(name) + 1;
    failure_t* failure = malloc(sizeof(failure_t) + name_len);
    failure->cause = cause;
    failure->rc = rc;
    memcpy(failure->name, name, name_len);
    list_add_tail(failures, &failure->node);
}

enum {
    FAILED_TO_LAUNCH,
    FAILED_TO_WAIT,
    FAILED_TO_RETURN_CODE,
    FAILED_NONZERO_RETURN_CODE,
};
static int mxc_runtests(int argc, char** argv) {
    list_node_t failures = LIST_INITIAL_VALUE(failures);

    int total_count = 0;
    int failed_count = 0;

    const char* dirn = "/boot/test";
    DIR* dir = opendir(dirn);
    if (dir == NULL) {
        printf("error: cannot open '%s'\n", dirn);
        return -1;
    }

    // We want the default to be the same, whether the test is run by us
    // or run standalone. Do this by leaving the verbosity unspecified unless
    // provided by the user.
    int verbosity = -1;

    if (argc > 1) {
        if (strcmp(argv[1], "-q") == 0) {
            verbosity = 0;
        } else if (strcmp(argv[1], "-v") == 0) {
            printf("verbose output. enjoy.\n");
            verbosity = 1;
        } else {
            printf("unknown option. usage: %s [-q|-v]\n", argv[0]);
            return -1;
        }
    }

    struct dirent* de;
    struct stat stat_buf;
    while ((de = readdir(dir)) != NULL) {
        char name[11 + NAME_MAX + 1];
        snprintf(name, sizeof(name), "/boot/test/%s", de->d_name);
        if (stat(name, &stat_buf) != 0 || !S_ISREG(stat_buf.st_mode)) {
            continue;
        }

        total_count++;
        if (verbosity) {
            printf(
                "\n------------------------------------------------\n"
                "RUNNING TEST: %s\n\n",
                de->d_name);
        }

        char verbose_opt[] = {'v','=', verbosity + '0', 0};
        const char* argv[] = {name, verbose_opt};
        int argc = verbosity >= 0 ? 2 : 1;

        mx_handle_t handle = launchpad_launch_mxio(name, argc, argv);
        if (handle < 0) {
            printf("FAILURE: Failed to launch %s: %d\n", de->d_name, handle);
            mxc_fail_test(&failures, de->d_name, FAILED_TO_LAUNCH, 0);
            failed_count++;
            continue;
        }

        mx_status_t status = mx_handle_wait_one(handle, MX_SIGNAL_SIGNALED,
                                                      MX_TIME_INFINITE, NULL);
        if (status != NO_ERROR) {
            printf("FAILURE: Failed to wait for process exiting %s: %d\n", de->d_name, status);
            mxc_fail_test(&failures, de->d_name, FAILED_TO_WAIT, 0);
            failed_count++;
            continue;
        }

        // read the return code
        mx_info_process_t proc_info;
        mx_ssize_t info_status = mx_object_get_info(handle, MX_INFO_PROCESS, sizeof(proc_info.rec),
                &proc_info, sizeof(proc_info));
        mx_handle_close(handle);

        if (info_status != sizeof(proc_info)) {
            printf("FAILURE: Failed to get process return code %s: %ld\n", de->d_name, info_status);
            mxc_fail_test(&failures, de->d_name, FAILED_TO_RETURN_CODE, 0);
            failed_count++;
            continue;
        }

        if (proc_info.rec.return_code == 0) {
            printf("PASSED: %s passed\n", de->d_name);
        } else {
            printf("FAILED: %s exited with nonzero status: %d\n", de->d_name, proc_info.rec.return_code);
            mxc_fail_test(&failures, de->d_name, FAILED_NONZERO_RETURN_CODE, proc_info.rec.return_code);
            failed_count++;
        }
    }

    closedir(dir);

    printf("\nSUMMARY: Ran %d tests: %d failed\n", total_count, failed_count);

    if (failed_count) {
        printf("\nThe following tests failed:\n");
        failure_t* failure = NULL;
        failure_t* temp = NULL;
        list_for_every_entry_safe (&failures, failure, temp, failure_t, node) {
            switch (failure->cause) {
            case FAILED_TO_LAUNCH:
                printf("%s: failed to launch\n", failure->name);
                break;
            case FAILED_TO_WAIT:
                printf("%s: failed to wait\n", failure->name);
                break;
            case FAILED_TO_RETURN_CODE:
                printf("%s: failed to return exit code\n", failure->name);
                break;
            case FAILED_NONZERO_RETURN_CODE:
                printf("%s: returned nonzero: %d\n", failure->name, failure->rc);
                break;
            }
            free(failure);
        }
    }

    return 0;
}

static int mxc_dm(int argc, char** argv) {
    if (argc != 2) {
        printf("usage: dm <command>\n");
        return -1;
    }
    int fd = open("/dev/dmctl", O_RDWR);
    if (fd >= 0) {
        int r = write(fd, argv[1], strlen(argv[1]));
        if (r < 0) {
            fprintf(stderr, "error: cannot write dmctl: %d\n", r);
        }
        close(fd);
        return r;
    } else {
        fprintf(stderr, "error: cannot open dmctl: %d\n", fd);
        return fd;
    }
}

static int mxc_help(int argc, char** argv);

builtin_t builtins[] = {
    {"cd", mxc_cd, "change directory"},
    {"cp", mxc_cp, "copy a file"},
    {"dump", mxc_dump, "display a file in hexadecimal"},
    {"echo", mxc_echo, "print its arguments"},
    {"help", mxc_help, "list built-in shell commands"},
    {"dm", mxc_dm, "send command to device manager"},
    {"list", mxc_list, "display a text file with line numbers"},
    {"ls", mxc_ls, "list directory contents"},
    {"mkdir", mxc_mkdir, "create a directory" },
    {"mv", mxc_mv, "rename a file or directory" },
    {"rm", mxc_rm, "delete a file"},
    {"runtests", mxc_runtests, "run all test programs"},
    {"msleep", mxc_msleep, "pause for milliseconds"},
    {NULL, NULL, NULL},
};

static int mxc_help(int argc, char** argv) {
    builtin_t* b;
    int n = 8;
    for (b = builtins; b->name != NULL; b++) {
        int len = strlen(b->name);
        if (len > n)
            n = len;
    }
    for (b = builtins; b->name != NULL; b++) {
        printf("%-*s  %s\n", n, b->name, b->desc);
    }
    printf("%-*s %s\n", n, "<program>", "run <program>");
    printf("%-*s  %s\n\n", n, "`command", "send command to kernel console");
    return 0;
}

// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <launchpad/launchpad.h>
#include <launchpad/vmo.h>

#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls-ddk.h>

#include <mxio/debug.h>
#include <mxio/io.h>
#include <mxio/remoteio.h>

#include <magenta/listnode.h>

#include "mxsh.h"

#define LINE_MAX 1024

static bool interactive = false;

void cputc(uint8_t ch) {
    write(1, &ch, 1);
}

void cputs(const char* s, size_t len) {
    write(1, s, len);
}

int cgetc(void) {
    uint8_t ch;
    for (;;) {
        mxio_wait_fd(0, MXIO_EVT_READABLE, NULL, MX_TIME_INFINITE);
        int r = read(0, &ch, 1);
        if (r < 0) {
            return r;
        }
        if (r == 1) {
            return ch;
        }
    }
}

void beep(void) {
}

#define CTRL_C 3
#define BACKSPACE 8
#define NL 10
#define CTRL_L 12
#define CR 13
#define ESC 27
#define DELETE 127

#define EXT_UP 'A'
#define EXT_DOWN 'B'
#define EXT_RIGHT 'C'
#define EXT_LEFT 'D'

typedef struct {
    list_node_t node;
    int len;
    char line[LINE_MAX];
} hitem;

list_node_t history = LIST_INITIAL_VALUE(history);

static const char nl[2] = {'\r', '\n'};
static const char erase_line[5] = {ESC, '[', '2', 'K', '\r'};
static const char cursor_left[3] = {ESC, '[', 'D'};
static const char cursor_right[3] = {ESC, '[', 'C'};

typedef struct {
    int pos;
    int len;
    int save_len;
    hitem* item;
    char save[LINE_MAX];
    char line[LINE_MAX + 1];
} editstate;

void history_add(editstate* es) {
    hitem* item;
    if (es->len && ((item = malloc(sizeof(hitem))) != NULL)) {
        item->len = es->len;
        memset(item->line, 0, sizeof(item->line));
        memcpy(item->line, es->line, es->len);
        list_add_tail(&history, &item->node);
    }
}

int history_up(editstate* es) {
    hitem* next;
    if (es->item) {
        next = list_prev_type(&history, &es->item->node, hitem, node);
        if (next != NULL) {
            es->item = next;
            memcpy(es->line, es->item->line, es->item->len);
            es->pos = es->len = es->item->len;
            cputs(erase_line, sizeof(erase_line));
            return 1;
        } else {
            beep();
            return 0;
        }
    } else {
        next = list_peek_tail_type(&history, hitem, node);
        if (next != NULL) {
            es->item = next;
            memset(es->save, 0, sizeof(es->save));
            memcpy(es->save, es->line, es->len);
            es->save_len = es->len;
            es->pos = es->len = es->item->len;
            memcpy(es->line, es->item->line, es->len);
            cputs(erase_line, sizeof(erase_line));
            return 1;
        } else {
            return 0;
        }
    }
}

int history_down(editstate* es) {
    if (es->item == NULL) {
        beep();
        return 0;
    }
    hitem* next = list_next_type(&history, &es->item->node, hitem, node);
    if (next != NULL) {
        es->item = next;
        es->pos = es->len = es->item->len;
        memcpy(es->line, es->item->line, es->len);
    } else {
        memcpy(es->line, es->save, es->save_len);
        es->pos = es->len = es->save_len;
        es->item = NULL;
    }
    cputs(erase_line, sizeof(erase_line));
    return 1;
}

void settitle(const char* title) {
    if (!interactive) {
        return;
    }
    char str[16];
    int n = snprintf(str, sizeof(str) - 1, "\033]2;%s", title);
    if (n < 0) {
        return; // error
    } else if ((size_t)n >= sizeof(str) - 1) {
        n = sizeof(str) - 2; // truncated
    }
    str[n] = '\007';
    str[n+1] = '\0';
    cputs(str, n + 1);
}

int readline(editstate* es) {
    int a, b, c;
    es->len = 0;
    es->pos = 0;
    es->save_len = 0;
    es->item = NULL;
again:
    cputc('>');
    cputc(' ');
    if (es->len) {
        cputs(es->line, es->len);
    }
    if (es->len != es->pos) {
        char tmp[16];
        sprintf(tmp, "%c[%dG", ESC, es->pos + 3);
        cputs(tmp, strlen(tmp));
    }
    for (;;) {
        if ((c = cgetc()) < 0) {
            es->item = NULL;
            return c;
        }
        if ((c >= ' ') && (c < 127)) {
            if (es->len < LINE_MAX) {
                if (es->pos != es->len) {
                    memmove(es->line + es->pos + 1, es->line + es->pos, es->len - es->pos);
                    // expensive full redraw of line
                    es->len++;
                    es->line[es->pos++] = c;
                    es->item = NULL;
                    cputs(erase_line, sizeof(erase_line));
                    goto again;
                }
                es->len++;
                es->line[es->pos++] = c;
                cputc(c);
            }
            beep();
            continue;
        }
        switch (c) {
        case CTRL_C:
            es->len = 0;
            es->pos = 0;
            es->item = NULL;
            cputs(nl, sizeof(nl));
            goto again;
        case CTRL_L:
            cputs(erase_line, sizeof(erase_line));
            goto again;
        case BACKSPACE:
        case DELETE:
        backspace:
            if (es->pos > 0) {
                es->pos--;
                es->len--;
                memmove(es->line + es->pos, es->line + es->pos + 1, es->len - es->pos);
                // expensive full redraw of line
                es->item = NULL;
                cputs(erase_line, sizeof(erase_line));
                goto again;
            } else {
                beep();
            }
            es->item = NULL;
            continue;
        case NL:
        case CR:
            es->line[es->len] = 0;
            cputs(nl, sizeof(nl));
            history_add(es);
            return 0;
        case ESC:
            if ((a = cgetc()) < 0) {
                return a;
            }
            if ((b = cgetc()) < 0) {
                return b;
            }
            if (a != '[') {
                break;
            }
            switch (b) {
            case EXT_UP:
                if (history_up(es)) {
                    goto again;
                }
                break;
            case EXT_DOWN:
                if (history_down(es)) {
                    goto again;
                }
                break;
            case EXT_RIGHT:
                if (es->pos < es->len) {
                    es->pos++;
                    cputs(cursor_right, sizeof(cursor_right));
                } else {
                    beep();
                }
                break;
            case EXT_LEFT:
                if (es->pos > 0) {
                    es->pos--;
                    cputs(cursor_left, sizeof(cursor_left));
                } else {
                    beep();
                }
                break;
            }
        }
        beep();
    }
}

static int split(char* line, char* argv[], int max) {
    int n = 0;
    while (max > 0) {
        while (isspace(*line))
            line++;
        if (*line == 0)
            break;
        argv[n++] = line;
        max--;
        line++;
        while (*line && (!isspace(*line)))
            line++;
        if (*line == 0)
            break;
        *line++ = 0;
    }
    return n;
}

void joinproc(mx_handle_t p) {
    mx_status_t r;
    mx_signals_state_t state;

    r = mx_handle_wait_one(p, MX_SIGNAL_SIGNALED, MX_TIME_INFINITE, &state);
    if (r != NO_ERROR) {
        fprintf(stderr, "[process(%x): wait failed? %d]\n", p, r);
        return;
    }

    // read the return code
    mx_process_info_t proc_info;
    mx_ssize_t ret = mx_handle_get_info(p, MX_INFO_PROCESS, &proc_info, sizeof(proc_info));
    if (ret != sizeof(proc_info)) {
        fprintf(stderr, "[process(%x): handle_get_info failed? %ld]\n", p, ret);
    } else {
        fprintf(stderr, "[process(%x): status: %d]\n", p, proc_info.return_code);
    }

    settitle("mxsh");
    mx_handle_close(p);
}

void* joiner(void* arg) {
    joinproc((uintptr_t)arg);
    return NULL;
}

mx_status_t lp_setup(launchpad_t** lp_out,
                     int argc, const char* const* argv,
                     const char* const* envp) {
    launchpad_t* lp;
    mx_status_t status;
    if ((status = launchpad_create(argv[0], &lp)) < 0) {
        return status;
    }
    if ((status = launchpad_arguments(lp, argc, argv)) < 0) {
        goto fail;
    }
    if ((status = launchpad_environ(lp, envp)) < 0) {
        goto fail;
    }
    if ((status = launchpad_add_vdso_vmo(lp)) < 0) {
        goto fail;
    }
    if ((status = launchpad_clone_mxio_root(lp)) < 0) {
        goto fail;
    }
    *lp_out = lp;
    return NO_ERROR;

fail:
    launchpad_destroy(lp);
    return status;
}

mx_status_t command(int argc, char** argv, bool runbg) {
    char tmp[LINE_MAX + 32];
    launchpad_t* lp;
    mx_status_t status = NO_ERROR;
    int i;

    // Leading FOO=BAR become environment strings prepended to the
    // inherited environ, just like in a real Bourne shell.
    const char** envp = (const char**)environ;
    for (i = 0; i < argc; ++i) {
        if (strchr(argv[i], '=') == NULL)
            break;
    }
    if (i > 0) {
        size_t envc = 1;
        for (char** ep = environ; *ep != NULL; ++ep)
            ++envc;
        envp = malloc((i + envc) * sizeof(*envp));
        if (envp == NULL) {
            puts("out of memory for environment strings!");
            return ERR_NO_MEMORY;
        }
        memcpy(mempcpy(envp, argv, i * sizeof(*envp)),
               environ, envc * sizeof(*envp));
        argc -= i;
        argv += i;
    }

    // Simplistic stdout redirection support
    int stdout_fd = -1;
    if ((argc > 0) && (argv[argc - 1][0] == '>')) {
        const char* fn = argv[argc - 1] + 1;
        while (isspace(*fn)) {
            fn++;
        }
        unlink(fn);
        if ((stdout_fd = open(fn, O_WRONLY | O_CREAT)) < 0) {
            fprintf(stderr, "cannot open '%s' for writing\n", fn);
            goto done_no_lp;
        }
        argc--;
    }

    if (argc == 0) {
        goto done_no_lp;
    }

    for (i = 0; builtins[i].name != NULL; i++) {
        if (strcmp(builtins[i].name, argv[0]))
            continue;
        if (stdout_fd >= 0) {
            fprintf(stderr, "redirection not supported for builtin functions\n");
            status = ERR_NOT_SUPPORTED;
            goto done_no_lp;
        }
        settitle(argv[0]);
        builtins[i].func(argc, argv);
        settitle("mxsh");
        goto done_no_lp;
    }

    //TODO: some kind of PATH processing
    if (argv[0][0] != '/') {
        snprintf(tmp, sizeof(tmp), "%s%s", "/boot/bin/", argv[0]);
        argv[0] = tmp;
    }

    if ((status = lp_setup(&lp, argc, (const char* const*) argv, envp)) < 0) {
        fprintf(stderr, "process setup failed (%d)\n", status);
        goto done_no_lp;
    }

    if ((status = launchpad_elf_load(lp, launchpad_vmo_from_file(argv[0]))) < 0) {
        fprintf(stderr, "could not load binary '%s' (%d)\n", argv[0], status);
        goto done;
    }

    if ((status = launchpad_load_vdso(lp, MX_HANDLE_INVALID)) < 0) {
        fprintf(stderr, "could not load vDSO after binary '%s' (%d)\n",
                argv[0], status);
        goto done;
    }

    // unclone-able files will end up as /dev/null in the launched process
    launchpad_clone_fd(lp, 0, 0);
    launchpad_clone_fd(lp, (stdout_fd >= 0) ? stdout_fd : 1, 1);
    launchpad_clone_fd(lp, 2, 2);

    mx_handle_t p;
    if ((p = launchpad_start(lp)) < 0) {
        fprintf(stderr, "process failed to start (%d)\n", p);
        status = p;
        goto done;
    }
    if (runbg) {
        // TODO: migrate to a unified waiter thread once we can wait
        //       on process exit
        pthread_t t;
        if (pthread_create(&t, NULL, joiner, (void*)((uintptr_t)p))) {
            mx_handle_close(p);
        }
    } else {
        char* bname = strrchr(argv[0], '/');
        if (!bname) {
            bname = argv[0];
        } else {
            bname += 1; // point to the first char after the last '/'
        }
        settitle(bname);
        joinproc(p);
    }
done:
    launchpad_destroy(lp);
done_no_lp:
    if (envp != (const char**)environ) {
        free(envp);
    }
    if (stdout_fd >= 0) {
        close(stdout_fd);
    }
    return status;
}

void execline(char* line) {
    bool runbg;
    char* argv[32];
    int argc;
    int len;

    if (line[0] == '`') {
        mx_debug_send_command(line + 1, strlen(line) - 1);
        return;
    }
    len = strlen(line);

    // trim whitespace
    while ((len > 0) && (line[len - 1] <= ' ')) {
        len--;
        line[len] = 0;
    }

    // handle backgrounding
    if ((len > 0) && (line[len - 1] == '&')) {
        line[len - 1] = 0;
        runbg = true;
    } else {
        runbg = false;
    }

    // tokenize and execute
    argc = split(line, argv, 32);
    if (argc) {
        command(argc, argv, runbg);
    }
}

void execscript(const char* fn) {
    char line[1024];
    FILE* fp;
    if ((fp = fopen(fn, "r")) == NULL) {
        printf("cannot open '%s'\n", fn);
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        execline(line);
    }
}

void console(void) {
    editstate es;

    while (readline(&es) == 0) {
        execline(es.line);
    }
}

int main(int argc, char** argv) {
    if ((argc == 3) && (strcmp(argv[1], "-c") == 0)) {
        execline(argv[2]);
        return 0;
    }
    if (argc > 1) {
        execscript(argv[1]);
        return 0;
    }

    interactive = true;
    const char* banner = "\033]2;mxsh\007\nMXCONSOLE...\n";
    cputs(banner, strlen(banner));
    console();
    return 0;
}

/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stddef.h>

#include "sd-journal.h"
#include "util.h"

/* We open a single fd, and we'll share it with the current process,
 * all its threads, and all its subprocesses. This means we need to
 * initialize it atomically, and need to operate on it atomically
 * never assuming we are the only user */

static int journal_fd(void) {
        int fd;
        static int fd_plus_one = 0;

retry:
        if (fd_plus_one > 0)
                return fd_plus_one - 1;

        fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;

        if (!__sync_bool_compare_and_swap(&fd_plus_one, 0, fd+1)) {
                close_nointr_nofail(fd);
                goto retry;
        }

        return fd;
}

int sd_journal_print(const char *format, ...) {
        int r;
        va_list ap;

        va_start(ap, format);
        r = sd_journal_printv(format, ap);
        va_end(ap);

        return r;
}

int sd_journal_printv(const char *format, va_list ap) {
        char buffer[8 + LINE_MAX];
        struct iovec iov;

        memcpy(buffer, "MESSAGE=", 8);
        vsnprintf(buffer+8, sizeof(buffer) - 8, format, ap);

        char_array_0(buffer);

        zero(iov);
        IOVEC_SET_STRING(iov, buffer);

        return sd_journal_sendv(&iov, 1);
}

int sd_journal_send(const char *format, ...) {
        int r, n = 0, i = 0, j;
        va_list ap;
        struct iovec *iov = NULL;

        va_start(ap, format);
        while (format) {
                struct iovec *c;
                char *buffer;

                if (i >= n) {
                        n = MAX(i*2, 4);
                        c = realloc(iov, n * sizeof(struct iovec));
                        if (!c) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        iov = c;
                }

                if (vasprintf(&buffer, format, ap) < 0) {
                        r = -ENOMEM;
                        goto fail;
                }

                IOVEC_SET_STRING(iov[i++], buffer);

                format = va_arg(ap, char *);
        }
        va_end(ap);

        r = sd_journal_sendv(iov, i);

fail:
        for (j = 0; j < i; j++)
                free(iov[j].iov_base);

        free(iov);

        return r;
}

int sd_journal_sendv(const struct iovec *iov, int n) {
        int fd;
        struct iovec *w;
        uint64_t *l;
        int i, j = 0;
        struct msghdr mh;
        struct sockaddr_un sa;

        if (!iov || n <= 0)
                return -EINVAL;

        w = alloca(sizeof(struct iovec) * n * 5);
        l = alloca(sizeof(uint64_t) * n);

        for (i = 0; i < n; i++) {
                char *c, *nl;

                c = memchr(iov[i].iov_base, '=', iov[i].iov_len);
                if (!c)
                        return -EINVAL;

                nl = memchr(iov[i].iov_base, '\n', iov[i].iov_len);
                if (nl) {
                        if (nl < c)
                                return -EINVAL;

                        /* Already includes a newline? Bummer, then
                         * let's write the variable name, then a
                         * newline, then the size (64bit LE), followed
                         * by the data and a final newline */

                        w[j].iov_base = iov[i].iov_base;
                        w[j].iov_len = c - (char*) iov[i].iov_base;
                        j++;

                        IOVEC_SET_STRING(w[j++], "\n");

                        l[i] = htole64(iov[i].iov_len - (c - (char*) iov[i].iov_base) - 1);
                        w[j].iov_base = &l[i];
                        w[j].iov_len = sizeof(uint64_t);
                        j++;

                        w[j].iov_base = c + 1;
                        w[j].iov_len = iov[i].iov_len - (c - (char*) iov[i].iov_base) - 1;
                        j++;

                } else
                        /* Nothing special? Then just add the line and
                         * append a newline */
                        w[j++] = iov[i];

                IOVEC_SET_STRING(w[j++], "\n");
        }

        fd = journal_fd();
        if (fd < 0)
                return fd;

        zero(sa);
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path,"/run/systemd/journal", sizeof(sa.sun_path));

        zero(mh);
        mh.msg_name = &sa;
        mh.msg_namelen = offsetof(struct sockaddr_un, sun_path) + strlen(sa.sun_path);
        mh.msg_iov = w;
        mh.msg_iovlen = j;

        if (sendmsg(fd, &mh, MSG_NOSIGNAL) < 0)
                return -errno;

        return 0;
}

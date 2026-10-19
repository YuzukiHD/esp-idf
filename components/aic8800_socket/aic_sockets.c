/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The BSD socket calls of the AIC8800 driver and supplicant, which are written against their own socket
 * header (same constants and structure layout as lwIP), on the lwIP sockets of the system. This file is a
 * component of its own because it needs the real lwIP headers, which the driver hides with its own.
 */
#include <errno.h>
#include <string.h>
#include "../lwip/lwip/src/include/lwip/sockets.h"   /* not the wrapper of the system, which defines socket() itself */
#include "lwip/inet.h"

int socket(int domain, int type, int protocol)
{
    return lwip_socket(domain, type, protocol);
}

int bind(int fd, const void *addr, uint32_t len)
{
    return lwip_bind(fd, addr, len);
}

int connect(int fd, const void *addr, uint32_t len)
{
    return lwip_connect(fd, addr, len);
}

int listen(int fd, int backlog)
{
    return lwip_listen(fd, backlog);
}

int accept(int fd, void *addr, uint32_t *len)
{
    return lwip_accept(fd, addr, len);
}

int getsockname(int fd, void *addr, uint32_t *len)
{
    return lwip_getsockname(fd, addr, len);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return lwip_send(fd, buf, len, flags);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return lwip_recv(fd, buf, len, flags);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const void *to, uint32_t tolen)
{
    return lwip_sendto(fd, buf, len, flags, to, tolen);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, void *from, uint32_t *fromlen)
{
    return lwip_recvfrom(fd, buf, len, flags, from, fromlen);
}

ssize_t sendmsg(int fd, const void *msg, int flags)
{
    return lwip_sendmsg(fd, msg, flags);
}

#undef inet_aton
#undef inet_ntop
int inet_aton(const char *cp, void *addr)
{
    return ip4addr_aton(cp, addr);
}

const char *inet_ntop(int af, const void *src, char *dst, uint32_t size)
{
    return lwip_inet_ntop(af, src, dst, size);
}

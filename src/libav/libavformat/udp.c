/*
 * UDP prototype streaming system
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * UDP protocol
 */

#define _BSD_SOURCE     /* Needed for using struct ip_mreq with recent glibc */

#include "avformat.h"
#include "avio_internal.h"
#include "libavutil/parseutils.h"
#include "libavutil/avstring.h"
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include "url.h"

#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#endif

typedef struct {
    int udp_fd;
    int ttl;
    int buffer_size;
    int is_multicast;
    int local_port;
    int reuse_socket;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    int is_connected;
} UDPContext;

#define UDP_TX_BUF_SIZE 32768
#define UDP_MAX_PKT_SIZE 65536

static void log_net_error(void *ctx, int level, const char* prefix)
{
    char errbuf[100];
    av_strerror(ff_neterrno(), errbuf, sizeof(errbuf));
    av_log(ctx, level, "%s: %s\n", prefix, errbuf);
}

static int udp_set_multicast_ttl(int sockfd, int mcastTTL,
                                 struct sockaddr *addr)
{
#ifdef IP_MULTICAST_TTL
    if (addr->sa_family == AF_INET) {
        if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &mcastTTL, sizeof(mcastTTL)) < 0) {
            log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IP_MULTICAST_TTL)");
            return -1;
        }
    }
#endif
#if defined(IPPROTO_IPV6) && defined(IPV6_MULTICAST_HOPS)
    if (addr->sa_family == AF_INET6) {
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &mcastTTL, sizeof(mcastTTL)) < 0) {
            log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IPV6_MULTICAST_HOPS)");
            return -1;
        }
    }
#endif
    return 0;
}

static int udp_join_multicast_group(int sockfd, struct sockaddr *addr)
{
#ifdef IP_ADD_MEMBERSHIP
    if (addr->sa_family == AF_INET) {
        struct ip_mreq mreq;

        mreq.imr_multiaddr.s_addr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;
        mreq.imr_interface.s_addr= INADDR_ANY;
        if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)&mreq, sizeof(mreq)) < 0) {
            log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IP_ADD_MEMBERSHIP)");
            return -1;
        }
    }
#endif
#if HAVE_STRUCT_IPV6_MREQ && defined(IPPROTO_IPV6)
    if (addr->sa_family == AF_INET6) {
        struct ipv6_mreq mreq6;

        memcpy(&mreq6.ipv6mr_multiaddr, &(((struct sockaddr_in6 *)addr)->sin6_addr), sizeof(struct in6_addr));
        mreq6.ipv6mr_interface= 0;
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq6, sizeof(mreq6)) < 0) {
            log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IPV6_ADD_MEMBERSHIP)");
            return -1;
        }
    }
#endif
    return 0;
}

static int udp_leave_multicast_group(int sockfd, struct sockaddr *addr)
{
#ifdef IP_DROP_MEMBERSHIP
    if (addr->sa_family == AF_INET) {
        struct ip_mreq mreq;

        mreq.imr_multiaddr.s_addr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;
        mreq.imr_interface.s_addr= INADDR_ANY;
        if (setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const void *)&mreq, sizeof(mreq)) < 0) {
            log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IP_DROP_MEMBERSHIP)");
            return -1;
        }
    }
#endif
#if HAVE_STRUCT_IPV6_MREQ && defined(IPPROTO_IPV6)
    if (addr->sa_family == AF_INET6) {
        struct ipv6_mreq mreq6;

        memcpy(&mreq6.ipv6mr_multiaddr, &(((struct sockaddr_in6 *)addr)->sin6_addr), sizeof(struct in6_addr));
        mreq6.ipv6mr_interface= 0;
        if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, &mreq6, sizeof(mreq6)) < 0) {
            log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IPV6_DROP_MEMBERSHIP)");
            return -1;
        }
    }
#endif
    return 0;
}

static struct addrinfo* udp_resolve_host(const char *hostname, int port,
                                         int type, int family, int flags)
{
    struct addrinfo hints = { 0 }, *res = 0;
    int error;
    char sport[16];
    const char *node = 0, *service = "0";

    if (port > 0) {
        snprintf(sport, sizeof(sport), "%d", port);
        service = sport;
    }
    if ((hostname) && (hostname[0] != '\0') && (hostname[0] != '?')) {
        node = hostname;
    }
    hints.ai_socktype = type;
    hints.ai_family   = family;
    hints.ai_flags = flags;
    if ((error = getaddrinfo(node, service, &hints, &res))) {
        res = NULL;
        av_log(NULL, AV_LOG_ERROR, "udp_resolve_host: %s\n", gai_strerror(error));
    }

    return res;
}

static int udp_set_multicast_sources(int sockfd, struct sockaddr *addr,
                                     int addr_len, char **sources,
                                     int nb_sources, int include)
{
#if HAVE_STRUCT_GROUP_SOURCE_REQ && defined(MCAST_BLOCK_SOURCE) && !defined(_WIN32)
    /* These ones are available in the microsoft SDK, but don't seem to work
     * as on linux, so just prefer the v4-only approach there for now. */
    int i;
    for (i = 0; i < nb_sources; i++) {
        struct group_source_req mreqs;
        int level = addr->sa_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
        struct addrinfo *sourceaddr = udp_resolve_host(sources[i], 0,
                                                       SOCK_DGRAM, AF_UNSPEC,
                                                       AI_NUMERICHOST);
        if (!sourceaddr)
            return AVERROR(ENOENT);

        mreqs.gsr_interface = 0;
        memcpy(&mreqs.gsr_group, addr, addr_len);
        memcpy(&mreqs.gsr_source, sourceaddr->ai_addr, sourceaddr->ai_addrlen);
        freeaddrinfo(sourceaddr);

        if (setsockopt(sockfd, level,
                       include ? MCAST_JOIN_SOURCE_GROUP : MCAST_BLOCK_SOURCE,
                       (const void *)&mreqs, sizeof(mreqs)) < 0) {
            if (include)
                log_net_error(NULL, AV_LOG_ERROR, "setsockopt(MCAST_JOIN_SOURCE_GROUP)");
            else
                log_net_error(NULL, AV_LOG_ERROR, "setsockopt(MCAST_BLOCK_SOURCE)");
            return ff_neterrno();
        }
    }
#elif HAVE_STRUCT_IP_MREQ_SOURCE && defined(IP_BLOCK_SOURCE)
    int i;
    if (addr->sa_family != AF_INET) {
        av_log(NULL, AV_LOG_ERROR,
               "Setting multicast sources only supported for IPv4\n");
        return AVERROR(EINVAL);
    }
    for (i = 0; i < nb_sources; i++) {
        struct ip_mreq_source mreqs;
        struct addrinfo *sourceaddr = udp_resolve_host(sources[i], 0,
                                                       SOCK_DGRAM, AF_UNSPEC,
                                                       AI_NUMERICHOST);
        if (!sourceaddr)
            return AVERROR(ENOENT);
        if (sourceaddr->ai_addr->sa_family != AF_INET) {
            freeaddrinfo(sourceaddr);
            av_log(NULL, AV_LOG_ERROR, "%s is of incorrect protocol family\n",
                   sources[i]);
            return AVERROR(EINVAL);
        }

        mreqs.imr_multiaddr.s_addr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;
        mreqs.imr_interface.s_addr = INADDR_ANY;
        mreqs.imr_sourceaddr.s_addr = ((struct sockaddr_in *)sourceaddr->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(sourceaddr);

        if (setsockopt(sockfd, IPPROTO_IP,
                       include ? IP_ADD_SOURCE_MEMBERSHIP : IP_BLOCK_SOURCE,
                       (const void *)&mreqs, sizeof(mreqs)) < 0) {
            if (include)
                log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IP_ADD_SOURCE_MEMBERSHIP)");
            else
                log_net_error(NULL, AV_LOG_ERROR, "setsockopt(IP_BLOCK_SOURCE)");
            return ff_neterrno();
        }
    }
#else
    return AVERROR(ENOSYS);
#endif
    return 0;
}
static int udp_set_url(struct sockaddr_storage *addr,
                       const char *hostname, int port)
{
    struct addrinfo *res0;
    int addr_len;

    res0 = udp_resolve_host(hostname, port, SOCK_DGRAM, AF_UNSPEC, 0);
    if (res0 == 0) return AVERROR(EIO);
    memcpy(addr, res0->ai_addr, res0->ai_addrlen);
    addr_len = res0->ai_addrlen;
    freeaddrinfo(res0);

    return addr_len;
}

static int udp_socket_create(UDPContext *s, struct sockaddr_storage *addr,
                             int *addr_len, const char *localaddr)
{
    int udp_fd = -1;
    struct addrinfo *res0 = NULL, *res = NULL;
    int family = AF_UNSPEC;

    if (((struct sockaddr *) &s->dest_addr)->sa_family)
        family = ((struct sockaddr *) &s->dest_addr)->sa_family;
    res0 = udp_resolve_host(localaddr[0] ? localaddr : NULL, s->local_port,
                            SOCK_DGRAM, family, AI_PASSIVE);
    if (res0 == 0)
        goto fail;
    for (res = res0; res; res=res->ai_next) {
        udp_fd = socket(res->ai_family, SOCK_DGRAM, 0);
        if (udp_fd != -1) break;
        log_net_error(NULL, AV_LOG_ERROR, "socket");
    }

    if (udp_fd < 0)
        goto fail;

    memcpy(addr, res->ai_addr, res->ai_addrlen);
    *addr_len = res->ai_addrlen;

    freeaddrinfo(res0);

    return udp_fd;

 fail:
    if (udp_fd >= 0)
        closesocket(udp_fd);
    if(res0)
        freeaddrinfo(res0);
    return -1;
}

static int udp_port(struct sockaddr_storage *addr, int addr_len)
{
    char sbuf[sizeof(int)*3+1];
    int error;

    if ((error = getnameinfo((struct sockaddr *)addr, addr_len, NULL, 0,  sbuf, sizeof(sbuf), NI_NUMERICSERV)) != 0) {
        av_log(NULL, AV_LOG_ERROR, "getnameinfo: %s\n", gai_strerror(error));
        return -1;
    }

    return strtol(sbuf, NULL, 10);
}


/**
 * If no filename is given to av_open_input_file because you want to
 * get the local port first, then you must call this function to set
 * the remote server address.
 *
 * url syntax: udp://host:port[?option=val...]
 * option: 'ttl=n'       : set the ttl value (for multicast only)
 *         'localport=n' : set the local port
 *         'pkt_size=n'  : set max packet size
 *         'reuse=1'     : enable reusing the socket
 *
 * @param h media file context
 * @param uri of the remote server
 * @return zero if no error.
 */
int ff_udp_set_remote_url(URLContext *h, const char *uri)
{
    UDPContext *s = h->priv_data;
    char hostname[256], buf[10];
    int port;
    const char *p;

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &port, NULL, 0, uri);

    /* set the destination address */
    s->dest_addr_len = udp_set_url(&s->dest_addr, hostname, port);
    if (s->dest_addr_len < 0) {
        return AVERROR(EIO);
    }
    s->is_multicast = ff_is_multicast_address((struct sockaddr*) &s->dest_addr);
    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "connect", p)) {
            int was_connected = s->is_connected;
            s->is_connected = strtol(buf, NULL, 10);
            if (s->is_connected && !was_connected) {
                if (connect(s->udp_fd, (struct sockaddr *) &s->dest_addr,
                            s->dest_addr_len)) {
                    s->is_connected = 0;
                    log_net_error(h, AV_LOG_ERROR, "connect");
                    return AVERROR(EIO);
                }
            }
        }
    }

    return 0;
}

/**
 * Return the local port used by the UDP connection
 * @param h media file context
 * @return the local port number
 */
int ff_udp_get_local_port(URLContext *h)
{
    UDPContext *s = h->priv_data;
    return s->local_port;
}

/**
 * Return the udp file handle for select() usage to wait for several RTP
 * streams at the same time.
 * @param h media file context
 */
static int udp_get_file_handle(URLContext *h)
{
    UDPContext *s = h->priv_data;
    return s->udp_fd;
}

/* put it in UDP context */
/* return non zero if error */
static int udp_open(URLContext *h, const char *uri, int flags)
{
    char hostname[1024], localaddr[1024] = "";
    int port, udp_fd = -1, tmp, bind_ret = -1;
    UDPContext *s = h->priv_data;
    int is_output;
    const char *p;
    char buf[256];
    struct sockaddr_storage my_addr;
    int len;
    int reuse_specified = 0;
    int i, include = 0, num_sources = 0;
    char *sources[32];

    h->is_streamed = 1;
    h->max_packet_size = 1472;

    is_output = !(flags & AVIO_FLAG_READ);

    s->ttl = 16;
    s->buffer_size = is_output ? UDP_TX_BUF_SIZE : UDP_MAX_PKT_SIZE;

    p = strchr(uri, '?');
    if (p) {
        if (av_find_info_tag(buf, sizeof(buf), "reuse", p)) {
            char *endptr = NULL;
            s->reuse_socket = strtol(buf, &endptr, 10);
            /* assume if no digits were found it is a request to enable it */
            if (buf == endptr)
                s->reuse_socket = 1;
            reuse_specified = 1;
        }
        if (av_find_info_tag(buf, sizeof(buf), "ttl", p)) {
            s->ttl = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localport", p)) {
            s->local_port = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "pkt_size", p)) {
            h->max_packet_size = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "buffer_size", p)) {
            s->buffer_size = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "connect", p)) {
            s->is_connected = strtol(buf, NULL, 10);
        }
        if (av_find_info_tag(buf, sizeof(buf), "localaddr", p)) {
            av_strlcpy(localaddr, buf, sizeof(localaddr));
        }
        if (av_find_info_tag(buf, sizeof(buf), "sources", p))
            include = 1;
        if (include || av_find_info_tag(buf, sizeof(buf), "block", p)) {
            char *source_start;

            source_start = buf;
            while (1) {
                char *next = strchr(source_start, ',');
                if (next)
                    *next = '\0';
                sources[num_sources] = av_strdup(source_start);
                if (!sources[num_sources])
                    goto fail;
                source_start = next + 1;
                num_sources++;
                if (num_sources >= FF_ARRAY_ELEMS(sources) || !next)
                    break;
            }
        }
    }

    /* fill the dest addr */
    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &port, NULL, 0, uri);

    /* XXX: fix av_url_split */
    if (hostname[0] == '\0' || hostname[0] == '?') {
        /* only accepts null hostname if input */
        if (!(flags & AVIO_FLAG_READ))
            goto fail;
    } else {
        if (ff_udp_set_remote_url(h, uri) < 0)
            goto fail;
    }

    if ((s->is_multicast || !s->local_port) && (h->flags & AVIO_FLAG_READ))
        s->local_port = port;
    udp_fd = udp_socket_create(s, &my_addr, &len, localaddr);
    if (udp_fd < 0)
        goto fail;

    /* Follow the requested reuse option, unless it's multicast in which
     * case enable reuse unless explicitly disabled.
     */
    if (s->reuse_socket || (s->is_multicast && !reuse_specified)) {
        s->reuse_socket = 1;
        if (setsockopt (udp_fd, SOL_SOCKET, SO_REUSEADDR, &(s->reuse_socket), sizeof(s->reuse_socket)) != 0)
            goto fail;
    }

    /* If multicast, try binding the multicast address first, to avoid
     * receiving UDP packets from other sources aimed at the same UDP
     * port. This fails on windows. This makes sending to the same address
     * using sendto() fail, so only do it if we're opened in read-only mode. */
    if (s->is_multicast && !(h->flags & AVIO_FLAG_WRITE)) {
        bind_ret = bind(udp_fd,(struct sockaddr *)&s->dest_addr, len);
    }
    /* bind to the local address if not multicast or if the multicast
     * bind failed */
    /* the bind is needed to give a port to the socket now */
    if (bind_ret < 0 && bind(udp_fd,(struct sockaddr *)&my_addr, len) < 0) {
        log_net_error(h, AV_LOG_ERROR, "bind failed");
        goto fail;
    }

    len = sizeof(my_addr);
    getsockname(udp_fd, (struct sockaddr *)&my_addr, &len);
    s->local_port = udp_port(&my_addr, len);

    if (s->is_multicast) {
        if (h->flags & AVIO_FLAG_WRITE) {
            /* output */
            if (udp_set_multicast_ttl(udp_fd, s->ttl, (struct sockaddr *)&s->dest_addr) < 0)
                goto fail;
        }
        if (h->flags & AVIO_FLAG_READ) {
            /* input */
            if (num_sources == 0 || !include) {
                if (udp_join_multicast_group(udp_fd, (struct sockaddr *)&s->dest_addr) < 0)
                    goto fail;

                if (num_sources) {
                    if (udp_set_multicast_sources(udp_fd, (struct sockaddr *)&s->dest_addr, s->dest_addr_len, sources, num_sources, 0) < 0)
                        goto fail;
                }
            } else if (include && num_sources) {
                if (udp_set_multicast_sources(udp_fd, (struct sockaddr *)&s->dest_addr, s->dest_addr_len, sources, num_sources, 1) < 0)
                    goto fail;
            } else {
                av_log(NULL, AV_LOG_ERROR, "invalid udp settings: inclusive multicast but no sources given\n");
                goto fail;
            }
        }
    }

    if (is_output) {
        /* limit the tx buf size to limit latency */
        tmp = s->buffer_size;
        if (setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp)) < 0) {
            log_net_error(h, AV_LOG_ERROR, "setsockopt(SO_SNDBUF)");
            goto fail;
        }
    } else {
        /* set udp recv buffer size to the largest possible udp packet size to
         * avoid losing data on OSes that set this too low by default. */
        tmp = s->buffer_size;
        if (setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &tmp, sizeof(tmp)) < 0) {
            log_net_error(h, AV_LOG_WARNING, "setsockopt(SO_RECVBUF)");
        }
        /* make the socket non-blocking */
        ff_socket_nonblock(udp_fd, 1);
    }
    if (s->is_connected) {
        if (connect(udp_fd, (struct sockaddr *) &s->dest_addr, s->dest_addr_len)) {
            log_net_error(h, AV_LOG_ERROR, "connect");
            goto fail;
        }
    }

    for (i = 0; i < num_sources; i++)
        av_free(sources[i]);

    s->udp_fd = udp_fd;
    return 0;
 fail:
    if (udp_fd >= 0)
        closesocket(udp_fd);
    for (i = 0; i < num_sources; i++)
        av_free(sources[i]);
    return AVERROR(EIO);
}

static int udp_read(URLContext *h, uint8_t *buf, int size)
{
    UDPContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->udp_fd, 0);
        if (ret < 0)
            return ret;
    }
    ret = recv(s->udp_fd, buf, size, 0);
    return ret < 0 ? ff_neterrno() : ret;
}

static int udp_write(URLContext *h, const uint8_t *buf, int size)
{
    UDPContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->udp_fd, 1);
        if (ret < 0)
            return ret;
    }

    if (!s->is_connected) {
        ret = sendto (s->udp_fd, buf, size, 0,
                      (struct sockaddr *) &s->dest_addr,
                      s->dest_addr_len);
    } else
        ret = send(s->udp_fd, buf, size, 0);

    return ret < 0 ? ff_neterrno() : ret;
}

static int udp_close(URLContext *h)
{
    UDPContext *s = h->priv_data;

    if (s->is_multicast && (h->flags & AVIO_FLAG_READ))
        udp_leave_multicast_group(s->udp_fd, (struct sockaddr *)&s->dest_addr);
    closesocket(s->udp_fd);
    return 0;
}

URLProtocol ff_udp_protocol = {
    .name                = "udp",
    .url_open            = udp_open,
    .url_read            = udp_read,
    .url_write           = udp_write,
    .url_close           = udp_close,
    .url_get_file_handle = udp_get_file_handle,
    .priv_data_size      = sizeof(UDPContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
};
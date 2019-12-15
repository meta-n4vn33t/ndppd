// This file is part of ndppd.
//
// Copyright (C) 2011-2019  Daniel Adolfsson <daniel@ashen.se>
//
// ndppd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// ndppd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with ndppd.  If not, see <https://www.gnu.org/licenses/>.
#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#ifdef __linux__
#    include <linux/rtnetlink.h>
#    define RTPROT_NDPPD 72
#else
#    include <net/if.h>
#    include <net/if_dl.h>
#    include <net/route.h>
#    include <stdlib.h>
#    include <sys/sysctl.h>
#    include <unistd.h>
#endif

#include "addr.h"
#include "io.h"
#include "ndppd.h"
#include "rt.h"

#ifdef __clang__
#    pragma clang diagnostic ignored "-Waddress-of-packed-member"
#endif

// Our AF_ROUTE or AF_NETLINK socket.
static nd_io_t *ndL_io;

// All IPV6 routes on the system.
static nd_rt_route_t *ndL_routes;

// Freelist.
static nd_rt_route_t *ndL_free_routes;

// All IPv6 addresses on the system.
static nd_rt_addr_t *ndL_addrs;

// Freelist.
static nd_rt_addr_t *ndL_free_addrs;

long nd_rt_dump_timeout;

static void ndL_new_route(nd_rt_route_t *route)
{
    nd_rt_route_t *new_route;

    ND_LL_FOREACH_NODEF(ndL_routes, new_route, next)
    {
        if ((nd_addr_eq(&new_route->dst, &route->dst) && new_route->pflen == route->pflen &&
             new_route->table == route->table))
            return;
    }

    if ((new_route = ndL_free_routes))
        ND_LL_DELETE(ndL_free_routes, new_route, next);
    else
        new_route = ND_ALLOC(nd_rt_route_t);

    *new_route = *route;

    // This will ensure the linked list is kept sorted, so it will be easier to find a match.

    nd_rt_route_t *prev = NULL;

    ND_LL_FOREACH(ndL_routes, cur, next)
    {
        if (new_route->pflen >= cur->pflen)
            break;

        prev = cur;
    }

    if (prev)
    {
        new_route->next = prev->next;
        prev->next = new_route;
    }
    else
    {
        ND_LL_PREPEND(ndL_routes, new_route, next);
    }

    nd_log_debug("rt: (event) new route %s/%d dev %d table %d %s", //
                 nd_aton(&route->dst), route->pflen, route->oif, route->table, route->owned ? "owned" : "");
}

static void ndL_delete_route(nd_rt_route_t *route)
{
    nd_rt_route_t *prev = NULL, *cur;

    ND_LL_FOREACH_NODEF(ndL_routes, cur, next)
    {
        if ((nd_addr_eq(&cur->dst, &route->dst) && cur->oif == route->oif && cur->pflen == route->pflen &&
             cur->table == route->table))
            break;

        prev = cur;
    }

    if (!cur)
        return;

    if (prev)
        prev->next = cur->next;
    else
        ndL_routes = cur->next;

    nd_log_debug("rt: (event) delete route %s/%d dev %d table %d", //
                 nd_aton(&cur->dst), cur->pflen, cur->oif, cur->table);

    ND_LL_PREPEND(ndL_free_routes, cur, next);
}

static void ndL_new_addr(unsigned index, nd_addr_t *addr, unsigned pflen)
{
    nd_rt_addr_t *rt_addr;

    ND_LL_FOREACH_NODEF(ndL_addrs, rt_addr, next)
    {
        if (rt_addr->iif == index && nd_addr_eq(&rt_addr->addr, addr) && rt_addr->pflen == pflen)
            return;
    }

    if ((rt_addr = ndL_free_addrs))
        ND_LL_DELETE(ndL_free_addrs, rt_addr, next);
    else
        rt_addr = ND_ALLOC(nd_rt_addr_t);

    ND_LL_PREPEND(ndL_addrs, rt_addr, next);

    rt_addr->pflen = pflen;
    rt_addr->iif = index;
    rt_addr->addr = *addr;

    nd_log_debug("rt: (event) new address %s/%d if %d", nd_aton(addr), pflen, index);
}

static void ndL_delete_addr(unsigned int index, nd_addr_t *addr, unsigned pflen)
{
    nd_rt_addr_t *prev = NULL, *rt_addr;

    ND_LL_FOREACH_NODEF(ndL_addrs, rt_addr, next)
    {
        if (rt_addr->iif == index && nd_addr_eq(&rt_addr->addr, addr) && rt_addr->pflen == pflen)
        {
            nd_log_debug("rt: (event) delete address %s/%d if %d", nd_aton(addr), pflen, index);

            if (prev)
                prev->next = rt_addr->next;
            else
                ndL_addrs = rt_addr->next;

            ND_LL_PREPEND(ndL_free_addrs, rt_addr, next);
            return;
        }

        prev = rt_addr;
    }
}

#ifdef __linux__
static void ndL_handle_newaddr(struct ifaddrmsg *msg, int length)
{
    nd_addr_t *addr = NULL;

    for (struct rtattr *rta = IFA_RTA(msg); RTA_OK(rta, length); rta = RTA_NEXT(rta, length))
    {
        if (rta->rta_type == IFA_ADDRESS)
            addr = (nd_addr_t *)RTA_DATA(rta);
    }

    if (!addr)
        return;

    ndL_new_addr(msg->ifa_index, addr, msg->ifa_prefixlen);
}

static void ndL_handle_deladdr(struct ifaddrmsg *msg, int length)
{
    nd_addr_t *addr = NULL;

    for (struct rtattr *rta = IFA_RTA(msg); RTA_OK(rta, length); rta = RTA_NEXT(rta, length))
    {
        if (rta->rta_type == IFA_ADDRESS)
            addr = (nd_addr_t *)RTA_DATA(rta);
    }

    if (!addr)
        return;

    ndL_delete_addr(msg->ifa_index, addr, msg->ifa_prefixlen);
}

static void ndL_handle_newroute(struct rtmsg *msg, int rtl)
{
    nd_addr_t *dst = NULL;
    int oif = 0;

    for (struct rtattr *rta = RTM_RTA(msg); RTA_OK(rta, rtl); rta = RTA_NEXT(rta, rtl))
    {
        if (rta->rta_type == RTA_OIF)
            oif = *(int *)RTA_DATA(rta);
        else if (rta->rta_type == RTA_DST)
            dst = (nd_addr_t *)RTA_DATA(rta);
    }

    if (!dst || !oif)
        return;

    nd_rt_route_t route = {
        .table = msg->rtm_table,
        .pflen = msg->rtm_dst_len,
        .oif = oif,
        .dst = *dst,
        .owned = msg->rtm_protocol == RTPROT_NDPPD,
    };

    ndL_new_route(&route);
}

static void ndL_handle_delroute(struct rtmsg *msg, int rtl)
{
    nd_addr_t *dst = NULL;
    int oif = 0;

    for (struct rtattr *rta = RTM_RTA(msg); RTA_OK(rta, rtl); rta = RTA_NEXT(rta, rtl))
    {
        if (rta->rta_type == RTA_OIF)
            oif = *(int *)RTA_DATA(rta);
        else if (rta->rta_type == RTA_DST)
            dst = (nd_addr_t *)RTA_DATA(rta);
    }

    if (!dst || !oif)
        return;

    nd_rt_route_t route = {
        .table = msg->rtm_table,
        .pflen = msg->rtm_dst_len,
        .oif = oif,
        .dst = *dst,
    };

    ndL_delete_route(&route);
}

static void ndL_io_handler(__attribute__((unused)) nd_io_t *unused1, __attribute__((unused)) int unused2)
{
    uint8_t buf[4096];

    for (;;)
    {
        ssize_t len = nd_io_recv(ndL_io, NULL, 0, buf, sizeof(buf));

        if (len < 0)
            return;

        for (struct nlmsghdr *hdr = (struct nlmsghdr *)buf; NLMSG_OK(hdr, len); hdr = NLMSG_NEXT(hdr, len))
        {
            if (hdr->nlmsg_type == NLMSG_DONE)
            {
                nd_rt_dump_timeout = 0;
                break;
            }

            if (hdr->nlmsg_type == NLMSG_ERROR)
            {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(hdr);
                nd_log_error("rt: Netlink: %s (%d)", strerror(-e->error), e->msg.nlmsg_type);
                continue;
            }

            if (hdr->nlmsg_type == RTM_NEWROUTE)
                ndL_handle_newroute((struct rtmsg *)NLMSG_DATA(hdr), RTM_PAYLOAD(hdr));
            else if (hdr->nlmsg_type == RTM_DELROUTE)
                ndL_handle_delroute((struct rtmsg *)NLMSG_DATA(hdr), RTM_PAYLOAD(hdr));
            else if (hdr->nlmsg_type == RTM_NEWADDR)
                ndL_handle_newaddr((struct ifaddrmsg *)NLMSG_DATA(hdr), IFA_PAYLOAD(hdr));
            else if (hdr->nlmsg_type == RTM_DELADDR)
                ndL_handle_deladdr((struct ifaddrmsg *)NLMSG_DATA(hdr), IFA_PAYLOAD(hdr));
        }
    }
}
#else
static void ndL_get_rtas(int addrs, struct sockaddr *sa, struct sockaddr **rtas)
{
    for (int i = 0; i < RTAX_MAX; i++)
    {
        if (addrs & (1 << i))
        {
            rtas[i] = sa;
            sa = (void *)sa + ((sa->sa_len + sizeof(u_long) - 1) & ~(sizeof(u_long) - 1));
        }
        else
        {
            rtas[i] = NULL;
        }
    }
}

static void ndL_handle_rt(struct rt_msghdr *hdr)
{
    struct sockaddr *rtas[RTAX_MAX];
    ndL_get_rtas(hdr->rtm_addrs, (struct sockaddr *)(hdr + 1), rtas);

    if (!rtas[RTAX_DST] || rtas[RTAX_DST]->sa_family != AF_INET6)
        return;

    int pflen = rtas[RTAX_NETMASK] ? nd_mask_to_pflen(&((struct sockaddr_in6 *)rtas[RTAX_NETMASK])->sin6_addr) : 128;

    // FIXME: Should we use RTAX_GATEWAY to get the interface index?

    nd_rt_route_t route = {
        .dst = ((struct sockaddr_in6 *)rtas[RTAX_DST])->sin6_addr,
        .oif = hdr->rtm_index,
        .pflen = pflen,
#    ifdef __FreeBSD__
        .table = 0,
#    else
        .table = hdr->rtm_tableid,
#    endif
        .owned = (hdr->rtm_flags & RTF_PROTO3) != 0,
    };

    if (hdr->rtm_type == RTM_GET || hdr->rtm_type == RTM_ADD)
        ndL_new_route(&route);
    else if (hdr->rtm_type == RTM_DELETE)
        ndL_delete_route(&route);
}

static void ndL_handle_ifa(struct ifa_msghdr *hdr)
{
    struct sockaddr *rtas[RTAX_MAX];
    ndL_get_rtas(hdr->ifam_addrs, (struct sockaddr *)(hdr + 1), rtas);

    if (!rtas[RTAX_IFA] || rtas[RTAX_IFA]->sa_family != AF_INET6)
        return;

    int pflen = rtas[RTAX_NETMASK] ? nd_mask_to_pflen(&((struct sockaddr_in6 *)rtas[RTAX_NETMASK])->sin6_addr) : 128;

    nd_addr_t *ifa = &((struct sockaddr_in6 *)rtas[RTAX_IFA])->sin6_addr;

    if (hdr->ifam_type == RTM_NEWADDR)
        ndL_new_addr(hdr->ifam_index, ifa, pflen);
    else if (hdr->ifam_type == RTM_DELADDR)
        ndL_delete_addr(hdr->ifam_index, ifa, pflen);
}

typedef struct
{
    u_short msglen;
    u_char version;
    u_char type;
} ndL_msghdr_t;

static void ndL_handle(void *buf, size_t buflen)
{
    for (size_t i = 0; i < buflen;)
    {
        ndL_msghdr_t *hdr = (ndL_msghdr_t *)(buf + i);
        i += hdr->msglen;

        if (i > buflen)
            break;

        switch (hdr->type)
        {
        case RTM_ADD:
        case RTM_GET:
        case RTM_DELETE:
            ndL_handle_rt((struct rt_msghdr *)hdr);
            break;

        case RTM_NEWADDR:
        case RTM_DELADDR:
            ndL_handle_ifa((struct ifa_msghdr *)hdr);
            break;
        }
    }
}

static bool ndL_dump(int type)
{
    int mib[] = { CTL_NET, PF_ROUTE, 0, 0, type, 0 };

    size_t size;
    if (sysctl(mib, 6, NULL, &size, NULL, 0) < 0)
    {
        nd_log_error("sysctl(): %s", strerror(errno));
        return false;
    }

    void *buf = malloc(size);

    // FIXME: Potential race condition as the number of routes might have increased since the previous syscall().
    if (sysctl(mib, 6, buf, &size, NULL, 0) < 0)
    {
        free(buf);
        nd_log_error("sysctl(): %s", strerror(errno));
        return false;
    }

    ndL_handle(buf, size);

    free(buf);
    return true;
}

static void ndL_io_handler(__attribute__((unused)) nd_io_t *unused1, __attribute__((unused)) int unused2)
{
    uint8_t buf[4096];

    for (;;)
    {
        ssize_t len = nd_io_recv(ndL_io, NULL, 0, buf, sizeof(buf));

        if (len < 0)
            return;

        ndL_handle(buf, len);
    }
}

#endif

bool nd_rt_open()
{
    if (ndL_io != NULL)
        return true;

#ifdef __linux__
    if (!(ndL_io = nd_io_socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE)))
    {
        nd_log_error("Failed to open netlink socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = (1 << (RTNLGRP_IPV6_IFADDR - 1)) | (1 << (RTNLGRP_IPV6_ROUTE - 1));

    if (!nd_io_bind(ndL_io, (struct sockaddr *)&addr, sizeof(addr)))
    {
        nd_log_error("Failed to bind netlink socket: %s", strerror(errno));
        nd_io_close(ndL_io);
        ndL_io = NULL;
        return false;
    }
#else
    if (!(ndL_io = nd_io_socket(AF_ROUTE, SOCK_RAW, AF_INET6)))
    {
        nd_log_error("Failed to open routing socket: %s", strerror(errno));
        return false;
    }
#endif

    ndL_io->handler = ndL_io_handler;

    return true;
}

void nd_rt_cleanup()
{
    if (ndL_io)
        nd_io_close(ndL_io);
}

bool nd_rt_query_routes()
{
#ifdef __linux__
    if (nd_rt_dump_timeout)
        return false;

    struct
    {
        struct nlmsghdr hdr;
        struct rtmsg msg;
    } req;

    memset(&req, 0, sizeof(req));

    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.hdr.nlmsg_type = RTM_GETROUTE;

    req.msg.rtm_protocol = RTPROT_UNSPEC;
    req.msg.rtm_table = RT_TABLE_UNSPEC;
    req.msg.rtm_family = AF_INET6;

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    nd_rt_dump_timeout = nd_current_time + 5000;

    nd_io_send(ndL_io, (struct sockaddr *)&addr, sizeof(addr), &req, sizeof(req));
    return true;
#else
    return ndL_dump(NET_RT_DUMP);
#endif
}

bool nd_rt_query_addresses()
{
#ifdef __linux__
    if (nd_rt_dump_timeout)
        return false;

    struct
    {
        struct nlmsghdr hdr;
        struct ifaddrmsg msg;
    } req;

    memset(&req, 0, sizeof(req));

    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.hdr.nlmsg_type = RTM_GETADDR;
    req.hdr.nlmsg_seq = 1;

    req.msg.ifa_family = AF_INET6;

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    nd_rt_dump_timeout = nd_current_time + 5000;

    nd_io_send(ndL_io, (struct sockaddr *)&addr, sizeof(addr), &req, sizeof(req));
    return true;
#else
    return ndL_dump(NET_RT_IFLIST);
#endif
}

nd_rt_route_t *nd_rt_find_route(nd_addr_t *addr, unsigned table)
{
    ND_LL_FOREACH(ndL_routes, route, next)
    {
        if (nd_addr_match(&route->dst, addr, route->pflen) && route->table == table)
            return route;
    }

    return NULL;
}

bool nd_rt_add_route(nd_addr_t *dst, unsigned pflen, unsigned oif, unsigned table)
{
#ifdef __linux__
    struct __attribute__((packed))
    {
        struct nlmsghdr hdr;
        struct rtmsg msg;
        struct rtattr oif_attr __attribute__((aligned(NLMSG_ALIGNTO)));
        uint32_t oif;
        struct rtattr dst_attr __attribute__((aligned(RTA_ALIGNTO)));
        nd_addr_t dst;
        //struct rtattr exp_attr __attribute__((aligned(RTA_ALIGNTO)));
        //uint32_t exp;
    } req;

    memset(&req, 0, sizeof(req));

    req.msg.rtm_protocol = RTPROT_NDPPD;
    req.msg.rtm_family = AF_INET6;
    req.msg.rtm_dst_len = pflen;
    req.msg.rtm_table = table;
    req.msg.rtm_scope = RT_SCOPE_UNIVERSE;

    req.oif_attr.rta_type = RTA_OIF;
    req.oif_attr.rta_len = RTA_LENGTH(sizeof(req.oif));
    req.oif = oif;

    req.dst_attr.rta_type = RTA_DST;
    req.dst_attr.rta_len = RTA_LENGTH(sizeof(req.dst));
    req.dst = *dst;

    //req.exp_attr.rta_type = RTA_EXPIRES;
    //req.exp_attr.rta_len = RTA_LENGTH(sizeof(req.exp));
    //req.exp = 60;

    req.hdr.nlmsg_type = RTM_NEWROUTE;
    req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE;
    req.hdr.nlmsg_len = sizeof(req);

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    return nd_io_send(ndL_io, (struct sockaddr *)&addr, sizeof(addr), &req, sizeof(req)) >= 0;
#else
    struct
    {
        struct rt_msghdr hdr;
        struct sockaddr_in6 dst;
        struct sockaddr_dl dl __aligned(sizeof(u_long));
        struct sockaddr_in6 mask __aligned(sizeof(u_long));
    } msg;

    memset(&msg, 0, sizeof(msg));

    msg.hdr.rtm_type = RTM_ADD;
    msg.hdr.rtm_version = RTM_VERSION;
    msg.hdr.rtm_pid = getpid();
    msg.hdr.rtm_flags = RTF_UP | RTF_PROTO3;
    msg.hdr.rtm_msglen = sizeof(msg);
    msg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
    msg.hdr.rtm_index = oif;

#    ifdef __FreeBSD__
    (void)table;
#    else
    msg.hdr->rtm_tableid = table;
#    endif

    msg.dst.sin6_family = AF_INET6;
    msg.dst.sin6_len = sizeof(msg.dst);
    msg.dst.sin6_addr = *dst;

    msg.dl.sdl_family = AF_LINK;
    msg.dl.sdl_index = oif;
    msg.dl.sdl_len = sizeof(msg.dl);

    msg.mask.sin6_family = AF_INET6;
    msg.mask.sin6_len = sizeof(msg.mask);
    nd_mask_from_pflen(pflen, &msg.mask.sin6_addr);

    nd_log_info("rt: Adding route %s/%d table %d", nd_aton(dst), pflen, table);

    return nd_io_write(ndL_io, &msg, sizeof(msg)) >= 0;
#endif
}

bool nd_rt_remove_route(nd_addr_t *dst, unsigned pflen, unsigned table)
{
#ifdef __linux__
    struct __attribute__((packed))
    {
        struct nlmsghdr hdr;
        struct rtmsg msg;
        struct rtattr dst_attr __attribute__((aligned(NLMSG_ALIGNTO)));
        nd_addr_t dst;
    } req;

    memset(&req, 0, sizeof(req));

    req.msg.rtm_protocol = RTPROT_NDPPD;
    req.msg.rtm_family = AF_INET6;
    req.msg.rtm_dst_len = pflen;
    req.msg.rtm_table = table;
    req.msg.rtm_scope = RT_SCOPE_UNIVERSE;

    req.dst_attr.rta_type = RTA_DST;
    req.dst_attr.rta_len = RTA_LENGTH(sizeof(req.dst));
    req.dst = *dst;

    req.hdr.nlmsg_type = RTM_DELROUTE;
    req.hdr.nlmsg_flags = NLM_F_REQUEST;
    req.hdr.nlmsg_len = sizeof(req);

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    return nd_io_send(ndL_io, (struct sockaddr *)&addr, sizeof(addr), &req, sizeof(req)) >= 0;
#else
    struct __attribute__((packed))
    {
        struct rt_msghdr hdr;
        struct sockaddr_in6 dst;
        struct sockaddr_in6 mask __aligned(sizeof(u_long));
    } req;

    memset(&req, 0, sizeof(req));
    req.hdr.rtm_type = RTM_DELETE;
    req.hdr.rtm_version = RTM_VERSION;
    req.hdr.rtm_pid = getpid();
    req.hdr.rtm_msglen = sizeof(req);
    req.hdr.rtm_addrs = RTA_DST | RTA_NETMASK;

#    ifdef __FreeBSD__
    (void)table;
#    else
    msg.hdr->rtm_tableid = table;
#    endif

    req.dst.sin6_family = AF_INET6;
    req.dst.sin6_len = sizeof(req.dst);
    req.dst.sin6_addr = *dst;

    req.mask.sin6_family = AF_INET6;
    req.mask.sin6_len = sizeof(req.mask);
    nd_mask_from_pflen(pflen, &req.mask.sin6_addr);

    nd_log_info("rt: Removing route %s/%d table %d", nd_aton(dst), pflen, table);

    return nd_io_write(ndL_io, &req, sizeof(req)) >= 0;
#endif
}

void nl_rt_remove_owned_routes()
{
    ND_LL_FOREACH_S(ndL_routes, route, tmp, next)
    {
        if (route->owned)
            nd_rt_remove_route(&route->dst, route->pflen, route->table);
    }
}
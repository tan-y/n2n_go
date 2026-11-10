/**
 * (C) 2007-22 - ntop.org and contributors
 * Modified for native utun (L3) support on macOS.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "n2n.h"

#ifdef __APPLE__

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define UTUN_CONTROL_NAME "com.apple.net.utun_control"
#define UTUN_HDR_SIZE     4 /* protocol family prepended by utun */

/* Edge owning this device; set by the glue layer after edge_init(). */
static n2n_edge_t *g_edge = NULL;

void tuntap_osx_bind_edge (n2n_edge_t *eee) {
    g_edge = eee;
}

/* Resolve the peer MAC registered for a destination IPv4 address (given in
 * network byte order). Native n2n peers keep multicast dropping enabled by
 * default, so a frame sent to a broadcast MAC never reaches their TAP; using
 * the peer's own MAC keeps the traffic unicast. */
static int peer_mac_for_ip (uint32_t ip_be, n2n_mac_t mac) {
    struct peer_info *peer, *tmp;

    if(g_edge == NULL)
        return 0;

    HASH_ITER(hh, g_edge->pending_peers, peer, tmp) {
        if(peer->dev_addr.net_addr == ntohl(ip_be)) {
            memcpy(mac, peer->mac_addr, sizeof(n2n_mac_t));
            return 1;
        }
    }

    HASH_ITER(hh, g_edge->known_peers, peer, tmp) {
        if(peer->dev_addr.net_addr == ntohl(ip_be)) {
            memcpy(mac, peer->mac_addr, sizeof(n2n_mac_t));
            return 1;
        }
    }

    return 0;
}

/* Run a tool with an argument vector (no shell, no command injection). */
static int run_tool(const char *path, char *const argv[]) {
    pid_t pid;
    int   status = 0;

    pid = fork();
    if(pid < 0) {
        traceEvent(TRACE_ERROR, "fork() failed: %s", strerror(errno));
        return -1;
    }

    if(pid == 0) {
        /* Tools such as route print their progress to stdout (e.g.
         * "add net 10.0.3.0: gateway utun4"). That line has no timestamp and
         * would be mixed into the edge log, so silence stdout; stderr remains
         * available for real errors. */
        freopen("/dev/null", "w", stdout);
        execv(path, argv);
        _exit(127);
    }

    if(waitpid(pid, &status, 0) < 0) {
        traceEvent(TRACE_ERROR, "waitpid() failed: %s", strerror(errno));
        return -1;
    }
    if(!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        traceEvent(TRACE_ERROR, "%s exited with status %d", path, WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

/* Count the leading set bits of a netmask stored in network byte order. */
static int mask_bits(uint32_t mask) {
    uint32_t m = ntohl(mask);
    int      bits = 0;

    while(bits < 32 && (m & 0x80000000u)) {
        bits++;
        m <<= 1;
    }
    return bits;
}

/* ********************************** */

int tuntap_open (tuntap_dev *device /* ignored */,
                 char *dev,
                 const char *address_mode, /* static only; dhcp is not supported */
                 char *device_ip,
                 char *device_mask,
                 const char *device_mac,
                 int mtu,
                 int metric) {

    struct sockaddr_ctl addr;
    struct ctl_info     info;
    int                 fd;
    char                utun_name[32];
    socklen_t           utun_name_len = sizeof(utun_name);
    char                mtu_str[16];
    char               *ifconfig_argv[11];
    char               *route_argv[9];
    char                net_str[INET_ADDRSTRLEN];
    char                mask_str[INET_ADDRSTRLEN];
    struct in_addr      net_addr;
    struct in_addr      mask_addr;
    int                 bits;
    int                 i;

    (void)address_mode;
    (void)metric; /* macOS has no per-interface metric here */

    device->fd = -1;

    fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if(fd < 0) {
        traceEvent(TRACE_ERROR, "socket(PF_SYSTEM) failed: %s", strerror(errno));
        return -1;
    }

    memset(&info, 0, sizeof(info));
    strncpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));
    if(ioctl(fd, CTLIOCGINFO, &info) < 0) {
        traceEvent(TRACE_ERROR, "ioctl(CTLIOCGINFO) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sc_id = info.ctl_id;
    addr.sc_len = sizeof(addr);
    addr.sc_family = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;
    addr.sc_unit = 0; /* let the system assign the next free utunN */

    if(dev && (strlen(dev) > 4) && (strncmp(dev, "utun", 4) == 0)) {
        int unit = atoi(dev + 4);
        if(unit > 0)
            addr.sc_unit = unit + 1;
    }

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        traceEvent(TRACE_ERROR, "connect(utun) failed: %s (root privileges required)", strerror(errno));
        close(fd);
        return -1;
    }

    if(getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, utun_name, &utun_name_len) < 0) {
        traceEvent(TRACE_ERROR, "getsockopt(UTUN_OPT_IFNAME) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    utun_name[sizeof(utun_name) - 1] = '\0';

    memset(device->dev_name, 0, sizeof(device->dev_name));
    strncpy(device->dev_name, utun_name, sizeof(device->dev_name) - 1);

    if(mtu <= 0)
        mtu = DEFAULT_MTU;
    if(mtu > 65535)
        mtu = 65535;

    device->ip_addr = inet_addr(device_ip);
    device->device_mask = inet_addr(device_mask);
    /* n2n derives the supernode net_bitlen from this field, so it must be set
     * in network byte order just like ip_addr. */
    device->mtu = (uint16_t)mtu;
    device->if_idx = 0;

    if(device_mac && (device_mac[0] != '\0')) {
        if(str2mac(device->mac_addr, device_mac) != 0) {
            traceEvent(TRACE_WARNING, "invalid MAC address %s, generating one", device_mac);
            device_mac = NULL;
        }
    }
    if(!device_mac || (device_mac[0] == '\0')) {
        if(memrnd(device->mac_addr, N2N_MAC_SIZE) != 0) {
            for(i = 0; i < N2N_MAC_SIZE; i++)
                device->mac_addr[i] = (uint8_t)(rand() & 0xFF);
        }
        device->mac_addr[0] &= 0xFE; /* unicast */
        device->mac_addr[0] |= 0x02; /* locally administered */
    }

    snprintf(mtu_str, sizeof(mtu_str), "%d", mtu);

    i = 0;
    ifconfig_argv[i++] = (char *)"/sbin/ifconfig";
    ifconfig_argv[i++] = device->dev_name;
    ifconfig_argv[i++] = (char *)"inet";
    ifconfig_argv[i++] = device_ip;   /* local address */
    ifconfig_argv[i++] = device_ip;   /* destination: self (point-to-point) */
    ifconfig_argv[i++] = (char *)"netmask";
    ifconfig_argv[i++] = device_mask;
    ifconfig_argv[i++] = (char *)"mtu";
    ifconfig_argv[i++] = mtu_str;
    ifconfig_argv[i++] = (char *)"up";
    ifconfig_argv[i] = NULL;

    if(run_tool("/sbin/ifconfig", ifconfig_argv) < 0) {
        traceEvent(TRACE_ERROR, "failed to configure %s via ifconfig", device->dev_name);
        close(fd);
        device->fd = -1;
        return -1;
    }

    /* A point-to-point ifconfig only installs the host route for the local
     * address. Without the subnet route, traffic to peers would follow the
     * default route instead of the tunnel. /32 peers already have their host
     * route, and /0 would hijack the default route, so skip both. */
    bits = mask_bits(device->device_mask);
    if(bits > 0 && bits < 32) {
        net_addr.s_addr = device->ip_addr & device->device_mask;
        mask_addr.s_addr = device->device_mask;
        snprintf(net_str, sizeof(net_str), "%s", inet_ntoa(net_addr));
        snprintf(mask_str, sizeof(mask_str), "%s", inet_ntoa(mask_addr));

        i = 0;
        route_argv[i++] = (char *)"/sbin/route";
        route_argv[i++] = (char *)"add";
        route_argv[i++] = (char *)"-net";
        route_argv[i++] = net_str;
        route_argv[i++] = (char *)"-netmask";
        route_argv[i++] = mask_str;
        route_argv[i++] = (char *)"-interface";
        route_argv[i++] = device->dev_name;
        route_argv[i] = NULL;

        if(run_tool("/sbin/route", route_argv) < 0)
            traceEvent(TRACE_WARNING, "failed to add route %s/%d via %s (it may already exist)",
                       net_str, bits, device->dev_name);
    }

    device->fd = fd;

    traceEvent(TRACE_NORMAL, "Native %s up and running (%s/%s, MTU %d)",
               device->dev_name, device_ip, device_mask, mtu);

    return device->fd;
}

/*
 * Checksum helpers. macOS utun hands TCP/UDP packets up with the transport
 * checksum pending (offload); native n2n peers verify it and silently drop
 * mismatching frames. Completing it here is a no-op when it is already valid.
 */
static uint32_t csum_partial (uint32_t sum, const uint8_t *data, int len) {
    while(len > 1) {
        sum += (uint32_t)((data[0] << 8) | data[1]);
        data += 2;
        len -= 2;
    }
    if(len == 1)
        sum += (uint32_t)data[0] << 8;
    return sum;
}

static uint16_t csum_fold (uint32_t sum) {
    while(sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

static int complete_transport_csum (uint8_t *ip) {
    int       ihl;
    int       total;
    uint8_t   proto;
    int       off;
    int       len;
    uint16_t  old_sum;
    uint16_t  new_sum;
    uint32_t  sum;

    ihl = (ip[0] & 0x0F) * 4;
    total = (ip[2] << 8) | ip[3];
    proto = ip[9];

    if((proto != 6) && (proto != 17))            /* TCP / UDP only */
        return 0;
    if((((ip[6] << 8) | ip[7]) & 0x3FFF) != 0)   /* skip fragments */
        return 0;
    if(total < ihl + 8)
        return 0;

    off = ihl + ((proto == 6) ? 16 : 6);
    len = total - ihl;
    old_sum = (uint16_t)((ip[off] << 8) | ip[off + 1]);
    if((proto == 17) && (old_sum == 0))          /* UDP checksum disabled */
        return 0;

    ip[off] = 0;
    ip[off + 1] = 0;
    sum = csum_partial(0, ip + 12, 8);           /* src + dst */
    sum += proto;
    sum += (uint32_t)len;
    sum = csum_partial(sum, ip + ihl, len);
    new_sum = csum_fold(sum);
    ip[off] = (uint8_t)(new_sum >> 8);
    ip[off + 1] = (uint8_t)(new_sum & 0xFF);

    return (new_sum != old_sum);
}

/*
 * L3 to L2 bridge: utun provides raw IP packets with a 4-byte protocol family
 * header, while n2n expects Ethernet frames.
 */
int tuntap_read (struct tuntap_dev *tuntap, unsigned char *buf, int len) {

    size_t         raw_size;
    unsigned char *raw;
    int            nread;
    int            ip_len;
    int            unicast;
    ether_hdr_t   *hdr;
    uint32_t       family;
    uint32_t       dst_ip = 0;
    n2n_mac_t      peer_mac;

    if(tuntap->fd < 0)
        return -1;

    raw_size = (size_t)((tuntap->mtu > 0) ? tuntap->mtu : DEFAULT_MTU) + UTUN_HDR_SIZE;
    raw = malloc(raw_size);
    if(raw == NULL) {
        traceEvent(TRACE_ERROR, "out of memory in tuntap_read");
        return -1;
    }

    for(;;) {
        nread = (int)read(tuntap->fd, raw, raw_size);
        if(nread < UTUN_HDR_SIZE) {
            free(raw);
            if(nread > 0)
                traceEvent(TRACE_WARNING, "short utun packet (%d bytes)", nread);
            return nread;
        }

        memcpy(&family, raw, sizeof(family));
        if(ntohl(family) != AF_INET) {
            /* utun is L3; IPv6 is out of scope for the first iteration. */
            traceEvent(TRACE_INFO, "dropping non-IPv4 packet from %s (family %u)",
                       tuntap->dev_name, (unsigned int)ntohl(family));
            continue;
        }
        break;
    }

    ip_len = nread - UTUN_HDR_SIZE;
    if(ip_len + (int)sizeof(ether_hdr_t) > len) {
        traceEvent(TRACE_WARNING, "utun packet (%d bytes) exceeds n2n buffer (%d bytes)",
                   ip_len, len);
        free(raw);
        return -1;
    }

    hdr = (ether_hdr_t *)buf;
    unicast = 0;
    if(ip_len >= (int)(IP4_DSTOFFSET + 4)) {
        memcpy(&dst_ip, raw + UTUN_HDR_SIZE + IP4_DSTOFFSET, sizeof(dst_ip));
        unicast = peer_mac_for_ip(dst_ip, peer_mac);
    }
    if(unicast)
        memcpy(hdr->dhost, peer_mac, ETH_ADDR_LEN);
    else
        memset(hdr->dhost, 0xFF, ETH_ADDR_LEN); /* broadcast; no peer MAC known */
    memcpy(hdr->shost, tuntap->mac_addr, ETH_ADDR_LEN);
    hdr->type = htons(0x0800); /* IPv4 */

    memcpy(buf + sizeof(ether_hdr_t), raw + UTUN_HDR_SIZE, (size_t)ip_len);
    free(raw);

    if(ip_len >= 20)
        complete_transport_csum(buf + sizeof(ether_hdr_t));

    return ip_len + (int)sizeof(ether_hdr_t);
}

int tuntap_write (struct tuntap_dev *tuntap, unsigned char *buf, int len) {

    ether_hdr_t   *hdr;
    unsigned char *out;
    uint32_t       family;
    int            written;

    if((tuntap->fd < 0) || (len < (int)sizeof(ether_hdr_t)))
        return 0;

    hdr = (ether_hdr_t *)buf;

    if(ntohs(hdr->type) == 0x0800) {
        /* IPv4: strip Ethernet header and prepend the 4-byte AF_INET marker. */
        out = malloc((size_t)(len - (int)sizeof(ether_hdr_t)) + UTUN_HDR_SIZE);
        if(out == NULL)
            return -1;

        family = htonl(AF_INET);
        memcpy(out, &family, UTUN_HDR_SIZE);
        memcpy(out + UTUN_HDR_SIZE, buf + sizeof(ether_hdr_t),
               (size_t)(len - (int)sizeof(ether_hdr_t)));

        written = (int)write(tuntap->fd, out, (size_t)(len - (int)sizeof(ether_hdr_t)) + UTUN_HDR_SIZE);
        free(out);
        return written;
    }

    if(ntohs(hdr->type) == 0x0806) {
        /* ARP is meaningless on a layer-3 interface; report success and drop. */
        return len;
    }

    traceEvent(TRACE_INFO, "dropping unsupported Ethernet type 0x%04x (utun is IPv4 only)",
               (unsigned int)ntohs(hdr->type));
    return len;
}

void tuntap_close (struct tuntap_dev *tuntap) {

    if(tuntap->fd >= 0) {
        close(tuntap->fd);
        tuntap->fd = -1;
    }
}

/* Called to pick up dynamic address changes; utun keeps the static address. */
void tuntap_get_address (struct tuntap_dev *tuntap) {

    (void)tuntap;
}

#endif /* __APPLE__ */

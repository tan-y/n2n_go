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
#include <ifaddrs.h>
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
#include <time.h>
#include <unistd.h>

#define UTUN_CONTROL_NAME "com.apple.net.utun_control"
#define UTUN_HDR_SIZE     4 /* protocol family prepended by utun */

/* Edge owning this device; set by the glue layer after edge_init(). */
static n2n_edge_t *g_edge = NULL;

/* IPv6 is opt-in (--ipv6); while disabled IPv6 packets are dropped as before. */
static int g_ipv6_enabled = 0;

/* Link-local address derived from the device MAC (EUI-64); zero until set. */
static uint8_t g_ip6_ll[16];
static int     g_ip6_ll_set = 0;

/* All IPv6 addresses assigned to our device. macOS adds its own host-wide
 * link-local on top of ours and may pick it as the source address, so neighbour
 * solicitations arrive for any of them. */
#define LOCAL_IP6_MAX 8
static uint8_t g_ip6_addrs[LOCAL_IP6_MAX][16];
static int     g_ip6_addr_count = 0;
static time_t  g_ip6_refresh = 0;

static void ip6_addrs_refresh (const char *dev_name) {
    struct ifaddrs *list = NULL;
    struct ifaddrs *ifa;

    g_ip6_addr_count = 0;
    if(getifaddrs(&list) != 0) {
        traceEvent(TRACE_WARNING, "getifaddrs() failed: %s", strerror(errno));
        return;
    }

    for(ifa = list; (ifa != NULL) && (g_ip6_addr_count < LOCAL_IP6_MAX); ifa = ifa->ifa_next) {
        struct sockaddr_in6 *sa6;

        if((ifa->ifa_addr == NULL) || (ifa->ifa_addr->sa_family != AF_INET6))
            continue;
        if(strcmp(ifa->ifa_name, dev_name) != 0)
            continue;

        sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
        memcpy(g_ip6_addrs[g_ip6_addr_count++], &sa6->sin6_addr, 16);
    }
    freeifaddrs(list);
    g_ip6_refresh = time(NULL);
}

/* Is the solicited target address ours? Unknown addresses trigger a rate
 * limited refresh: the kernel may add its automatic link-local after we
 * configured the interface. */
static int ip6_addr_is_ours (const char *dev_name, const uint8_t *target) {
    int i;

    if(g_ip6_ll_set && (memcmp(target, g_ip6_ll, 16) == 0))
        return 1;
    for(i = 0; i < g_ip6_addr_count; i++)
        if(memcmp(target, g_ip6_addrs[i], 16) == 0)
            return 1;
    if((time(NULL) - g_ip6_refresh) < 1)
        return 0;

    ip6_addrs_refresh(dev_name);
    for(i = 0; i < g_ip6_addr_count; i++)
        if(memcmp(target, g_ip6_addrs[i], 16) == 0)
            return 1;
    return 0;
}

void tuntap_osx_enable_ipv6 (int on) {
    g_ipv6_enabled = (on != 0);
}

/* Minimal IPv6 -> MAC neighbour cache, learned from inbound NS/NA so unicast
 * frames can be sent to the peer's MAC instead of flooding them. Only the edge
 * loop calls tuntap_read/tuntap_write, so the table needs no locking. */
#define NEIGHBOR_MAX 64
#define NEIGHBOR_TTL 300

typedef struct {
    uint8_t   addr[16];
    n2n_mac_t mac;
    time_t    seen;
} neighbor_entry_t;

static neighbor_entry_t g_neighbors[NEIGHBOR_MAX];
static int              g_neighbor_count = 0;

static neighbor_entry_t *neighbor_find (const uint8_t *addr, time_t now) {
    int i;

    for(i = 0; i < g_neighbor_count; i++) {
        if(memcmp(g_neighbors[i].addr, addr, 16) == 0) {
            if((now - g_neighbors[i].seen) > NEIGHBOR_TTL) {
                g_neighbors[i] = g_neighbors[g_neighbor_count - 1];
                g_neighbor_count--;
                return NULL;
            }
            return &g_neighbors[i];
        }
    }
    return NULL;
}

static void neighbor_learn (const uint8_t *addr, const n2n_mac_t mac, time_t now) {
    neighbor_entry_t *entry;
    int               i;
    int               oldest = 0;

    entry = neighbor_find(addr, now);
    if(entry != NULL) {
        memcpy(entry->mac, mac, sizeof(n2n_mac_t));
        entry->seen = now;
        return;
    }

    if(g_neighbor_count == NEIGHBOR_MAX) {
        for(i = 1; i < NEIGHBOR_MAX; i++)
            if(g_neighbors[i].seen < g_neighbors[oldest].seen)
                oldest = i;
        entry = &g_neighbors[oldest];
    } else
        entry = &g_neighbors[g_neighbor_count++];

    memcpy(entry->addr, addr, 16);
    memcpy(entry->mac, mac, sizeof(n2n_mac_t));
    entry->seen = now;
}

/* RFC 2464: ff02::1 -> 33:33:00:00:00:01, i.e. 33:33 plus the low 32 bits. */
static void ipv6_mcast_mac (const uint8_t *addr, n2n_mac_t mac) {
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = addr[12];
    mac[3] = addr[13];
    mac[4] = addr[14];
    mac[5] = addr[15];
}

/* Learn the sender's MAC from neighbour discovery messages: an NA carries its
 * own address as the target, an NS carries it as the source (but DAD uses ::). */
static void learn_neighbor_from_icmpv6 (const uint8_t *ip, int ip_len,
                                        const n2n_mac_t mac) {
    static const uint8_t unspecified[16] = {0};
    const uint8_t       *icmp;

    if((ip_len < 40) || (ip[6] != 58)) /* ICMPv6 only, no extension headers */
        return;

    icmp = ip + 40;
    if((icmp[0] == 136) && (ip_len >= 40 + 24))
        neighbor_learn(icmp + 8, mac, time(NULL)); /* NA: target address */
    else if((icmp[0] == 135) && (ip_len >= 40 + 24) &&
            (memcmp(ip + 8, unspecified, 16) != 0))
        neighbor_learn(ip + 8, mac, time(NULL));   /* NS: source address */
}

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

/* Applies an IPv4 address/netmask (dotted strings; both empty while the address
 * is still unknown) and the MTU to an already opened utun device, and installs
 * the subnet route. Shared by tuntap_open() and the SN_ASSIGN path, which
 * configures the device only once the supernode has assigned an address. */
int tuntap_osx_configure (tuntap_dev *device, const char *device_ip,
                          const char *device_mask, int mtu) {

    char            mtu_str[16];
    char           *ifconfig_argv[16];
    char           *route_argv[9];
    char            net_str[INET_ADDRSTRLEN];
    char            mask_str[INET_ADDRSTRLEN];
    struct in_addr  net_addr;
    struct in_addr  mask_addr;
    int             bits;
    int             i;
    int             has_ip;

    if((device == NULL) || (device->dev_name[0] == '\0'))
        return -1;

    if(mtu <= 0)
        mtu = DEFAULT_MTU;
    if(mtu > 65535)
        mtu = 65535;

    has_ip = (device_ip != NULL) && (device_ip[0] != '\0') &&
             (device_mask != NULL) && (device_mask[0] != '\0');

    snprintf(mtu_str, sizeof(mtu_str), "%d", mtu);

    i = 0;
    ifconfig_argv[i++] = (char *)"/sbin/ifconfig";
    ifconfig_argv[i++] = device->dev_name;
    if(has_ip) {
        ifconfig_argv[i++] = (char *)"inet";
        ifconfig_argv[i++] = (char *)device_ip;   /* local address */
        ifconfig_argv[i++] = (char *)device_ip;   /* destination: self (point-to-point) */
        ifconfig_argv[i++] = (char *)"netmask";
        ifconfig_argv[i++] = (char *)device_mask;
        /* n2n derives the supernode net_bitlen from this field, so it must be
         * stored in network byte order just like ip_addr. */
        device->ip_addr = inet_addr(device_ip);
        device->device_mask = inet_addr(device_mask);
    }
    ifconfig_argv[i++] = (char *)"mtu";
    ifconfig_argv[i++] = mtu_str;
    ifconfig_argv[i++] = (char *)"up";
    ifconfig_argv[i] = NULL;

    if(run_tool("/sbin/ifconfig", ifconfig_argv) < 0) {
        traceEvent(TRACE_ERROR, "failed to configure %s via ifconfig", device->dev_name);
        return -1;
    }

    device->mtu = (uint16_t)mtu;

    if(!has_ip)
        return 0;

    if(g_ipv6_enabled) {
        char  ll_str[64];
        char *inet6_argv[8];

        /* macOS assigns no link-local address to utun devices, but neighbour
         * discovery needs one. Derive the EUI-64 interface id from the device
         * MAC (U/L bit flipped) so every edge gets a distinct address. */
        snprintf(ll_str, sizeof(ll_str), "fe80::%02x%02x:%02xff:fe%02x:%02x%02x",
                 (unsigned int)(device->mac_addr[0] ^ 0x02), (unsigned int)device->mac_addr[1],
                 (unsigned int)device->mac_addr[2], (unsigned int)device->mac_addr[3],
                 (unsigned int)device->mac_addr[4], (unsigned int)device->mac_addr[5]);

        i = 0;
        inet6_argv[i++] = (char *)"/sbin/ifconfig";
        inet6_argv[i++] = device->dev_name;
        inet6_argv[i++] = (char *)"inet6";
        inet6_argv[i++] = ll_str;
        inet6_argv[i++] = (char *)"prefixlen";
        inet6_argv[i++] = (char *)"64";
        inet6_argv[i++] = (char *)"up";
        inet6_argv[i] = NULL;

        if(run_tool("/sbin/ifconfig", inet6_argv) < 0)
            traceEvent(TRACE_WARNING, "failed to assign IPv6 link-local %s to %s",
                       ll_str, device->dev_name);
        else {
            /* Same address in binary form, used to answer neighbour solicitation. */
            g_ip6_ll[0] = 0xfe;
            g_ip6_ll[1] = 0x80;
            g_ip6_ll[8] = device->mac_addr[0] ^ 0x02;
            g_ip6_ll[9] = device->mac_addr[1];
            g_ip6_ll[10] = device->mac_addr[2];
            g_ip6_ll[11] = 0xff;
            g_ip6_ll[12] = 0xfe;
            g_ip6_ll[13] = device->mac_addr[3];
            g_ip6_ll[14] = device->mac_addr[4];
            g_ip6_ll[15] = device->mac_addr[5];
            g_ip6_ll_set = 1;
            ip6_addrs_refresh(device->dev_name);
            traceEvent(TRACE_NORMAL, "IPv6 link-local %s on %s", ll_str, device->dev_name);
        }
    }

    /* A point-to-point ifconfig only installs the host route for the local
     * address. Without the subnet route, traffic to peers would follow the
     * default route instead of the tunnel. /32 peers already have their host
     * route, and /0 would hijack the default route, so skip both. */
    bits = mask_bits(device->device_mask);
    if(bits > 0 && bits < 32) {
        net_addr.s_addr = device->ip_addr & device->device_mask;
        mask_addr.s_addr = device->device_mask;
        /* inet_ntop() instead of inet_ntoa(): this function can run on the Go
         * polling thread while the edge loop uses inet_ntoa() on its own. */
        inet_ntop(AF_INET, &net_addr, net_str, sizeof(net_str));
        inet_ntop(AF_INET, &mask_addr, mask_str, sizeof(mask_str));

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

    return 0;
}

/* ********************************** */

int tuntap_open (tuntap_dev *device /* ignored */,
                 char *dev,
                 const char *address_mode, /* static/sn_assign; dhcp is not supported */
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

    /* In SN_ASSIGN mode both strings are empty until the supernode answers;
     * tuntap_osx_configure() then applies the assigned address later. */
    device->ip_addr = 0;
    device->device_mask = 0;
    device->if_idx = 0;

    if(tuntap_osx_configure(device, device_ip, device_mask, mtu) < 0) {
        close(fd);
        device->fd = -1;
        return -1;
    }

    device->fd = fd;

    if(device->ip_addr != 0)
        traceEvent(TRACE_NORMAL, "Native %s up and running (%s/%s, MTU %d)",
                   device->dev_name, device_ip, device_mask, device->mtu);
    else
        traceEvent(TRACE_NORMAL, "Native %s up and running (address pending, MTU %d)",
                   device->dev_name, device->mtu);

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

/* IPv6 variant. Only the common case is handled: TCP, UDP or ICMPv6 directly
 * after the fixed header. Packets carrying extension headers (including the
 * fragment header) are left untouched, matching the IPv4 fragment policy. */
static int complete_transport_csum_v6 (uint8_t *ip) {
    int       payload_len;
    uint8_t   next;
    int       off;
    uint16_t  old_sum;
    uint16_t  new_sum;
    uint32_t  sum;

    payload_len = (ip[4] << 8) | ip[5];
    next = ip[6];

    if((next != 6) && (next != 17) && (next != 58)) /* TCP / UDP / ICMPv6 */
        return 0;
    if(payload_len < 8)
        return 0;

    if(next == 6)
        off = 40 + 16;
    else if(next == 17)
        off = 40 + 6;
    else
        off = 40 + 2;

    old_sum = (uint16_t)((ip[off] << 8) | ip[off + 1]);
    ip[off] = 0;
    ip[off + 1] = 0;

    sum = csum_partial(0, ip + 8, 32);           /* src + dst */
    sum += (uint32_t)(payload_len >> 16);
    sum += (uint32_t)(payload_len & 0xFFFF);
    sum += next;
    sum = csum_partial(sum, ip + 40, payload_len);
    new_sum = csum_fold(sum);
    ip[off] = (uint8_t)(new_sum >> 8);
    ip[off + 1] = (uint8_t)(new_sum & 0xFF);

    return (new_sum != old_sum);
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

    if((ip[0] >> 4) == 6)
        return complete_transport_csum_v6(ip);

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
        family = ntohl(family);
        if((family != AF_INET) && (family != AF_INET6)) {
            traceEvent(TRACE_INFO, "dropping non-IP packet from %s (family %u)",
                       tuntap->dev_name, (unsigned int)family);
            continue;
        }
        if((family == AF_INET6) && !g_ipv6_enabled) {
            traceEvent(TRACE_INFO, "dropping IPv6 packet from %s (--ipv6 not enabled)",
                       tuntap->dev_name);
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
    memcpy(hdr->shost, tuntap->mac_addr, ETH_ADDR_LEN);

    if(family == AF_INET6) {
        /* IPv6 multicast maps to 33:33:low32; unicast needs the neighbour
         * cache and falls back to a broadcast frame when it misses. */
        memset(hdr->dhost, 0xFF, ETH_ADDR_LEN);
        if(ip_len >= 40) {
            const uint8_t    *dst = raw + UTUN_HDR_SIZE + 24;
            neighbor_entry_t *entry;

            if(dst[0] == 0xFF)
                ipv6_mcast_mac(dst, hdr->dhost);
            else if((entry = neighbor_find(dst, time(NULL))) != NULL)
                memcpy(hdr->dhost, entry->mac, ETH_ADDR_LEN);
        }
        hdr->type = htons(0x86DD); /* IPv6 */
    } else {
        unicast = 0;
        if(ip_len >= (int)(IP4_DSTOFFSET + 4)) {
            memcpy(&dst_ip, raw + UTUN_HDR_SIZE + IP4_DSTOFFSET, sizeof(dst_ip));
            unicast = peer_mac_for_ip(dst_ip, peer_mac);
        }
        if(unicast)
            memcpy(hdr->dhost, peer_mac, ETH_ADDR_LEN);
        else
            memset(hdr->dhost, 0xFF, ETH_ADDR_LEN); /* broadcast; no peer MAC known */
        hdr->type = htons(0x0800); /* IPv4 */
    }

    memcpy(buf + sizeof(ether_hdr_t), raw + UTUN_HDR_SIZE, (size_t)ip_len);
    free(raw);

    /* IPv6 transport checksums are completed in T4; IPv4 keeps the existing fix. */
    if((family == AF_INET) && (ip_len >= 20))
        complete_transport_csum(buf + sizeof(ether_hdr_t));

    return ip_len + (int)sizeof(ether_hdr_t);
}

/* Answer ARP requests for our own overlay address. Native TAP peers resolve
 * our pseudo MAC this way; without a reply their kernel never sends overlay
 * traffic to us. The reply goes straight into the tunnel: a layer-3 utun
 * cannot carry Ethernet frames. */
static void arp_maybe_reply (struct tuntap_dev *device, uint8_t *arp, int len) {
    uint8_t  frame[42];
    uint32_t target;

    if((len < 28) || (g_edge == NULL) || (device->ip_addr == 0))
        return;
    if((arp[0] != 0x00) || (arp[1] != 0x01) ||        /* Ethernet */
       (arp[2] != 0x08) || (arp[3] != 0x00) ||        /* IPv4 */
       (arp[6] != 0x00) || (arp[7] != 0x01))          /* request */
        return;

    memcpy(&target, arp + 24, 4);
    if(target != device->ip_addr)
        return;

    memset(frame, 0, sizeof(frame));
    memcpy(frame, arp + 8, 6);                        /* to the requester */
    memcpy(frame + 6, device->mac_addr, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;
    frame[14] = 0x00;
    frame[15] = 0x01;                                 /* htype: Ethernet */
    frame[16] = 0x08;
    frame[17] = 0x00;                                 /* ptype: IPv4 */
    frame[18] = 6;
    frame[19] = 4;
    frame[20] = 0x00;
    frame[21] = 0x02;                                 /* opcode: reply */
    memcpy(frame + 22, device->mac_addr, 6);          /* sender MAC */
    memcpy(frame + 28, &device->ip_addr, 4);          /* sender IP */
    memcpy(frame + 32, arp + 8, 6);                   /* target MAC */
    memcpy(frame + 38, arp + 14, 4);                  /* target IP */

    edge_send_packet2net(g_edge, frame, sizeof(frame));
}

/* Answer neighbour solicitations for our link-local address. Native TAP peers
 * must resolve our pseudo MAC before they can send anything to us, and a
 * layer-3 utun cannot carry Ethernet frames, so the advertisement is injected
 * into the tunnel directly. */
static void ns_maybe_reply (struct tuntap_dev *device, uint8_t *ip, int len,
                            const n2n_mac_t eth_src) {
    static const uint8_t unspecified[16] = {0};
    uint8_t              frame[sizeof(ether_hdr_t) + 40 + 32];
    uint8_t             *out_ip;
    uint8_t             *icmp;

    if((g_edge == NULL) || !g_ip6_ll_set || (len < 40 + 24))
        return;
    if(((ip[0] >> 4) != 6) || (ip[6] != 58))      /* ICMPv6, no extension headers */
        return;
    icmp = ip + 40;
    if(icmp[0] != 135)                             /* neighbour solicitation */
        return;
    if(!ip6_addr_is_ours(device->dev_name, icmp + 8)) /* asking for our address */
        return;
    if(memcmp(ip + 8, unspecified, 16) == 0)       /* duplicate address detection */
        return;

    memset(frame, 0, sizeof(frame));
    memcpy(frame, eth_src, ETH_ADDR_LEN);          /* back to the requester */
    memcpy(frame + ETH_ADDR_LEN, device->mac_addr, ETH_ADDR_LEN);
    frame[12] = 0x86;
    frame[13] = 0xDD;

    out_ip = frame + sizeof(ether_hdr_t);
    out_ip[0] = 0x60;
    out_ip[5] = 32;                                /* NA (24) + MAC option (8) */
    out_ip[6] = 58;                                /* ICMPv6 */
    out_ip[7] = 255;                               /* hop limit, as required */
    memcpy(out_ip + 8, icmp + 8, 16);              /* source: the address asked for */
    memcpy(out_ip + 24, ip + 8, 16);               /* destination: the soliciting node */

    icmp = out_ip + 40;
    icmp[0] = 136;                                 /* neighbour advertisement */
    icmp[4] = 0x60;                                /* solicited + override */
    memcpy(icmp + 8, out_ip + 8, 16);              /* target address */
    icmp[24] = 2;                                  /* target link-layer address */
    icmp[25] = 1;
    memcpy(icmp + 26, device->mac_addr, ETH_ADDR_LEN);

    complete_transport_csum_v6(out_ip);
    edge_send_packet2net(g_edge, frame, sizeof(frame));
}

int tuntap_write (struct tuntap_dev *tuntap, unsigned char *buf, int len) {

    ether_hdr_t   *hdr;
    unsigned char *out;
    uint32_t       family;
    uint16_t       etype;
    int            ip_len;
    int            written;

    if((tuntap->fd < 0) || (len < (int)sizeof(ether_hdr_t)))
        return 0;

    hdr = (ether_hdr_t *)buf;
    etype = ntohs(hdr->type);
    ip_len = len - (int)sizeof(ether_hdr_t);

    if(etype == 0x0806) {
        /* A layer-3 interface cannot receive ARP, but native TAP peers need
         * our MAC to reach us, so answer requests for our own address. */
        arp_maybe_reply(tuntap, buf + sizeof(ether_hdr_t), ip_len);
        return len;
    }

    if(etype == 0x86DD) {
        if(!g_ipv6_enabled) {
            traceEvent(TRACE_INFO, "dropping IPv6 frame from the overlay (--ipv6 not enabled)");
            return len;
        }
        family = htonl(AF_INET6);
        /* Learn the sender's address from its neighbour advertisements. */
        learn_neighbor_from_icmpv6(buf + sizeof(ether_hdr_t), ip_len, hdr->shost);
        ns_maybe_reply(tuntap, buf + sizeof(ether_hdr_t), ip_len, hdr->shost);
    } else if(etype == 0x0800)
        family = htonl(AF_INET);
    else {
        traceEvent(TRACE_INFO, "dropping unsupported Ethernet type 0x%04x",
                   (unsigned int)etype);
        return len;
    }

    /* Strip the pseudo-Ethernet header and prepend the AF_* marker utun wants. */
    out = malloc((size_t)ip_len + UTUN_HDR_SIZE);
    if(out == NULL)
        return -1;

    memcpy(out, &family, UTUN_HDR_SIZE);
    memcpy(out + UTUN_HDR_SIZE, buf + sizeof(ether_hdr_t), (size_t)ip_len);

    written = (int)write(tuntap->fd, out, (size_t)ip_len + UTUN_HDR_SIZE);
    free(out);
    return written;
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

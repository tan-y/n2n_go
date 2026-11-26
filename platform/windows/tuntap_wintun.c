/*
 * Windows wintun device layer for n2n.
 *
 * n2n assumes an L2 TAP device. Wintun only provides an L3 IP interface, so
 * this file replaces n2n's src/win32/wintap.c and adds a small L3 <-> L2
 * shim:
 *   - tuntap_read  reads an IPv4/IPv6 packet from wintun and builds an Ethernet
 *                  header (learned unicast, otherwise broadcast/multicast);
 *   - tuntap_write strips the Ethernet header, answers ARP and ICMPv6 neighbour
 *                  solicitations, and learns IP -> MAC mappings from incoming
 *                  overlay traffic.
 *
 * Only one edge runs per process; the wintun session, read thread stop event
 * and learning tables live in file-level state.
 */

#include "defs.h"

#include <iphlpapi.h>
#include <netioapi.h>
#include <time.h>
#include <wchar.h>

#include "n2n.h"
#include "n2n_win32.h"
#include "tuntap_wintun.h"

/* ------------------------------------------------------------------------- */
/* Minimal wintun 0.14 ABI */

typedef struct _WINTUN_ADAPTER *wintun_adapter_t;
typedef struct _WINTUN_SESSION *wintun_session_t;

typedef wintun_adapter_t (WINAPI *fn_wintun_create_adapter_t)(LPCWSTR, LPCWSTR, const GUID *);
typedef void (WINAPI *fn_wintun_close_adapter_t)(wintun_adapter_t);
typedef DWORD (WINAPI *fn_wintun_get_running_driver_version_t)(void);
typedef wintun_session_t (WINAPI *fn_wintun_start_session_t)(wintun_adapter_t, DWORD);
typedef void (WINAPI *fn_wintun_end_session_t)(wintun_session_t);
typedef HANDLE (WINAPI *fn_wintun_get_read_wait_event_t)(wintun_session_t);
typedef BYTE *(WINAPI *fn_wintun_receive_packet_t)(wintun_session_t, DWORD *);
typedef void (WINAPI *fn_wintun_release_receive_packet_t)(wintun_session_t, const BYTE *);
typedef BYTE *(WINAPI *fn_wintun_allocate_send_packet_t)(wintun_session_t, DWORD);
typedef void (WINAPI *fn_wintun_send_packet_t)(wintun_session_t, const BYTE *);
typedef void (WINAPI *fn_wintun_get_adapter_luid_t)(wintun_adapter_t, NET_LUID *);

static HMODULE g_wintun;
static wintun_adapter_t g_adapter;
static wintun_session_t g_session;
static HANDLE g_stop_event;
static int g_open;

static fn_wintun_create_adapter_t fn_create_adapter;
static fn_wintun_close_adapter_t fn_close_adapter;
static fn_wintun_get_running_driver_version_t fn_get_running_driver_version;
static fn_wintun_start_session_t fn_start_session;
static fn_wintun_end_session_t fn_end_session;
static fn_wintun_get_read_wait_event_t fn_get_read_wait_event;
static fn_wintun_receive_packet_t fn_receive_packet;
static fn_wintun_release_receive_packet_t fn_release_receive_packet;
static fn_wintun_allocate_send_packet_t fn_allocate_send_packet;
static fn_wintun_send_packet_t fn_send_packet;
static fn_wintun_get_adapter_luid_t fn_get_adapter_luid;

/* ------------------------------------------------------------------------- */
/* State shared by the tun read thread and the n2n main loop */

#define LEARN_MAX_ENTRIES 64
#define LEARN6_MAX_ENTRIES 64
#define LEARN_TTL_SECONDS 60
#define ARP_QUEUE_DEPTH 8
#define ARP_FRAME_MAX 160
#define IP6_ADDR_MAX 8
#define IP6_REFRESH_SECONDS 30
#define ADAPTER_NAME_MAX 128
#define WIN_RING_CAPACITY 0x400000 /* WintunStartSession ring size (power of two) */

typedef struct {
    uint32_t ip; /* network byte order, 0 = free */
    uint8_t mac[ETH_ADDR_LEN];
    time_t seen;
} learn_entry_t;

typedef struct {
    uint8_t ip[16];
    uint8_t mac[ETH_ADDR_LEN];
    time_t seen;
} learn6_entry_t;

typedef struct {
    int len;
    uint8_t data[ARP_FRAME_MAX];
} arp_frame_t;

static char g_adapter_name[ADAPTER_NAME_MAX];
static char g_if_name[256];
static char g_guid_seed[64];
static uint8_t g_mac[ETH_ADDR_LEN];
static uint32_t g_local_ip; /* network byte order */
static uint32_t g_local_mask;
static uint16_t g_mtu;

static learn_entry_t g_learn[LEARN_MAX_ENTRIES];
static int g_learn_count;

static learn6_entry_t g_learn6[LEARN6_MAX_ENTRIES];
static int g_learn6_count;

static int g_ipv6_enabled;
static NET_LUID g_luid;
static uint8_t g_local_ip6[IP6_ADDR_MAX][16];
static int g_local_ip6_count;
static time_t g_ip6_refresh;

static arp_frame_t g_arpq[ARP_QUEUE_DEPTH];
static int g_arpq_head, g_arpq_tail, g_arpq_count;

static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;

static BOOL CALLBACK lock_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void win_lock(void) {
    InitOnceExecuteOnce(&g_lock_once, lock_init_once, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void win_unlock(void) {
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------------- */
/* Helpers */

static void fnv1a_128(const uint8_t *data, size_t len, uint8_t out[16]) {
#if defined(__SIZEOF_INT128__)
    __uint128_t h = ((__uint128_t)0x6c62272e07bb0142ULL << 64) | (__uint128_t)0x62b821756295c58dULL;
    __uint128_t p = ((__uint128_t)1 << 88) | ((__uint128_t)1 << 8) | (__uint128_t)0x3b;
    size_t i;

    for(i = 0; i < len; i++) {
        h ^= data[i];
        h *= p;
    }
    for(i = 0; i < 16; i++)
        out[15 - i] = (uint8_t)(h >> (8 * i));
#else
    /* 64-bit fallback for compilers without __int128. */
    uint64_t h1 = 0xcbf29ce484222325ULL;
    uint64_t h2 = 0x84222325cbf29ce4ULL;
    size_t i;

    for(i = 0; i < len; i++) {
        h1 = (h1 ^ data[i]) * 0x100000001b3ULL;
        h2 = (h2 ^ data[i]) * 0x100000001b3ULL;
    }
    for(i = 0; i < 8; i++) {
        out[7 - i] = (uint8_t)(h1 >> (8 * i));
        out[15 - i] = (uint8_t)(h2 >> (8 * i));
    }
#endif
}

static size_t append_str(uint8_t *buf, size_t off, size_t cap, const char *s) {
    size_t n;

    if(s == NULL)
        return off;
    n = strlen(s);
    if(n > cap - off)
        n = cap - off;
    memcpy(buf + off, s, n);
    return off + n;
}

static void derive_guid(const char *name, const char *seed, GUID *guid) {
    uint8_t buf[256];
    uint8_t hash[16];
    size_t n = 0;

    n = append_str(buf, n, sizeof(buf), name);
    if(n < sizeof(buf))
        buf[n++] = '|';
    n = append_str(buf, n, sizeof(buf), seed);
    fnv1a_128(buf, n, hash);
    memcpy(guid, hash, sizeof(hash));
    guid->Data3 = (uint16_t)((guid->Data3 & 0x0fff) | 0x4000);
    guid->Data4[0] = (uint8_t)((guid->Data4[0] & 0x3f) | 0x80);
}

static void derive_mac(const char *name, const char *seed, const char *ip) {
    uint8_t buf[256];
    uint8_t hash[16];
    char host[128];
    DWORD host_len = (DWORD)sizeof(host);
    size_t n = 0;

    host[0] = '\0';
    if(!GetComputerNameA(host, &host_len))
        host[0] = '\0';

    n = append_str(buf, n, sizeof(buf), name);
    if(n < sizeof(buf))
        buf[n++] = '|';
    n = append_str(buf, n, sizeof(buf), seed);
    if(n < sizeof(buf))
        buf[n++] = '|';
    n = append_str(buf, n, sizeof(buf), ip);
    if(n < sizeof(buf))
        buf[n++] = '|';
    /* The hostname keeps two default-named nodes with the same community
     * from deriving the same MAC. */
    n = append_str(buf, n, sizeof(buf), host);
    fnv1a_128(buf, n, hash);
    memcpy(g_mac, hash, sizeof(g_mac));
    /* Locally administered, unicast. */
    g_mac[0] = (uint8_t)((g_mac[0] & 0xfc) | 0x02);
}

static int normalize_adapter_name(const char *in, char *out, size_t out_size) {
    const char *name = (in != NULL && in[0] != '\0') ? in : "n2n0";
    size_t i, n = strlen(name);

    if(n == 0 || n > ADAPTER_NAME_MAX - 1 || n >= out_size)
        return -1;
    for(i = 0; i < n; i++) {
        char c = name[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')
            continue;
        return -1;
    }
    memcpy(out, name, n + 1);
    return 0;
}

static uint32_t prefix_length_from_mask(const char *mask) {    struct in_addr a;
    uint32_t m;
    uint32_t bits = 0;

    if(mask == NULL || mask[0] == '\0')
        return 0;
    a.s_addr = inet_addr(mask);
    m = ntohl(a.s_addr);
    while(bits < 32 && (m & 0x80000000u)) {
        bits++;
        m <<= 1;
    }
    return bits;
}

static HMODULE load_wintun_dll(void) {
    wchar_t exe[MAX_PATH];
    wchar_t dll[MAX_PATH];
    wchar_t *slash;
    DWORD n;

    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if(n == 0 || n >= MAX_PATH)
        return NULL;
    slash = wcsrchr(exe, L'\\');
    if(slash == NULL)
        return NULL;
    slash[1] = L'\0';
    if(wcslen(exe) + wcslen(L"wintun.dll") + 1 > MAX_PATH)
        return NULL;
    wcscpy(dll, exe);
    wcscat(dll, L"wintun.dll");
    return LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static int resolve_wintun_api(void) {
    fn_create_adapter = (fn_wintun_create_adapter_t)GetProcAddress(g_wintun, "WintunCreateAdapter");
    fn_close_adapter = (fn_wintun_close_adapter_t)GetProcAddress(g_wintun, "WintunCloseAdapter");
    fn_get_running_driver_version = (fn_wintun_get_running_driver_version_t)GetProcAddress(g_wintun, "WintunGetRunningDriverVersion");
    fn_start_session = (fn_wintun_start_session_t)GetProcAddress(g_wintun, "WintunStartSession");
    fn_end_session = (fn_wintun_end_session_t)GetProcAddress(g_wintun, "WintunEndSession");
    fn_get_read_wait_event = (fn_wintun_get_read_wait_event_t)GetProcAddress(g_wintun, "WintunGetReadWaitEvent");
    fn_receive_packet = (fn_wintun_receive_packet_t)GetProcAddress(g_wintun, "WintunReceivePacket");
    fn_release_receive_packet = (fn_wintun_release_receive_packet_t)GetProcAddress(g_wintun, "WintunReleaseReceivePacket");
    fn_allocate_send_packet = (fn_wintun_allocate_send_packet_t)GetProcAddress(g_wintun, "WintunAllocateSendPacket");
    fn_send_packet = (fn_wintun_send_packet_t)GetProcAddress(g_wintun, "WintunSendPacket");
    fn_get_adapter_luid = (fn_wintun_get_adapter_luid_t)GetProcAddress(g_wintun, "WintunGetAdapterLUID");

    if(fn_create_adapter == NULL || fn_close_adapter == NULL ||
       fn_get_running_driver_version == NULL || fn_start_session == NULL ||
       fn_end_session == NULL || fn_get_read_wait_event == NULL ||
       fn_receive_packet == NULL || fn_release_receive_packet == NULL ||
       fn_allocate_send_packet == NULL || fn_send_packet == NULL ||
       fn_get_adapter_luid == NULL) {
        const char *missing = (fn_create_adapter == NULL) ? "WintunCreateAdapter"
                            : (fn_close_adapter == NULL) ? "WintunCloseAdapter"
                            : (fn_get_running_driver_version == NULL) ? "WintunGetRunningDriverVersion"
                            : (fn_start_session == NULL) ? "WintunStartSession"
                            : (fn_end_session == NULL) ? "WintunEndSession"
                            : (fn_get_read_wait_event == NULL) ? "WintunGetReadWaitEvent"
                            : (fn_receive_packet == NULL) ? "WintunReceivePacket"
                            : (fn_release_receive_packet == NULL) ? "WintunReleaseReceivePacket"
                            : (fn_allocate_send_packet == NULL) ? "WintunAllocateSendPacket"
                            : (fn_send_packet == NULL) ? "WintunSendPacket"
                            : "WintunGetAdapterLUID";
        traceEvent(TRACE_ERROR, "wintun.dll is missing required export %s (module %p, error %lu)",
                   missing, (void *)g_wintun, (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

static int configure_interface(NET_LUID luid, const char *ip, const char *mask, int mtu) {
    MIB_UNICASTIPADDRESS_ROW address;
    MIB_IPINTERFACE_ROW iface;
    NETIO_STATUS rc;

    if(ip != NULL && ip[0] != '\0') {
        InitializeUnicastIpAddressEntry(&address);
        address.InterfaceLuid = luid;
        address.Address.si_family = AF_INET;
        address.Address.Ipv4.sin_family = AF_INET;
        address.Address.Ipv4.sin_addr.s_addr = inet_addr(ip);
        address.OnLinkPrefixLength = (UINT8)prefix_length_from_mask(mask);
        address.DadState = IpDadStatePreferred;

        rc = CreateUnicastIpAddressEntry(&address);
        if(rc != NO_ERROR && rc != ERROR_OBJECT_ALREADY_EXISTS) {
            traceEvent(TRACE_ERROR, "CreateUnicastIpAddressEntry(%s) failed [%lu]",
                       ip, (unsigned long)rc);
            return -1;
        }
    }

    InitializeIpInterfaceEntry(&iface);
    iface.InterfaceLuid = luid;
    iface.Family = AF_INET;
    iface.NlMtu = (ULONG)mtu;
    iface.SitePrefixLength = 0;

    rc = SetIpInterfaceEntry(&iface);
    if(rc != NO_ERROR) {
        traceEvent(TRACE_ERROR, "SetIpInterfaceEntry(mtu=%d) failed [%lu]",
                   mtu, (unsigned long)rc);
        return -1;
    }
    return 0;
}

/* Applies the address assigned by the supernode after tuntap_open(). */
int tuntap_win_configure(tuntap_dev *device, const char *ip, const char *mask, int mtu) {
    if((device == NULL) || (ip == NULL) || (ip[0] == '\0') ||
       (mask == NULL) || (mask[0] == '\0'))
        return -1;

    if(configure_interface(g_luid, ip, mask, mtu) < 0)
        return -1;

    g_local_ip = inet_addr(ip);
    g_local_mask = inet_addr(mask);
    device->ip_addr = g_local_ip;
    device->device_mask = g_local_mask;
    device->mtu = (uint16_t)mtu;

    return 0;
}

static int configure_interface6(NET_LUID luid, int mtu) {    MIB_IPINTERFACE_ROW iface;
    NETIO_STATUS rc;

    InitializeIpInterfaceEntry(&iface);
    iface.InterfaceLuid = luid;
    iface.Family = AF_INET6;
    iface.NlMtu = (ULONG)mtu;
    iface.SitePrefixLength = 0;

    rc = SetIpInterfaceEntry(&iface);
    if(rc != NO_ERROR) {
        traceEvent(TRACE_WARNING, "SetIpInterfaceEntry(IPv6 mtu=%d) failed [%lu]",
                   mtu, (unsigned long)rc);
        return -1;
    }
    return 0;
}

static void teardown_wintun(void) {
    if(g_session != NULL) {
        if(fn_end_session != NULL)
            fn_end_session(g_session);
        g_session = NULL;
    }
    if(g_adapter != NULL) {
        if(fn_close_adapter != NULL)
            fn_close_adapter(g_adapter);
        g_adapter = NULL;
    }
    if(g_stop_event != NULL) {
        /* Keep the handle alive: n2n_go_edge_stop() may race with teardown
         * and SetEvent() on a closed handle would fault. */
        ResetEvent(g_stop_event);
    }
    win_lock();
    g_learn_count = 0;
    memset(g_learn, 0, sizeof(g_learn));
    g_learn6_count = 0;
    memset(g_learn6, 0, sizeof(g_learn6));
    g_local_ip6_count = 0;
    memset(g_local_ip6, 0, sizeof(g_local_ip6));
    g_ip6_refresh = 0;
    g_arpq_head = g_arpq_tail = g_arpq_count = 0;
    win_unlock();
    if(g_wintun != NULL) {
        FreeLibrary(g_wintun);
        g_wintun = NULL;
    }
    fn_create_adapter = NULL;
    fn_close_adapter = NULL;
    fn_get_running_driver_version = NULL;
    fn_start_session = NULL;
    fn_end_session = NULL;
    fn_get_read_wait_event = NULL;
    fn_receive_packet = NULL;
    fn_release_receive_packet = NULL;
    fn_allocate_send_packet = NULL;
    fn_send_packet = NULL;
    fn_get_adapter_luid = NULL;
    g_open = 0;
}

/* ------------------------------------------------------------------------- */
/* L3 <-> L2 helpers */

static void learn(uint32_t ip, const uint8_t *mac, time_t now) {
    int i;

    if(ip == 0 || mac == NULL)
        return;
    if(mac[0] & 0x01) /* multicast/broadcast source */
        return;

    win_lock();
    for(i = 0; i < g_learn_count; i++) {
        if(g_learn[i].ip == ip) {
            memcpy(g_learn[i].mac, mac, ETH_ADDR_LEN);
            g_learn[i].seen = now;
            win_unlock();
            return;
        }
    }
    if(g_learn_count < LEARN_MAX_ENTRIES) {
        i = g_learn_count++;
    } else {
        int oldest = 0;
        time_t oldest_seen = g_learn[0].seen;
        for(i = 1; i < g_learn_count; i++) {
            if(g_learn[i].seen < oldest_seen) {
                oldest = i;
                oldest_seen = g_learn[i].seen;
            }
        }
        i = oldest;
    }
    g_learn[i].ip = ip;
    memcpy(g_learn[i].mac, mac, ETH_ADDR_LEN);
    g_learn[i].seen = now;
    win_unlock();
}

/* Returns 1 and fills mac on hit, 0 on miss. Caller must hold g_lock. */
static int learn_lookup_locked(uint32_t ip, uint8_t mac[ETH_ADDR_LEN], time_t now) {
    int i;

    for(i = 0; i < g_learn_count; i++) {
        if(g_learn[i].ip != 0 && now - g_learn[i].seen > LEARN_TTL_SECONDS)
            g_learn[i].ip = 0;
        if(g_learn[i].ip == ip) {
            memcpy(mac, g_learn[i].mac, ETH_ADDR_LEN);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* IPv6 helpers (NDP uses the same learning table approach as ARP) */

static int ip6_is_unspecified(const uint8_t *ip) {
    int i;

    for(i = 0; i < 16; i++)
        if(ip[i] != 0)
            return 0;
    return 1;
}

static int ip6_is_multicast(const uint8_t *ip) {
    return (ip[0] == 0xff);
}

static void ip6_multicast_mac(const uint8_t *ip, uint8_t mac[ETH_ADDR_LEN]) {
    mac[0] = 0x33;
    mac[1] = 0x33;
    memcpy(mac + 2, ip + 12, 4);
}

static void learn6(const uint8_t *ip, const uint8_t *mac, time_t now) {
    int i;

    if(ip == NULL || mac == NULL)
        return;
    if((mac[0] & 0x01) || ip6_is_unspecified(ip) || ip6_is_multicast(ip))
        return;

    win_lock();
    for(i = 0; i < g_learn6_count; i++) {
        if(memcmp(g_learn6[i].ip, ip, 16) == 0) {
            memcpy(g_learn6[i].mac, mac, ETH_ADDR_LEN);
            g_learn6[i].seen = now;
            win_unlock();
            return;
        }
    }
    if(g_learn6_count < LEARN6_MAX_ENTRIES) {
        i = g_learn6_count++;
    } else {
        int oldest = 0;
        time_t oldest_seen = g_learn6[0].seen;
        for(i = 1; i < g_learn6_count; i++) {
            if(g_learn6[i].seen < oldest_seen) {
                oldest = i;
                oldest_seen = g_learn6[i].seen;
            }
        }
        i = oldest;
    }
    memcpy(g_learn6[i].ip, ip, 16);
    memcpy(g_learn6[i].mac, mac, ETH_ADDR_LEN);
    g_learn6[i].seen = now;
    win_unlock();
}

/* Returns 1 and fills mac on hit, 0 on miss. Caller must hold g_lock. */
static int learn6_lookup_locked(const uint8_t *ip, uint8_t mac[ETH_ADDR_LEN], time_t now) {
    int i;

    for(i = 0; i < g_learn6_count; i++) {
        if(g_learn6[i].seen != 0 && now - g_learn6[i].seen > LEARN_TTL_SECONDS)
            g_learn6[i].seen = 0;
        if((g_learn6[i].seen != 0) && (memcmp(g_learn6[i].ip, ip, 16) == 0)) {
            memcpy(mac, g_learn6[i].mac, ETH_ADDR_LEN);
            return 1;
        }
    }
    return 0;
}

/* Caches the adapter's IPv6 addresses (link-local and any configured global
 * addresses). wintun is an L3 device, so Windows owns the addresses; the shim
 * only needs them to answer neighbour solicitations. */
static void refresh_local_ip6(void) {
    PMIB_UNICASTIPADDRESS_TABLE table = NULL;
    time_t now = time(NULL);
    uint8_t addrs[IP6_ADDR_MAX][16];
    int count = 0;
    int have;
    ULONG i;

    win_lock();
    have = (g_local_ip6_count > 0);
    if((g_ip6_refresh != 0) &&
       (now - g_ip6_refresh < (have ? IP6_REFRESH_SECONDS : 2))) {
        win_unlock();
        return;
    }
    g_ip6_refresh = now;
    win_unlock();

    if(GetUnicastIpAddressTable(AF_INET6, &table) != NO_ERROR)
        return;

    for(i = 0; (i < table->NumEntries) && (count < IP6_ADDR_MAX); i++) {
        MIB_UNICASTIPADDRESS_ROW *row = &table->Table[i];

        if(row->InterfaceLuid.Value != g_luid.Value)
            continue;
        if(row->DadState != IpDadStatePreferred)
            continue;
        memcpy(addrs[count], row->Address.Ipv6.sin6_addr.s6_addr, 16);
        count++;
    }
    FreeMibTable(table);

    win_lock();
    memcpy(g_local_ip6, addrs, (size_t)count * 16);
    g_local_ip6_count = count;
    win_unlock();
}

static int local_ip6_owns(const uint8_t *addr) {
    int i, hit = 0;

    win_lock();
    for(i = 0; i < g_local_ip6_count; i++) {
        if(memcmp(g_local_ip6[i], addr, 16) == 0) {
            hit = 1;
            break;
        }
    }
    win_unlock();
    return hit;
}


static void arp_enqueue(const uint8_t *frame, int len) {
    if(len <= 0 || len > ARP_FRAME_MAX)
        return;
    win_lock();
    if(g_arpq_count == ARP_QUEUE_DEPTH) {
        /* Drop the oldest frame; ARP traffic is retried by the sender. */
        g_arpq_head = (g_arpq_head + 1) % ARP_QUEUE_DEPTH;
        g_arpq_count--;
    }
    g_arpq[g_arpq_tail].len = len;
    memcpy(g_arpq[g_arpq_tail].data, frame, len);
    g_arpq_tail = (g_arpq_tail + 1) % ARP_QUEUE_DEPTH;
    g_arpq_count++;
    win_unlock();
}

static int arp_dequeue(unsigned char *buf, int len) {
    int n = 0;

    win_lock();
    if(g_arpq_count > 0 && g_arpq[g_arpq_head].len <= len) {
        n = g_arpq[g_arpq_head].len;
        memcpy(buf, g_arpq[g_arpq_head].data, n);
        g_arpq_head = (g_arpq_head + 1) % ARP_QUEUE_DEPTH;
        g_arpq_count--;
    }
    win_unlock();
    return n;
}

static void build_arp_frame(uint8_t *frame,
                            const uint8_t *dst_mac,
                            const uint8_t *target_mac,
                            uint32_t target_ip,
                            uint16_t opcode) {
    ether_hdr_t *eth = (ether_hdr_t *)frame;
    uint8_t *arp = frame + ETH_FRAMESIZE;

    memcpy(eth->dhost, dst_mac, ETH_ADDR_LEN);
    memcpy(eth->shost, g_mac, ETH_ADDR_LEN);
    eth->type = htons(0x0806);

    arp[0] = 0;
    arp[1] = 1; /* Ethernet */
    arp[2] = 0x08;
    arp[3] = 0x00; /* IPv4 */
    arp[4] = ETH_ADDR_LEN;
    arp[5] = 4;
    arp[6] = (uint8_t)(opcode >> 8);
    arp[7] = (uint8_t)(opcode & 0xff);
    memcpy(arp + 8, g_mac, ETH_ADDR_LEN);
    memcpy(arp + 14, &g_local_ip, 4);
    memcpy(arp + 18, target_mac, ETH_ADDR_LEN);
    memcpy(arp + 24, &target_ip, 4);
}

static void queue_gratuitous_arp(void) {
    uint8_t frame[ETH_FRAMESIZE + 28];
    uint8_t broadcast[ETH_ADDR_LEN];

    if(g_local_ip == 0)
        return;
    memset(broadcast, 0xff, sizeof(broadcast));
    build_arp_frame(frame, broadcast, g_mac, g_local_ip, 2);
    arp_enqueue(frame, (int)sizeof(frame));
}

static void handle_arp_frame(const uint8_t *frame, int len) {
    const uint8_t *arp;
    uint16_t opcode;
    uint32_t sender_ip, target_ip;
    uint8_t reply[ETH_FRAMESIZE + 28];

    if(len < ETH_FRAMESIZE + 28)
        return;
    arp = frame + ETH_FRAMESIZE;
    if(arp[0] != 0 || arp[1] != 1 || arp[2] != 0x08 || arp[3] != 0x00 ||
       arp[4] != ETH_ADDR_LEN || arp[5] != 4)
        return;
    opcode = (uint16_t)((arp[6] << 8) | arp[7]);
    if(opcode != 1) /* only requests need an answer */
        return;

    memcpy(&sender_ip, arp + 14, 4);
    memcpy(&target_ip, arp + 24, 4);
    learn(sender_ip, arp + 8, time(NULL));

    if(g_local_ip == 0 || target_ip != g_local_ip)
        return;

    build_arp_frame(reply, arp + 8, arp + 8, sender_ip, 2);
    arp_enqueue(reply, (int)sizeof(reply));
}

#define IP6_HDR_SIZE  40
#define ICMP6_NA_SIZE 32

static uint32_t csum_partial(uint32_t sum, const uint8_t *buf, size_t len) {
    size_t i;

    for(i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((buf[i] << 8) | buf[i + 1]);
    if(len & 1)
        sum += (uint32_t)(buf[len - 1] << 8);
    return sum;
}

static uint16_t csum_fold(uint32_t sum) {
    while(sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum);
}

static uint16_t icmp6_checksum(const uint8_t *src, const uint8_t *dst,
                               const uint8_t *icmp, size_t len) {
    uint32_t sum = 0;
    uint8_t pseudo[4];

    sum = csum_partial(sum, src, 16);
    sum = csum_partial(sum, dst, 16);
    pseudo[0] = (uint8_t)(len >> 24);
    pseudo[1] = (uint8_t)(len >> 16);
    pseudo[2] = (uint8_t)(len >> 8);
    pseudo[3] = (uint8_t)len;
    sum = csum_partial(sum, pseudo, sizeof(pseudo));
    sum += 58; /* next header: ICMPv6 */
    return csum_fold(csum_partial(sum, icmp, len));
}

static int build_na_frame(uint8_t *frame, const uint8_t *dst_mac,
                          const uint8_t *our_addr, const uint8_t *dst_addr,
                          const uint8_t *target_addr) {
    ether_hdr_t *eth = (ether_hdr_t *)frame;
    uint8_t *ip = frame + ETH_FRAMESIZE;
    uint8_t *icmp = ip + IP6_HDR_SIZE;
    uint16_t c;

    memcpy(eth->dhost, dst_mac, ETH_ADDR_LEN);
    memcpy(eth->shost, g_mac, ETH_ADDR_LEN);
    eth->type = htons(0x86DD);

    memset(ip, 0, IP6_HDR_SIZE);
    ip[0] = 0x60;
    ip[4] = 0;
    ip[5] = ICMP6_NA_SIZE;
    ip[6] = 58;  /* ICMPv6 */
    ip[7] = 255; /* hop limit */
    memcpy(ip + 8, our_addr, 16);  /* source = solicited target */
    memcpy(ip + 24, dst_addr, 16); /* destination = solicitor */

    memset(icmp, 0, ICMP6_NA_SIZE);
    icmp[0] = 136; /* Neighbor Advertisement */
    icmp[4] = 0x60; /* Solicited | Override */
    memcpy(icmp + 8, target_addr, 16);
    icmp[24] = 2; /* target link-layer address */
    icmp[25] = 1; /* length in 8-byte units */
    memcpy(icmp + 26, g_mac, ETH_ADDR_LEN);

    c = icmp6_checksum(ip + 8, ip + 24, icmp, ICMP6_NA_SIZE);
    icmp[2] = (uint8_t)(c >> 8);
    icmp[3] = (uint8_t)(c & 0xff);

    return ETH_FRAMESIZE + IP6_HDR_SIZE + ICMP6_NA_SIZE;
}

/* Answers an ICMPv6 neighbour solicitation for one of the local addresses.
 * Returns 1 when a reply was queued (the solicitation need not reach Windows),
 * 0 otherwise. */
static int handle_ns_frame(const uint8_t *frame, int len) {
    const ether_hdr_t *eth = (const ether_hdr_t *)frame;
    const uint8_t *ip, *icmp;
    uint8_t reply[ETH_FRAMESIZE + IP6_HDR_SIZE + ICMP6_NA_SIZE];
    uint8_t dst_mac[ETH_ADDR_LEN];
    int n;

    if(len < ETH_FRAMESIZE + IP6_HDR_SIZE + 24)
        return 0;
    ip = frame + ETH_FRAMESIZE;
    if(((ip[0] >> 4) != 6) || (ip[6] != 58))
        return 0;
    icmp = ip + IP6_HDR_SIZE;
    if(icmp[0] != 135) /* Neighbor Solicitation */
        return 0;

    learn6(ip + 8, eth->shost, time(NULL));

    if(ip6_is_unspecified(ip + 8)) /* duplicate address detection */
        return 0;

    refresh_local_ip6();
    if(!local_ip6_owns(icmp + 8))
        return 0;

    if(eth->shost[0] & 0x01)
        ip6_multicast_mac(ip + 8, dst_mac);
    else
        memcpy(dst_mac, eth->shost, ETH_ADDR_LEN);

    n = build_na_frame(reply, dst_mac, icmp + 8, ip + 8, icmp + 8);
    arp_enqueue(reply, n);
    return 1;
}


/* ------------------------------------------------------------------------- */
/* Public tuntap API (see include/n2n.h) */

int tuntap_open(tuntap_dev *device, char *dev, const char *address_mode,
                char *device_ip, char *device_mask, const char *device_mac,
                int mtu, int metric) {
    wchar_t adapter_name_w[ADAPTER_NAME_MAX];
    wchar_t if_alias_w[256];
    NET_LUID luid;
    NET_IFINDEX if_index = 0;
    GUID guid;
    DWORD version;

    (void)address_mode;
    (void)metric;

    memset(device, 0, sizeof(*device));
    device->device_handle = INVALID_HANDLE_VALUE;

    if(g_adapter_name[0] == '\0') {
        if(normalize_adapter_name(dev, g_adapter_name, sizeof(g_adapter_name)) != 0) {
            traceEvent(TRACE_ERROR, "invalid wintun adapter name \"%s\"", (dev != NULL) ? dev : "");
            return -1;
        }
    } else {
        /* Re-open after a read error: n2n passes tuntap_dev_name, which is
         * only 64 bytes on Windows, so keep the original full name. */
        traceEvent(TRACE_DEBUG, "re-opening wintun adapter %s", g_adapter_name);
    }

    if(g_open)
        teardown_wintun();

    if(g_wintun == NULL) {
        g_wintun = load_wintun_dll();
        if(g_wintun == NULL) {
            traceEvent(TRACE_ERROR, "wintun.dll not found next to the executable [%lu]",
                       (unsigned long)GetLastError());
            return -1;
        }
        if(resolve_wintun_api() != 0) {
            FreeLibrary(g_wintun);
            g_wintun = NULL;
            return -1;
        }
    }

    /* WintunCreateAdapter installs the signed driver on first use, so the
     * driver version is only available after the adapter is created. */
    if(MultiByteToWideChar(CP_UTF8, 0, g_adapter_name, -1, adapter_name_w,
                           (int)(sizeof(adapter_name_w) / sizeof(adapter_name_w[0]))) <= 0) {
        traceEvent(TRACE_ERROR, "failed to convert adapter name to UTF-16");
        return -1;
    }

    derive_guid(g_adapter_name,
                g_guid_seed[0] != '\0' ? g_guid_seed : ((device_ip != NULL) ? device_ip : ""),
                &guid);

    g_adapter = fn_create_adapter(adapter_name_w, L"n2n", &guid);
    if(g_adapter == NULL) {
        DWORD err = GetLastError();
        traceEvent(TRACE_ERROR, "WintunCreateAdapter(\"%s\") failed [%lu]",
                   g_adapter_name, (unsigned long)err);
        if(err == ERROR_ALREADY_EXISTS)
            traceEvent(TRACE_ERROR, "adapter \"%s\" already exists; choose another -d",
                       g_adapter_name);
        return -1;
    }

    version = fn_get_running_driver_version();
    traceEvent(TRACE_NORMAL, "wintun driver version=%d.%d",
               (int)(version >> 16), (int)(version & 0xffff));

    g_session = fn_start_session(g_adapter, WIN_RING_CAPACITY);
    if(g_session == NULL) {
        traceEvent(TRACE_ERROR, "WintunStartSession failed [%lu]",
                   (unsigned long)GetLastError());
        teardown_wintun();
        return -1;
    }

    if(g_stop_event == NULL) {
        g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if(g_stop_event == NULL) {
            traceEvent(TRACE_ERROR, "CreateEvent for wintun stop failed [%lu]",
                       (unsigned long)GetLastError());
            teardown_wintun();
            return -1;
        }
    } else {
        ResetEvent(g_stop_event);
    }

    memset(&luid, 0, sizeof(luid));
    fn_get_adapter_luid(g_adapter, &luid);
    g_luid = luid;

    g_mtu = (uint16_t)((mtu > 0) ? mtu : 1400);
    if(configure_interface(luid, device_ip, device_mask, (int)g_mtu) != 0) {
        teardown_wintun();
        return -1;
    }

    if(ConvertInterfaceLuidToIndex(&luid, &if_index) != NO_ERROR)
        if_index = 0;
    if(ConvertInterfaceLuidToAlias(&luid, if_alias_w,
                                   (int)(sizeof(if_alias_w) / sizeof(if_alias_w[0]))) == NO_ERROR) {
        WideCharToMultiByte(CP_UTF8, 0, if_alias_w, -1, g_if_name,
                            (int)sizeof(g_if_name), NULL, NULL);
    } else {
        strncpy(g_if_name, g_adapter_name, sizeof(g_if_name) - 1);
        g_if_name[sizeof(g_if_name) - 1] = '\0';
    }

    if(device_mac != NULL && device_mac[0] != '\0') {
        str2mac(g_mac, device_mac);
    } else {
        derive_mac(g_adapter_name, g_guid_seed,
                   device_ip != NULL ? device_ip : "");
    }

    g_local_ip = (device_ip != NULL && device_ip[0] != '\0') ? inet_addr(device_ip) : 0;
    g_local_mask = (device_mask != NULL && device_mask[0] != '\0') ? inet_addr(device_mask) : 0;
    g_open = 1;

    if(g_ipv6_enabled) {
        /* Windows assigns the link-local address itself; the shim only reads it
         * back so it can answer neighbour solicitations. */
        configure_interface6(luid, (int)g_mtu);
        refresh_local_ip6();
        traceEvent(TRACE_NORMAL, "IPv6 enabled on %s: %d local address(es) visible",
                   g_if_name, g_local_ip6_count);
    }

    memcpy(device->mac_addr, g_mac, sizeof(g_mac));
    device->ip_addr = g_local_ip;
    device->device_mask = g_local_mask;
    device->mtu = g_mtu;
    device->device_handle = (HANDLE)g_adapter;
    device->device_name = _strdup(g_adapter_name);
    device->ifName = _strdup(g_if_name);
    device->if_idx = (int)if_index;
    device->metric = 0;

    traceEvent(TRACE_NORMAL,
               "wintun adapter=%s if=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
               g_adapter_name, g_if_name,
               g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    queue_gratuitous_arp();
    return 0;
}

int tuntap_read(tuntap_dev *tuntap, unsigned char *buf, int len) {
    int n;

    (void)tuntap;

    if(g_session == NULL || g_stop_event == NULL)
        return -1;

    n = arp_dequeue(buf, len);
    if(n > 0)
        return n;

    for(;;) {
        HANDLE events[2];
        DWORD wait;
        DWORD packet_size = 0;
        BYTE *packet;
        uint8_t version;
        uint32_t dst_ip;
        uint8_t dst_mac[ETH_ADDR_LEN];
        ether_hdr_t *eth;
        time_t now;

        if(WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0)
            return 0;

        /* wintun requires polling WintunReceivePacket first and only waiting
         * on the read-wait event when it reports ERROR_NO_MORE_ITEMS; the
         * event is auto-reset and may already be consumed by earlier packets,
         * so waiting first can stall while packets are still queued. */
        packet = fn_receive_packet(g_session, &packet_size);
        if(packet == NULL) {
            DWORD err = GetLastError();

            if(err == ERROR_HANDLE_EOF)
                return 0;
            if(err != ERROR_NO_MORE_ITEMS)
                return -1;

            events[0] = fn_get_read_wait_event(g_session);
            events[1] = g_stop_event;
            wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if(wait == WAIT_OBJECT_0 + 1)
                return 0;
            if(wait != WAIT_OBJECT_0)
                return -1;
            continue;
        }

        if(packet_size < 20 || (int)packet_size + ETH_FRAMESIZE > len) {
            fn_release_receive_packet(g_session, packet);
            continue;
        }

        version = (uint8_t)(packet[0] >> 4);
        if(version == 6) {
            uint8_t dst_ip6[16];

            if(!g_ipv6_enabled || packet_size < 40) {
                fn_release_receive_packet(g_session, packet);
                continue;
            }

            refresh_local_ip6();
            memcpy(dst_ip6, packet + 24, 16); /* IPv6 destination */
            now = time(NULL);

            eth = (ether_hdr_t *)buf;
            win_lock();
            if(ip6_is_multicast(dst_ip6)) {
                ip6_multicast_mac(dst_ip6, dst_mac);
            } else if(!learn6_lookup_locked(dst_ip6, dst_mac, now)) {
                /* No NDP entry yet: use the solicited-node multicast MAC so the
                 * first packet reaches every on-link IPv6 node. */
                dst_mac[0] = 0x33;
                dst_mac[1] = 0x33;
                dst_mac[2] = 0xff;
                dst_mac[3] = dst_ip6[13];
                dst_mac[4] = dst_ip6[14];
                dst_mac[5] = dst_ip6[15];
            }
            win_unlock();

            memcpy(eth->dhost, dst_mac, ETH_ADDR_LEN);
            memcpy(eth->shost, g_mac, ETH_ADDR_LEN);
            eth->type = htons(0x86DD);
            memcpy(buf + ETH_FRAMESIZE, packet, packet_size);
            fn_release_receive_packet(g_session, packet);

            return (int)packet_size + ETH_FRAMESIZE;
        }
        if(version != 4) {
            fn_release_receive_packet(g_session, packet);
            continue;
        }

        memcpy(&dst_ip, packet + IP4_DSTOFFSET, 4);
        now = time(NULL);

        eth = (ether_hdr_t *)buf;
        win_lock();
        if(!learn_lookup_locked(dst_ip, dst_mac, now) ||
           dst_ip == INADDR_BROADCAST || (ntohl(dst_ip) >> 28) == 0xE) {
            memset(dst_mac, 0xff, sizeof(dst_mac));
        }
        win_unlock();

        memcpy(eth->dhost, dst_mac, ETH_ADDR_LEN);
        memcpy(eth->shost, g_mac, ETH_ADDR_LEN);
        eth->type = htons(0x0800);
        memcpy(buf + ETH_FRAMESIZE, packet, packet_size);
        fn_release_receive_packet(g_session, packet);

        return (int)packet_size + ETH_FRAMESIZE;
    }
}

int tuntap_write(tuntap_dev *tuntap, unsigned char *buf, int len) {
    ether_hdr_t *eth;
    uint16_t type;
    time_t now;

    (void)tuntap;

    if(g_session == NULL)
        return -1;
    if(len < ETH_FRAMESIZE)
        return len;

    eth = (ether_hdr_t *)buf;
    type = ntohs(eth->type);

    if(type == 0x0806) {
        handle_arp_frame(buf, len);
        return len;
    }

    if(type != 0x0800 && type != 0x86DD) {
        /* Other L2 protocols are not carried by this shim. */
        return len;
    }

    if(type == 0x86DD && !g_ipv6_enabled)
        return len;

    /* Frames for another unicast MAC are not meant for this interface; the
     * multicast/broadcast bit is allowed (wintun accepts those as-is). */
    if(!(eth->dhost[0] & 0x01) && memcmp(eth->dhost, g_mac, ETH_ADDR_LEN) != 0)
        return len;

    if(type == 0x86DD) {
        /* Answer NDP requests for our own addresses; wintun is L3 and its
         * stack does not emit neighbour advertisements. */
        if(handle_ns_frame(buf, len))
            return len;
        if(len >= ETH_FRAMESIZE + IP6_HDR_SIZE) {
            const uint8_t *ip6 = buf + ETH_FRAMESIZE;
            if((ip6[0] >> 4) == 6)
                learn6(ip6 + 8, eth->shost, time(NULL));
        }
    } else if(len > ETH_FRAMESIZE + 19) {
        uint8_t version = (uint8_t)(buf[ETH_FRAMESIZE] >> 4);
        if(version == 4) {
            uint32_t src_ip;
            memcpy(&src_ip, buf + ETH_FRAMESIZE + 12, 4);
            now = time(NULL);
            learn(src_ip, eth->shost, now);
        }
    }

    {
        DWORD payload = (DWORD)(len - ETH_FRAMESIZE);
        BYTE *packet = fn_allocate_send_packet(g_session, payload);
        if(packet == NULL) {
            static time_t last_log;
            now = time(NULL);
            if(now != last_log) {
                last_log = now;
                traceEvent(TRACE_WARNING, "WintunAllocateSendPacket failed [%lu]",
                           (unsigned long)GetLastError());
            }
            return len;
        }
        memcpy(packet, buf + ETH_FRAMESIZE, payload);
        fn_send_packet(g_session, packet);
    }
    return len;
}

void tuntap_close(tuntap_dev *tuntap) {
    teardown_wintun();

    if(tuntap != NULL) {
        if(tuntap->device_name != NULL) {
            free(tuntap->device_name);
            tuntap->device_name = NULL;
        }
        if(tuntap->ifName != NULL) {
            free(tuntap->ifName);
            tuntap->ifName = NULL;
        }
        tuntap->device_handle = INVALID_HANDLE_VALUE;
    }
}

void tuntap_get_address(tuntap_dev *tuntap) {
    if(tuntap == NULL)
        return;
    tuntap->ip_addr = g_local_ip;
    tuntap->device_mask = g_local_mask;
}

/* ------------------------------------------------------------------------- */
/* Control hooks used by the cgo glue and n2n's win32 code */

void tuntap_interrupt(tuntap_dev *tuntap) {
    (void)tuntap;
    if(g_stop_event != NULL)
        SetEvent(g_stop_event);
}

const char *tuntap_win_driver_version(void) {
    static char buf[32];
    DWORD version;
    unsigned ver_major, ver_minor;

    if(g_wintun == NULL) {
        g_wintun = load_wintun_dll();
        if(g_wintun == NULL)
            return "";
        if(resolve_wintun_api() != 0) {
            FreeLibrary(g_wintun);
            g_wintun = NULL;
            return "";
        }
    }

    version = fn_get_running_driver_version();
    if(version == 0)
        return "not installed";
    ver_major = (unsigned)(version >> 16);
    ver_minor = (unsigned)(version & 0xffff);
    snprintf(buf, sizeof(buf), "%u.%u", ver_major, ver_minor);
    return buf;
}

void tuntap_win_set_community(const char *community) {
    g_guid_seed[0] = '\0';
    if(community == NULL)
        return;
    strncpy(g_guid_seed, community, sizeof(g_guid_seed) - 1);
    g_guid_seed[sizeof(g_guid_seed) - 1] = '\0';
}

void tuntap_win_enable_ipv6(int on) {
    g_ipv6_enabled = (on != 0);
}

static int g_wsa_started;

void initWin32(void) {
    WSADATA wsa;

    if(g_wsa_started)
        return;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        traceEvent(TRACE_ERROR, "WSAStartup failed [%lu]", (unsigned long)WSAGetLastError());
        return;
    }
    g_wsa_started = 1;
}

void destroyWin32(void) {
    if(g_wsa_started) {
        WSACleanup();
        g_wsa_started = 0;
    }
}

void win_print_available_adapters(void) {
    ULONG size = 0;
    PIP_ADAPTER_ADDRESSES addrs;
    PIP_ADAPTER_ADDRESSES cur;
    DWORD rc;

    rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER, NULL, NULL, &size);
    if(rc != ERROR_BUFFER_OVERFLOW)
        return;
    addrs = (PIP_ADAPTER_ADDRESSES)malloc(size);
    if(addrs == NULL)
        return;
    rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &size);
    if(rc == NO_ERROR) {
        for(cur = addrs; cur != NULL; cur = cur->Next) {
            char friendly[256];
            char description[256];

            WideCharToMultiByte(CP_UTF8, 0, cur->FriendlyName, -1,
                                friendly, sizeof(friendly), NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, cur->Description, -1,
                                description, sizeof(description), NULL, NULL);
            if(strstr(friendly, "Wintun") != NULL || strstr(description, "Wintun") != NULL ||
               strstr(friendly, "n2n") != NULL)
                traceEvent(TRACE_NORMAL, "adapter: %s (%s)", friendly, description);
        }
    }
    free(addrs);
}

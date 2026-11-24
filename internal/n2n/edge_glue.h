#ifndef N2N_GO_EDGE_GLUE_H
#define N2N_GO_EDGE_GLUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle owned by Go. */
typedef struct n2n_go_edge n2n_go_edge_t;

#define N2N_GO_MAX_SUPERNODES 8
#define N2N_GO_ERRBUF_SIZE    512

/* Mirrors internal/config.EdgeConfig. All strings are NUL-terminated UTF-8. */
typedef struct n2n_go_config {
    const char  *community;
    const char  *key;
    const char **supernodes;
    int          sn_num;

    const char  *ip;   /* dotted IPv4, e.g. 10.0.0.1; NULL/empty when auto */
    const char  *mask; /* dotted IPv4 netmask, e.g. 255.255.255.0; NULL/empty when auto */
    int          auto_address; /* non-zero: SN_ASSIGN, address comes from the supernode */
    int          ipv6;         /* non-zero: carry IPv6 over the tunnel (--ipv6) */

    const char  *device; /* macOS: utun/utunN; Linux: tapN or ""; Windows: wintun name */
    const char  *mac;    /* NULL/empty: derive (Windows) or generate (macOS/Linux) */

    int mtu;
    int transop_id;
    int compression;
    int local_port;
    int mgmt_port;
    int register_interval;
    int register_ttl;
    int allow_routing;
    int drop_multicast;
    int disable_pmtu_discovery;
    int tos;
    const char *description;
    int header_encryption;          /* non-zero enables -H header encryption */
    const char *federation_public_key; /* -P, ASCII encoded, may be NULL */
    const char *auth_password;         /* -J, may be NULL */

    /* runtime/cgo.Handle of the Go Edge, signalled via n2nGoOnStarted(). */
    uintptr_t started_handle;
} n2n_go_config_t;

/* Creates the edge, initializes n2n and opens the TUN/TAP device.
 * Returns NULL on failure and fills errbuf with a human readable message. */
n2n_go_edge_t *n2n_go_edge_create(const n2n_go_config_t *cfg,
                                 char *errbuf, int errbuf_len);

/* Blocking: runs the n2n edge loop until keep_running turns false.
 * Performs edge_term/edge_term_conf/tuntap_close before returning. */
int n2n_go_edge_run(n2n_go_edge_t *e);

/* Thread-safe/idempotent stop request; may be called before run(). */
void n2n_go_edge_stop(n2n_go_edge_t *e);

/* Applies the address assigned by the supernode to the utun device (SN_ASSIGN
 * mode only). Returns 1 once the device is configured (or there is nothing to
 * do because the loop has finished), 0 while the assignment has not arrived
 * yet, -1 on failure. Call it repeatedly from another thread/goroutine while
 * n2n_go_edge_run() is blocked. */
int n2n_go_edge_configure_auto(n2n_go_edge_t *e);

/* Frees the handle (and cleans up if run() was never called). */
void n2n_go_edge_destroy(n2n_go_edge_t *e);

/* n2n PACKAGE_VERSION as configured at build time. */
const char *n2n_go_version(void);

/* Device name actually opened (e.g. kernel-assigned tapN); NULL if not open. */
const char *n2n_go_edge_device_name(n2n_go_edge_t *e);

/* Running wintun driver version on Windows; "" on other platforms or when the
 * DLL is missing. Used by `n2n version`. */
const char *n2n_go_wintun_version(void);

#ifdef __cplusplus
}
#endif

#endif /* N2N_GO_EDGE_GLUE_H */

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

    const char  *ip;   /* dotted IPv4, e.g. 10.0.0.1 */
    const char  *mask; /* dotted IPv4 netmask, e.g. 255.255.255.0 */

    const char  *device; /* "utun" or "utunN" */
    const char  *mac;

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

/* Creates the edge, initializes n2n and opens the utun device.
 * Returns NULL on failure and fills errbuf with a human readable message. */
n2n_go_edge_t *n2n_go_edge_create(const n2n_go_config_t *cfg,
                                 char *errbuf, int errbuf_len);

/* Blocking: runs the n2n edge loop until keep_running turns false.
 * Performs edge_term/edge_term_conf/tuntap_close before returning. */
int n2n_go_edge_run(n2n_go_edge_t *e);

/* Thread-safe/idempotent stop request; may be called before run(). */
void n2n_go_edge_stop(n2n_go_edge_t *e);

/* Frees the handle (and cleans up if run() was never called). */
void n2n_go_edge_destroy(n2n_go_edge_t *e);

/* n2n PACKAGE_VERSION as configured at build time. */
const char *n2n_go_version(void);

#ifdef __cplusplus
}
#endif

#endif /* N2N_GO_EDGE_GLUE_H */

/**
 * cgo adapter around n2n 3.1.1's edge API.
 * The Go side owns the handle lifecycle; this file only maps configuration and
 * performs the same init/run/term sequence as n2n's own src/edge.c.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "n2n.h"
#include "auth.h"
#include "edge_glue.h"

/* Provided by cgo (//export n2nGoOnStarted). */
extern void n2nGoOnStarted(uintptr_t handle);

#ifdef __APPLE__
/* Implemented in platform/macos/tuntap_osx.c. */
extern void tuntap_osx_bind_edge(n2n_edge_t *eee);
extern void tuntap_osx_enable_ipv6(int on);
extern int  tuntap_osx_configure(tuntap_dev *device, const char *device_ip,
                                 const char *device_mask, int mtu);
#endif

struct n2n_go_edge {
    n2n_edge_conf_t conf;
    n2n_edge_t     *eee;
    tuntap_dev      dev;

    bool            keep_running;
    volatile int    stop_requested;
    uintptr_t       started_handle;

    int             conf_ready;
    int             run_finished;
};

static void set_err(char *buf, int len, const char *msg) {
    if((buf != NULL) && (len > 0))
        snprintf(buf, (size_t)len, "%s", msg);
}

/* Send n2n's trace output to stderr without letting closeTraceFile() close the
 * process-wide stderr (edge_term() calls closeTraceFile()). Re-running this for
 * every edge also repairs the dangling traceFile pointer left by edge_term(). */
static void init_trace(void) {
    int    fd;
    FILE  *f;

    fd = dup(STDERR_FILENO);
    if(fd < 0)
        return;
    f = fdopen(fd, "w");
    if(f == NULL) {
        close(fd);
        return;
    }
    setTraceFile(f);
}

/* Frees whatever create() managed to allocate before failing. */
static void cleanup(n2n_go_edge_t *e) {
    if(e == NULL)
        return;

    /* edge_term_conf() does not release these key buffers, so they are owned
     * and freed here (n2n's own edge.c leaks them until process exit). */
    if(e->conf.shared_secret != NULL) {
        free(e->conf.shared_secret);
        e->conf.shared_secret = NULL;
    }
    if(e->conf.federation_public_key != NULL) {
        free(e->conf.federation_public_key);
        e->conf.federation_public_key = NULL;
    }

    if(e->eee != NULL) {
        edge_term_conf(&e->conf);
        tuntap_close(&e->dev);
        edge_term(e->eee);
        e->eee = NULL;
        e->conf_ready = 0;
        return;
    }

    if(e->conf_ready) {
        edge_term_conf(&e->conf);
        clear_peer_list(&e->conf.supernodes);
        e->conf_ready = 0;
    }
    if(e->dev.fd >= 0)
        tuntap_close(&e->dev);
}

n2n_go_edge_t *n2n_go_edge_create(const n2n_go_config_t *cfg, char *errbuf, int errbuf_len) {
    n2n_go_edge_t *e;
    size_t         clen;
    int            rc;
    int            i;
    const char    *ip;
    const char    *mask;

    if((cfg == NULL) || (cfg->community == NULL) || (cfg->community[0] == '\0') ||
       (cfg->sn_num < 1) || (cfg->supernodes == NULL)) {
        set_err(errbuf, errbuf_len, "invalid edge configuration");
        return NULL;
    }

    /* In SN_ASSIGN mode the address is unknown until the supernode answers, so
     * empty strings are legitimate there (open first, configure later). */
    ip = (cfg->ip != NULL) ? cfg->ip : "";
    mask = (cfg->mask != NULL) ? cfg->mask : "";
    if(!cfg->auto_address && ((ip[0] == '\0') || (mask[0] == '\0'))) {
        set_err(errbuf, errbuf_len, "ip and netmask are required unless -a auto is used");
        return NULL;
    }

    e = calloc(1, sizeof(*e));
    if(e == NULL) {
        set_err(errbuf, errbuf_len, "out of memory");
        return NULL;
    }
    e->dev.fd = -1;
    e->keep_running = 1;
    e->started_handle = cfg->started_handle;

    init_trace();

    edge_init_conf_defaults(&e->conf);
    e->conf_ready = 1;

    clen = strlen(cfg->community);
    if(clen >= N2N_COMMUNITY_SIZE) {
        set_err(errbuf, errbuf_len, "community is too long");
        cleanup(e);
        free(e);
        return NULL;
    }
    memset(e->conf.community_name, 0, sizeof(e->conf.community_name));
    memcpy(e->conf.community_name, cfg->community, clen);

    if(e->conf.encrypt_key != NULL) { /* possibly set from N2N_KEY */
        free(e->conf.encrypt_key);
        e->conf.encrypt_key = NULL;
    }
    if((cfg->key != NULL) && (cfg->key[0] != '\0')) {
        e->conf.encrypt_key = strdup(cfg->key);
        if(e->conf.encrypt_key == NULL) {
            set_err(errbuf, errbuf_len, "out of memory duplicating the encryption key");
            cleanup(e);
            free(e);
            return NULL;
        }
    }

    e->conf.transop_id = (n2n_transform_t)cfg->transop_id;
    e->conf.compression = (uint8_t)cfg->compression;
    if(cfg->header_encryption)
        e->conf.header_encryption = HEADER_ENCRYPTION_ENABLED;

    if((cfg->federation_public_key != NULL) && (cfg->federation_public_key[0] != '\0')) {
        e->conf.federation_public_key = calloc(1, sizeof(n2n_private_public_key_t));
        if(e->conf.federation_public_key == NULL) {
            set_err(errbuf, errbuf_len, "out of memory allocating the federation public key");
            cleanup(e);
            free(e);
            return NULL;
        }
        if(ascii_to_bin(*(e->conf.federation_public_key), (char *)cfg->federation_public_key) < 0) {
            set_err(errbuf, errbuf_len, "invalid federation public key");
            cleanup(e);
            free(e);
            return NULL;
        }
    }
    if((cfg->auth_password != NULL) && (cfg->auth_password[0] != '\0')) {
        e->conf.shared_secret = calloc(1, sizeof(n2n_private_public_key_t));
        if(e->conf.shared_secret == NULL) {
            set_err(errbuf, errbuf_len, "out of memory allocating the authentication password");
            cleanup(e);
            free(e);
            return NULL;
        }
        generate_private_key(*(e->conf.shared_secret), (char *)cfg->auth_password);
    }

    e->conf.local_port = cfg->local_port;
    e->conf.mgmt_port = cfg->mgmt_port;
    e->conf.allow_routing = (uint8_t)(cfg->allow_routing ? 1 : 0);
    e->conf.drop_multicast = (uint8_t)(cfg->drop_multicast ? 1 : 0);
    e->conf.disable_pmtu_discovery = (uint8_t)(cfg->disable_pmtu_discovery ? 1 : 0);
    e->conf.tos = (uint8_t)cfg->tos;
    if(cfg->register_interval > 0)
        e->conf.register_interval = cfg->register_interval;
    if(cfg->register_ttl >= 0)
        e->conf.register_ttl = cfg->register_ttl;
    e->conf.tuntap_ip_mode = cfg->auto_address ? TUNTAP_IP_MODE_SN_ASSIGN : TUNTAP_IP_MODE_STATIC;

    if((cfg->description != NULL) && (cfg->description[0] != '\0'))
        strncpy((char *)e->conf.dev_desc, cfg->description, N2N_DESC_SIZE - 1);

    for(i = 0; i < cfg->sn_num; i++) {
        if(edge_conf_add_supernode(&e->conf, cfg->supernodes[i]) != 0) {
            set_err(errbuf, errbuf_len, "failed to add supernode (invalid host:port)");
            cleanup(e);
            free(e);
            return NULL;
        }
    }

    rc = edge_verify_conf(&e->conf);
    if(rc != 0) {
        switch(rc) {
            case -1: set_err(errbuf, errbuf_len, "community must not be empty"); break;
            case -2:
            case -5: set_err(errbuf, errbuf_len, "no usable supernode"); break;
            case -3: set_err(errbuf, errbuf_len, "registration interval must be >= 1"); break;
            case -4: set_err(errbuf, errbuf_len, "cipher and encryption key mismatch"); break;
            default: set_err(errbuf, errbuf_len, "invalid edge configuration (edge_verify_conf)"); break;
        }
        cleanup(e);
        free(e);
        return NULL;
    }

    e->eee = edge_init(&e->conf, &rc);
    if(e->eee == NULL) {
        set_err(errbuf, errbuf_len, "edge_init failed");
        cleanup(e);
        free(e);
        return NULL;
    }

#ifdef __APPLE__
    tuntap_osx_enable_ipv6(cfg->ipv6);
#endif

    if(tuntap_open(&e->dev, (char *)((cfg->device != NULL) ? cfg->device : "utun"),
                   cfg->auto_address ? "sn_assign" : "static",
                   (char *)ip, (char *)mask, cfg->mac, cfg->mtu, 0) < 0) {
        set_err(errbuf, errbuf_len, "failed to open utun device (root privileges required?)");
        cleanup(e);
        free(e);
        return NULL;
    }
    memcpy(&e->eee->device, &e->dev, sizeof(e->dev));
#ifdef __APPLE__
    /* Let tuntap_read resolve unicast peer MACs from the edge's peer lists. */
    tuntap_osx_bind_edge(e->eee);
#endif

    /* n2n re-opens the device from tuntap_priv_conf after a read error. */
    memset(&e->eee->tuntap_priv_conf, 0, sizeof(e->eee->tuntap_priv_conf));
    strncpy(e->eee->tuntap_priv_conf.tuntap_dev_name, e->dev.dev_name,
            sizeof(e->eee->tuntap_priv_conf.tuntap_dev_name) - 1);
    strncpy(e->eee->tuntap_priv_conf.ip_mode, cfg->auto_address ? "sn_assign" : "static",
            sizeof(e->eee->tuntap_priv_conf.ip_mode) - 1);
    strncpy(e->eee->tuntap_priv_conf.ip_addr, ip,
            sizeof(e->eee->tuntap_priv_conf.ip_addr) - 1);
    strncpy(e->eee->tuntap_priv_conf.netmask, mask,
            sizeof(e->eee->tuntap_priv_conf.netmask) - 1);
    if(cfg->mac != NULL)
        strncpy(e->eee->tuntap_priv_conf.device_mac, cfg->mac,
                sizeof(e->eee->tuntap_priv_conf.device_mac) - 1);
    e->eee->tuntap_priv_conf.mtu = cfg->mtu;
    e->eee->tuntap_priv_conf.metric = 0;
    e->eee->tuntap_priv_conf.userid = 0;
    e->eee->tuntap_priv_conf.groupid = 0;

    return e;
}

int n2n_go_edge_run(n2n_go_edge_t *e) {
    int rc;

    if((e == NULL) || (e->eee == NULL))
        return -1;

    e->eee->keep_running = &e->keep_running;
    if(e->stop_requested)
        e->keep_running = 0;

    n2nGoOnStarted(e->started_handle);

    rc = run_edge_loop(e->eee);

    cleanup(e);
    e->run_finished = 1;
    return rc;
}

void n2n_go_edge_stop(n2n_go_edge_t *e) {
    if(e == NULL)
        return;
    e->stop_requested = 1;
    e->keep_running = 0;
}

int n2n_go_edge_configure_auto(n2n_go_edge_t *e) {
    const char *ip;
    const char *mask;

    if((e == NULL) || (e->eee == NULL))
        return 1; /* the loop already finished: nothing left to configure */
    if(e->eee->conf.tuntap_ip_mode != TUNTAP_IP_MODE_SN_ASSIGN)
        return 1; /* static address, configured by tuntap_open */
    if(e->eee->device.ip_addr != 0)
        return 1; /* assignment already applied */

    /* The REGISTER_SUPER_ACK handler stores the assignment here. */
    ip = e->eee->tuntap_priv_conf.ip_addr;
    mask = e->eee->tuntap_priv_conf.netmask;
    if((ip[0] == '\0') || (mask[0] == '\0'))
        return 0; /* not assigned yet */

#ifdef __APPLE__
    if(tuntap_osx_configure(&e->dev, ip, mask, e->dev.mtu) < 0) {
        traceEvent(TRACE_ERROR, "failed to apply the supernode-assigned address %s/%s", ip, mask);
        return -1;
    }
#else
    return -1;
#endif

    /* send_register_super() reads the address from this copy, so it has to be
     * updated as well; otherwise every registration reports 0/0 and the
     * supernode keeps re-assigning. */
    e->eee->device.ip_addr = e->dev.ip_addr;
    e->eee->device.device_mask = e->dev.device_mask;

    traceEvent(TRACE_NORMAL, "supernode assigned address %s/%s, applied to %s",
               ip, mask, e->dev.dev_name);

    /* The supernode only learns the address from the next REGISTER_SUPER (it
     * does not record its own assignment), and until then it keeps the peer at
     * 0/0, so other edges that register meanwhile get the same address. Forcing
     * an immediate re-registration closes that window. */
    e->eee->sn_wait = 2;

    return 1;
}

void n2n_go_edge_destroy(n2n_go_edge_t *e) {
    if(e == NULL)
        return;
    if(!e->run_finished)
        cleanup(e);
    free(e);
}

const char *n2n_go_version(void) {
    return PACKAGE_VERSION;
}

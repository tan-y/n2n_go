/*
 * Windows wintun device layer for n2n.
 *
 * This header is used by the cgo glue (internal/n2n/edge_glue.c) and by
 * platform/windows/tuntap_wintun.c, which replaces n2n's src/win32/wintap.c
 * at build time.
 */

#ifndef N2N_GO_TUNTAP_WINTUN_H
#define N2N_GO_TUNTAP_WINTUN_H

#ifdef _WIN32

#include "n2n.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wakes the tun read thread; called by n2n_go_edge_stop(). */
void tuntap_interrupt(struct tuntap_dev *tuntap);

/* Sets the seed used to derive the adapter GUID and the virtual MAC.
 * Must be called before tuntap_open(); the pointer is copied. */
void tuntap_win_set_community(const char *community);

/* Lists adapters whose description mentions Wintun, for diagnostics. */
void win_print_available_adapters(void);

/* Running wintun driver version as "major.minor"; "" when the DLL is missing
 * and "not installed" when the driver is not loaded. */
const char *tuntap_win_driver_version(void);

/* Provided by this file as well because n2n's win32 build expects them from
 * wintap.c; the Go glue calls initWin32() once. */
void initWin32(void);
void destroyWin32(void);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* N2N_GO_TUNTAP_WINTUN_H */

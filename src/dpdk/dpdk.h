//src/dpdk/dpdk.h

#ifndef DPDK_IFACE_H
#define DPDK_IFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize DPDK. eal_args is a NULL-terminated array of strings (argv style). 
 * Return 0 on success, <0 on error.
 */
int dpdk_init(int argc, char **argv, unsigned port_id);

/* Send a prepared packet in `buf` of `len` bytes.
 * Return number of packets enqueued (1) or negative on error.
 */
int dpdk_send(const uint8_t *buf, uint16_t len);

/* Poll for received packets. For each packet, call the supplied callback:
 *   void recv_cb(const uint8_t *pkt, uint16_t len, void *user)
 *
 * Return number of packets processed or negative on error.
 */
typedef void (*dpdk_recv_cb)(const uint8_t *, uint16_t, void *);

int dpdk_poll_recv(dpdk_recv_cb cb, void *user);

/* Graceful shutdown */
void dpdk_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* DPDK_IFACE_H */
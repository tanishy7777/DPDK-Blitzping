//
// Created by sriv on 7/11/25.
//

//src/dpdk/dpdk.c
//
//        Minimal, skeleton DPDK glue. Fill with real logic.
// You will need to include dpdk headers and link against DPDK.
#include "dpdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

static struct rte_mempool *mbuf_pool = NULL;
static uint16_t dpdk_port_id = 0;

int dpdk_init(int argc, char **argv, unsigned port_id) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "rte_eal_init failed\n");
        return -1;
    }
    dpdk_port_id = (uint16_t)port_id;

    // create mempool
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", 8192, 256, 0,
                                        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        fprintf(stderr, "rte_pktmbuf_pool_create failed\n");
        return -2;
    }

    // configure port
    struct rte_eth_conf port_conf = { .rxmode = { .max_rx_pkt_len = RTE_ETHER_MAX_LEN } };
    if (rte_eth_dev_configure(dpdk_port_id, 1, 1, &port_conf) < 0) {
        fprintf(stderr, "rte_eth_dev_configure failed\n");
        return -3;
    }

    if (rte_eth_rx_queue_setup(dpdk_port_id, 0, 1024, rte_eth_dev_socket_id(dpdk_port_id), NULL, mbuf_pool) < 0) {
        fprintf(stderr, "rte_eth_rx_queue_setup failed\n");
        return -4;
    }
    if (rte_eth_tx_queue_setup(dpdk_port_id, 0, 1024, rte_eth_dev_socket_id(dpdk_port_id), NULL) < 0) {
        fprintf(stderr, "rte_eth_tx_queue_setup failed\n");
        return -5;
    }

    if (rte_eth_dev_start(dpdk_port_id) < 0) {
        fprintf(stderr, "rte_eth_dev_start failed\n");
        return -6;
    }

    // optionally enable promiscuous
    rte_eth_promiscuous_enable(dpdk_port_id);
    return 0;
}

int dpdk_send(const uint8_t *buf, uint16_t len) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
    if (!m) return -1;
    char *pkt = rte_pktmbuf_mtod(m, char *);
    rte_memcpy(pkt, buf, len);
    m->pkt_len = len;
    m->data_len = len;

    uint16_t sent = rte_eth_tx_burst(dpdk_port_id, 0, &m, 1);
    if (sent == 0) {
        rte_pktmbuf_free(m);
        return -2;
    }
    return (int)sent;
}

int dpdk_poll_recv(dpdk_recv_cb cb, void *user) {
    struct rte_mbuf *pkts[32];
    uint16_t nb = rte_eth_rx_burst(dpdk_port_id, 0, pkts, 32);
    for (uint16_t i = 0; i < nb; ++i) {
        struct rte_mbuf *m = pkts[i];
        uint8_t *pkt = rte_pktmbuf_mtod(m, uint8_t *);
        uint16_t pkt_len = rte_pktmbuf_pkt_len(m);
        // call user callback
        cb(pkt, pkt_len, user);
        rte_pktmbuf_free(m);
    }
    return (int)nb;
}

void dpdk_shutdown(void) {
    rte_eth_dev_stop(dpdk_port_id);
    rte_eth_dev_close(dpdk_port_id);
    // mempool freed by DPDK on exit
}
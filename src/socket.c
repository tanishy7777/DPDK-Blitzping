// ---------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
// socket.c is a part of Blitzping.
// ---------------------------------------------------------------------


#include "socket.h"

#include <rte_ether.h>
#include "./netlib/netinet.h"

#if defined(_POSIX_C_SOURCE)

// NOTE: Document somewhere that raw sockets would require
// enabling the "Mirrored" networking mode for WSL2.
int setup_posix_socket(const bool is_raw, const bool is_async) {
    // Setting the 'errno' flag to 0 indicates "no errors" so
    // that a previously set value does not affect us.
    errno = 0;

    int sock_opts = 0;
    sock_opts |= is_raw ? SOCK_RAW : SOCK_STREAM;

    // Attempt to create the specified socket.
    // https://stackoverflow.com/questions/49309029
    // TODO: Kernel fills-in IPs whenever they are zero'ed; find
    // a way to actually let the 0 pass unchanged.
    // TODO: see if pf_packet + bind() is faster
    // TODO: MSG_DONTROUTE?
    // TODO: MSG_OOB  and out-of-bound?
    int socket_descriptor = socket(
        AF_INET,    // Domain
        sock_opts,  // Type (+ options)
        IPPROTO_RAW // Protocol (implies IP_HDRINCL)
    );

	if (socket_descriptor == -1) {
		// Socket creation failed (maybe non-root privileges?)
        logger(LOG_CRIT,
                "Failed to create POSIX socket: %s", strerror(errno));
        return -1;
	}

    // NOTE: This, under Linux, would have been a one-liner:
    //     sock_opts |= is_async ? SOCK_NONBLOCK : 0;
    // Unfortunately, that is a Linux-only and non-POSIX-compliant way;
    // POSIX 2001 requires using O_NONBLOCK with fcntl() instead.
    if (is_async) {
        // Get the current flags for the socket
        int flags = fcntl(socket_descriptor, F_GETFL, 0);
        if (flags == -1) {
            logger(LOG_ERROR,
                "Failed to get socket flags: %s", strerror(errno));
            return -1;
        }

        flags |= O_NONBLOCK;

        int status = fcntl(socket_descriptor, F_SETFL, flags);
        if (status == -1) {
            logger(LOG_ERROR,
                "Failed to set socket to asynchronous mode: %s",
                strerror(errno));
            return -1;
        }
    }


    return socket_descriptor;
}

#   if defined(__linux__)

// AF_PACKET + SOCK_RAW is the "lowest" you can go in terms of raw
// sockets; they're also known as a packet-socket.  However, unlike
// the above function, they aren't POSIX-compliant.
//     The most important advantage of packet-sockets is that they
// let you memory-map (mmap) them using shared ring buffers, bypassing
// most kernel layers and removing some of the overhead of syscalls:
//     https://stackoverflow.com/questions/49309029
//     https://docs.kernel.org/networking/packet_mmap.html
//     https://blog.cloudflare.com/kernel-bypass/
//     https://stackoverflow.com/questions/4873956/
int setup_mmap_socket(const char *const interface_name) {
    // NOTE: A protocol of 0 means we only want to transmit packets via
    // this socket; this will avoid expensive syscalls to packet_rcv().
    const int socket_descriptor = socket(
        AF_PACKET,
        SOCK_RAW,
        0
    );
    if (socket_descriptor == -1) {
        logger(LOG_CRIT,
            "Failed to create packet-socket: %s", strerror(errno));
        return -1;
    }

    // Identifies the link-layer "address" and protocol:
    //     https://stackoverflow.com/questions/70995951/
    // TODO: On systems with multiple network interfaces, see if
    // passing 0 to sll_ifindex improves performance.
    const struct sockaddr_ll socket_address = {
        .sll_family = AF_PACKET,
        .sll_protocol = 0,
        .sll_ifindex = if_nametoindex(interface_name)
    };
    if (socket_address.sll_ifindex == 0) {
        logger(LOG_CRIT,
            "Failed to get interface index: %s", strerror(errno));
        return -1;
    }

    // Bind the socket
    const int bind_status = bind(
        socket_descriptor,
        (struct sockaddr*)&socket_address,
        sizeof(socket_address)
    );
    if (bind_status == -1) {
        logger(LOG_ERROR,
            "Failed to bind the socket: %s", strerror(errno));
        return -1;
    }

    // Set up the PACKET_TX_RING option
    // TODO: Make these configurable and also calculate maximums
    // https://www.kernel.org/doc/Documentation/networking/pktgen.txt
    // https://www.reddit.com/r/golang/comments/1bcexhp/
    const struct tpacket_req ring_buffer_cfg = {
        .tp_block_size = 4096,
        .tp_block_nr = 64,
        .tp_frame_size = 4096,
        .tp_frame_nr = 64
    };
    const int opt_status = setsockopt(
        socket_descriptor,
        SOL_PACKET,
        PACKET_TX_RING,
        &ring_buffer_cfg,
        sizeof(ring_buffer_cfg)
    );
    if (opt_status == -1) {
        logger(LOG_ERROR,
            "Failed to set PACKET_TX_RING option: %s", strerror(errno));
        return -1;
    }

    // Memory-map the ring buffer to user-space
    const void *const map = mmap(
        NULL,
        ring_buffer_cfg.tp_block_size * ring_buffer_cfg.tp_block_nr,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        socket_descriptor,
        0
    );
    if (map == MAP_FAILED) {
        logger(LOG_ERROR,
            "Failed to memory-map packet-socket's ring buffer: %s",
            strerror(errno)
        );
        return -1;
    }

    // TODO: PACKET_QDISC_BYPASS seems very promising  (kernel 3.14+)
    // TODO: MSG_ZEROCOPY vs. af_packet?
    
    // The socket is now ready to use with the mapped buffer
    return socket_descriptor;
}

// TODO: Investigate AF_XDP, as it appears to have the potential to
// be faster than packet-sockets, but it might also require loading
// BPF objects into kernel and/or having specific models of NICs. (?)
// Other than running directly on the NIC (where very few smartNICs
// even support this capability), you can also hook the XDP in the
// driver itself (again, without widespreads upport); finally,
// you may also run the XDP in a "generic" SKB mode, which seems to
// defeat its performance benefits and also prevent zero-copy'ing.
//     https://www.youtube.com/watch?v=hO2tlxURXJ0
//     https://qmonnet.github.io/whirl-offload/2016/09/01/dive-into-bpf
//     https://github.com/xdp-project/
//         xdp-project/blob/master/areas/drivers/README.org
//     https://pantheon.tech/what-is-af_xdp
//     https://stackoverflow.com/questions/78990613/
//     https://forum.suricata.io/t/
//         difference-between-af-packet-mode-and-af-xdp-mode/4754
//     https://stackoverflow.com/questions/
//     https://blog.cloudflare.com/
//         a-story-about-af-xdp-network-namespaces-and-a-cookie/
//     https://blog.freifunk.net/2024/05/31/gsoc-2024-ebpf-
//         performance-optimizations-for-a-new-openwrt-firewall/
//     https://toonk.io/building-an-xdp-express-data-path-
//         based-bgp-peering-router/index.html
//     https://www.netdevconf.org/0x14/pub/slides/37/
//         Adding%20AF_XDP%20zero-copy%20support%20to%20drivers.pdf
// Also, it appears that AF_XDP is the "successor" to AF_PACKET v3;
// AF_PACKET v4 never seems to have taken off (?):
//     https://lore.kernel.org/netdev/
//         95aaafdc-ef8a-c4b9-6104-a1a753c81820@intel.com/
//     https://lwn.net/Articles/737947/
//     https://www.netdevconf.info/2.2/slides/karlsson-afpacket-talk.pdf
//     https://www.youtube.com/watch?v=RSFX7z1qF2g
//
// Ultimately, it appears that the "fastest" method is to write our
// own driver for a specific NIC, but that is obviously not portable.
// (DPDK, VPP, libpcap, etc. do exactly this for a handful of NICs.)

#   endif /* defined(__linux__) */

#endif /* defined(_POSIX_C_SOURCE) */

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static struct rte_mempool *mbuf_pool = NULL;

int setup_dpdk_environment(int argc, char **argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        logger(LOG_CRIT, "Failed to initialize DPDK EAL");
        return -1;
    }

    logger(LOG_INFO, "DPDK: Setting up");

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * rte_eth_dev_count_avail(),
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                        rte_socket_id());
    if (!mbuf_pool) {
        logger(LOG_CRIT, "Failed to create mbuf pool");
        return -1;
    }
    return ret;
}

int setup_dpdk_socket(uint16_t port_id) {
    struct rte_eth_conf port_conf = {
            .txmode = {
                    .offloads =
                    RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                    RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
            },
    };

    const uint16_t nb_rxd = 128;
    const uint16_t nb_txd = 512;

    int ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret < 0) {
        logger(LOG_CRIT, "DPDK: device configure failed");
        return ret;
    }

    rte_eth_rx_queue_setup(port_id, 0, nb_rxd, rte_eth_dev_socket_id(port_id), NULL, mbuf_pool);
    rte_eth_tx_queue_setup(port_id, 0, nb_txd, rte_eth_dev_socket_id(port_id), NULL);

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        logger(LOG_CRIT, "DPDK: device start failed");
        return ret;
    }

    rte_eth_dev_set_link_up(port_id);
    logger(LOG_INFO, "DPDK: Port %u started", port_id);
    return ret;
}

//int dpdk_send_packet(uint16_t port_id, const void *pkt_data, size_t len) {
//    struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
//    if (!m) return -1;
//
//    void *dst = rte_pktmbuf_append(m, len);
//    memcpy(dst, pkt_data, len);
//
//    uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &m, 1);
//    if (nb_tx == 0) {
//        rte_pktmbuf_free(m);
//        logger(LOG_INFO, "DPDK: Packet Sending Failed");
//        return -1;
//    }
//    return 0;
//}

#include <inttypes.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>


void dump_port_and_mbuf_info(uint16_t port_id, struct rte_mempool *mp) {
    struct rte_eth_link link;
    int ret;

    ret = rte_eth_link_get_nowait(port_id, &link);
    if (ret == 0) {
        logger(LOG_INFO,
               "DPDK: port=%u link_up=%d speed=%u duplex=%u",
               port_id, link.link_status, link.link_speed, link.link_duplex);
    } else {
        logger(LOG_WARN,
               "DPDK: port=%u rte_eth_link_get_nowait failed (ret=%d)",
               port_id, ret);
    }

    struct rte_eth_dev_info dev_info;
    ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret == 0) {
        logger(LOG_INFO,
               "DPDK: dev_info: max_tx_queues=%u max_rx_queues=%u tx_offload_capa=0x%" PRIx64,
                dev_info.max_tx_queues,
                dev_info.max_rx_queues,
                dev_info.tx_offload_capa);
    } else {
        logger(LOG_WARN,
               "DPDK: port=%u rte_eth_dev_info_get failed (ret=%d)",
               port_id, ret);
    }

    struct rte_eth_stats stats;
    ret = rte_eth_stats_get(port_id, &stats);
    if (ret == 0) {
        logger(LOG_INFO,
               "DPDK: stats: ipackets=%" PRIu64 " opackets=%" PRIu64
        " ibytes=%" PRIu64 " obytes=%" PRIu64
        " imissed=%" PRIu64 " oerrors=%" PRIu64,
                stats.ipackets, stats.opackets,
                stats.ibytes, stats.obytes,
                stats.imissed, stats.oerrors);
    } else {
        logger(LOG_WARN,
               "DPDK: port=%u rte_eth_stats_get failed (ret=%d)",
               port_id, ret);
    }

    if (mp) {
        unsigned int data_room = rte_pktmbuf_data_room_size(mp);
        logger(LOG_INFO,
               "DPDK: mempool name=%s size=%u eltsz=%u cache=%u data_room=%u headroom=%u tailroom=%u",
               mp->name,
               (unsigned)mp->size,
               (unsigned)mp->elt_size,
               (unsigned)mp->cache_size,
               data_room,
               RTE_PKTMBUF_HEADROOM,
               data_room - RTE_PKTMBUF_HEADROOM);
    } else {
        logger(LOG_WARN, "DPDK: mempool pointer is NULL");
    }
}

static int ctr_Fail = 0;
static int ctr_sent = 0;
static double total_send_time_us = 0.0;

int dpdk_send_packet(uint16_t port_id, const void *pkt_data, size_t len) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
//    dump_port_and_mbuf_info(port_id, mbuf_pool);
    if (m == NULL) {
        logger(LOG_ERROR, "DPDK: mbuf alloc failed");
        return -1;
    }

    void *dst = rte_pktmbuf_append(m, len);
    if (dst == NULL) {
        // Not enough tailroom in single mbuf
        rte_pktmbuf_free(m);
        logger(LOG_ERROR, "DPDK: not enough tailroom for len=%zu", len);
        return -1;
    }

    memcpy(dst, pkt_data, len);

    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = sizeof(struct ip_hdr);
    m->ol_flags = RTE_MBUF_F_TX_IPV4 |
                  RTE_MBUF_F_TX_IP_CKSUM |
                  RTE_MBUF_F_TX_UDP_CKSUM;

    struct rte_mbuf *bufs[1] = { m };

    // try to transmit once, optionally retry a few times (simple retry loop)
    uint64_t start_tsc = rte_get_tsc_cycles();
    uint16_t sent = rte_eth_tx_burst(port_id, 0, bufs, 1);
//    dump_port_and_mbuf_info(port_id, mbuf_pool);

    uint64_t end_tsc = rte_get_tsc_cycles();
    double elapsed_us = ((end_tsc - start_tsc) * 1e6) / rte_get_tsc_hz();

    total_send_time_us += elapsed_us;
    if (sent == 0) {
        // burst failed; free the mbuf
        rte_pktmbuf_free(m);
//        logger(LOG_INFO, "DPDK: Packet Sending Failed (nb_tx=0) %d", ctr_Fail);
        ctr_Fail++;
        return -1;
    }
    else{
//        logger(LOG_INFO, "DPDK: Packet Sent");
        ctr_sent++;
        if (ctr_sent % 10 == 0) {
            double avg_us = total_send_time_us / 10.0;
//            logger(LOG_INFO, "DPDK: avg send time per packet = %.2f µs (last 100 pkts)", avg_us);
            total_send_time_us = 0.0;
        }
    }

//    logger(LOG_INFO, "Failed: %d , Sent: %d",ctr_Fail,ctr_sent);

    // success (note: if you send more than one you must handle partial sends)
    return 0;
}

// ---------------------------------------------------------------------
// END OF FILE: socket.c
// ---------------------------------------------------------------------

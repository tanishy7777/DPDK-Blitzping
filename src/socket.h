// ---------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
// socket.h is a part of Blitzping.
// ---------------------------------------------------------------------


#pragma once
#ifndef SOCKET_H
#define SOCKET_H

#include "./cmdline/logger.h"

#include <string.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
// ---------------------------------------------------------------------
// DPDK mode (optional compile-time enable)
// ---------------------------------------------------------------------
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

#if defined(_POSIX_C_SOURCE)
#   include <fcntl.h>
#   include <sys/mman.h>
#   include <sys/socket.h>
#   include <arpa/inet.h>
#   include <net/if.h>
#   if defined(__linux__)
#       include <linux/if_packet.h>
#   endif
#elif defined(_WIN32)
//#include <winsock2.h>
#endif

// extern int errno; // Declared in <errno.h>
#include <errno.h>


int setup_posix_socket(const bool is_raw, const bool is_async);

int setup_dpdk_environment(int argc, char **argv);
int setup_dpdk_socket(uint16_t port_id);
int dpdk_send_packet(uint16_t port_id, const void *pkt_data, size_t len);
int dpdk_receive_packets(uint16_t port_id);

#endif // SOCKET_H

// ---------------------------------------------------------------------
// END OF FILE: socket.h
// ---------------------------------------------------------------------

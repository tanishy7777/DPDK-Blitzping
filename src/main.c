// ---------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Blitzping: Sending IP packets as fast as possible in userland.
// Copyright (C) 2024  Fereydoun Memarzanjany
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.
// ---------------------------------------------------------------------

#include <arpa/inet.h>

#include "./program.h"
#include "./cmdline/logger.h"
#include "./cmdline/parser.h"
#include "packet.h"
#include "socket.h"
#include "./netlib/netinet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>
// C11 threads (glibc >=2.28, musl >=1.1.5, Windows SDK >~10.0.22620)
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#   include <threads.h>
int __attribute__((weak)) thrd_create(
    thrd_t *thr, thrd_start_t func, void *arg
);
int __attribute__((weak)) thrd_join(
    thrd_t thr, int *res
);
#endif

#if defined(_POSIX_C_SOURCE)
#   include <unistd.h>
#   if defined(_POSIX_THREADS) && _POSIX_THREADS >= 0
#       include <pthread.h>
int __attribute__((weak)) pthread_create(
    pthread_t *thread, const pthread_attr_t *attr,
    void *(*start_routine) (void *), void *arg
);
int __attribute__((weak)) pthread_join(
    pthread_t thread, void **retval
);
#   endif
#elif defined(_WIN32)
//#
#endif



void diagnose_system(struct ProgramArgs *const program_args) {
    //bool checks_succeeded = true;

    program_args->diagnostics.runtime.endianness = check_endianness();
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    program_args->diagnostics.runtime.c11_threads =
        (&thrd_create != NULL) && (&thrd_join != NULL);
#endif
#if defined(_POSIX_THREADS) && _POSIX_THREADS >= 0
    program_args->diagnostics.runtime.posix_threads =
        (&pthread_create != NULL) && (&pthread_join != NULL);
#endif
    program_args->diagnostics.runtime.num_cores =
#if defined(_POSIX_C_SOURCE)
        sysconf(_SC_NPROCESSORS_ONLN);
#else
        0;
#endif

/*
// TODO: use capabilities to also verify privilage level in raw sockets
    // Check to see if the currently running machine's endianness
    // matches what was expected to be the target's endianness at
    // the time of compilation.
#if defined(__LITTLE_ENDIAN__)
    if (runtime_endianness != little_endian) {
        logger(LOG_ERROR,
            "Program was compiled for little endian,\n"
            "but this machine is somehow big endian!\n"
        );
#elif defined(__BIG_ENDIAN__)
    if (runtime_endianness != big_endian) {
        logger(LOG_ERROR,
            "Program was compiled for big endian,\n"
            "but this machine is somehow little endian!\n"
        );
#endif
        checks_succeeded = false;
    }

    if (!thrd_create || !thrd_join) {
        fprintf(stderr,
            "This program was compiled with C11 <threads.h>,\n"
            "but this system appears to lack thrd_create() or\n"
            "thrd_join(); this could be due to an old C library.\n"
            "try using \"--native-threads\" for POSIX/Win32\n"
            "threads or \"--num-threads=0\" to disable threading.\n"
        );
        return 1;
    }
    
    fprintf(stderr,
        "This program was compiled without C11 <threads.h>;\n"
        "try using \"--native-threads\" for POSIX/Win32\n"
        "threads or \"--num-threads=0\" to disable threading.\n"
    );
*/
    //return checks_succeeded;
}

void fill_defaults(struct ProgramArgs *const program_args) {
    // General
    program_args->general.logger_level = LOG_INFO;

    // Advanced
    program_args->advanced.num_threads =
        program_args->diagnostics.runtime.num_cores;

    // IPv4
    //
    // NOTE: Unfortunately, there is no POSIX-compliant way to
    // get the current interface's ip address; getifaddrs() is
    // not standardized.
    // TODO: Use unprivilaged sendto() as an alternative.
    *(program_args->ipv4) = (struct ip_hdr){
        .ver = 4,
        .ihl = 5,
        .ttl = 128,
        .proto = IP_PROTO_TCP,
        .len = htons(sizeof(struct ip_hdr) + sizeof(struct tcp_hdr)),
        .saddr.address = 0,
        .daddr.address = 0
    };
}

int main(int argc, char *argv[]) {
    struct ProgramArgs program_args = {0};
    struct ip_hdr *ipv4_header_args =
            (struct ip_hdr *)calloc(1, sizeof(struct ip_hdr));

    if (ipv4_header_args == NULL) {
        program_args.diagnostics.unrecoverable_error = true;
        logger(LOG_ERROR,
               "Failed to allocate memory for program arguments.");
        goto CLEANUP;
    }
    program_args.ipv4 = ipv4_header_args;

    bool dpdk_mode = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--use-dpdk") == 0) {
            dpdk_mode = true;
            break;
        }
    }

    int eal_args_consumed = 0;
    int socket_descriptor = -1;

    char *program_name = argv[0];

    if (dpdk_mode) {
        eal_args_consumed = setup_dpdk_environment(argc, argv);

        if (eal_args_consumed < 0) {
            logger(LOG_ERROR, "Failed to initialize DPDK EAL; see EAL errors above.");
            program_args.diagnostics.unrecoverable_error = true;
            goto CLEANUP;
        }

        int ret = rte_eth_macaddr_get(0, &program_args.port_mac); // 0 = port_id
        if (ret != 0) {
            logger(LOG_ERROR, "Failed to get MAC address for port 0: %s",
                   strerror(-ret));
            program_args.diagnostics.unrecoverable_error = true;
            goto CLEANUP;
        }
        char mac_str[18];
        rte_ether_format_addr(mac_str, sizeof(mac_str), &program_args.port_mac);
        logger(LOG_INFO, "DPDK: Port 0 MAC is %s", mac_str);
        logger(LOG_INFO, "Initializing DPDK environment...");

        // Adjust argc and argv for our application's parser
        int app_argc = argc - eal_args_consumed;
        char **app_argv = &argv[eal_args_consumed];
        app_argv[0] = program_name; // Put the program name back

        // Use these new values for the rest of main()
        argc = app_argc;
        argv = app_argv;
        // --- END OF CORRECTION ---

        logger(LOG_INFO, "DPDK EAL initialized successfully.");
        socket_descriptor = setup_dpdk_socket(0); // port 0 by default

        logger(LOG_INFO, "Waiting for DPDK port 0 link to come up (up to 30s)...");
        struct rte_eth_link link = {0}; // Initialize link status to 0
        bool link_up = false;
        for (int i = 0; i < 30; i++) { // Check for 30 seconds
            ret = rte_eth_link_get_nowait(0, &link);
            if (ret == 0 && link.link_status == RTE_ETH_LINK_UP) {
                logger(LOG_INFO, "DPDK: Port 0 Link Up! Speed %u Mbps %s-duplex",
                       (unsigned)link.link_speed,
                       (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "full" : "half");
                link_up = true;
                break; // Link is up, exit the loop
            }
            sleep(1); // Wait 1 second
        }

        if (!link_up) {
            logger(LOG_ERROR, "DPDK: Port 0 link is still DOWN after 30s. Exiting.");
            program_args.diagnostics.unrecoverable_error = true;
            goto CLEANUP;
        }

        if (socket_descriptor < 0) { // setup_dpdk_socket returns < 0 on error
            logger(LOG_ERROR, "Failed to setup DPDK socket; see errors above.");
            program_args.diagnostics.unrecoverable_error = true;
            goto CLEANUP;
        }
    }

    diagnose_system(&program_args);
    fill_defaults(&program_args);

    if (parse_args(argc, argv, &program_args) != 0) {
        program_args.diagnostics.unrecoverable_error = true;
        logger(LOG_INFO, "Quitting due to invalid arguments.");
        goto CLEANUP;
    }
    else if (program_args.general.opt_info) {
        goto CLEANUP;
    }

    program_args.advanced.use_dpdk = dpdk_mode;

    logger_set_level(program_args.general.logger_level);
    logger_set_timestamps(!program_args.advanced.no_log_timestamp);

    if (socket_descriptor == -1) {
        socket_descriptor = setup_posix_socket(
                true, !program_args.advanced.no_async_sock
        );
    }

    if (socket_descriptor == -1) {
        program_args.diagnostics.unrecoverable_error = true;
        logger(LOG_INFO, "Quitting after failing to create a socket.");
        goto CLEANUP;
    }

    program_args.socket = socket_descriptor;

    send_packets(&program_args);

    if (shutdown(socket_descriptor, SHUT_RDWR) == -1) {
//        logger(LOG_WARN, "Socket shutdown failed: %s", strerror(errno));
    }
    else {
        logger(LOG_INFO, "Socket shutdown successfully.");
    }

    if (close(socket_descriptor) == -1) {
        logger(LOG_WARN, "Socket closing failed: %s", strerror(errno));
    }
    else {
        logger(LOG_INFO, "Socket closed successfully.");
    }

CLEANUP:

    free(ipv4_header_args);

    logger(LOG_INFO, "Done; exiting program...");

    if (program_args.diagnostics.unrecoverable_error) {
        return EXIT_FAILURE;
    }
    else {
        return EXIT_SUCCESS;
    }
}


// ---------------------------------------------------------------------
// END OF FILE: main.c
// ---------------------------------------------------------------------

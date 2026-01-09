// ---------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
// packet.c is a part of Blitzping.
// ---------------------------------------------------------------------

#include "socket.h"
#include "packet.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>

/* WORK IN-PROGRESS */


struct running_stats {
    long count;
    double mean;
    double m2;     // sum of squares of differences from the current mean
    long min;
    long max;
};

// initialize
static inline void rs_init(struct running_stats *rs) {
    rs->count = 0;
    rs->mean = 0.0;
    rs->m2 = 0.0;
    rs->min = LONG_MAX;
    rs->max = LONG_MIN;
}

// update with a new sample (rtt_ms)
static inline void rs_update(struct running_stats *rs, long x) {
    rs->count += 1;
    double delta = (double)x - rs->mean;
    rs->mean += delta / (double)rs->count;
    double delta2 = (double)x - rs->mean;
    rs->m2 += delta * delta2;
    if (x < rs->min) rs->min = x;
    if (x > rs->max) rs->max = x;
}

// get avg and stddev (population)
static inline double rs_mean(const struct running_stats *rs) {
    return rs->count ? rs->mean : NAN;
}
static inline double rs_stddev(const struct running_stats *rs) {
    if (!rs->count) return NAN;
    // population stddev: sqrt(m2 / n)
    return sqrt(rs->m2 / (double)rs->count);
}
// for sample stddev use m2 / (n-1) when count > 1
static inline double rs_sample_stddev(const struct running_stats *rs) {
    if (rs->count <= 1) return NAN;
    return sqrt(rs->m2 / (double)(rs->count - 1));
}

// simple dynamic array to keep all RTTs if you need medians/p95
struct rtt_array {
    long *vals;
    size_t n;
    size_t cap;
};
static inline void rtt_array_init(struct rtt_array *a) { a->n = 0; a->cap = 0; a->vals = NULL; }
static inline void rtt_array_free(struct rtt_array *a) { free(a->vals); a->vals = NULL; a->n = a->cap = 0; }
static inline int rtt_array_push(struct rtt_array *a, long v) {
    if (a->n == a->cap) {
        size_t newcap = a->cap ? a->cap * 2 : 64;
        long *tmp = realloc(a->vals, newcap * sizeof(*tmp));
        if (!tmp) return -1;
        a->vals = tmp;
        a->cap = newcap;
    }
    a->vals[a->n++] = v;
    return 0;
}
/* comparator for qsort (plain C) */
static int long_cmp(const void *p, const void *q) {
    long A = *(const long *)p;
    long B = *(const long *)q;
    if (A < B) return -1;
    if (A > B) return 1;
    return 0;
}

static int rtt_array_percentile(struct rtt_array *a, double pct, long *out) {
    if (!a->n) return -1;
    long *tmp = malloc(a->n * sizeof(*tmp));
    if (!tmp) return -1;
    memcpy(tmp, a->vals, a->n * sizeof(*tmp));
    qsort(tmp, a->n, sizeof(long), long_cmp);
    /* choose index using nearest-rank (you can change interpolation if desired) */
    size_t idx = (size_t)floor((pct / 100.0) * (double)(a->n - 1) + 0.5);
    *out = tmp[idx];
    free(tmp);
    return 0;
}


// Required includes (add to top of file)
#include <stdatomic.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>    // for sleep/nanosleep
#include <errno.h>
#include <string.h>

// ---------- Globals for stats ----------
static atomic_uint_fast64_t stats_tx_pkts = 0;        // pkts in current aggregate (we'll use deltas)
static atomic_uint_fast64_t stats_tx_bytes = 0;       // bytes in current aggregate
static atomic_uint_fast64_t stats_total_pkts = 0;     // ever-incrementing total pkts
static atomic_uint_fast64_t stats_tx_latency_ns = 0;  // accumulated latency in nanoseconds

// Control flag for stats thread (optional)
static atomic_int stats_thread_running = 0;

// ---------- Helper: timespec difference ----------
static inline uint64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b) {
    // returns (b - a) in ns
    uint64_t sec = (uint64_t)(b->tv_sec - a->tv_sec);
    int64_t nsec = (int64_t)(b->tv_nsec - a->tv_nsec);
    return sec * 1000000000ULL + (uint64_t)nsec;
}

// ---------- Stats printer thread ----------
static void *stats_printer(void *arg) {
    (void)arg;
    atomic_store(&stats_thread_running, 1);

    struct timespec last_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_ts);

    // store last cumulative counters to compute delta
    uint64_t last_pkts = atomic_load(&stats_tx_pkts);
    uint64_t last_bytes = atomic_load(&stats_tx_bytes);
    uint64_t last_latency_ns = atomic_load(&stats_tx_latency_ns);
    uint64_t last_total_pkts = atomic_load(&stats_total_pkts);

    while (atomic_load(&stats_thread_running)) {
        // sleep 1 second (nanosleep to be more precise)
        struct timespec req = {1, 0};
        nanosleep(&req, NULL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t interval_ns = timespec_diff_ns(&last_ts, &now);
        if (interval_ns == 0) interval_ns = 1000000000ULL; // fallback

        uint64_t cur_pkts = atomic_load(&stats_tx_pkts);
        uint64_t cur_bytes = atomic_load(&stats_tx_bytes);
        uint64_t cur_latency_ns = atomic_load(&stats_tx_latency_ns);
        uint64_t cur_total_pkts = atomic_load(&stats_total_pkts);

        uint64_t delta_pkts = (cur_pkts >= last_pkts) ? (cur_pkts - last_pkts) : cur_pkts;
        uint64_t delta_bytes = (cur_bytes >= last_bytes) ? (cur_bytes - last_bytes) : cur_bytes;
        uint64_t delta_latency_ns = (cur_latency_ns >= last_latency_ns) ? (cur_latency_ns - last_latency_ns) : cur_latency_ns;

        double interval_s = (double)interval_ns / 1e9;

        double txpps = delta_pkts / interval_s;
        double txbps = ((double)delta_bytes * 8.0) / interval_s; // bits/sec

        double avg_latency_us = 0.0;
        if (delta_pkts > 0) {
            avg_latency_us = ((double)delta_latency_ns / (double)delta_pkts) / 1000.0;
        }

        // Print using same format as your example
        printf("[PERF] TXpps=%.2f  TXbps=%.2f  avg_latency=%.2f µs  total_pkts=%" PRIu64 "\n",
                txpps, txbps, avg_latency_us, cur_total_pkts);
        fflush(stdout);

        // advance snapshots
        last_ts = now;
        last_pkts = cur_pkts;
        last_bytes = cur_bytes;
        last_latency_ns = cur_latency_ns;
        last_total_pkts = cur_total_pkts;
    }

    return NULL;
}

// Call this function from your main/init code (before or when you start workers)
static inline int start_stats_thread(pthread_t *out_thread) {
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, stats_printer, NULL);
    if (rc != 0) {
        fprintf(stderr, "Failed to create stats thread: %s\n", strerror(rc));
        return -1;
    }
    if (out_thread) *out_thread = tid;
    return 0;
}

// Call this to stop the thread (if you want clean shutdown)
static inline void stop_stats_thread(pthread_t tid) {
    atomic_store(&stats_thread_running, 0);
    pthread_join(tid, NULL);
}

// ---------- Modified send_loop (only the body changed where sendto() is invoked) ----------
//static int send_loop(void *arg) {
//    const struct ProgramArgs *const program_args =
//            (const struct ProgramArgs *const)arg;
//
//    srand(time(0));
//
//    _Alignas (_Alignof (max_align_t)) uint8_t
//            packet_buffer[IP_PKT_MTU] = {0};
//
//    struct ip_hdr  *ip_header  =
//            (struct ip_hdr *)packet_buffer;
//    struct udp_hdr *udp_header =
//            (struct udp_hdr *)(packet_buffer + sizeof (struct ip_hdr));
//
//    *ip_header = *(program_args->ipv4);
//    ip_header->proto = IPPROTO_UDP;
//
//    *udp_header = (struct udp_hdr){
//            .sport = htons(rand() % 65536),
//            .dport = htons(80),
//            .len = htons(sizeof(struct udp_hdr)),
//            .chksum = 0
//    };
//
//    size_t packet_length = sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
//    ip_header->len = htons(packet_length);
//
//
//    uint32_t ip_diff = 1;
//
//    struct sockaddr_in dest_info = {
//            .sin_family = AF_INET,
//            .sin_port = udp_header->dport,
//            // --- THIS IS THE FIX ---
//            // Re-add ntohl() to match the program's convention
//            .sin_addr.s_addr = ntohl(ip_header->daddr.address)
//    };
//
//    // --- Removed connectionless connect() call ---
//
//    if (!program_args->advanced.no_cpu_prefetch) {
//        PREFETCH(packet_buffer, 1, 3);
//    }
//
//    uint32_t next_ip;
//    uint16_t next_port;
//    ssize_t bytes_written;
//
//    struct pollfd pfd;
//    pfd.fd = program_args->socket;
//    pfd.events = POLLOUT;
//
//    for (;;) {
//        next_ip = htonl(
//                ntohl(program_args->ipv4->saddr.address)
//                + (rand() % ip_diff)
//        );
//        next_port = htons(rand() % 65536);
//
//        // Measure sendto latency (time spent in syscall)
//        struct timespec t0, t1;
//        clock_gettime(CLOCK_MONOTONIC, &t0);
//        bytes_written = sendto(
//                program_args->socket,
//                packet_buffer,
//                packet_length,
//                0, // flags
//                (struct sockaddr *)&dest_info,
//                sizeof(dest_info)
//        );
//        clock_gettime(CLOCK_MONOTONIC, &t1);
//
//        uint64_t send_ns = timespec_diff_ns(&t0, &t1);
//
//        if (bytes_written == -1) {
//            if (errno == EAGAIN || errno == EWOULDBLOCK) {
//                if (poll(&pfd, 1, -1) == -1) {
//                    logger(LOG_ERROR,
//                           "Failed to poll the socket: %s",
//                           strerror(errno)
//                    );
//                    break;
//                }
//                continue;
//            }
//            else {
//                logger(LOG_ERROR,
//                       "Failed to write packet to the socket: %s",
//                       strerror(errno)
//                );
//                break;
//            }
//        }
//        else if (bytes_written < (ssize_t)(packet_length)) {
//            logger(LOG_WARN,
//                   "Partial send detected (%ld bytes); this is unusual for UDP.",
//                   bytes_written
//            );
//        }
//
//        // Update counters (atomic, lock-free)
//        atomic_fetch_add(&stats_tx_pkts, 1);
//        atomic_fetch_add(&stats_total_pkts, 1);
//        atomic_fetch_add(&stats_tx_bytes, (uint64_t) (bytes_written > 0 ? bytes_written : 0));
//        atomic_fetch_add(&stats_tx_latency_ns, send_ns);
//
//        ip_header->saddr.address = next_ip;
//        udp_header->sport = next_port;
//    }
//
//#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
//    return thrd_success;
//#else
//    return 0;
//#endif
//}

#include <time.h>
#include <inttypes.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h> // for close()

/* Helper: returns (b - a) in nanoseconds */
//static inline uint64_t timespec_diff_ns(const struct timespec *a, const struct timespec *b) {
//    uint64_t sec = (uint64_t)(b->tv_sec - a->tv_sec);
//    int64_t nsec = (int64_t)(b->tv_nsec - a->tv_nsec);
//    return sec * 1000000000ULL + (uint64_t)nsec;
//}

/* Helper: convert ns to seconds double */
static inline double ns_to_s(uint64_t ns) { return (double)ns / 1e9; }

/* Replacement send_loop that prints stats every ~1 second without threads */
static int send_loop(void *arg) {
    const struct ProgramArgs *const program_args =
            (const struct ProgramArgs *const)arg;

    srand(time(0));

    _Alignas (_Alignof (max_align_t)) uint8_t
            packet_buffer[IP_PKT_MTU] = {0};

    struct ip_hdr  *ip_header  =
            (struct ip_hdr *)packet_buffer;
    struct udp_hdr *udp_header =
            (struct udp_hdr *)(packet_buffer + sizeof (struct ip_hdr));

    *ip_header = *(program_args->ipv4);
    ip_header->proto = IPPROTO_UDP;

    *udp_header = (struct udp_hdr){
            .sport = htons(rand() % 65536),
            .dport = htons(80),
            .len = htons(sizeof(struct udp_hdr)),
            .chksum = 0
    };

    size_t packet_length = sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
    ip_header->len = htons(packet_length);


    uint32_t ip_diff = 1;

    struct sockaddr_in dest_info = {
            .sin_family = AF_INET,
            .sin_port = udp_header->dport,
            /* Re-add ntohl() to match program convention if you used that earlier */
            .sin_addr.s_addr = ntohl(ip_header->daddr.address)
    };

    if (!program_args->advanced.no_cpu_prefetch) {
        PREFETCH(packet_buffer, 1, 3);
    }

    uint32_t next_ip;
    uint16_t next_port;
    ssize_t bytes_written;

    struct pollfd pfd;
    pfd.fd = program_args->socket;
    pfd.events = POLLOUT;

    /* Stats accumulators (single-threaded inside this loop) */
    uint64_t total_pkts = 0;
    uint64_t total_bytes = 0;
    uint64_t total_latency_ns = 0;

    /* Snapshot values for delta (for per-second rate calculation) */
    uint64_t last_pkts = 0;
    uint64_t last_bytes = 0;
    uint64_t last_latency_ns = 0;

    struct timespec last_print_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_print_ts);

    for (;;) {
        next_ip = htonl(
                ntohl(program_args->ipv4->saddr.address)
                + (rand() % ip_diff)
        );
        next_port = htons(rand() % 65536);

        /* Measure sendto latency (time spent in syscall) */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        bytes_written = sendto(
                program_args->socket,
                packet_buffer,
                packet_length,
                0, // flags
                (struct sockaddr *)&dest_info,
                sizeof(dest_info)
        );
        clock_gettime(CLOCK_MONOTONIC, &t1);

        uint64_t send_ns = timespec_diff_ns(&t0, &t1);

        if (bytes_written == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* compute time until next 1s boundary so we can still wake to print stats */
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                uint64_t elapsed_ns = timespec_diff_ns(&last_print_ts, &now);
                int timeout_ms;
                if (elapsed_ns >= 1000000000ULL) {
                    /* already due for a print: use 0 to return immediately and continue loop */
                    timeout_ms = 0;
                } else {
                    uint64_t rem_ns = 1000000000ULL - elapsed_ns;
                    timeout_ms = (int)(rem_ns / 1000000ULL);
                    if (timeout_ms <= 0) timeout_ms = 0;
                }

                int rc = poll(&pfd, 1, timeout_ms);
                if (rc == -1) {
                    logger(LOG_ERROR,
                           "Failed to poll the socket: %s",
                           strerror(errno)
                    );
                    break;
                }
                /* After poll returns (or timeout), continue the loop and let the time-based
                   printer logic below decide whether to print stats. */
                continue;
            }
            else {
                logger(LOG_ERROR,
                       "Failed to write packet to the socket: %s",
                       strerror(errno)
                );
                break;
            }
        }
        else if (bytes_written < (ssize_t)(packet_length)) {
            logger(LOG_WARN,
                   "Partial send detected (%ld bytes); this is unusual for UDP.",
                   bytes_written
            );
        }

        /* Update counters */
        total_pkts++;
        total_bytes += (uint64_t)(bytes_written > 0 ? bytes_written : 0);
        total_latency_ns += send_ns;

        ip_header->saddr.address = next_ip;
        udp_header->sport = next_port;

        /* Time to check whether 1 second has passed and print stats */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed_ns = timespec_diff_ns(&last_print_ts, &now);

        if (elapsed_ns >= 1000000000ULL) {
            uint64_t delta_pkts = total_pkts - last_pkts;
            uint64_t delta_bytes = total_bytes - last_bytes;
            uint64_t delta_latency_ns = total_latency_ns - last_latency_ns;

            double interval_s = ns_to_s(elapsed_ns);
            if (interval_s <= 0.0) interval_s = 1.0; /* defensive */

            double txpps = (double)delta_pkts / interval_s;
            double txbps = ((double)delta_bytes * 8.0) / interval_s; /* bits/sec */

            double avg_latency_us = 0.0;
            if (delta_pkts > 0) {
                avg_latency_us = ((double)delta_latency_ns / (double)delta_pkts) / 1000.0;
            }

            /* Print in same format you used */
            printf("[PERF] TXpps=%.2f  TXbps=%.2f  avg_latency=%.2f µs  total_pkts=%" PRIu64 "\n",
                    txpps, txbps, avg_latency_us, total_pkts);
            fflush(stdout);

            /* advance snapshots */
            last_print_ts = now;
            last_pkts = total_pkts;
            last_bytes = total_bytes;
            last_latency_ns = total_latency_ns;
        }
    }

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    return thrd_success;
#else
    return 0;
#endif
}



#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

// Constants tuned for traceroute behavior
#ifndef TRACERT_MAX_TTL
# define TRACERT_MAX_TTL 30
#endif
#ifndef TRACERT_PROBES_PER_TTL
# define TRACERT_PROBES_PER_TTL 1
#endif
#ifndef TRACERT_TIMEOUT_MS
# define TRACERT_TIMEOUT_MS 2000
#endif

// This function replaces the old send_loop when running traceroute-style probes.
// It sends TRACERT_PROBES_PER_TTL probes for each TTL value from 1..TRACERT_MAX_TTL,
// listens for ICMP replies, matches them to probes using the quoted transport header
// (we use the TCP source port as the token), and records RTT and responder IP.
//static int send_loop_tracert(void *arg) {
//    const struct ProgramArgs *const program_args =
//            (const struct ProgramArgs *const)arg;
//
//    // local buffers
//    _Alignas(_Alignof(max_align_t)) uint8_t packet_buffer[IP_PKT_MTU] = {0};
//    struct ip_hdr  *ip_header  = (struct ip_hdr *)packet_buffer;
//    struct udp_hdr *udp_header = (struct udp_hdr *)(packet_buffer + sizeof(struct ip_hdr));
//
//    // initialize template headers from program args
//    *ip_header = *(program_args->ipv4);
//    ip_header->proto = IPPROTO_UDP; // use UDP
//
//    // base UDP template (destination port will be varied)
//    *udp_header = (struct udp_hdr){
//            .sport = htons(33434),
//            .dport = htons(33434),
//            .len   = htons(sizeof(struct udp_hdr)), // only header, no payload
//            .chksum = 0
//    };
//
//    size_t packet_length = sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
//    ip_header->len = htons(packet_length);
//
//    // Destination info
//    struct sockaddr_in dest_info = {
//            .sin_family = AF_INET,
//            .sin_port = udp_header->dport,
//            .sin_addr.s_addr = ntohl(ip_header->daddr.address)
//    };
//
//    // Create ICMP socket to receive replies
//    int icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
//    if (icmp_sock == -1) {
//        logger(LOG_ERROR, "send_loop_tracert: failed to create ICMP socket: %s", strerror(errno));
//        return 1;
//    }
//
//    // Non-blocking ICMP socket
//    int flags = fcntl(icmp_sock, F_GETFL, 0);
//    if (flags != -1)
//        fcntl(icmp_sock, F_SETFL, flags | O_NONBLOCK);
//
//    struct pollfd pfd = {
//            .fd = icmp_sock,
//            .events = POLLIN
//    };
//
//    struct sent_probe {
//        uint16_t sport;
//        struct timespec ts;
//        bool answered;
//        struct sockaddr_in responder;
//        long rtt_ms;
//    } probes[TRACERT_PROBES_PER_TTL];
//
//    uint8_t recvbuf[2048];
//
//    // TTL loop
//    for (int ttl = 1; ttl <= TRACERT_MAX_TTL; ++ttl) {
//        for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
//            probes[i].answered = false;
//            probes[i].rtt_ms = -1;
//            memset(&probes[i].responder, 0, sizeof(probes[i].responder));
//        }
//
//        for (int p = 0; p < TRACERT_PROBES_PER_TTL; ++p) {
//            // unique source port (so we can identify replies)
//            uint16_t token = (uint16_t)((getpid() & 0xffff) ^ (ttl << 8) ^ p);
//            udp_header->sport = htons(token);
//            udp_header->dport = htons(33434 + ttl); // vary dest port per hop
//
//            ip_header->ttl = (uint8_t)ttl;
//
//            // timestamp
//            clock_gettime(CLOCK_MONOTONIC, &probes[p].ts);
//            probes[p].sport = udp_header->sport;
//
//            // send packet
//            ssize_t sent = sendto(program_args->socket, packet_buffer, packet_length, 0,
//                                  (struct sockaddr *)&dest_info, sizeof(dest_info));
//            if (sent == -1)
//                logger(LOG_WARN, "sendto failed (ttl=%d probe=%d): %s", ttl, p, strerror(errno));
//        }
//
//        // listen for ICMP replies
//        int remaining_ms = TRACERT_TIMEOUT_MS;
//        int unanswered = TRACERT_PROBES_PER_TTL;
//        while (remaining_ms > 0 && unanswered > 0) {
//            int n = poll(&pfd, 1, remaining_ms);
//            if (n == -1) {
//                if (errno == EINTR) continue;
//                logger(LOG_ERROR, "poll failed: %s", strerror(errno));
//                break;
//            }
//            if (n == 0)
//                break;
//
//            struct sockaddr_in src_addr;
//            socklen_t addrlen = sizeof(src_addr);
//            ssize_t rlen = recvfrom(icmp_sock, recvbuf, sizeof(recvbuf), 0,
//                                    (struct sockaddr *)&src_addr, &addrlen);
//            if (rlen <= 0) continue;
//
//            struct ip_hdr *outer_ip = (struct ip_hdr *)recvbuf;
//            int outer_ihl = (outer_ip->ihl & 0x0f) * 4;
//            if (rlen < outer_ihl + 8) continue;
//
//            uint8_t *icmp_ptr = recvbuf + outer_ihl;
//            uint8_t icmp_type = icmp_ptr[0];
//            //uint8_t icmp_code = icmp_ptr[1];
//
//            if (icmp_type != 11 && icmp_type != 3)
//                continue; // only handle Time Exceeded and Dest Unreachable
//
//            uint8_t *quoted_ptr = icmp_ptr + 8;
//            struct ip_hdr *quoted_ip = (struct ip_hdr *)quoted_ptr;
//            int q_ihl = (quoted_ip->ihl & 0x0f) * 4;
//            struct udp_hdr *quoted_udp = (struct udp_hdr *)((uint8_t *)quoted_ip + q_ihl);
//
//            uint16_t quoted_sport = quoted_udp->sport;
//
//            for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
//                if (!probes[i].answered && probes[i].sport == quoted_sport) {
//                    struct timespec now;
//                    clock_gettime(CLOCK_MONOTONIC, &now);
//                    long ms = (now.tv_sec - probes[i].ts.tv_sec) * 1000L +
//                              (now.tv_nsec - probes[i].ts.tv_nsec) / 1000000L;
//                    probes[i].answered = true;
//                    probes[i].rtt_ms = ms;
//                    probes[i].responder = src_addr;
//                    unanswered--;
//                    break;
//                }
//            }
//            remaining_ms = 0; // single poll pass for simplicity
//        }
//
//        // Print results
//        for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
//            if (probes[i].answered) {
//                char ipstr[INET_ADDRSTRLEN];
//                inet_ntop(AF_INET, &probes[i].responder.sin_addr, ipstr, sizeof(ipstr));
//                logger(LOG_INFO, "ttl=%d probe=%d reply from %s rtt=%ld ms", ttl, i, ipstr, probes[i].rtt_ms);
//            } else {
//                logger(LOG_INFO, "ttl=%d probe=%d no reply", ttl, i);
//            }
//        }
//
//        bool reached = false;
//        for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
//            if (probes[i].answered &&
//                probes[i].responder.sin_addr.s_addr == dest_info.sin_addr.s_addr) {
//                reached = true;
//                break;
//            }
//        }
//        if (reached) {
//            logger(LOG_INFO, "Destination reached at ttl=%d; stopping traceroute.", ttl);
//            break;
//        }
//    }
//
//    close(icmp_sock);
//
//#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
//    return thrd_success;
//#else
//    return 0;
//#endif
//}


static int send_loop_tracert(void *arg) {
    const struct ProgramArgs *const program_args =
            (const struct ProgramArgs *const)arg;

    _Alignas(_Alignof(max_align_t)) uint8_t packet_buffer[IP_PKT_MTU] = {0};
    struct ip_hdr  *ip_header  = (struct ip_hdr *)packet_buffer;
    struct udp_hdr *udp_header = (struct udp_hdr *)(packet_buffer + sizeof(struct ip_hdr));

    *ip_header = *(program_args->ipv4);
    ip_header->proto = IPPROTO_UDP;

    *udp_header = (struct udp_hdr){
            .sport = htons(33434),
            .dport = htons(33434),
            .len   = htons(sizeof(struct udp_hdr)),
            .chksum = 0
    };

    size_t packet_length = sizeof(struct ip_hdr) + sizeof(struct udp_hdr);
    ip_header->len = htons(packet_length);

    struct sockaddr_in dest_info = {
            .sin_family = AF_INET,
            .sin_port = udp_header->dport,
            .sin_addr.s_addr = ntohl(ip_header->daddr.address)
    };

    int icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (icmp_sock == -1) {
        logger(LOG_ERROR, "send_loop_tracert: failed to create ICMP socket: %s", strerror(errno));
        return 1;
    }

    int flags = fcntl(icmp_sock, F_GETFL, 0);
    if (flags != -1)
        fcntl(icmp_sock, F_SETFL, flags | O_NONBLOCK);

    struct pollfd pfd = {.fd = icmp_sock, .events = POLLIN};

    struct sent_probe {
        uint16_t sport;
        struct timespec ts;
        bool answered;
        struct sockaddr_in responder;
        long rtt_ms;
    } probes[TRACERT_PROBES_PER_TTL];

    uint8_t recvbuf[2048];
    long overall_sent = 0, overall_recv = 0;

    struct running_stats overall_stats;
    rs_init(&overall_stats);
    struct rtt_array all_rtts;
    rtt_array_init(&all_rtts);

    for (int ttl = 1; ttl <= TRACERT_MAX_TTL; ++ttl) {
        struct running_stats ttl_stats;
        rs_init(&ttl_stats);

        for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
            probes[i].answered = false;
            probes[i].rtt_ms = -1;
        }

        for (int p = 0; p < TRACERT_PROBES_PER_TTL; ++p) {
            uint16_t token = (uint16_t)((getpid() & 0xffff) ^ (ttl << 8) ^ p);
            udp_header->sport = htons(token);
            udp_header->dport = htons(33434 + ttl);
            ip_header->ttl = (uint8_t)ttl;
            clock_gettime(CLOCK_MONOTONIC, &probes[p].ts);
            probes[p].sport = udp_header->sport;

            ssize_t sent = sendto(program_args->socket, packet_buffer, packet_length, 0,
                                  (struct sockaddr *)&dest_info, sizeof(dest_info));
            if (sent == -1)
                logger(LOG_WARN, "sendto failed (ttl=%d probe=%d): %s", ttl, p, strerror(errno));
            else
                overall_sent++;
        }

        int remaining_ms = TRACERT_TIMEOUT_MS;
        int unanswered = TRACERT_PROBES_PER_TTL;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);

        while (remaining_ms > 0 && unanswered > 0) {
            int n = poll(&pfd, 1, remaining_ms);
            if (n == -1 && errno == EINTR) continue;
            if (n <= 0) break;

            while (1) {
                struct sockaddr_in src_addr;
                socklen_t addrlen = sizeof(src_addr);
                ssize_t rlen = recvfrom(icmp_sock, recvbuf, sizeof(recvbuf), 0,
                                        (struct sockaddr *)&src_addr, &addrlen);
                if (rlen <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    else break;
                }

                if (rlen < (ssize_t)sizeof(struct ip_hdr) + 8) continue;
                struct ip_hdr *outer_ip = (struct ip_hdr *)recvbuf;
                int outer_ihl = (outer_ip->ihl & 0x0f) * 4;
                if (rlen < outer_ihl + 8) continue;

                uint8_t *icmp_ptr = recvbuf + outer_ihl;
                uint8_t icmp_type = icmp_ptr[0];
                if (icmp_type != 11 && icmp_type != 3) continue;

                uint8_t *quoted_ptr = icmp_ptr + 8;
                struct ip_hdr *quoted_ip = (struct ip_hdr *)quoted_ptr;
                int q_ihl = (quoted_ip->ihl & 0x0f) * 4;
                struct udp_hdr *quoted_udp = (struct udp_hdr *)((uint8_t *)quoted_ip + q_ihl);

                uint16_t quoted_sport = quoted_udp->sport;

                for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
                    if (!probes[i].answered && probes[i].sport == quoted_sport) {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long ms = (now.tv_sec - probes[i].ts.tv_sec) * 1000L +
                                  (now.tv_nsec - probes[i].ts.tv_nsec) / 1000000L;
                        probes[i].answered = true;
                        probes[i].rtt_ms = ms;
                        probes[i].responder = src_addr;
                        unanswered--;
                        overall_recv++;
                        rs_update(&ttl_stats, ms);
                        rs_update(&overall_stats, ms);
                        rtt_array_push(&all_rtts, ms);
                        break;
                    }
                }
            }
            // recompute timeout
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000L +
                           (now.tv_nsec - start.tv_nsec) / 1000000L;
            remaining_ms = TRACERT_TIMEOUT_MS - (int)elapsed;
        }

        for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
            if (probes[i].answered) {
                char ipstr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &probes[i].responder.sin_addr, ipstr, sizeof(ipstr));
                logger(LOG_INFO, "ttl=%d probe=%d reply from %s rtt=%ld ms",
                       ttl, i, ipstr, probes[i].rtt_ms);
            } else {
                logger(LOG_INFO, "ttl=%d probe=%d no reply", ttl, i);
            }
        }

//        double loss_pct = 100.0 * (TRACERT_PROBES_PER_TTL - ttl_stats.count) /
//                          TRACERT_PROBES_PER_TTL;
        if (ttl_stats.count > 0) {
//            logger(LOG_INFO,
//                   "ttl=%d stats: sent=%d received=%ld loss=%.1f%% min=%ldms avg=%.2fms max=%ldms stddev=%.2fms",
//                   ttl, TRACERT_PROBES_PER_TTL, ttl_stats.count, loss_pct,
//                   ttl_stats.min, rs_mean(&ttl_stats), ttl_stats.max, rs_stddev(&ttl_stats));
        } else {
//            logger(LOG_INFO,
//                   "ttl=%d stats: sent=%d received=0 loss=100.0%% min=N/A avg=N/A max=N/A stddev=N/A",
//                   ttl, TRACERT_PROBES_PER_TTL);
        }

        bool reached = false;
        for (int i = 0; i < TRACERT_PROBES_PER_TTL; ++i) {
            if (probes[i].answered &&
                probes[i].responder.sin_addr.s_addr == dest_info.sin_addr.s_addr) {
                reached = true;
                break;
            }
        }
        if (reached) {
            logger(LOG_INFO, "Destination reached at ttl=%d; stopping traceroute.", ttl);
            break;
        }
    }

    double overall_loss = 0.0;
    if (overall_sent > 0)
        overall_loss = 100.0 * (overall_sent - overall_recv) / (double)overall_sent;

    long median = 0, p95 = 0;
    int have_median = rtt_array_percentile(&all_rtts, 50.0, &median);
    int have_p95 = rtt_array_percentile(&all_rtts, 95.0, &p95);

    logger(LOG_INFO,
           "traceroute summary: sent=%ld received=%ld loss=%.1f%% min RTT=%ldms avg RTT=%.2fms max RTT=%ldms stddev=%.2fms",
           overall_sent, overall_recv, overall_loss,
           overall_stats.min, rs_mean(&overall_stats),
           overall_stats.max, rs_stddev(&overall_stats)
//           have_median == 0 ? " median=" : "",
//           have_median == 0 ? "" : "",
//           have_median == 0 ? "" : "",
//           have_p95 == 0 ? "" : ""
           );

    if (have_median == 0)
        logger(LOG_INFO, "overall median RTT = %ld ms", median);
    if (have_p95 == 0)
        logger(LOG_INFO, "overall p95 RTT = %ld ms", p95);

    rtt_array_free(&all_rtts);
    close(icmp_sock);

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    return thrd_success;
#else
    return 0;
#endif
}


//
//static int send_loop_dpdk(void *arg) {
//    const struct ProgramArgs *const program_args =
//            (const struct ProgramArgs *const)arg;
//
//    _Alignas (_Alignof (max_align_t)) uint8_t
//            packet_buffer[IP_PKT_MTU] = {0};
//
//    struct ip_hdr  *ip_header  =
//            (struct ip_hdr *)packet_buffer;
//    struct tcp_hdr *tcp_header =
//            (struct tcp_hdr *)(packet_buffer + sizeof (struct ip_hdr));
//
//    *ip_header = *(program_args->ipv4);
//    *tcp_header = (struct tcp_hdr){
//            .sport = htons(rand() % 65536),
//            .dport = htons(80),
//            .seqnum = rand(),
//            .flags.syn = true
//    };
//
//    uint32_t ip_diff = 1;
//    size_t packet_length = ntohs(ip_header->len);
//
//    const uint16_t port_id = 0;
//
//    for (int cnt = 1500; cnt >= 0; cnt--) {
//        ip_header->saddr.address = htonl(
//                program_args->ipv4->saddr.address
//                + (rand() % ip_diff)
//        );
//        tcp_header->sport = htons(rand() % 65536);
//
//        if (cnt < 100 || dpdk_send_packet(port_id, packet_buffer, packet_length) != 0) {
////             logger(LOG_WARN, "TEST TESTSETSETSETSETSETSETSETSETSET ------------");
//        }
//    }
//
//#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
//    return thrd_success;
//#else
//    return 0;
//#endif
//}





// In packet.c, replace send_loop_dpdk with this:

// --- Make sure this header is included at the top of packet.c ---
#include <rte_ether.h>
// ---------------------------------------------------------------

#include <rte_cycles.h>   // for rte_get_tsc_cycles(), rte_get_tsc_hz()

void dpdk_monitor(uint16_t port_id) {
    struct rte_eth_stats stats;
    static struct rte_eth_stats prev = {0};

    rte_eth_stats_get(port_id, &stats);
    uint64_t tx_pps = stats.opackets - prev.opackets;
    uint64_t rx_pps = stats.ipackets - prev.ipackets;

    logger(LOG_INFO,
           "[DPDK PERF] TXpps=%" PRIu64 " RXpps=%" PRIu64
    " TXerr=%" PRIu64 " RXmiss=%" PRIu64,
            tx_pps, rx_pps, stats.oerrors, stats.imissed);

    prev = stats;
}



static int send_loop_dpdk(void *arg) {
    const struct ProgramArgs *const program_args =
            (const struct ProgramArgs *const)arg;

    char mac_str[18];
    rte_ether_format_addr(mac_str, sizeof(mac_str), &program_args->gate_mac);
    logger(LOG_INFO, "DPDK: Using gateway MAC: %s", mac_str);
    // ---------------------------------

    _Alignas (_Alignof (max_align_t)) uint8_t
            packet_buffer[IP_PKT_MTU] = {0};

    // --- Packet headers now include Ethernet ---
    struct rte_ether_hdr *eth_header =
            (struct rte_ether_hdr *)packet_buffer;
    struct ip_hdr  *ip_header  =
            (struct ip_hdr *)(packet_buffer + sizeof(struct rte_ether_hdr));
    struct udp_hdr *udp_header =
            (struct udp_hdr *)(packet_buffer + sizeof(struct rte_ether_hdr)
                               + sizeof(struct ip_hdr));

    // --- 1. Build Ethernet Header ---
    eth_header->ether_type = htons(RTE_ETHER_TYPE_IPV4);
    eth_header->src_addr = program_args->port_mac; // From Step 2
    eth_header->dst_addr = program_args->gate_mac; // The hardcoded one

    // --- 2. Build IP Header ---
    *ip_header = *(program_args->ipv4);
    ip_header->saddr.address = htonl(program_args->ipv4->saddr.address);
    ip_header->daddr.address = htonl(program_args->ipv4->daddr.address);
    ip_header->proto = IPPROTO_UDP;
    // Set checksum to 0; HW offload will do it
    ip_header->chksum = 0;

    logger(LOG_INFO, "DPDK: Source IP Address: %s", inet_ntoa(*(struct in_addr*)&ip_header->saddr.address));

    // --- 3. Build UDP Header ---
    *udp_header = (struct udp_hdr){
            .sport  = htons(rand() % 65536),
            .dport  = htons(33434),
            .len    = htons(sizeof(struct udp_hdr)),
            // Set checksum to 0; HW offload will do it
            .chksum = 0
    };

    // --- 4. Set final packet length (L2 + L3 + L4) ---
    size_t packet_length = sizeof(struct rte_ether_hdr)
                           + sizeof(struct ip_hdr)
                           + sizeof(struct udp_hdr);
    ip_header->len = htons(packet_length - sizeof(struct rte_ether_hdr));


    uint32_t ip_diff = 1;
    const uint16_t port_id = 0;

    uint64_t last_tsc = rte_get_tsc_cycles();
    const uint64_t tsc_hz = rte_get_tsc_hz();
    const uint64_t one_sec_cycles = tsc_hz; // 1 second worth of cycles


    uint64_t counter = 0;

    for (;;) {
        ip_header->saddr.address = htonl(
                ntohl(program_args->ipv4->saddr.address)
                + (rand() % ip_diff)
        );
        ip_header->ttl = 127;
        udp_header->sport = htons(rand() % 65536);

        if (dpdk_send_packet(port_id, packet_buffer, packet_length) != 0) {
            /* logger(LOG_WARN, "UDP send test..."); */
        }
        uint64_t now_tsc = rte_get_tsc_cycles();
        if (now_tsc - last_tsc >= one_sec_cycles) {
            dpdk_monitor(port_id);
            last_tsc = now_tsc;
        }
    }

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
    return thrd_success;
#else
    return 0;
#endif
}




// TODO: Use xorshift
// TODO: check for POSIX_MEMLOCK
int send_packets(struct ProgramArgs *const program_args) {

    if (!program_args->advanced.no_mem_lock) {
        if (mlockall(MCL_FUTURE) == -1) {
            logger(LOG_ERROR,
                   "Failed to lock memory: %s", strerror(errno)
            );
            return 1;
        }
        else {
            logger(LOG_INFO, "Locked memory.");
        }
    }

    // TODO: See if this is required for a raw sendto()?
    /*
    struct sockaddr_in dest_info;
    dest_info.sin_family = AF_INET;
    dest_info.sin_port = tcp_header->dport;
    dest_info.sin_addr.s_addr = ip_header->daddr.address;
    size_t packet_length = ntohs(ip_header->len);
    struct sockaddr *dest_addr = (struct sockaddr *)&dest_info;
    size_t addr_len = sizeof (dest_info);
    */

    /*struct msghdr msg = {
        .msg_name = &(struct sockaddr_in){
            .sin_family = AF_INET,
            .sin_port = tcp_header->dport,
            .sin_addr.s_addr = ip_header->daddr.address
        },
        .msg_namelen = sizeof (struct sockaddr_in),
        .msg_iov = (struct iovec[1]){
            {
                .iov_base = packet_buffer,
                .iov_len = ntohs(ip_header->len)
            }
        },
        .msg_iovlen = 1
    };*/

    /*
    printf("Packet:\n");
    printf("Source IP: %s\n", inet_ntoa(*(struct in_addr*)&ip_header->saddr.address));
    printf("Destination IP: %s\n", inet_ntoa(*(struct in_addr*)&ip_header->daddr.address));
    printf("Source Port: %d\n", ntohs(tcp_header->sport));
    printf("Destination Port: %d\n", ntohs(tcp_header->dport));
    printf("TTL: %d\n", ip_header->ttl);
    printf("Header Length: %d\n", ip_header->ihl * 4); // ihl is in 32-bit words
    printf("Total Length: %d\n", ntohs(ip_header->len));
    printf("SYN Flag: %s\n", tcp_header->flags.syn ? "Set" : "Not set");
    printf("\n");
    */



    unsigned int num_threads = program_args->advanced.num_threads;

    thrd_start_t send_func;

    if (program_args->advanced.use_dpdk) {
        if (program_args->advanced.tracert) {
            logger(LOG_ERROR, "Traceroute mode is not compatible with DPDK.");
            return 1;
        }
        logger(LOG_INFO, "Using DPDK send loop.");
        send_func = send_loop_dpdk;
    } else {
        if (program_args->advanced.tracert) {
            logger(LOG_INFO, "Using POSIX traceroute loop.");
            send_func = send_loop_tracert;
        } else {
            logger(LOG_INFO, "Using POSIX send loop.");
            send_func = send_loop;
        }
    }

    if (num_threads == 0) {
        send_func(program_args);
    }
    else {
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
        logger(LOG_INFO, "Spawning %u threads.", num_threads);
        thrd_t threads[MAX_THREADS];

        for (unsigned int i = 0; i < num_threads; i++) {
            int thread_status = thrd_create(
                &threads[i], send_func, program_args
            );
            if (thread_status != thrd_success) {
                logger(LOG_ERROR, "Failed to spawn thread %d.", i);
                // Cleanup already-created threads
                for (unsigned int j = 0; j < i; j++) {
                    thrd_join(threads[j], NULL);
                }
                //free(threads);
                return 1;
            }
        }

        // TODO: This is never reached; add a signal handler?
        for (unsigned int i = 0; i < num_threads; i++) {
            thrd_join(threads[i], NULL);
        }
#else
        return 1;
#endif
    }


    if (!program_args->advanced.no_mem_lock) {
        if (munlockall() == -1) {
            logger(LOG_ERROR,
                   "Failed to unlock used memory: %s", strerror(errno)
            );
            return 1;
        }
        else {
            logger(LOG_INFO, "Unlocked used memory.");
        }
    }

    return 0;
}


// ---------------------------------------------------------------------
// END OF FILE: packet.c
// ---------------------------------------------------------------------

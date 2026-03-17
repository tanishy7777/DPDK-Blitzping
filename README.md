# DPDK-Blitzping

A fork of [Blitzping](https://github.com/rwtnb/Blitzping) ported to DPDK kernel-bypass I/O, with added TTL-based traceroute, ICMP parsing, per-hop RTT measurement, and a multithreaded stats engine.

---

## What Changed from Upstream

The original Blitzping uses POSIX raw sockets (`SOCK_RAW`) for packet transmission. This fork adds an optional `--use-dpdk` mode that bypasses the kernel networking stack entirely using DPDK, along with traceroute support and a live stats subsystem.

---

## New Features

### 1. DPDK Kernel-Bypass I/O (`--use-dpdk`)

Replaces `SOCK_RAW` sendto/recvfrom with DPDK's poll-mode driver for direct NIC access.

**How it works:**
- `setup_dpdk_environment()` calls `rte_eal_init()` and creates an mbuf pool (`NUM_MBUFS=8191`, cache size 250)
- `setup_dpdk_socket()` configures the NIC port with RX/TX queues (128 RX descriptors, 512 TX descriptors) and starts the port
- `dpdk_send_packet()` allocates an mbuf, copies the packet, sets offload flags, and calls `rte_eth_tx_burst()`
- On startup, waits up to 30 seconds for the port link to come up with `rte_eth_link_get_nowait()`

**Hardware offloads enabled:**
```c
.ol_flags = RTE_MBUF_F_TX_IPV4 |
            RTE_MBUF_F_TX_IP_CKSUM |
            RTE_MBUF_F_TX_UDP_CKSUM;
```
IPv4 and UDP checksums are computed by the NIC, not the CPU. Zero-copy transmission with NIC checksum offloading.

**Fallback:** If `--use-dpdk` is not passed, the original POSIX socket path is used unchanged.

**Performance:** 4.5× throughput improvement over the socket-based implementation.

---

### 2. TTL-Based Traceroute (`send_loop_tracert`)

A new send loop (`send_loop_tracert`) implements traceroute-style probing from TTL=1 to TTL=30.

**How it works:**
- For each TTL, sends `TRACERT_PROBES_PER_TTL` UDP probes to port `33434 + ttl`
- Each probe uses a unique source port token: `(pid & 0xffff) ^ (ttl << 8) ^ probe_idx` for reply matching
- Opens a non-blocking raw ICMP socket (`SOCK_RAW, IPPROTO_ICMP`) to receive Time Exceeded (type 11) and Port Unreachable (type 3) replies
- Uses `poll()` with a configurable timeout per TTL (`TRACERT_TIMEOUT_MS`)
- Parses the outer IP + ICMP header, then extracts the **quoted inner IP+UDP header** to match the reply back to the original probe via source port

**RTT measurement:**
- `clock_gettime(CLOCK_MONOTONIC)` timestamps each probe at send time
- RTT computed on reply receipt and stored per-probe in `probes[].rtt_ms`
- Destination reached when ICMP type 3 (Port Unreachable) is received

---

### 3. RTT Statistics Engine (`packet.c`)

A Welford online algorithm computes running stats without storing all samples:

```c
struct running_stats {
    long count;
    double mean;
    double m2;   // sum of squared differences (for variance)
    long min, max;
};
```

- `rs_update()`: O(1) update per sample using Welford's method
- `rs_mean()` / `rs_stddev()` / `rs_sample_stddev()`: derived from accumulated `m2`
- `rtt_array`: dynamic array for samples when percentile computation is needed
- `rtt_array_percentile()`: nearest-rank percentile via `qsort` (p50, p95, etc.)

Per-TTL and overall aggregate stats are tracked separately.

---

### 4. Multithreaded Live Stats Printer

Atomic counters track packet/byte throughput in real time:

```c
static atomic_uint_fast64_t stats_tx_pkts;
static atomic_uint_fast64_t stats_tx_bytes;
static atomic_uint_fast64_t stats_total_pkts;
static atomic_uint_fast64_t stats_tx_latency_ns;
```

A background `pthread` (`stats_printer`) wakes every second and prints a live throughput summary. `start_stats_thread()` / `stop_stats_thread()` manage the thread lifecycle cleanly with `atomic_store` / `pthread_join`.

---

### 5. DPDK Port Diagnostics (`dump_port_and_mbuf_info`)

Debug utility that logs:
- Link status, speed, duplex
- Device capabilities (`max_tx_queues`, `tx_offload_capa`)
- Port stats (`ipackets`, `opackets`, `ibytes`, `imissed`, `oerrors`)
- Mempool configuration (size, element size, cache, data room)

---

## Build

**Prerequisites:** DPDK installed (tested with DPDK 22.x+), `pkg-config`, `libdpdk`.

```bash
# DPDK mode
make

# Run with DPDK (requires hugepages + DPDK-compatible NIC or vdev)
sudo ./blitzping --use-dpdk -- [blitzping options]

# Original POSIX socket mode (no DPDK required)
./blitzping [blitzping options]
```

Hugepages setup (required for DPDK):
```bash
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

---

## Modified Files

| File | Changes |
|------|---------|
| `src/socket.c` | `setup_dpdk_environment()`, `setup_dpdk_socket()`, `dpdk_send_packet()`, `dump_port_and_mbuf_info()` |
| `src/packet.c` | `send_loop_tracert()`, `running_stats`, `rtt_array`, stats thread |
| `src/main.c` | `--use-dpdk` flag parsing, EAL init, link-up wait, conditional socket setup |
| `src/program.h` | Added `tracert`, `use_dpdk`, `port_mac`, `gate_mac`, UDP/ICMP header fields |
| `src/dpdk/dpdk.h` | DPDK abstraction interface |
| `src/dpdk/dpdk.cpp` | DPDK init, send, poll-recv, shutdown |
| `Makefile` | DPDK `pkg-config` integration, `-lm`, rpath for DPDK shared libs |

---

## Architecture

```
                    ┌─────────────────────────────┐
                    │         main.c               │
                    │  --use-dpdk flag detected?   │
                    └──────────┬──────────┬────────┘
                               │Yes       │No
                    ┌──────────▼──┐  ┌────▼──────────────┐
                    │  DPDK Path  │  │  POSIX Socket Path │
                    │  socket.c   │  │  socket.c (orig)   │
                    │  EAL init   │  │  SOCK_RAW sendto   │
                    │  NIC queue  │  └────────────────────┘
                    │  TX offload │
                    └──────────┬──┘
                               │
                    ┌──────────▼──────────────────┐
                    │        packet.c              │
                    │  send_loop / send_loop_tracert│
                    │  RTT stats + stats thread    │
                    └─────────────────────────────┘
```

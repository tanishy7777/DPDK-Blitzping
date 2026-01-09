// ---------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
// udp.h is a part of Blitzping.
// ---------------------------------------------------------------------

_Pragma ("once")
#ifndef UDP_H
#define UDP_H



struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t chksum;
};

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t chksum;
    uint16_t id;
    uint16_t seq;
};

#endif // UDP_H

// ---------------------------------------------------------------------
// END OF FILE: udp.h
// ---------------------------------------------------------------------

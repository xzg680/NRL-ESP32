#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Self-contained captive-portal DNS responder. Replies to every A query with
// the AP IP. Replaces the Arduino DNSServer used by wifi_config_portal.cpp.
//
// Usage:
//   NRL_CaptiveDNS_Start(htonl(0x0A0A1401)) // 10.10.20.1, in network byte order
//   ...
//   NRL_CaptiveDNS_Poll()   // call from the main loop; non-blocking
//   ...
//   NRL_CaptiveDNS_Stop()

// Start a non-blocking UDP listener on port 53 bound to INADDR_ANY. The
// `ap_ip` argument is the IPv4 address (network byte order — same format as
// sockaddr_in.sin_addr.s_addr and lwIP's ip4_addr_t::addr) returned in the A
// answer for every query.
bool NRL_CaptiveDNS_Start(uint32_t ap_ip);

// Drain any pending DNS queries and reply to each one with the cached AP IP.
// Returns the number of replies sent (0 if nothing was waiting).
int NRL_CaptiveDNS_Poll(void);

// Close the socket. Safe to call when not started.
void NRL_CaptiveDNS_Stop(void);

// True once Start() succeeded and Stop() hasn't been called.
bool NRL_CaptiveDNS_IsRunning(void);

#ifdef __cplusplus
}
#endif

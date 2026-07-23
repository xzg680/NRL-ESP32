#ifndef SRC_SERVICES_LAN_DISCOVERY_H
#define SRC_SERVICES_LAN_DISCOVERY_H

// Lightweight LAN discovery for the mini program. A client broadcasts the
// ASCII datagram "NRL_DISCOVER/1" to UDP port 60051 and each device replies
// directly to the sender with a compact JSON identity document.
void LAN_DISCOVERY_Init(void);
void LAN_DISCOVERY_Poll(void);

#endif // SRC_SERVICES_LAN_DISCOVERY_H

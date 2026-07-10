#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initialize the board Ethernet interface. On boards without NRL_HAS_ETHERNET
// this is an inexpensive no-op that returns true.
bool nrlEthernetInit();

bool nrlEthernetIsStarted();
bool nrlEthernetLinkUp();
bool nrlEthernetHasIp();
uint32_t nrlEthernetIp();


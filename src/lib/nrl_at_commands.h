#ifndef SRC_LIB_NRL_AT_COMMANDS_H
#define SRC_LIB_NRL_AT_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

constexpr size_t NRL_AT_REPLY_CAPACITY = 1072u;

struct NrlAtCommandResult {
    bool should_reply = false;
    bool restart_wifi = false;
    bool restart_udp = false;
    bool reboot = false;
    uint8_t payload[NRL_AT_REPLY_CAPACITY] = {};
    size_t payload_size = 0u;
};

void NRL_AT_HandlePayload(const uint8_t *payload,
                          size_t payload_size,
                          NrlAtCommandResult *result);

#endif // SRC_LIB_NRL_AT_COMMANDS_H

#pragma once

#include <stdint.h>

enum MainLoopProfileStage : uint8_t {
    MAIN_LOOP_STAGE_STATUS = 0,
    MAIN_LOOP_STAGE_WIFI_PORTAL,
    MAIN_LOOP_STAGE_BLE,
    MAIN_LOOP_STAGE_BT,
    MAIN_LOOP_STAGE_SERIAL,
    MAIN_LOOP_STAGE_DISPLAY,
    MAIN_LOOP_STAGE_COUNT,
};

struct MainLoopProfileSnapshot {
    bool enabled;
    uint32_t loops;
    uint64_t total_us[MAIN_LOOP_STAGE_COUNT];
    uint32_t max_us[MAIN_LOOP_STAGE_COUNT];
};

// Diagnostic-only wall-time profiler for the calls made by nrl_main_loop.
// AT+LOOP=1 resets/starts it, AT+LOOP=? reads it, and AT+LOOP=0 stops it.
void MAIN_LOOP_PROFILE_SetEnabled(bool enabled);
void MAIN_LOOP_PROFILE_Get(MainLoopProfileSnapshot *snapshot);

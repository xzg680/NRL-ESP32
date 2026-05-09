#include "status_io.h"

#include "board_pins.h"

#ifndef ENABLE_OPENCV
#include <Arduino.h>
#else
#include "../opencv/Arduino.hpp"
#endif

namespace {

constexpr unsigned long kSlowBlinkMs = 500UL;
constexpr unsigned long kHeartbeatMissedBlinkMs = 6500UL;

bool s_ptt_active = false;
unsigned long s_last_heartbeat_rx_ms = 0UL;
int s_last_sql1_level = -1;
int s_last_sql2_level = -1;

static bool blinkPhase(const unsigned long now_ms, const unsigned long period_ms)
{
    return ((now_ms / period_ms) & 1UL) == 0UL;
}

static void writeLed(const int pin, const bool on)
{
    digitalWrite(pin, on ? LOW : HIGH);
}

static bool sql1Active()
{
    return digitalRead(NRL_PIN_SQL1) == HIGH;
}

static bool sql2Active()
{
    return digitalRead(NRL_PIN_SQL2) == LOW;
}

} // namespace

extern "C" bool STATUS_IO_IsSqlActive(void)
{
    return sql1Active() || sql2Active();
}

extern "C" bool STATUS_IO_IsPttActive(void)
{
    return s_ptt_active;
}

extern "C" void STATUS_IO_Init(void)
{
    pinMode(NRL_PIN_PTT_OUT, OUTPUT);
    pinMode(NRL_PIN_STATUS_PTT_LED, OUTPUT);
    pinMode(NRL_PIN_STATUS_IO1, OUTPUT);
    pinMode(NRL_PIN_STATUS_IO2, OUTPUT);

    pinMode(NRL_PIN_SQL1, INPUT_PULLDOWN);
    pinMode(NRL_PIN_SQL2, INPUT_PULLUP);

    digitalWrite(NRL_PIN_PTT_OUT, LOW);
    writeLed(NRL_PIN_STATUS_PTT_LED, false);
    writeLed(NRL_PIN_STATUS_IO1, false);
    writeLed(NRL_PIN_STATUS_IO2, false);
}

extern "C" void STATUS_IO_SetPttActive(const bool active)
{
    if (s_ptt_active != active) {
        Serial.printf("[IO] ptt_out=%u\n", active ? 1u : 0u);
    }
    s_ptt_active = active;
    digitalWrite(NRL_PIN_PTT_OUT, active ? HIGH : LOW);
    writeLed(NRL_PIN_STATUS_PTT_LED, active);
}

extern "C" void STATUS_IO_NotifyHeartbeatReceived(void)
{
    s_last_heartbeat_rx_ms = millis();
}

extern "C" void STATUS_IO_Poll(void)
{
    const unsigned long now = millis();
    const int sql1_level = digitalRead(NRL_PIN_SQL1);
    const int sql2_level = digitalRead(NRL_PIN_SQL2);
    if (sql1_level != s_last_sql1_level || sql2_level != s_last_sql2_level) {
        Serial.printf("[IO] sql1=%u sql2=%u active=%u\n",
                      static_cast<unsigned>(sql1_level == HIGH ? 1u : 0u),
                      static_cast<unsigned>(sql2_level == HIGH ? 1u : 0u),
                      static_cast<unsigned>((sql1_level == HIGH || sql2_level == LOW) ? 1u : 0u));
        s_last_sql1_level = sql1_level;
        s_last_sql2_level = sql2_level;
    }

    const bool sql_active = (sql1_level == HIGH) || (sql2_level == LOW);
    const bool heartbeat_ok =
        s_last_heartbeat_rx_ms != 0UL && (now - s_last_heartbeat_rx_ms) <= kHeartbeatMissedBlinkMs;

    // IO1 is the blue status LED, used for server-alive indication.
    writeLed(NRL_PIN_STATUS_IO1, heartbeat_ok ? true : blinkPhase(now, kSlowBlinkMs));
    // IO2 is the green status LED, used for radio SQL indication.
    writeLed(NRL_PIN_STATUS_IO2, sql_active);

    // Keep these two outputs refreshed even if callers only update the latch state.
    digitalWrite(NRL_PIN_PTT_OUT, s_ptt_active ? HIGH : LOW);
    writeLed(NRL_PIN_STATUS_PTT_LED, s_ptt_active);
}

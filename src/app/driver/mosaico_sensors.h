#ifndef DRIVER_MOSAICO_SENSORS_H
#define DRIVER_MOSAICO_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct MosaicoSensorSnapshot {
    bool imu_present;
    bool imu_valid;
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;

    bool mag2_present;
    bool mag2_valid;
    bool mag3_present;
    bool mag3_valid;
    float mag2_x_ut;
    float mag2_y_ut;
    float mag2_z_ut;
    float mag3_x_ut;
    float mag3_y_ut;
    float mag3_z_ut;
    float heading_deg;  // from mag2 X/Y, normalized to 0-360

    bool gauge_present;
    bool gauge_valid;
    uint16_t battery_mv;
    int16_t battery_current_ma;
    uint8_t battery_soc_percent;
    bool battery_charging;

    uint32_t updated_ms;
};

// Starts the ESP-Mosaico sensor worker. Other boards provide harmless stubs.
bool MOSAICO_SENSORS_Init(void);
bool MOSAICO_SENSORS_GetSnapshot(MosaicoSensorSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_MOSAICO_SENSORS_H

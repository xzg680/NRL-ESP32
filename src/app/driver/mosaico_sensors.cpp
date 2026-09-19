#include "mosaico_sensors.h"

#include "board_pins.h"

#include <string.h>

#if NRL_BOARD == NRL_BOARD_ESP_MOSAICO

#include "i2c1.h"
#include "i2c_device_discovery.h"

#include <bmi270.h>

#ifndef BMI270_CHIP_ID
#error "espressif/bmi270 component header missing or incompatible (run idf.py reconfigure)"
#endif

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <math.h>

namespace {

constexpr const char *TAG = "MOSAICO_SNS";
constexpr uint32_t kImuMagPeriodMs = 50u;
constexpr uint32_t kGaugePeriodMs = 1000u;
constexpr uint32_t kSensorRetryPeriodMs = 10000u;

// BMM150 register map (register-level driver; two instances share the bus).
constexpr uint8_t kBmmChipIdReg = 0x40u;
constexpr uint8_t kBmmChipId = 0x32u;
constexpr uint8_t kBmmPowerControlReg = 0x4Bu;
constexpr uint8_t kBmmOpModeReg = 0x4Cu;
constexpr uint8_t kBmmRepXYReg = 0x51u;
constexpr uint8_t kBmmRepZReg = 0x52u;
constexpr uint8_t kBmmDataReg = 0x42u;
constexpr uint8_t kBmmTrimX1Y1Reg = 0x5Du;
constexpr uint8_t kBmmTrimZ4X2Y2Reg = 0x62u;
constexpr uint8_t kBmmTrimZ2TailReg = 0x68u;
// Regular preset: rep XY = 0x04 (nXY = 9), rep Z = 0x0E (nZ = 15), ODR 10 Hz.
constexpr uint8_t kBmmRepXYRegular = 0x04u;
constexpr uint8_t kBmmRepZRegular = 0x0Eu;
constexpr int16_t kBmmOverflowXY = -4096;
constexpr int16_t kBmmOverflowZ = -16384;

// BQ27220 standard commands (2-byte little-endian reads).
constexpr uint8_t kGaugeCmdVoltage = 0x08u;
constexpr uint8_t kGaugeCmdFlags = 0x0Au;
constexpr uint8_t kGaugeCmdCurrent = 0x0Cu;
constexpr uint8_t kGaugeCmdStateOfCharge = 0x2Cu;
constexpr uint16_t kGaugeFlagDischarging = 0x0001u;  // Flags bit0 DSG

struct Bmm150Trim {
    int8_t dig_x1;
    int8_t dig_y1;
    int8_t dig_x2;
    int8_t dig_y2;
    uint16_t dig_z1;
    int16_t dig_z2;
    int16_t dig_z3;
    int16_t dig_z4;
    uint8_t dig_xy1;
    int8_t dig_xy2;
    uint16_t dig_xyz1;
};

struct Bmm150 {
    uint8_t address;
    Bmm150Trim trim;
    bool have_sample;
    float x_ut;
    float y_ut;
    float z_ut;
};

MosaicoSensorSnapshot s_snapshot = {};
portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
bool s_started = false;
bmi270_handle_t *s_imu = nullptr;
Bmm150 s_mag2 = {NRL_BMM150_I2C_ADDR_2, {}, false, 0.0f, 0.0f, 0.0f};
Bmm150 s_mag3 = {NRL_BMM150_I2C_ADDR_3, {}, false, 0.0f, 0.0f, 0.0f};

uint16_t u16le(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

int16_t s16le(const uint8_t *p)
{
    return static_cast<int16_t>(u16le(p));
}

bool readRegisters(const uint8_t address, const uint8_t reg,
                   uint8_t *data, const size_t size)
{
    return I2C_MasterTransmitReceive(address, &reg, 1u, data, size, 100);
}

bool writeRegister(const uint8_t address, const uint8_t reg, const uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return I2C_MasterTransmit(address, data, sizeof(data), 100);
}

// BMI270 (espressif/bmi270 low-level driver on the shared I2C bus).

bool initBmi270()
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!I2C_MasterGetBus(&bus) || bus == nullptr) return false;
    // Field-by-field assignment avoids -Werror=missing-field-initializers on
    // the C designated-initializer style of the component README.
    bmi270_driver_config_t driver_config = {};
    driver_config.addr = NRL_BMI270_I2C_ADDR;
    driver_config.interface = BMI270_USE_I2C;
    driver_config.i2c_bus = bus;
    bmi270_handle_t *handle = nullptr;
    if (bmi270_create(&driver_config, &handle) != ESP_OK || handle == nullptr) {
        return false;
    }
    bmi270_config_t config = {};
    config.acce_odr = BMI270_ACC_ODR_100_HZ;
    config.acce_range = BMI270_ACC_RANGE_4_G;
    config.gyro_odr = BMI270_GYR_ODR_100_HZ;
    config.gyro_range = BMI270_GYR_RANGE_1000_DPS;
    if (bmi270_start(handle, &config) != ESP_OK) {
        (void)bmi270_delete(handle);
        return false;
    }
    s_imu = handle;
    ESP_LOGI(TAG, "BMI270 ready at 0x%02X", NRL_BMI270_I2C_ADDR);
    return true;
}

void releaseBmi270()
{
    if (s_imu != nullptr) {
        (void)bmi270_stop(s_imu);
        (void)bmi270_delete(s_imu);
        s_imu = nullptr;
    }
}

bool readBmi270(float *accel_g, float *gyro_dps)
{
    if (s_imu == nullptr) return false;
    return bmi270_get_acce_data(s_imu, &accel_g[0], &accel_g[1], &accel_g[2]) == ESP_OK &&
           bmi270_get_gyro_data(s_imu, &gyro_dps[0], &gyro_dps[1], &gyro_dps[2]) == ESP_OK;
}

// BMM150 x2.

bool initBmm150(Bmm150 *mag)
{
    if (!I2C_MasterProbe(mag->address, 100)) return false;
    // Power control bit: suspend -> sleep, required before the chip id and
    // trim registers answer (both read 0x00 in suspend mode).
    if (!writeRegister(mag->address, kBmmPowerControlReg, 0x01u)) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t id = 0u;
    if (!readRegisters(mag->address, kBmmChipIdReg, &id, 1u) || id != kBmmChipId) {
        return false;
    }

    uint8_t trim_x1y1[2] = {};
    uint8_t trim_z4x2y2[4] = {};
    uint8_t trim_z2_tail[10] = {};
    if (!readRegisters(mag->address, kBmmTrimX1Y1Reg, trim_x1y1, sizeof(trim_x1y1)) ||
        !readRegisters(mag->address, kBmmTrimZ4X2Y2Reg, trim_z4x2y2, sizeof(trim_z4x2y2)) ||
        !readRegisters(mag->address, kBmmTrimZ2TailReg, trim_z2_tail, sizeof(trim_z2_tail))) {
        return false;
    }
    mag->trim.dig_x1 = static_cast<int8_t>(trim_x1y1[0]);
    mag->trim.dig_y1 = static_cast<int8_t>(trim_x1y1[1]);
    mag->trim.dig_z4 = s16le(trim_z4x2y2 + 0);
    mag->trim.dig_x2 = static_cast<int8_t>(trim_z4x2y2[2]);
    mag->trim.dig_y2 = static_cast<int8_t>(trim_z4x2y2[3]);
    mag->trim.dig_z2 = s16le(trim_z2_tail + 0);
    mag->trim.dig_z1 = u16le(trim_z2_tail + 2);
    mag->trim.dig_xyz1 = static_cast<uint16_t>(
        (static_cast<uint16_t>(trim_z2_tail[5] & 0x7Fu) << 8u) | trim_z2_tail[4]);
    mag->trim.dig_z3 = s16le(trim_z2_tail + 6);
    mag->trim.dig_xy2 = static_cast<int8_t>(trim_z2_tail[8]);
    mag->trim.dig_xy1 = trim_z2_tail[9];

    // Normal mode (op-mode bits 0b00, ODR 10 Hz) with the regular preset.
    if (!writeRegister(mag->address, kBmmOpModeReg, 0x00u) ||
        !writeRegister(mag->address, kBmmRepXYReg, kBmmRepXYRegular) ||
        !writeRegister(mag->address, kBmmRepZReg, kBmmRepZRegular)) {
        return false;
    }
    mag->have_sample = false;
    ESP_LOGI(TAG, "BMM150 ready at 0x%02X", mag->address);
    return true;
}

// Bosch BMM150_SensorAPI floating-point compensation (bmm150.c compensate_x/y/z).
float compensateBmmXY(const Bmm150Trim *trim, const int16_t raw,
                      const uint16_t rhall, const bool is_x, bool *ok)
{
    if (raw == kBmmOverflowXY || rhall == 0u || trim->dig_xyz1 == 0u) {
        *ok = false;
        return 0.0f;
    }
    const float partial0 = static_cast<float>(trim->dig_xyz1) * 16384.0f / rhall;
    const float partial = partial0 - 16384.0f;
    const float partial1 = static_cast<float>(trim->dig_xy2) *
                           (partial * partial / 268435456.0f);
    const float partial2 = partial1 + partial * static_cast<float>(trim->dig_xy1) / 16384.0f;
    const float partial3 = static_cast<float>(is_x ? trim->dig_x2 : trim->dig_y2) + 160.0f;
    const float partial4 = raw * ((partial2 + 256.0f) * partial3);
    const int8_t offset = is_x ? trim->dig_x1 : trim->dig_y1;
    *ok = true;
    return (partial4 / 8192.0f + static_cast<float>(offset) * 8.0f) / 16.0f;
}

float compensateBmmZ(const Bmm150Trim *trim, const int16_t raw,
                     const uint16_t rhall, bool *ok)
{
    if (raw == kBmmOverflowZ || trim->dig_z2 == 0 || trim->dig_z1 == 0u ||
        trim->dig_xyz1 == 0u || rhall == 0u) {
        *ok = false;
        return 0.0f;
    }
    const float partial0 = static_cast<float>(raw) - static_cast<float>(trim->dig_z4);
    const float partial1 = static_cast<float>(rhall) - static_cast<float>(trim->dig_xyz1);
    const float partial2 = static_cast<float>(trim->dig_z3) * partial1;
    const float partial3 = static_cast<float>(trim->dig_z1) *
                           static_cast<float>(rhall) / 32768.0f;
    const float partial4 = static_cast<float>(trim->dig_z2) + partial3;
    const float partial5 = (partial0 * 131072.0f) - partial2;
    *ok = true;
    return (partial5 / (partial4 * 4.0f)) / 16.0f;
}

bool readBmm150(Bmm150 *mag)
{
    uint8_t raw[8] = {};
    if (!readRegisters(mag->address, kBmmDataReg, raw, sizeof(raw))) return false;
    // DRDY is bit0 of the RHALL LSB register (0x48): at 10 Hz ODR most 50 ms
    // polls see no new sample; keep the last compensated values then.
    if ((raw[6] & 0x01u) == 0u) return mag->have_sample;
    const int16_t raw_x = static_cast<int16_t>(s16le(raw + 0) >> 3);
    const int16_t raw_y = static_cast<int16_t>(s16le(raw + 2) >> 3);
    const int16_t raw_z = static_cast<int16_t>(s16le(raw + 4) >> 1);
    const uint16_t rhall = static_cast<uint16_t>(u16le(raw + 6) >> 2);
    bool ok_x = false;
    bool ok_y = false;
    bool ok_z = false;
    const float x_ut = compensateBmmXY(&mag->trim, raw_x, rhall, true, &ok_x);
    const float y_ut = compensateBmmXY(&mag->trim, raw_y, rhall, false, &ok_y);
    const float z_ut = compensateBmmZ(&mag->trim, raw_z, rhall, &ok_z);
    if (!ok_x || !ok_y || !ok_z) return false;
    mag->x_ut = x_ut;
    mag->y_ut = y_ut;
    mag->z_ut = z_ut;
    mag->have_sample = true;
    return true;
}

// BQ27220 fuel gauge.

bool initGauge()
{
    return I2C_MasterProbe(NRL_BQ27220_I2C_ADDR, 100);
}

bool readGauge(uint16_t *voltage_mv, int16_t *current_ma,
               uint8_t *soc_percent, bool *charging)
{
    uint8_t buf[2] = {};
    if (!readRegisters(NRL_BQ27220_I2C_ADDR, kGaugeCmdVoltage, buf, sizeof(buf))) {
        return false;
    }
    const uint16_t voltage = u16le(buf);
    if (!readRegisters(NRL_BQ27220_I2C_ADDR, kGaugeCmdFlags, buf, sizeof(buf))) {
        return false;
    }
    const uint16_t flags = u16le(buf);
    if (!readRegisters(NRL_BQ27220_I2C_ADDR, kGaugeCmdCurrent, buf, sizeof(buf))) {
        return false;
    }
    const int16_t current = s16le(buf);
    if (!readRegisters(NRL_BQ27220_I2C_ADDR, kGaugeCmdStateOfCharge, buf, sizeof(buf))) {
        return false;
    }
    const uint16_t soc = u16le(buf);
    if (voltage == 0u || soc > 100u) return false;
    *voltage_mv = voltage;
    *current_ma = current;
    *soc_percent = static_cast<uint8_t>(soc);
    *charging = (flags & kGaugeFlagDischarging) == 0u;
    return true;
}

void sensorTask(void *)
{
    MosaicoSensorSnapshot local = {};
    uint32_t discovery_revision = UINT32_MAX;
    uint32_t last_gauge_ms = 0u;
    uint32_t last_retry_ms = 0u;
    for (;;) {
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        const uint32_t current_revision = I2C_DEVICE_DISCOVERY_GetRevision();
        if (current_revision != discovery_revision) {
            discovery_revision = current_revision;
            local = {};
            releaseBmi270();
            local.imu_present = initBmi270();
            local.mag2_present = initBmm150(&s_mag2);
            local.mag3_present = initBmm150(&s_mag3);
            local.gauge_present = initGauge();
            last_gauge_ms = 0u;
            last_retry_ms = now;
        } else if ((!local.imu_present || !local.mag2_present ||
                    !local.mag3_present || !local.gauge_present) &&
                   now - last_retry_ms >= kSensorRetryPeriodMs) {
            last_retry_ms = now;
            if (!local.imu_present) local.imu_present = initBmi270();
            if (!local.mag2_present) local.mag2_present = initBmm150(&s_mag2);
            if (!local.mag3_present) local.mag3_present = initBmm150(&s_mag3);
            if (!local.gauge_present) local.gauge_present = initGauge();
        }
        if (local.imu_present) {
            float accel_g[3] = {};
            float gyro_dps[3] = {};
            local.imu_valid = readBmi270(accel_g, gyro_dps);
            if (local.imu_valid) {
                local.accel_x_g = accel_g[0];
                local.accel_y_g = accel_g[1];
                local.accel_z_g = accel_g[2];
                local.gyro_x_dps = gyro_dps[0];
                local.gyro_y_dps = gyro_dps[1];
                local.gyro_z_dps = gyro_dps[2];
            }
        }
        if (local.mag2_present) {
            local.mag2_valid = readBmm150(&s_mag2);
            if (local.mag2_valid) {
                local.mag2_x_ut = s_mag2.x_ut;
                local.mag2_y_ut = s_mag2.y_ut;
                local.mag2_z_ut = s_mag2.z_ut;
                constexpr float kRadiansToDegrees = 57.2957795131f;
                float heading = atan2f(s_mag2.y_ut, s_mag2.x_ut) * kRadiansToDegrees;
                if (heading < 0.0f) heading += 360.0f;
                local.heading_deg = heading;
            }
        }
        if (local.mag3_present) {
            local.mag3_valid = readBmm150(&s_mag3);
            if (local.mag3_valid) {
                local.mag3_x_ut = s_mag3.x_ut;
                local.mag3_y_ut = s_mag3.y_ut;
                local.mag3_z_ut = s_mag3.z_ut;
            }
        }
        if (last_gauge_ms == 0u || now - last_gauge_ms >= kGaugePeriodMs) {
            last_gauge_ms = now;
            if (local.gauge_present) {
                local.gauge_valid = readGauge(&local.battery_mv,
                                              &local.battery_current_ma,
                                              &local.battery_soc_percent,
                                              &local.battery_charging);
            }
        }
        local.updated_ms = now;
        portENTER_CRITICAL(&s_snapshot_lock);
        s_snapshot = local;
        portEXIT_CRITICAL(&s_snapshot_lock);
        vTaskDelay(pdMS_TO_TICKS(kImuMagPeriodMs));
    }
}

} // namespace

bool MOSAICO_SENSORS_Init(void)
{
    if (s_started) return true;
    s_started = xTaskCreate(sensorTask, "mosaico_sensors", 4096, nullptr, 3, nullptr) == pdPASS;
    if (!s_started) ESP_LOGE(TAG, "failed to create sensor task");
    return s_started;
}

bool MOSAICO_SENSORS_GetSnapshot(MosaicoSensorSnapshot *out)
{
    if (out == nullptr) return false;
    portENTER_CRITICAL(&s_snapshot_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
    return s_started;
}

#else

bool MOSAICO_SENSORS_Init(void) { return true; }
bool MOSAICO_SENSORS_GetSnapshot(MosaicoSensorSnapshot *out)
{
    if (out != nullptr) memset(out, 0, sizeof(*out));
    return false;
}

#endif

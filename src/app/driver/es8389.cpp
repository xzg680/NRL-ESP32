#include "driver/es8389.h"

#include "driver/audio_passthrough.h"
#include "driver/board_pins.h"
#include "driver/s31_i2c.h"

#if defined(NRL_AUDIO_CODEC_ES8389) && NRL_AUDIO_CODEC_ES8389

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_err.h>
#include <esp_log.h>

static const char *TAG = "ES8389";

namespace {

constexpr int kI2sPort = I2S_NUM_0;
constexpr int kSampleRate = 16000;
constexpr int kChannels = 2;
constexpr int kChannelMask = 0x03;
constexpr int kDefaultOutVolume = 70;
constexpr float kDefaultInGain = 30.0f;

static bool s_es8389_ready = false;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static const audio_codec_ctrl_if_t *s_ctrl_if = nullptr;
static const audio_codec_data_if_t *s_data_if = nullptr;
static const audio_codec_gpio_if_t *s_gpio_if = nullptr;
static esp_codec_dev_handle_t s_codec = nullptr;

static bool es8389_init_i2c(void) {
    if (s_i2c_bus != nullptr) {
        return true;
    }

    if (!S31_I2C_GetBus(&s_i2c_bus)) {
        ESP_LOGE(TAG, "shared I2C bus unavailable");
        return false;
    }
    return true;
}

static bool es8389_create_codec(i2s_chan_handle_t tx_handle, i2s_chan_handle_t rx_handle) {
    if (s_codec != nullptr) {
        return true;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = ES8389_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = s_i2c_bus;
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == nullptr) {
        ESP_LOGE(TAG, "create I2C control interface failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = static_cast<uint8_t>(kI2sPort);
    i2s_cfg.tx_handle = tx_handle;
    i2s_cfg.rx_handle = rx_handle;
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == nullptr) {
        ESP_LOGE(TAG, "create I2S data interface failed");
        return false;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == nullptr) {
        ESP_LOGE(TAG, "create GPIO interface failed");
        return false;
    }

    esp_codec_dev_hw_gain_t gain = {};
    gain.pa_voltage = 5.0f;
    gain.codec_dac_voltage = 3.3f;

    es8389_codec_cfg_t es8389_cfg = {};
    es8389_cfg.ctrl_if = s_ctrl_if;
    es8389_cfg.gpio_if = s_gpio_if;
    es8389_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8389_cfg.pa_pin = static_cast<int16_t>(NRL_PIN_PA_EN);
    es8389_cfg.pa_reverted = false;
    es8389_cfg.master_mode = false;
    es8389_cfg.use_mclk = false;
    es8389_cfg.digital_mic = false;
    es8389_cfg.invert_mclk = false;
    es8389_cfg.invert_sclk = false;
    es8389_cfg.hw_gain = gain;
    es8389_cfg.no_dac_ref = false;

    const audio_codec_if_t *codec_if = es8389_codec_new(&es8389_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(TAG, "create ES8389 codec interface failed");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = s_data_if;
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (s_codec == nullptr) {
        ESP_LOGE(TAG, "create codec device failed");
        return false;
    }

    return true;
}

static bool es8389_open_codec(void) {
    esp_codec_dev_sample_info_t sample_cfg = {};
    sample_cfg.bits_per_sample = 16;
    sample_cfg.channel = kChannels;
    sample_cfg.channel_mask = kChannelMask;
    sample_cfg.sample_rate = kSampleRate;

    if (esp_codec_dev_open(s_codec, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open codec failed");
        return false;
    }
    if (esp_codec_dev_set_out_vol(s_codec, kDefaultOutVolume) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set output volume failed");
        return false;
    }
    if (esp_codec_dev_set_in_gain(s_codec, kDefaultInGain) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set input gain failed");
        return false;
    }
    return true;
}

} // namespace

extern "C" bool ES8389_Init(void) {
    if (s_es8389_ready) {
        return AUDIO_StartPassthrough();
    }

    AUDIO_SetMode(AUDIO_MODE_RECEIVE);

    if (NRL_PIN_PA_EN >= 0) {
        gpio_reset_pin(static_cast<gpio_num_t>(NRL_PIN_PA_EN));
        gpio_set_direction(static_cast<gpio_num_t>(NRL_PIN_PA_EN), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(NRL_PIN_PA_EN), 1);
    }

    if (!AUDIO_SetupI2S()) {
        ESP_LOGE(TAG, "I2S setup failed");
        return false;
    }

    i2s_chan_handle_t tx_handle = nullptr;
    i2s_chan_handle_t rx_handle = nullptr;
    if (!AUDIO_GetI2SHandles(&tx_handle, &rx_handle)) {
        ESP_LOGE(TAG, "I2S handles unavailable");
        return false;
    }

    if (!es8389_init_i2c() || !es8389_create_codec(tx_handle, rx_handle) || !es8389_open_codec()) {
        return false;
    }

    if (!AUDIO_StartPassthrough()) {
        ESP_LOGE(TAG, "start passthrough failed");
        return false;
    }

    s_es8389_ready = true;
    ESP_LOGI(TAG, "ready: i2c=0x%02X", ES8389_CODEC_DEFAULT_ADDR);
    return true;
}

extern "C" bool ES8389_IsReady(void) {
    return s_es8389_ready;
}

extern "C" bool ES8389_SetReceiveMode(void) {
    return ES8389_Init();
}

extern "C" bool ES8389_SetOutputVolume(const uint8_t value) {
    if (s_codec == nullptr) {
        return false;
    }
    const int percent = (static_cast<int>(value) * 100 + 127) / 255;
    return esp_codec_dev_set_out_vol(s_codec, percent) == ESP_CODEC_DEV_OK;
}

#else

extern "C" bool ES8389_Init(void) {
    return false;
}

extern "C" bool ES8389_IsReady(void) {
    return false;
}

extern "C" bool ES8389_SetReceiveMode(void) {
    return false;
}

extern "C" bool ES8389_SetOutputVolume(uint8_t) {
    return false;
}

#endif

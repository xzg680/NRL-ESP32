#include "wifi_config_portal_view.h"

#include "wifi_config_portal_page.generated.h"
#include "wifi_config_portal_sections.generated.h"
#include "wifi_update_portal_page.generated.h"
#include "nrl_version.h"
#include "../app/driver/board_pins.h"

#include <WiFi.h>

#include <IPAddress.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr unsigned long kDacEqCoefficientMax = 1073741823UL;

static String formatHex32(const uint32_t value)
{
    char buffer[12];
    snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(value));
    return String(buffer);
}

static String htmlEscape(const char *text)
{
    String out;
    if (text == nullptr) {
        return out;
    }

    for (size_t i = 0; text[i] != '\0'; ++i) {
        switch (text[i]) {
            case '&':
                out += F("&amp;");
                break;
            case '<':
                out += F("&lt;");
                break;
            case '>':
                out += F("&gt;");
                break;
            case '"':
                out += F("&quot;");
                break;
            default:
                out += text[i];
                break;
        }
    }
    return out;
}

static void replaceToken(String &html, const char *token, const String &value)
{
    html.replace(token, value);
}

static String buildPageWithFormSections(const String &form_sections)
{
    static const char kFormSectionsToken[] = "{{FORM_SECTIONS}}";
    const char *template_html = kWifiConfigPortalHtmlTemplate;
    const char *token = strstr(template_html, kFormSectionsToken);
    if (token == nullptr) {
        return String(template_html);
    }

    const size_t token_len = strlen(kFormSectionsToken);
    const size_t prefix_len = static_cast<size_t>(token - template_html);
    const size_t template_len = strlen(template_html);

    String html;
    html.reserve(template_len - token_len + form_sections.length() + 64);
    html.concat(template_html, prefix_len);
    html += form_sections;
    html += token + token_len;
    return html;
}

static String checkedAttr(const bool checked)
{
    return checked ? String("checked") : String("");
}

static String boolValue(const bool enabled)
{
    return enabled ? String("1") : String("0");
}

static String ipToString(const uint32_t value)
{
    return IPAddress(value).toString();
}

static String wifiDisplayIp(const ExternalRadioConfig *config,
                            const uint32_t configured_value,
                            const IPAddress &dhcp_value)
{
    if (config != nullptr && config->wifi_dhcp_enabled && WiFi.status() == WL_CONNECTED) {
        return dhcp_value.toString();
    }
    return ipToString(configured_value);
}

static String buildDacEqSlider(const char *field_name,
                               const char *label,
                               const char *i18n_key,
                               const uint32_t value)
{
    String html = String(kWifiConfigPortalEqSliderTemplate);
    String i18n_attr;
    if (i18n_key != nullptr && i18n_key[0] != '\0') {
        i18n_attr = String(" data-i18n=\"") + i18n_key + "\"";
    }
    replaceToken(html, "{{I18N_ATTR}}", i18n_attr);
    replaceToken(html, "{{LABEL}}", label);
    replaceToken(html, "{{FIELD}}", field_name);
    replaceToken(html, "{{MAX}}", String(kDacEqCoefficientMax));
    replaceToken(html, "{{VALUE}}", String(value));
    replaceToken(html, "{{HEX_VALUE}}", formatHex32(value));
    return html;
}

static String buildAutoSubmitSlider(const char *field_name,
                                    const char *label,
                                    const char *i18n_key,
                                    const uint32_t min_value,
                                    const uint32_t max_value,
                                    const uint32_t value)
{
    String html = String(kWifiConfigPortalAutoSliderTemplate);
    String i18n_attr;
    if (i18n_key != nullptr && i18n_key[0] != '\0') {
        i18n_attr = String(" data-i18n=\"") + i18n_key + "\"";
    }
    replaceToken(html, "{{I18N_ATTR}}", i18n_attr);
    replaceToken(html, "{{LABEL}}", label);
    replaceToken(html, "{{FIELD}}", field_name);
    replaceToken(html, "{{MIN}}", String(min_value));
    replaceToken(html, "{{MAX}}", String(max_value));
    replaceToken(html, "{{VALUE}}", String(value));
    return html;
}

} // namespace

String WifiConfigPortalView_BuildNetworkSection(const ExternalRadioConfig *config,
                                                const WifiConfigPortalScanEntry *scan_entries,
                                                const size_t scan_count)
{
    String wifi_options;
    bool current_ssid_found = false;
    for (size_t i = 0; i < scan_count; ++i) {
        const String escaped = htmlEscape(scan_entries[i].ssid.c_str());
        String option = String(kWifiConfigPortalWifiOptionTemplate);
        replaceToken(option, "{{VALUE}}", escaped);
        if (strcmp(scan_entries[i].ssid.c_str(), config->wifi_ssid) == 0) {
            current_ssid_found = true;
            replaceToken(option, "{{SELECTED}}", " selected");
        }
        replaceToken(option, "{{SELECTED}}", "");
        replaceToken(option, "{{LABEL}}", escaped + " (" + String(scan_entries[i].rssi) + " dBm)");
        wifi_options += option;
    }
    if (!current_ssid_found && config->wifi_ssid[0] != '\0') {
        const String escaped = htmlEscape(config->wifi_ssid);
        String option = String(kWifiConfigPortalWifiOptionTemplate);
        replaceToken(option, "{{VALUE}}", escaped);
        replaceToken(option, "{{SELECTED}}", " selected");
        replaceToken(option, "{{LABEL}}", escaped);
        wifi_options += option;
    }
    String html = String(kWifiConfigPortalNetworkSectionTemplate);
    replaceToken(html, "{{WIFI_OPTIONS}}", wifi_options);
    replaceToken(html, "{{WIFI_PASSWORD}}", htmlEscape(config->wifi_password));
    replaceToken(html, "{{DHCP_CHECKED}}", checkedAttr(config->wifi_dhcp_enabled));
    replaceToken(html, "{{WIFI_IP}}", wifiDisplayIp(config, config->wifi_ip, WiFi.localIP()));
    replaceToken(html, "{{WIFI_MASK}}", wifiDisplayIp(config, config->wifi_netmask, WiFi.subnetMask()));
    replaceToken(html, "{{WIFI_GATEWAY}}", wifiDisplayIp(config, config->wifi_gateway, WiFi.gatewayIP()));
    replaceToken(html, "{{WIFI_DNS}}", wifiDisplayIp(config, config->wifi_dns, WiFi.dnsIP()));
    return html;
}

String WifiConfigPortalView_BuildDeviceSections(const ExternalRadioConfig *config)
{
    String html = String(kWifiConfigPortalDeviceSectionsTemplate);
    replaceToken(html, "{{SERVER_HOST}}", htmlEscape(config->server_host));
    replaceToken(html, "{{SERVER_PORT}}", String(config->server_port));
    replaceToken(html, "{{CHANNEL}}", String(config->channel));
    replaceToken(html, "{{CALLSIGN}}", htmlEscape(config->callsign));
    replaceToken(html, "{{CALLSIGN_SSID}}", String(config->callsign_ssid));
    replaceToken(html, "{{PTT_TIMEOUT}}", String(config->ptt_timeout_s));
    return html;
}

String WifiConfigPortalView_BuildAudioSections(const ExternalRadioConfig *config)
{
    String html = String(kWifiConfigPortalAudioSectionsTemplate);
    html.reserve(strlen(kWifiConfigPortalAudioSectionsTemplate) + 9000u);
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    String aec_section = String(kWifiConfigPortalAecSectionTemplate);
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    String aec_toggle = String(kWifiConfigPortalAecToggleTemplate);
    replaceToken(aec_toggle, "{{AEC_CHECKED}}", checkedAttr(config->aec_enabled));
    replaceToken(aec_toggle, "{{AEC_REF_NETWORK_SELECTED}}",
                 (config->aec_reference_source == EXTERNAL_RADIO_AEC_REF_NETWORK) ? " selected" : "");
    replaceToken(aec_toggle, "{{AEC_REF_MIC_SELECTED}}",
#if NRL_HAS_ES7210
                 (config->aec_reference_source == EXTERNAL_RADIO_AEC_REF_MIC) ? " selected" : ""
#else
                 " disabled"
#endif
    );
    replaceToken(aec_section, "{{AFE_GROUP_I18N}}", "aec");
    replaceToken(aec_section, "{{AFE_GROUP_LABEL}}", "AEC / AI");
#else
    String aec_toggle = "";
    replaceToken(aec_section, "{{AFE_GROUP_I18N}}", "aiNoiseLabel");
    replaceToken(aec_section, "{{AFE_GROUP_LABEL}}", "AI Noise Reduction");
#endif
    replaceToken(aec_section, "{{AEC_TOGGLE}}", aec_toggle);
    replaceToken(aec_section, "{{AI_NOISE_CHECKED}}", checkedAttr(config->ai_noise_enabled));
#endif
    replaceToken(html, "{{AEC_SECTION}}",
#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
                 aec_section
#else
                 ""
#endif
    );
    replaceToken(html, "{{ES8311_ADC_SECTION}}",
#if NRL_HAS_ES7210
                 ""
#else
                 String(kWifiConfigPortalEs8311AdcSectionTemplate)
#endif
    );
    replaceToken(html, "{{HP_DRIVE_CHECKED}}", checkedAttr(config->hp_drive_enabled));
    replaceToken(html, "{{MIC_VOLUME_SLIDER}}", buildAutoSubmitSlider("mic_volume", "Mic Volume (0-255)", "micVolume", 0u, 255u, config->mic_volume));
    replaceToken(html, "{{LINE_OUT_VOLUME_SLIDER}}", buildAutoSubmitSlider("line_out_volume", "Line Out Volume (0-255)", "lineOutVolume", 0u, 255u, config->line_out_volume));
    replaceToken(html, "{{ADC_DMIC_CHECKED}}", checkedAttr(config->adc_dmic_enabled));
    replaceToken(html, "{{ADC_DMIC_VALUE}}", boolValue(config->adc_dmic_enabled));
    replaceToken(html, "{{ADC_LINSEL_CHECKED}}", checkedAttr(config->adc_linsel));
    replaceToken(html, "{{ADC_LINSEL_VALUE}}", boolValue(config->adc_linsel));
    replaceToken(html, "{{ADC_PGA_GAIN}}", String(config->adc_pga_gain));
    replaceToken(html, "{{ADC_RAMPRATE}}", String(config->adc_ramprate));
    replaceToken(html, "{{ADC_SCALE}}", String(config->adc_scale));
    replaceToken(html, "{{ADC_PGA_GAIN_SLIDER}}", buildAutoSubmitSlider("adc_pga_gain", "ADC PGA Gain (0-10)", "adcPgaGain", 0u, 10u, config->adc_pga_gain));
    replaceToken(html, "{{ADC_RAMPRATE_SLIDER}}", buildAutoSubmitSlider("adc_ramprate", "ADC VC Ramp Rate (0-15)", "adcRampRate", 0u, 15u, config->adc_ramprate));
    replaceToken(html, "{{ADC_SCALE_SLIDER}}", buildAutoSubmitSlider("adc_scale", "ADC Gain Scale (0-7)", "adcGainScale", 0u, 7u, config->adc_scale));
    replaceToken(html, "{{ADC_DMIC_SENSE_CHECKED}}", checkedAttr(config->adc_dmic_sense));
    replaceToken(html, "{{ADC_SYNC_CHECKED}}", checkedAttr(config->adc_sync));
    replaceToken(html, "{{ADC_SYNC_VALUE}}", boolValue(config->adc_sync));
    replaceToken(html, "{{ADC_INV_CHECKED}}", checkedAttr(config->adc_inv));
    replaceToken(html, "{{ADC_INV_VALUE}}", boolValue(config->adc_inv));
    replaceToken(html, "{{ADC_RAMCLR_CHECKED}}", checkedAttr(config->adc_ramclr));
    replaceToken(html, "{{ADC_RAMCLR_VALUE}}", boolValue(config->adc_ramclr));
    replaceToken(html, "{{ALC_CHECKED}}", checkedAttr(config->alc_enabled));
    replaceToken(html, "{{ALC_ENABLED_VALUE}}", boolValue(config->alc_enabled));
    replaceToken(html, "{{ADC_AUTOMUTE_CHECKED}}", checkedAttr(config->adc_automute_enabled));
    replaceToken(html, "{{ADC_AUTOMUTE_ENABLED_VALUE}}", boolValue(config->adc_automute_enabled));
    replaceToken(html, "{{ALC_WINSIZE_SLIDER}}", buildAutoSubmitSlider("alc_winsize", "ALC Window Size (0-15)", "alcWindowSize", 0u, 15u, config->alc_winsize));
    replaceToken(html, "{{ALC_MAXLEVEL_SLIDER}}", buildAutoSubmitSlider("alc_maxlevel", "ALC Max Level (0-15)", "alcMaxLevel", 0u, 15u, config->alc_maxlevel));
    replaceToken(html, "{{ALC_MINLEVEL_SLIDER}}", buildAutoSubmitSlider("alc_minlevel", "ALC Min Level (0-15)", "alcMinLevel", 0u, 15u, config->alc_minlevel));
    replaceToken(html, "{{ADC_AUTOMUTE_WINSIZE_SLIDER}}", buildAutoSubmitSlider("adc_automute_winsize", "ADC Automute Window (0-15)", "adcAutomuteWindow", 0u, 15u, config->adc_automute_winsize));
    replaceToken(html, "{{ADC_AUTOMUTE_NOISE_GATE_SLIDER}}", buildAutoSubmitSlider("adc_automute_noise_gate", "ADC Automute Noise Gate (0-15)", "adcAutomuteNoiseGate", 0u, 15u, config->adc_automute_noise_gate));
    replaceToken(html, "{{ADC_AUTOMUTE_VOLUME_SLIDER}}", buildAutoSubmitSlider("adc_automute_volume", "ADC Automute Volume (0-7)", "adcAutomuteVolume", 0u, 7u, config->adc_automute_volume));
    replaceToken(html, "{{ADC_HPFS1}}", String(config->adc_hpfs1));
    replaceToken(html, "{{ADC_HPFS2}}", String(config->adc_hpfs2));
    replaceToken(html, "{{ADC_HPFS1_SLIDER}}", buildAutoSubmitSlider("adc_hpfs1", "ADC HPF Stage 1 (0-31)", "adcHpfStage1", 0u, 31u, config->adc_hpfs1));
    replaceToken(html, "{{ADC_HPFS2_SLIDER}}", buildAutoSubmitSlider("adc_hpfs2", "ADC HPF Stage 2 (0-31)", "adcHpfStage2", 0u, 31u, config->adc_hpfs2));
    replaceToken(html, "{{ADC_EQ_BYPASS_CHECKED}}", checkedAttr(config->adc_eq_bypass));
    replaceToken(html, "{{ADC_EQ_BYPASS_VALUE}}", boolValue(config->adc_eq_bypass));
    replaceToken(html, "{{ADC_HPF_CHECKED}}", checkedAttr(config->adc_hpf));
    replaceToken(html, "{{ADC_HPF_VALUE}}", boolValue(config->adc_hpf));
    replaceToken(html, "{{MIC_HPF_CHECKED}}", checkedAttr(config->mic_hpf_enabled));
    replaceToken(html, "{{ADCEQ_B0_SLIDER}}", buildDacEqSlider("adceq_b0", "ADCEQ B0 (REG1D-20)", "adceqB0", config->adceq_b0));
    replaceToken(html, "{{ADCEQ_A1_SLIDER}}", buildDacEqSlider("adceq_a1", "ADCEQ A1 (REG21-24)", "adceqA1", config->adceq_a1));
    replaceToken(html, "{{ADCEQ_A2_SLIDER}}", buildDacEqSlider("adceq_a2", "ADCEQ A2 (REG25-28)", "adceqA2", config->adceq_a2));
    replaceToken(html, "{{ADCEQ_B1_SLIDER}}", buildDacEqSlider("adceq_b1", "ADCEQ B1 (REG29-2C)", "adceqB1", config->adceq_b1));
    replaceToken(html, "{{ADCEQ_B2_SLIDER}}", buildDacEqSlider("adceq_b2", "ADCEQ B2 (REG2D-30)", "adceqB2", config->adceq_b2));
    replaceToken(html, "{{DRC_CHECKED}}", checkedAttr(config->drc_enabled));
    replaceToken(html, "{{DRC_WINSIZE_SLIDER}}", buildAutoSubmitSlider("drc_winsize", "DRC Window Size (0-15)", "drcWindowSize", 0u, 15u, config->drc_winsize));
    replaceToken(html, "{{DRC_MAXLEVEL_SLIDER}}", buildAutoSubmitSlider("drc_maxlevel", "DRC Max Level (0-15)", "drcMaxLevel", 0u, 15u, config->drc_maxlevel));
    replaceToken(html, "{{DRC_MINLEVEL_SLIDER}}", buildAutoSubmitSlider("drc_minlevel", "DRC Min Level (0-15)", "drcMinLevel", 0u, 15u, config->drc_minlevel));
    replaceToken(html, "{{DAC_RAMPRATE_SLIDER}}", buildAutoSubmitSlider("dac_ramprate", "DAC Ramp Rate (0-15)", "dacRampRate", 0u, 15u, config->dac_ramprate));
    replaceToken(html, "{{DAC_EQ_BYPASS_CHECKED}}", checkedAttr(config->dac_eq_bypass));
    replaceToken(html, "{{DACEQ_B0_SLIDER}}", buildDacEqSlider("daceq_b0", "DACEQ B0 (REG38-3B)", "daceqB0", config->daceq_b0));
    replaceToken(html, "{{DACEQ_B1_SLIDER}}", buildDacEqSlider("daceq_b1", "DACEQ B1 (REG3C-3F)", "daceqB1", config->daceq_b1));
    replaceToken(html, "{{DACEQ_A1_SLIDER}}", buildDacEqSlider("daceq_a1", "DACEQ A1 (REG40-43)", "daceqA1", config->daceq_a1));
    return html;
}

String WifiConfigPortalView_BuildConfigPage(const ExternalRadioConfig *config,
                                            const WifiConfigPortalPageState &state,
                                            const String &form_sections)
{
    String html = buildPageWithFormSections(form_sections);
    replaceToken(html, "{{TITLE}}", state.title);
    replaceToken(html, "{{HEADLINE}}", state.headline);
    replaceToken(html, "{{HEADLINE_KEY}}", state.headline_key);
    replaceToken(html, "{{INTRO}}", state.intro);
    replaceToken(html, "{{INTRO_KEY}}", state.intro_key);
    replaceToken(html, "{{NETWORK_ACTIVE}}", state.network_active ? "active" : "");
    replaceToken(html, "{{DEVICE_ACTIVE}}", state.device_active ? "active" : "");
    replaceToken(html, "{{AUDIO_ACTIVE}}", state.audio_active ? "active" : "");
    replaceToken(html, "{{AP_IP}}", WiFi.softAPIP().toString());
    replaceToken(html, "{{STA_IP}}", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected"));
    replaceToken(html, "{{SSID_OPTIONS}}", "");
    replaceToken(html, "{{WIFI_SSID}}", htmlEscape(config->wifi_ssid));
    replaceToken(html, "{{WIFI_PASSWORD}}", htmlEscape(config->wifi_password));
    replaceToken(html, "{{SERVER_HOST}}", htmlEscape(config->server_host));
    replaceToken(html, "{{SERVER_PORT}}", String(config->server_port));
    replaceToken(html, "{{CHANNEL}}", String(config->channel));
    replaceToken(html, "{{CALLSIGN}}", htmlEscape(config->callsign));
    replaceToken(html, "{{CALLSIGN_SSID}}", String(config->callsign_ssid));
    replaceToken(html, "{{MIC_VOLUME}}", String(config->mic_volume));
    replaceToken(html, "{{LINE_OUT_VOLUME}}", String(config->line_out_volume));
    replaceToken(html, "{{HP_DRIVE_CHECKED}}", config->hp_drive_enabled ? "checked" : "");
    replaceToken(html, "{{FORM_ACTION}}", state.form_action);
    replaceToken(html, "{{FOOTER}}", state.footer);
    replaceToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
    return html;
}

String WifiConfigPortalView_BuildUpdatePage(const char *headline,
                                            const char *headline_key,
                                            const char *intro,
                                            const char *intro_key)
{
    String html = String(kWifiUpdatePortalHtmlTemplate);
    replaceToken(html, "{{HEADLINE}}", headline);
    replaceToken(html, "{{HEADLINE_KEY}}", headline_key);
    replaceToken(html, "{{INTRO}}", intro);
    replaceToken(html, "{{INTRO_KEY}}", intro_key);
    replaceToken(html, "{{AP_IP}}", WiFi.softAPIP().toString());
    replaceToken(html, "{{STA_IP}}", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("not connected"));
    replaceToken(html, "{{VERSION}}", NRL_FIRMWARE_VERSION);
    return html;
}

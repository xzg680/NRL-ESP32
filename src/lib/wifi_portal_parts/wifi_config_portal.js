const translations = {
      en: {
        language: 'Language',
        wifiConfig: 'WiFi Config',
        nrlConfig: 'NRL Config',
        audioSettings: 'Audio Settings',
        firmwareUpdate: 'Firmware Update',
        configAp: 'Config AP',
        stationIp: 'Station IP',
        bootNotice: 'Hold BOOT for 5 seconds after startup to reset WiFi and server settings.',
        firmwareVersion: 'Firmware version',
        wifiHeadline: 'WiFi Config',
        wifiIntro: 'Set the WiFi network the device should join.',
        nrlHeadline: 'NRL Config',
        nrlIntro: 'Set server address and radio identity.',
        audioHeadline: 'Audio Settings',
        audioIntro: 'Set ES8311 output, volume, DRC, and EQ.',
        wifiAppliedHeadline: 'WiFi Saved',
        wifiAppliedIntro: 'WiFi settings were saved.',
        wifiErrorHeadline: 'WiFi Save Failed',
        wifiErrorIntro: 'Check the WiFi name and password.',
        nrlAppliedHeadline: 'NRL Saved',
        nrlAppliedIntro: 'NRL, server, and audio settings were saved.',
        nrlErrorHeadline: 'NRL Save Failed',
        nrlErrorIntro: 'Check server address, port, channel, callsign, SSID, and audio values.',
        network: 'Network',
        scanIdle: 'Click Scan to find nearby WiFi networks.',
        wifiSsid: 'WiFi SSID',
        wifiPassword: 'WiFi Password',
        nearbyWifi: 'Nearby WiFi',
        selectWifi: 'Select detected WiFi...',
        scan: 'Scan',
        server: 'Server',
        serverHost: 'Server Host / IP',
        serverPort: 'Server Port',
        saveWifi: 'Save WiFi Config',
        radio: 'Radio',
        channel: 'Channel (0-7)',
        callsign: 'Callsign',
        callsignSsid: 'Callsign SSID',
        hpDrive: 'ES8311 HP Drive',
        hpDriveText: 'Enable headphone output drive (REG13 HPSW)',
        audio: 'Audio',
        micVolume: 'Mic Volume (0-255)',
        lineOutVolume: 'Line Out Volume (0-255)',
        volume: 'Volume',
        adc: 'ADC',
        dac: 'DAC',
        aec: 'AEC',
        drc: 'DRC',
        eq: 'EQ',
        input: 'Input',
        aecLabel: 'Acoustic Echo Cancellation',
        aecText: 'Enable esp-sr echo cancellation on mic uplink (restart to apply)',
        dmic: 'DMIC',
        dmicText: 'REG14 bit6 select DMIC from MIC1P',
        micInput: 'MIC1P-MIC1N Input',
        micInputText: 'REG14 bit4 line input select',
        adcPgaGain: 'ADC PGA Gain (0-10)',
        adcRampRate: 'ADC VC Ramp Rate (0-15)',
        adcGainScale: 'ADC Gain Scale (0-7)',
        dmicLatchSense: 'DMIC Latch Sense',
        dmicLatchSenseText: 'REG15 bit0 latch on negative edge',
        adcSync: 'ADC Sync',
        adcSyncText: 'REG16 bit5 standard audio clock',
        adcInvert: 'ADC Invert',
        adcInvertText: 'REG16 bit4 ADC polarity inverted',
        adcRamClear: 'ADC RAM Clear',
        adcRamClearText: 'REG16 bit3 clear ADC RAM',
        alcAutomute: 'ALC / Automute',
        alcEnable: 'ALC Enable',
        alcEnableText: 'REG18 bit7 auto level control',
        adcAutomuteEnable: 'ADC Automute Enable',
        adcAutomuteEnableText: 'REG18 bit6 automute',
        alcWindowSize: 'ALC Window Size (0-15)',
        alcMaxLevel: 'ALC Max Level (0-15)',
        alcMinLevel: 'ALC Min Level (0-15)',
        adcAutomuteWindow: 'ADC Automute Window (0-15)',
        adcAutomuteNoiseGate: 'ADC Automute Noise Gate (0-15)',
        adcAutomuteVolume: 'ADC Automute Volume (0-7)',
        hpfEq: 'HPF / EQ',
        adcHpfStage1: 'ADC HPF Stage 1 (0-31)',
        adcHpfStage2: 'ADC HPF Stage 2 (0-31)',
        adcEqBypass: 'ADC EQ Bypass',
        adcEqBypassText: 'REG1C bit6 bypass ADC EQ',
        adcDynamicHpf: 'ADC Dynamic HPF',
        adcDynamicHpfText: 'REG1C bit5 dynamic HPF',
        adcEqCoefficients: 'ADC EQ Coefficients',
        adcEqCoefficientsText: '30-bit unsigned register values for B0/A1/A2/B1/B2',
        adceqB0: 'ADCEQ B0 (REG1D-20)',
        adceqA1: 'ADCEQ A1 (REG21-24)',
        adceqA2: 'ADCEQ A2 (REG25-28)',
        adceqB1: 'ADCEQ B1 (REG29-2C)',
        adceqB2: 'ADCEQ B2 (REG2D-30)',
        output: 'Output',
        drcEnable: 'ES8311 DRC Enable',
        drcEnableText: 'Enable DAC DRC (REG34 bit7)',
        drcWindowSize: 'DRC Window Size (0-15)',
        drcMaxLevel: 'DRC Max Level (0-15)',
        drcMinLevel: 'DRC Min Level (0-15)',
        dacRampRate: 'DAC Ramp Rate (0-15)',
        dacEqBypass: 'DAC EQ Bypass',
        dacEqBypassText: 'Bypass DAC EQ (REG37 bit3)',
        dacEqCoefficients: 'DAC EQ Coefficients',
        dacEqCoefficientsText: '30-bit unsigned register values for B0/B1/A1',
        daceqB0: 'DACEQ B0 (REG38-3B)',
        daceqB1: 'DACEQ B1 (REG3C-3F)',
        daceqA1: 'DACEQ A1 (REG40-43)',
        saveNrl: 'Save NRL Config',
        scanning: 'Scanning...',
        foundPrefix: 'Found ',
        foundSuffix: ' WiFi networks',
        noneFound: 'No WiFi networks found',
        scanFailed: 'Scan failed',
        saveItem: 'Save',
        saved: 'Saved',
        saveFailed: 'Save failed'
      },
      zh: {
        language: '语言',
        wifiConfig: 'WiFi配置',
        nrlConfig: 'NRL配置',
        audioSettings: '音频设置',
        firmwareUpdate: '固件升级',
        configAp: '配置热点',
        stationIp: '联网地址',
        bootNotice: '开机后按住 BOOT 5 秒可重置 WiFi 和服务器设置。',
        firmwareVersion: '固件版本',
        wifiHeadline: 'WiFi配置',
        wifiIntro: '设置设备要连接的 WiFi 网络。',
        nrlHeadline: 'NRL配置',
        nrlIntro: '设置服务器地址和电台身份。',
        audioHeadline: '音频设置',
        audioIntro: '设置 ES8311 输出、音量、DRC 和 EQ。',
        wifiAppliedHeadline: 'WiFi已保存',
        wifiAppliedIntro: 'WiFi 设置已保存。',
        wifiErrorHeadline: 'WiFi保存失败',
        wifiErrorIntro: '请检查 WiFi 名称和密码。',
        nrlAppliedHeadline: 'NRL已保存',
        nrlAppliedIntro: 'NRL、服务器和音频设置已保存。',
        nrlErrorHeadline: 'NRL保存失败',
        nrlErrorIntro: '请检查服务器地址、端口、信道、呼号、SSID 和音频数值。',
        network: '网络',
        scanIdle: '点击扫描查找附近 WiFi。',
        wifiSsid: 'WiFi名称',
        wifiPassword: 'WiFi密码',
        nearbyWifi: '附近WiFi',
        selectWifi: '选择扫描到的 WiFi...',
        scan: '扫描',
        server: '服务器',
        serverHost: '服务器地址 / IP',
        serverPort: '服务器端口',
        saveWifi: '保存WiFi配置',
        radio: '电台',
        channel: '信道 (0-7)',
        callsign: '呼号',
        callsignSsid: '呼号SSID',
        hpDrive: 'ES8311耳机驱动',
        hpDriveText: '启用耳机输出驱动 (REG13 HPSW)',
        audio: '音频',
        micVolume: '麦克风音量 (0-255)',
        lineOutVolume: '线路输出音量 (0-255)',
        volume: '音量',
        adc: 'ADC 输入',
        dac: 'DAC 输出',
        aec: 'AEC 回声消除',
        drc: 'DRC 动态范围控制',
        eq: 'EQ 均衡',
        input: '输入',
        aecLabel: '声学回声消除',
        aecText: '启用 esp-sr 麦克风上行回声消除（重启后生效）',
        dmic: '数字麦克风',
        dmicText: 'REG14 bit6 从 MIC1P 选择 DMIC',
        micInput: 'MIC1P-MIC1N 输入',
        micInputText: 'REG14 bit4 选择线路输入',
        adcPgaGain: 'ADC PGA 增益 (0-10)',
        adcRampRate: 'ADC 音量爬升速率 (0-15)',
        adcGainScale: 'ADC 增益缩放 (0-7)',
        dmicLatchSense: 'DMIC 锁存边沿',
        dmicLatchSenseText: 'REG15 bit0 负边沿锁存',
        adcSync: 'ADC 同步',
        adcSyncText: 'REG16 bit5 标准音频时钟',
        adcInvert: 'ADC 反相',
        adcInvertText: 'REG16 bit4 ADC 极性反相',
        adcRamClear: '清除 ADC RAM',
        adcRamClearText: 'REG16 bit3 清除 ADC RAM',
        alcAutomute: 'ALC / 自动静音',
        alcEnable: '启用 ALC',
        alcEnableText: 'REG18 bit7 自动电平控制',
        adcAutomuteEnable: '启用 ADC 自动静音',
        adcAutomuteEnableText: 'REG18 bit6 自动静音',
        alcWindowSize: 'ALC 窗口大小 (0-15)',
        alcMaxLevel: 'ALC 最大电平 (0-15)',
        alcMinLevel: 'ALC 最小电平 (0-15)',
        adcAutomuteWindow: 'ADC 自动静音窗口 (0-15)',
        adcAutomuteNoiseGate: 'ADC 自动静音噪声门限 (0-15)',
        adcAutomuteVolume: 'ADC 自动静音音量 (0-7)',
        hpfEq: '高通滤波 / EQ',
        adcHpfStage1: 'ADC 高通滤波 1 级 (0-31)',
        adcHpfStage2: 'ADC 高通滤波 2 级 (0-31)',
        adcEqBypass: '旁路 ADC EQ',
        adcEqBypassText: 'REG1C bit6 旁路 ADC EQ',
        adcDynamicHpf: 'ADC 动态高通滤波',
        adcDynamicHpfText: 'REG1C bit5 动态高通滤波',
        adcEqCoefficients: 'ADC EQ 系数',
        adcEqCoefficientsText: 'B0/A1/A2/B1/B2 的 30 位无符号寄存器值',
        adceqB0: 'ADCEQ B0 (REG1D-20)',
        adceqA1: 'ADCEQ A1 (REG21-24)',
        adceqA2: 'ADCEQ A2 (REG25-28)',
        adceqB1: 'ADCEQ B1 (REG29-2C)',
        adceqB2: 'ADCEQ B2 (REG2D-30)',
        output: '输出',
        drcEnable: '启用 ES8311 DRC',
        drcEnableText: '启用 DAC DRC (REG34 bit7)',
        drcWindowSize: 'DRC 窗口大小 (0-15)',
        drcMaxLevel: 'DRC 最大电平 (0-15)',
        drcMinLevel: 'DRC 最小电平 (0-15)',
        dacRampRate: 'DAC 爬升速率 (0-15)',
        dacEqBypass: '旁路 DAC EQ',
        dacEqBypassText: '旁路 DAC EQ (REG37 bit3)',
        dacEqCoefficients: 'DAC EQ 系数',
        dacEqCoefficientsText: 'B0/B1/A1 的 30 位无符号寄存器值',
        daceqB0: 'DACEQ B0 (REG38-3B)',
        daceqB1: 'DACEQ B1 (REG3C-3F)',
        daceqA1: 'DACEQ A1 (REG40-43)',
        saveNrl: '保存NRL配置',
        scanning: '正在扫描...',
        foundPrefix: '找到 ',
        foundSuffix: ' 个 WiFi 网络',
        noneFound: '没有找到 WiFi 网络',
        scanFailed: '扫描失败',
        saveItem: '保存',
        saved: '已保存',
        saveFailed: '保存失败'
      }
    };

    function currentLang() {
      const saved = localStorage.getItem('nrl_lang');
      if (saved === 'zh' || saved === 'en') return saved;
      return navigator.language && navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en';
    }

    function t(key) {
      const lang = currentLang();
      return (translations[lang] && translations[lang][key]) || translations.en[key] || key;
    }

    function applyLanguage(lang) {
      localStorage.setItem('nrl_lang', lang);
      document.documentElement.lang = lang === 'zh' ? 'zh-CN' : 'en';
      document.querySelectorAll('input[name="lang"]').forEach((r) => {
        r.checked = (r.value === lang);
      });
      document.querySelectorAll('[data-i18n]').forEach((el) => {
        const key = el.getAttribute('data-i18n');
        if (translations[lang] && translations[lang][key]) {
          el.textContent = translations[lang][key];
        }
      });
      document.querySelectorAll('[data-i18n-title]').forEach((el) => {
        const key = el.getAttribute('data-i18n-title');
        if (translations[lang] && translations[lang][key]) {
          el.title = translations[lang][key];
        }
      });
    }

    function syncDhcpFields() {
      const dhcp = document.getElementById('wifi-dhcp-enabled');
      if (!dhcp) return;
      document.querySelectorAll('.wifi-static-field').forEach((input) => {
        input.disabled = dhcp.checked;
      });
    }

    function clampEqValue(value) {
      const text = String(value).trim();
      const n = text.toLowerCase().startsWith('0x') ? Number.parseInt(text.slice(2), 16) : Number(text);
      if (!Number.isFinite(n)) return 0;
      return Math.max(0, Math.min(1073741823, Math.round(n)));
    }

    function formatEqHex(value) {
      return '0x' + clampEqValue(value).toString(16).toUpperCase().padStart(8, '0');
    }

    function syncEqValue(slider) {
      const box = slider.parentElement.querySelector('.eq-value');
      if (box) box.value = formatEqHex(slider.value);
    }

    function syncEqSlider(box) {
      const slider = box.parentElement.querySelector('.eq-slider');
      const value = clampEqValue(box.value);
      box.value = formatEqHex(value);
      if (slider) slider.value = value;
    }

    // POST a form as multipart/form-data and return the parsed JSON reply:
    //   { ok: bool, fields: { name: stored_value, ... } }
    // The server echoes back the just-saved value of every submitted field so
    // the caller can refresh inputs with on-device truth without a page reload.
    function postForm(form) {
      if (!form) return Promise.resolve({ ok: false });
      const body = new FormData(form);
      return fetch(form.action || window.location.href, {
        method: form.method ? form.method.toUpperCase() : 'POST',
        body,
        cache: 'no-store',
        credentials: 'same-origin',
      }).then((r) => r.json().then((data) => Object.assign({ ok: r.ok }, data)))
        .catch(() => ({ ok: false }));
    }

    function applyEchoFields(fields) {
      if (!fields) return;
      Object.keys(fields).forEach((name) => {
        const value = fields[name];
        const selector = 'input[name="' + name + '"], select[name="' + name + '"]';
        document.querySelectorAll(selector).forEach((input) => {
          if (input.type === 'checkbox') {
            input.checked = (value === '1' || value === 'true' || value === 'on');
          } else if (input !== document.activeElement) {
            // Don't fight the user mid-edit. Active field keeps their text;
            // they'll see the canonical value on next interaction.
            input.value = value;
          }
          if (input.classList.contains('auto-slider')) {
            const box = input.parentElement.querySelector('.eq-value');
            if (box) box.value = input.value;
          } else if (input.classList.contains('eq-slider')) {
            syncEqValue(input);
          }
        });
      });
      syncDhcpFields();
    }

    function postAndApply(form) {
      return postForm(form).then((reply) => {
        if (reply && reply.fields) applyEchoFields(reply.fields);
        return reply;
      });
    }

    function flashButtonFeedback(button, ok) {
      if (!button) return;
      const orig = button.textContent;
      const origI18n = button.getAttribute('data-i18n');
      button.removeAttribute('data-i18n');
      button.textContent = ok ? t('saved') : t('saveFailed');
      button.disabled = true;
      setTimeout(() => {
        button.textContent = orig;
        if (origI18n) button.setAttribute('data-i18n', origI18n);
        button.disabled = false;
      }, 1200);
    }

    function submitFormFromButton(form) {
      const button = form.querySelector('button[type="submit"]');
      if (button) button.disabled = true;
      postAndApply(form).then((reply) => flashButtonFeedback(button, reply && reply.ok));
    }

    function submitEqSlider(slider) {
      syncEqValue(slider);
      postAndApply(slider.form);
    }

    function syncAutoSliderValue(slider) {
      const box = slider.parentElement.querySelector('.eq-value');
      if (box) box.value = slider.value;
    }

    function submitAutoSlider(slider) {
      syncAutoSliderValue(slider);
      postAndApply(slider.form);
    }

    function submitSwitch(input) {
      postAndApply(input.form);
    }

    async function scanWifi() {
      const status = document.getElementById('scan-status');
      const select = document.getElementById('wifi-ssid-select');
      status.textContent = t('scanning');
      try {
        const res = await fetch('/scan', {cache: 'no-store'});
        if (!res.ok) throw new Error('scan failed');
        const items = await res.json();
        renderWifiOptions(select, items);
        status.textContent = items.length ? (t('foundPrefix') + items.length + t('foundSuffix')) : t('noneFound');
      } catch (e) {
        status.textContent = t('scanFailed');
      }
    }

    function renderWifiOptions(select, items) {
      let foundSelected = false;
      const selected = select ? select.value : '';
      if (select) {
        select.innerHTML = '';
        const placeholder = document.createElement('option');
        placeholder.value = '';
        placeholder.textContent = t('selectWifi');
        placeholder.setAttribute('data-i18n', 'selectWifi');
        select.appendChild(placeholder);
      }
      if (!Array.isArray(items)) {
        if (select) select.value = selected;
        return;
      }
      items.forEach((item) => {
        if (!item || !item.ssid) return;
        const label = item.label || item.ssid;
        if (select) {
          const opt = document.createElement('option');
          opt.value = item.ssid;
          opt.textContent = label;
          select.appendChild(opt);
          if (item.ssid === selected) foundSelected = true;
        }
      });
      if (select && selected && !foundSelected) {
        const opt = document.createElement('option');
        opt.value = selected;
        opt.textContent = selected;
        select.appendChild(opt);
      }
      if (select) select.value = selected;
    }

    function initPortal() {
      applyLanguage(currentLang());
      syncDhcpFields();
      document.querySelectorAll('form').forEach((form) => {
        // Intercept explicit Save-button submits: send via AJAX so the page
        // never reloads, then reflect the server's echoed values back into the
        // inputs and flash the button.
        form.addEventListener('submit', (e) => {
          e.preventDefault();
          submitFormFromButton(form);
        });
      });
      document.querySelectorAll('input[name="lang"]').forEach((r) => {
        r.addEventListener('change', function () {
          if (this.checked) applyLanguage(this.value);
        });
      });
    }

    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', initPortal);
    } else {
      initPortal();
    }

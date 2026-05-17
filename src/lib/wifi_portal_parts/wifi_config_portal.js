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
        saveNrl: 'Save NRL Config',
        scanning: 'Scanning...',
        foundPrefix: 'Found ',
        foundSuffix: ' WiFi networks',
        noneFound: 'No WiFi networks found',
        scanFailed: 'Scan failed',
        saveItem: 'Save'
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
        saveNrl: '保存NRL配置',
        scanning: '正在扫描...',
        foundPrefix: '找到 ',
        foundSuffix: ' 个 WiFi 网络',
        noneFound: '没有找到 WiFi 网络',
        scanFailed: '扫描失败',
        saveItem: '保存'
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

    function submitEqSlider(slider) {
      syncEqValue(slider);
      const form = slider.form;
      if (!form || form.dataset.submitting === '1') return;
      form.dataset.submitting = '1';
      if (form.requestSubmit) form.requestSubmit();
      else form.submit();
    }

    function syncAutoSliderValue(slider) {
      const box = slider.parentElement.querySelector('.eq-value');
      if (box) box.value = slider.value;
    }

    function submitAutoSlider(slider) {
      syncAutoSliderValue(slider);
      const form = slider.form;
      if (!form || form.dataset.submitting === '1') return;
      form.dataset.submitting = '1';
      if (form.requestSubmit) form.requestSubmit();
      else form.submit();
    }

    function submitSwitch(input) {
      const form = input.form;
      if (!form || form.dataset.submitting === '1') return;
      form.dataset.submitting = '1';
      if (form.requestSubmit) form.requestSubmit();
      else form.submit();
    }

    function saveScrollPosition() {
      sessionStorage.setItem('nrl_config_scroll_y', String(window.scrollY || 0));
    }

    function restoreScrollPosition() {
      const saved = sessionStorage.getItem('nrl_config_scroll_y');
      if (saved === null) return;
      sessionStorage.removeItem('nrl_config_scroll_y');
      const y = Number(saved);
      if (Number.isFinite(y)) {
        requestAnimationFrame(() => window.scrollTo(0, y));
      }
    }

    function setEqField(name, value) {
      const slider = document.querySelector('input.eq-slider[name="' + name + '"]');
      if (!slider) return;
      slider.value = clampEqValue(value);
      syncEqValue(slider);
    }

    function applyDacEqPreset(name) {
      const bypass = document.querySelector('input[name="dac_eq_bypass"]');
      const presets = {
        flat: {bypass: true, b0: 0, b1: 0, a1: 0},
        neutral: {bypass: false, b0: 536870912, b1: 0, a1: 0},
        voice_bright: {bypass: false, b0: 570425344, b1: 33554432, a1: 503316480},
        voice_soft: {bypass: false, b0: 503316480, b1: 67108864, a1: 469762048},
        low_cut: {bypass: false, b0: 536870912, b1: 1040187392, a1: 503316480}
      };
      const p = presets[name] || presets.flat;
      if (bypass) bypass.checked = p.bypass;
      setEqField('daceq_b0', p.b0);
      setEqField('daceq_b1', p.b1);
      setEqField('daceq_a1', p.a1);
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

    applyLanguage(currentLang());
    syncDhcpFields();
    restoreScrollPosition();
    document.querySelectorAll('form').forEach((form) => {
      form.addEventListener('submit', saveScrollPosition);
    });
    document.querySelectorAll('input[name="lang"]').forEach((r) => {
      r.addEventListener('change', function () {
        if (this.checked) applyLanguage(this.value);
      });
    });

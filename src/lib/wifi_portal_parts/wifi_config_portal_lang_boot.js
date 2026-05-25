(function () {
  const zh = {
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
    selectWifi: '选择扫描到的 WiFi...',
    scan: '扫描',
    server: '服务器',
    serverHost: '服务器地址 / IP',
    serverPort: '服务器端口',
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
    aec: 'AEC / AI 降噪',
    drc: 'DRC 动态范围控制',
    eq: 'EQ 均衡',
    input: '输入',
    aecLabel: '声学回声消除',
    aecText: '启用 esp-sr 麦克风上行回声消除',
    aecReferenceSource: 'AEC 参照来源',
    aecRefNetwork: '网络下行语音',
    aecRefMic: '第二路麦克风',
    aiNoiseLabel: 'AI降噪',
    aiNoiseText: '启用 esp-sr NSNET2 麦克风上行降噪',
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
    saveItem: '保存'
  };

  function savedLang() {
    try {
      return localStorage.getItem('nrl_lang');
    } catch (e) {
      return '';
    }
  }

  function apply() {
    const lang = savedLang();
    if (lang !== 'zh') return;
    document.documentElement.lang = 'zh-CN';
    document.querySelectorAll('input[name="lang"]').forEach((r) => {
      r.checked = (r.value === 'zh');
    });
    document.querySelectorAll('[data-i18n]').forEach((el) => {
      const text = zh[el.getAttribute('data-i18n')];
      if (text) el.textContent = text;
    });
    document.querySelectorAll('[data-i18n-title]').forEach((el) => {
      const text = zh[el.getAttribute('data-i18n-title')];
      if (text) el.title = text;
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', apply);
  } else {
    apply();
  }
})();

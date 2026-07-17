const translations = {
      en: {
        language: 'Language',
        wifiConfig: 'WiFi Config',
        nrlConfig: 'NRL Config',
        audioSettings: 'Audio Settings',
        firmwareUpdate: 'Firmware Update',
        configAp: 'Config AP',
        stationIp: 'Station IP',
        bootNotice: 'Hold BOOT for 5 seconds after startup to reset WiFi settings.',
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
        dhcp: 'DHCP',
        useDhcp: 'Use DHCP for station IP',
        wifiIp: 'WiFi IP',
        wifiMask: 'WiFi Mask',
        wifiGateway: 'WiFi Gateway',
        wifiDns: 'WiFi DNS',
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
        pttTimeout: 'PTT Timeout (seconds, 5-3600)',
        voicePayloadBytes: 'Voice Packet Size (bytes, 160-500)',
        tailSuppressMs: 'Tail Suppression (ms, 0-5000, 0=off)',
        hpDrive: 'ES8311 HP Drive',
        hpDriveText: 'Enable headphone output drive (REG13 HPSW)',
        audio: 'Audio',
        micVolume: 'Mic Volume (0-255)',
        lineOutVolume: 'Line Out Volume (0-255)',
        volume: 'Volume',
        audioProcessing: 'Audio Processing',
        adc: 'ADC',
        dac: 'DAC',
        aec: 'AEC / AI',
        micHpfFilter: 'Mute Filter',
        micHpfFilterText: 'Software high-pass filter (~200Hz)',
        drc: 'DRC',
        eq: 'EQ',
        input: 'Input',
        aecLabel: 'Acoustic Echo Cancellation',
        aecText: 'Enable esp-sr echo cancellation on mic uplink',
        aecReferenceSource: 'AEC Reference Source',
        aecRefNetwork: 'Network playback',
        aecRefMic: 'Second microphone',
        aiNoiseLabel: 'AI Noise Reduction',
        aiNoiseText: 'Enable esp-sr NSNET2 noise suppression on mic uplink',
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
        audioResetDefaults: 'Restore Defaults',
        audioExpertMode: 'Expert Mode',
        saved: 'Saved',
        saveFailed: 'Save failed',
        mediaConfig: 'Media / Nanny',
        aprsConfig: 'APRS',
        aprsHeadline: 'APRS',
        aprsIntro: 'GPS beacons to APRS-IS and AFSK over the radio; stations heard are listed below.',
        aprsSwitches: 'APRS',
        aprsEnabled: 'APRS',
        aprsEnabledText: 'Master switch for the APRS transceiver',
        aprsNet: 'APRS-IS Network',
        aprsNetText: 'Send beacons to and hear stations from the APRS-IS server',
        aprsRfTx: 'RF Transmit (AFSK)',
        aprsRfTxText: 'Modulate beacons out through the speaker into the radio',
        aprsRfRx: 'RF Receive (AFSK)',
        aprsRfRxText: 'Demodulate APRS audio heard on the microphone/radio',
        aprsSettings: 'APRS Settings',
        aprsSettingsHint: 'Callsign comes from the Radio page; passcode is derived automatically.',
        aprsServer: 'APRS-IS Server',
        aprsPort: 'Port',
        aprsSsid: 'SSID (0-15)',
        aprsSymbol: 'Symbol (table+code)',
        aprsInterval: 'Beacon interval (s, 10-3600)',
        aprsPath: 'RF path',
        aprsLat: 'Default latitude (WGS-84 ddmm.mmmm; N added automatically)',
        aprsLon: 'Default longitude (WGS-84 dddmm.mmmm; E added automatically)',
        aprsComment: 'Beacon comment',
        aprsBeaconNow: 'Beacon now',
        aprsStations: 'Stations Heard',
        aprsStnCall: 'Callsign',
        aprsStnPos: 'Lat / Lon',
        aprsStnAlt: 'Alt',
        aprsStnDist: 'Distance',
        aprsStnSpeed: 'Speed',
        aprsStnCourse: 'Course',
        aprsStnAge: 'Heard',
        aprsStnComment: 'Comment',
        aprsStnEmpty: 'No stations heard yet.',
        mediaHeadline: 'Media / Nanny',
        mediaIntro: 'Configure playback target, nanny beacon, net radio, and SMB network share.',
        playback: 'Playback',
        musicTarget: 'Playback Target',
        musicTargetHint: 'One shared setting for everything the player outputs: music, nanny beacon, and net radio.',
        targetLocal: 'Local speaker',
        targetNet: 'NRL network',
        targetBoth: 'Local + network',
        espnowLabel: 'ESP-NOW Intercom',
        espnowText: 'Off-grid voice link between nearby devices',
        espnowRxLabel: 'ESP-NOW Receive',
        espnowRxText: 'Hear intercom voice even while TX stays off',
        espnowCodec: 'ESP-NOW Voice Codec (TX)',
        nannyBeacon: 'Nanny Beacon',
        nannyBeaconHint: 'Play a beacon file every N minutes through the configured target.',
        beaconPath: 'Beacon file path',
        beaconInterval: 'Interval (minutes, 1-1440)',
        beaconEnabledText: 'Beacon armed (uncheck and Save to disable)',
        netRadio: 'Net Radio',
        radioUrl: 'Stream URL (http:// or https://)',
        radioPlay: 'Play',
        radioStop: 'Stop',
        radioFavs: 'Favorite stations',
        radioFavName: 'Station name',
        radioFavUrl: 'Stream URL (http:// or https://)',
        radioFavAdd: 'Add favorite',
        radioFavDelete: 'Delete',
        radioFavEmpty: 'No favorites yet. Add a station below.',
        smbShare: 'Network Share (SMB)',
        smbServer: 'Server (NAS / PC)',
        smbShareName: 'Share name',
        smbUser: 'Username (empty = guest)',
        smbPassword: 'Password',
        smbClear: 'Clear',
        musicOutput: 'Music Output Device',
        outputSpk: 'Onboard speaker',
        outputBt: 'Bluetooth headset (A2DP)',
        voiceCodec: 'NRL Voice Codec (TX)',
        codecG711: 'G.711 8 kHz (compatible)',
        codecOpus: 'Opus 16 kHz wideband',
        voiceLink: 'Voice Link',
        pttMode: 'PTT Mode',
        pttModeNrl: 'NRL network PTT',
        pttModeEspnow: 'ESP-NOW PTT',
        aiAssistant: 'AI Assistant (xiaozhi)',
        aiUrl: 'Server URL (ws:// or wss://)',
        aiToken: 'Access Token',
        aiEnabledText: 'Enable the assistant connection'
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
        dhcp: 'DHCP',
        useDhcp: '联网地址使用 DHCP 自动获取',
        wifiIp: 'WiFi IP 地址',
        wifiMask: 'WiFi 子网掩码',
        wifiGateway: 'WiFi 网关',
        wifiDns: 'WiFi DNS',
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
        pttTimeout: 'PTT 超时 (秒, 5-3600)',
        voicePayloadBytes: '语音包大小 (字节, 160-500)',
        tailSuppressMs: '尾音消除 (毫秒, 0-5000, 0=关闭)',
        hpDrive: 'ES8311耳机驱动',
        hpDriveText: '启用耳机输出驱动 (REG13 HPSW)',
        audio: '音频',
        micVolume: '麦克风音量 (0-255)',
        lineOutVolume: '线路输出音量 (0-255)',
        volume: '音量',
        audioProcessing: '音频处理',
        adc: 'ADC 输入',
        dac: 'DAC 输出',
        aec: 'AEC / AI 降噪',
        micHpfFilter: '哑音过滤',
        micHpfFilterText: '软件高通滤波 (~200Hz)',
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
        scanning: '正在扫描...',
        foundPrefix: '找到 ',
        foundSuffix: ' 个 WiFi 网络',
        noneFound: '没有找到 WiFi 网络',
        scanFailed: '扫描失败',
        saveItem: '保存',
        saved: '已保存',
        saveFailed: '保存失败',
        battery: '电池',
        batteryHint: '使用万用表校准板载电池电压采样。',
        batteryRaw: '原始读数 (mV)',
        batteryCalibrated: '校准后 (mV)',
        batteryActual: '万用表测量值 (mV) -- 点击校准按当前读数拟合系数',
        batteryCalibrate: '校准',
        batteryScale: '校准系数 (500-2000，1000 表示不修正)',
        mediaConfig: '媒体/保姆',
        aprsConfig: 'APRS',
        aprsHeadline: 'APRS 收发',
        aprsIntro: 'GPS 位置上报到 APRS-IS 服务器，或经电台 AFSK 收发；下方列出收到的台站。',
        aprsSwitches: 'APRS',
        aprsEnabled: 'APRS 总开关',
        aprsEnabledText: 'APRS 收发功能总开关',
        aprsNet: 'APRS-IS 网络',
        aprsNetText: '向 APRS-IS 服务器上报位置并接收周边台站',
        aprsRfTx: '射频发射 (AFSK)',
        aprsRfTxText: '信标经扬声器调制后由电台发射',
        aprsRfRx: '射频接收 (AFSK)',
        aprsRfRxText: '解调麦克风/电台收到的 APRS 音频',
        aprsSettings: 'APRS 设置',
        aprsSettingsHint: '呼号使用"电台"页配置；APRS-IS 验证码自动计算。',
        aprsServer: 'APRS-IS 服务器',
        aprsPort: '端口',
        aprsSsid: 'SSID (0-15)',
        aprsSymbol: '符号 (表+代码)',
        aprsInterval: '信标间隔 (秒, 10-3600)',
        aprsPath: '射频路径',
        aprsLat: '默认纬度 (WGS-84 ddmm.mmmm，自动补N)',
        aprsLon: '默认经度 (WGS-84 dddmm.mmmm，自动补E)',
        aprsComment: '信标注释',
        aprsBeaconNow: '立即发信标',
        aprsStations: '收到的台站',
        aprsStnCall: '呼号',
        aprsStnPos: '纬度/经度',
        aprsStnAlt: '高度',
        aprsStnDist: '距离',
        aprsStnSpeed: '速度',
        aprsStnCourse: '航向',
        aprsStnAge: '最后收到',
        aprsStnComment: '注释',
        aprsStnEmpty: '尚未收到台站。',
        mediaHeadline: '媒体/保姆',
        mediaIntro: '配置播放目标、保姆信标、网络收音机和 SMB 网络共享。',
        playback: '播放',
        musicTarget: '播放目标',
        musicTargetHint: '全局共用设置：音乐、保姆信标、网络收音机的播放都按它路由。',
        targetLocal: '本地扬声器',
        targetNet: 'NRL 网络',
        targetBoth: '本地 + 网络',
        espnowLabel: 'ESP-NOW 对讲',
        espnowText: '附近设备间脱网语音互通',
        espnowRxLabel: 'ESP-NOW 接收',
        espnowRxText: '发射关闭时也能听到对讲呼入',
        espnowCodec: 'ESP-NOW 语音编码 (发射)',
        nannyBeacon: '保姆信标',
        nannyBeaconHint: '每隔 N 分钟按播放目标播放一段信标音频。',
        beaconPath: '信标文件路径',
        beaconInterval: '间隔 (分钟, 1-1440)',
        beaconEnabledText: '信标已启用 (取消勾选并保存即停用)',
        netRadio: '网络收音机',
        radioUrl: '直播流地址 (http:// 或 https://)',
        radioPlay: '播放',
        radioStop: '停止',
        radioFavs: '收藏电台',
        radioFavName: '电台名称',
        radioFavUrl: '直播流地址 (http:// 或 https://)',
        radioFavAdd: '添加收藏',
        radioFavDelete: '删除',
        radioFavEmpty: '还没有收藏电台，在下方添加。',
        smbShare: '网络共享 (SMB)',
        smbServer: '服务器 (NAS / 电脑)',
        smbShareName: '共享名',
        smbUser: '用户名 (留空为来宾)',
        smbPassword: '密码',
        smbClear: '清除',
        musicOutput: '音乐输出设备',
        outputSpk: '板载扬声器',
        outputBt: '蓝牙耳机 (A2DP)',
        voiceCodec: 'NRL 语音编码 (发送)',
        codecG711: 'G.711 8kHz (兼容)',
        codecOpus: 'Opus 16kHz 宽带',
        voiceLink: '语音链路',
        pttMode: 'PTT 模式',
        pttModeNrl: 'NRL 网络 PTT',
        pttModeEspnow: 'ESP-NOW PTT',
        aiAssistant: 'AI 助手 (小智)',
        aiUrl: '服务器地址 (ws:// 或 wss://)',
        aiToken: '访问令牌 (Token)',
        aiEnabledText: '启用助手连接'
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

    function toggleAudioExpert(enabled) {
      document.querySelectorAll('[data-audio-expert]').forEach((panel) => {
        panel.hidden = !enabled;
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

    // POST a form as application/x-www-form-urlencoded and return the parsed
    // JSON reply: { ok: bool, fields: { name: stored_value, ... } }
    // Server-side parseFormBody in wifi_config_portal.cpp only handles
    // urlencoded; using FormData would send multipart/form-data and every
    // field would be silently ignored. The server echoes back the just-saved
    // value of every submitted field so the caller can refresh inputs with
    // on-device truth without a page reload.
    function postForm(form) {
      if (!form) return Promise.resolve({ ok: false });
      const body = new URLSearchParams();
      // FormData walks the form's successful controls (skips unchecked
      // checkboxes, etc.) -- exactly the set we want urlencoded.
      for (const [k, v] of new FormData(form)) {
        body.append(k, v);
      }
      return fetch(form.action || window.location.href, {
        method: form.method ? form.method.toUpperCase() : 'POST',
        body,
        cache: 'no-store',
        credentials: 'same-origin',
        referrerPolicy: 'no-referrer',
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
        // /save_media replies carry the favorite-station list so the page
        // reflects adds/deletes/tunes without a reload.
        if (reply && reply.favs) renderRadioFavs(reply.favs, reply.fav_cur);
        return reply;
      });
    }

    // Favorite net-radio stations. The list is rendered client-side from
    // window.RADIO_FAVS (initial page data) and from the JSON echoed by every
    // /save_media POST; rows post their action straight back to /save_media.
    function renderRadioFavs(favs, current) {
      const list = document.getElementById('radio-fav-list');
      if (!list || !Array.isArray(favs)) return;
      list.innerHTML = '';
      if (!favs.length) {
        const empty = document.createElement('span');
        empty.className = 'hint';
        empty.setAttribute('data-i18n', 'radioFavEmpty');
        empty.textContent = t('radioFavEmpty');
        list.appendChild(empty);
        return;
      }
      favs.forEach((fav, idx) => {
        const row = document.createElement('div');
        row.className = 'fav-row' + (idx === current ? ' current' : '');
        const text = document.createElement('div');
        text.className = 'fav-text';
        const name = document.createElement('span');
        name.className = 'fav-name';
        name.textContent = (idx === current ? '▶ ' : '') + (fav.name || fav.url);
        const url = document.createElement('span');
        url.className = 'fav-url mono';
        url.textContent = fav.url;
        text.appendChild(name);
        text.appendChild(url);
        row.appendChild(text);
        const play = document.createElement('button');
        play.type = 'button';
        play.className = 'btn-small';
        play.textContent = t('radioPlay');
        play.setAttribute('data-i18n', 'radioPlay');
        play.addEventListener('click', () => radioFavAction(play, 'fav_play', idx));
        const del = document.createElement('button');
        del.type = 'button';
        del.className = 'secondary btn-small';
        del.textContent = t('radioFavDelete');
        del.setAttribute('data-i18n', 'radioFavDelete');
        del.addEventListener('click', () => radioFavAction(del, 'fav_del', idx));
        row.appendChild(play);
        row.appendChild(del);
        list.appendChild(row);
      });
    }

    function radioFavAction(button, action, index) {
      button.disabled = true;
      const body = new URLSearchParams();
      body.append(action, String(index));
      fetch('/save_media', {
        method: 'POST',
        body,
        cache: 'no-store',
        credentials: 'same-origin',
        referrerPolicy: 'no-referrer',
      }).then((r) => r.json().then((data) => Object.assign({ ok: r.ok }, data)))
        .catch(() => ({ ok: false }))
        .then((reply) => {
          button.disabled = false;
          if (reply && reply.fields) applyEchoFields(reply.fields);
          if (reply && reply.favs) renderRadioFavs(reply.favs, reply.fav_cur);
        });
    }

    function addRadioFav(button) {
      const form = button ? button.form : null;
      if (!form) return;
      const flag = document.createElement('input');
      flag.type = 'hidden';
      flag.name = 'fav_add';
      flag.value = '1';
      form.appendChild(flag);
      button.disabled = true;
      postAndApply(form).then((reply) => {
        flag.remove();
        button.disabled = false;
        flashButtonFeedback(button, reply && reply.ok);
        if (reply && reply.ok) {
          form.querySelectorAll('input[name="fav_name"], input[name="fav_url"]').forEach((input) => {
            input.value = '';
          });
        }
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

    window.applyAdcEqPreset = function (button) {
      const form = button ? button.form : null;
      if (!form) return;
      form.querySelectorAll('input[type="hidden"][name]').forEach((preset) => {
        document.querySelectorAll('input.eq-slider[name="' + preset.name + '"]').forEach((slider) => {
          slider.value = preset.value;
          syncEqValue(slider);
        });
      });
      button.disabled = true;
      postAndApply(form).then((reply) => flashButtonFeedback(button, reply && reply.ok));
    };

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

    // Post a form plus one extra action flag (e.g. radio_play=1). Needed
    // because postForm serialises FormData without the clicked submit
    // button, so action buttons are type="button" with an onclick instead.
    function submitFormAction(button, name) {
      const form = button ? button.form : null;
      if (!form) return;
      const flag = document.createElement('input');
      flag.type = 'hidden';
      flag.name = name;
      flag.value = '1';
      form.appendChild(flag);
      button.disabled = true;
      postAndApply(form).then((reply) => {
        flag.remove();
        button.disabled = false;
        flashButtonFeedback(button, reply && reply.ok);
      });
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
      if (window.RADIO_FAVS) {
        renderRadioFavs(window.RADIO_FAVS, window.RADIO_FAV_CUR);
        applyLanguage(currentLang()); // translate the freshly rendered rows
      }
      const expert = document.getElementById('audio-expert-mode');
      if (expert) {
        expert.checked = false;
        toggleAudioExpert(false);
      }
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

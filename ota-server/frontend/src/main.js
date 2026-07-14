// The full esm-bundler build (not the runtime-only default) is required because
// the app below uses an inline `template` string, which needs Vue's runtime
// template compiler. Without it the app never renders and the page is stuck on
// the "Loading NRL OTA…" fallback.
import { createApp, computed, ref, watch } from "vue/dist/vue.esm-bundler.js";
import "./style.css";

// The deployed OTA API already owns its /api/v1 routes; static files are
// served separately by the web server.
const apiURL = (path) => path;

// Board catalog for the public "功能介绍" section. Kept here (not in the DB) so
// the marketing copy ships with the frontend and stays bilingual. Order defines
// the order boards appear on the page and in the firmware history.
const boards = [
  {
    id: "gezipai",
    chip: "ESP32-S3",
    flashable: true,
    image: "/boards/gezipai.jpg",
    zh: {
      name: "格子派 gezipai",
      tagline: "ESP32-S3 彩色显示终端",
      features: ["彩色 LCD 显示（非触摸，按键操作）", "LVGL 图形界面", "Wi-Fi 联网、语音播报与 OTA 升级"],
    },
    en: {
      name: "Gezipai",
      tagline: "ESP32-S3 color display terminal",
      features: ["Color LCD (display-only, button controls)", "LVGL graphical UI", "Wi-Fi, voice prompts and OTA updates"],
    },
  },
  {
    id: "bh4tdv",
    chip: "ESP32-S3",
    flashable: true,
    image: "/boards/bh4tdv-esp32-3188.jpg",
    zh: {
      name: "BH4TDV ESP32 3188",
      tagline: "ESP32-S3 无屏电台接口",
      features: ["连接外部电台的射频接口", "无显示屏、精简固件", "Wi-Fi 远程控制与 AT 指令"],
    },
    en: {
      name: "BH4TDV ESP32 3188",
      tagline: "ESP32-S3 headless radio interface",
      features: ["Interfaces with an external transceiver", "Headless, minimal firmware", "Wi-Fi remote control and AT commands"],
    },
  },
  {
    id: "s31_korvo",
    chip: "ESP32-S31 · RISC-V",
    flashable: false,
    image: "/boards/s31-korvo.png",
    zh: {
      name: "S31 Korvo",
      tagline: "ESP32-S31 全功能开发板",
      features: ["RGB LCD 与电容触摸", "OV3660 摄像头（720p）", "音频编解码与 WS2812 灯效", "LVGL 图形界面"],
    },
    en: {
      name: "S31 Korvo",
      tagline: "ESP32-S31 full-featured dev board",
      features: ["RGB LCD with capacitive touch", "OV3660 camera (720p)", "Audio codec and WS2812 lighting", "LVGL graphical UI"],
    },
  },
  {
    id: "s31_function_coreboard",
    chip: "ESP32-S31 · RISC-V",
    flashable: false,
    image: "/boards/s31-function-coreboard.png",
    zh: {
      name: "S31 功能核心板",
      tagline: "ESP32-S31 精简核心板",
      features: ["紧凑无屏核心板", "WS2812 状态指示灯", "Wi-Fi 联网与 OTA 升级"],
    },
    en: {
      name: "S31 Function Coreboard",
      tagline: "ESP32-S31 compact core board",
      features: ["Compact, screenless core board", "WS2812 status indicator LED", "Wi-Fi networking and OTA updates"],
    },
  },
];

const messages = {
  zh: {
    title: "NRL OTA 固件中心",
    subtitle: "查看各板卡的固件版本与更新日志。管理员登录后可发布固件并查看联网设备状态。",
    language: "语言",
    chinese: "中文",
    english: "English",
    brandName: "NRL OTA",
    navHome: "首页",
    navFirmware: "固件下载",
    navFlash: "网页刷机",
    navDevices: "设备管理",
    navPublish: "发布固件",
    adminLogin: "管理登录",
    loginTitle: "管理员登录",
    username: "用户名",
    password: "密码",
    welcome: "{user}",
    boardsHeading: "支持的板卡",
    chip: "主控",
    features: "特性",
    historyHeading: "固件历史",
    latest: "最新",
    version: "版本",
    channel: "渠道",
    stable: "稳定版",
    beta: "测试版",
    size: "大小",
    notes: "更新日志",
    noNotes: "（无更新说明）",
    releasedAt: "发布时间",
    download: "下载",
    noReleases: "该板卡暂无已发布固件。",
    flashHeading: "USB 网页刷机",
    flashIntro: "用 USB 数据线连接设备，在 Chrome 或 Edge 浏览器里刷写完整固件（含引导程序与分区表）。需要 HTTPS 或本机访问。仅支持 ESP32-S3 板卡。",
    flashReady: "支持 USB 网页刷机",
    flashButton: "USB 刷机",
    flashUnsupported: "当前浏览器不支持 Web Serial，请使用 Chrome 或 Edge。",
    flashNeedsHttps:
      "当前页面使用非安全的 HTTP 地址。Web Serial 要求使用 HTTPS（localhost 除外），请通过配置了 HTTPS 的服务器地址访问。",
    flashNotAllowed: "Web Serial 权限被阻止，请检查浏览器的网站权限或管理员策略。",
    flashSerialOnly:
      "RISC-V 芯片暂不支持网页刷机，请使用串口烧录（scripts/build.py <board> flash）。",
    flashUnavailable: "该板卡暂未上传可刷写的固件包。",
    flashTip: "若无法识别设备，请按住 BOOT 再插入 USB 后重试。",
    loadFailed: "加载失败：{error}",
    adminArea: "管理员",
    adminHint: "输入管理员用户名和密码，登录后可查看设备状态并发布固件。",
    login: "登录",
    logout: "退出登录",
    loginFailed: "登录失败：{error}",
    loggedIn: "已登录",
    dashboardTitle: "设备管理",
    statTotal: "设备总数",
    statOnline: "在线设备",
    statBoards: "板卡型号",
    statOutdated: "待升级",
    onlineHint: "5 分钟内有上报即视为在线",
    status: "状态",
    statusOnline: "在线",
    statusOffline: "离线",
    upToDate: "最新",
    updateAvailable: "可升级",
    deviceStatusHeading: "设备状态",
    searchPlaceholder: "搜索 设备ID / 板卡 / 呼号 / IP…",
    filterAll: "全部",
    noMatch: "没有匹配的设备。",
    pageInfo: "第 {from}–{to} 条，共 {total} 条",
    prevPage: "上一页",
    nextPage: "下一页",
    pageOf: "{page} / {count}",
    refresh: "刷新",
    noDevices: "尚无设备上报。",
    deviceId: "设备 ID",
    board: "板卡",
    firmware: "固件",
    callsign: "NRL 呼号",
    ssid: "SSID",
    ipAddress: "IP 地址",
    lastSeen: "最后在线",
    publishHeading: "发布固件",
    boardType: "板卡类型",
    firmwareVersion: "版本",
    firmwareFile: "固件文件",
    releaseNotes: "更新日志",
    publish: "发布固件",
    publishing: "正在上传固件…",
    published: "已发布 {version}（{size} 字节）",
    uploadFailed: "发布失败：{error}",
    unknownError: "请求失败",
  },
  en: {
    title: "NRL OTA Firmware Center",
    subtitle:
      "Browse firmware versions and changelogs for every board. Administrators can publish firmware and view connected-device status after logging in.",
    language: "Language",
    chinese: "中文",
    english: "English",
    brandName: "NRL OTA",
    navHome: "Home",
    navFirmware: "Firmware",
    navFlash: "USB Flash",
    navDevices: "Devices",
    navPublish: "Publish",
    adminLogin: "Admin login",
    loginTitle: "Administrator login",
    username: "Username",
    password: "Password",
    welcome: "{user}",
    boardsHeading: "Supported boards",
    chip: "SoC",
    features: "Features",
    historyHeading: "Firmware history",
    latest: "Latest",
    version: "Version",
    channel: "Channel",
    stable: "Stable",
    beta: "Beta",
    size: "Size",
    notes: "Changelog",
    noNotes: "(no release notes)",
    releasedAt: "Released",
    download: "Download",
    noReleases: "No firmware has been published for this board yet.",
    flashHeading: "USB web flashing",
    flashIntro:
      "Connect the device over USB and flash the full firmware (bootloader and partition table included) from Chrome or Edge. Requires HTTPS or localhost. ESP32-S3 boards only.",
    flashReady: "Web-flashable over USB",
    flashButton: "Flash via USB",
    flashUnsupported: "This browser does not support Web Serial. Use Chrome or Edge.",
    flashNeedsHttps:
      "This page is using an insecure HTTP address. Web Serial requires HTTPS (except on localhost). Open the site through its HTTPS address.",
    flashNotAllowed:
      "Web Serial permission was blocked. Check the site's browser permissions or administrator policy.",
    flashSerialOnly:
      "RISC-V chip — not web-flashable. Use serial flashing (scripts/build.py <board> flash).",
    flashUnavailable: "No flashable firmware package has been staged for this board yet.",
    flashTip: "If the device is not detected, hold BOOT while plugging in USB, then retry.",
    loadFailed: "Load failed: {error}",
    adminArea: "Administrator",
    adminHint: "Sign in with the administrator username and password to view device status and publish firmware.",
    login: "Log in",
    logout: "Log out",
    loginFailed: "Login failed: {error}",
    loggedIn: "Logged in",
    dashboardTitle: "Device management",
    statTotal: "Total devices",
    statOnline: "Online",
    statBoards: "Board models",
    statOutdated: "Need update",
    onlineHint: "Reported within the last 5 minutes counts as online",
    status: "Status",
    statusOnline: "Online",
    statusOffline: "Offline",
    upToDate: "Up to date",
    updateAvailable: "Update available",
    deviceStatusHeading: "Device status",
    searchPlaceholder: "Search device ID / board / callsign / IP…",
    filterAll: "All",
    noMatch: "No devices match the filter.",
    pageInfo: "{from}–{to} of {total}",
    prevPage: "Prev",
    nextPage: "Next",
    pageOf: "{page} / {count}",
    refresh: "Refresh",
    noDevices: "No devices have reported yet.",
    deviceId: "Device ID",
    board: "Board",
    firmware: "Firmware",
    callsign: "NRL callsign",
    ssid: "SSID",
    ipAddress: "IP address",
    lastSeen: "Last seen",
    publishHeading: "Publish firmware",
    boardType: "Board type",
    firmwareVersion: "Version",
    firmwareFile: "Firmware file",
    releaseNotes: "Release notes",
    publish: "Publish firmware",
    publishing: "Uploading firmware…",
    published: "Published {version} ({size} bytes)",
    uploadFailed: "Publish failed: {error}",
    unknownError: "Request failed",
  },
};

function savedLanguage() {
  const saved = localStorage.otaLanguage;
  if (saved === "zh" || saved === "en") return saved;
  return navigator.language?.toLowerCase().startsWith("zh") ? "zh" : "en";
}

const app = createApp({
  setup() {
    const language = ref(savedLanguage());
    const view = ref("home");
    const session = ref(localStorage.otaSession || "");
    const sessionUser = ref(localStorage.otaUser || "");
    const authed = ref(false);
    const username = ref("");
    const password = ref("");
    const history = ref({}); // board id -> release[]
    const devices = ref([]);
    const loadError = ref("");
    const loginError = ref("");
    const secureContext = window.isSecureContext;
    const webSerialAvailable = "serial" in navigator;

    // publish form
    const board = ref("s31_korvo");
    const version = ref("");
    const channel = ref("stable");
    const notes = ref("");
    const firmware = ref();
    const publishMessage = ref("");

    const t = (key, values = {}) => {
      let text = messages[language.value][key] || messages.en[key] || key;
      for (const [name, value] of Object.entries(values)) text = text.replace(`{${name}}`, String(value));
      return text;
    };
    const setLanguage = (value) => {
      language.value = value;
    };
    const boardName = (id) => {
      const entry = boards.find((b) => b.id === id);
      return entry ? entry[language.value].name : id;
    };
    const requestError = async (response) => {
      const body = await response.json().catch(() => ({}));
      return body.error || t("unknownError");
    };

    async function loadHistory() {
      loadError.value = "";
      const next = {};
      try {
        await Promise.all(
          boards.map(async (b) => {
            const response = await fetch(apiURL(`/api/v1/releases?board=${encodeURIComponent(b.id)}`));
            if (!response.ok) throw new Error(await requestError(response));
            next[b.id] = (await response.json()).releases || [];
          }),
        );
        history.value = next;
      } catch (error) {
        loadError.value = t("loadFailed", { error: error.message });
      }
    }

    async function loadDevices() {
      const response = await fetch(apiURL("/api/v1/admin/devices"), { headers: { "X-OTA-Token": session.value } });
      if (!response.ok) throw new Error(await requestError(response));
      devices.value = (await response.json()).devices || [];
    }

    async function login() {
      loginError.value = "";
      try {
        const response = await fetch(apiURL("/api/v1/admin/login"), {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ username: username.value, password: password.value }),
        });
        if (!response.ok) throw new Error(await requestError(response));
        const body = await response.json();
        session.value = body.token;
        sessionUser.value = body.username;
        localStorage.otaSession = body.token;
        localStorage.otaUser = body.username;
        authed.value = true;
        password.value = "";
        await loadDevices();
        setView("devices");
      } catch (error) {
        authed.value = false;
        loginError.value = t("loginFailed", { error: error.message });
      }
    }
    function logout() {
      authed.value = false;
      devices.value = [];
      session.value = "";
      sessionUser.value = "";
      localStorage.removeItem("otaSession");
      localStorage.removeItem("otaUser");
      setView("home");
    }
    async function refreshDevices() {
      try {
        await loadDevices();
      } catch (error) {
        loginError.value = t("loginFailed", { error: error.message });
        authed.value = false;
        setView("login");
      }
    }

    // Restore an existing session on load by validating it against the API.
    async function restoreSession() {
      if (!session.value) return;
      try {
        await loadDevices();
        authed.value = true;
      } catch {
        logout();
      }
    }

    async function upload() {
      const file = firmware.value?.files?.[0];
      if (!file) return;
      publishMessage.value = t("publishing");
      const response = await fetch(apiURL("/api/v1/admin/releases"), {
        method: "POST",
        headers: {
          "X-OTA-Token": session.value,
          "X-Firmware-Board": board.value,
          "X-Firmware-Version": version.value,
          "X-Firmware-Channel": channel.value,
          "X-Release-Notes": encodeURIComponent(notes.value),
        },
        body: file,
      });
      if (!response.ok) {
        publishMessage.value = t("uploadFailed", { error: await requestError(response) });
        return;
      }
      const released = await response.json();
      publishMessage.value = t("published", { version: released.version, size: released.size });
      await Promise.all([loadHistory(), refreshDevices()]);
    }

    const formatTime = (timestamp) =>
      new Date(timestamp * 1000).toLocaleString(language.value === "zh" ? "zh-CN" : "en-US");
    const formatSize = (bytes) => {
      if (bytes < 1024) return `${bytes} B`;
      if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
      return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
    };
    const localizedBoards = computed(() =>
      boards.map((b) => ({ id: b.id, chip: b.chip, flashable: b.flashable, image: b.image, ...b[language.value] })),
    );

    // A board is only web-flashable if its full-flash manifest has been staged
    // into the server's data-dir. Probe so boards without a staged package show
    // a clear message instead of a button that fails when clicked.
    const flasherReady = ref({});
    async function loadFlasher() {
      const next = {};
      await Promise.all(
        boards
          .filter((b) => b.flashable)
          .map(async (b) => {
            try {
              const response = await fetch(apiURL(`/flasher/manifest-${b.id}.json`), { cache: "no-store" });
              next[b.id] = response.ok;
            } catch {
              next[b.id] = false;
            }
          }),
      );
      flasherReady.value = next;
    }

    // Views: home | firmware | flash | login | devices | publish. The devices
    // and publish views are gated behind a valid session; navigating to them
    // while logged out sends the user to login. Keep the URL hash in sync for
    // refresh/back support.
    const views = ["home", "firmware", "flash", "login", "devices", "publish"];
    const adminViews = ["devices", "publish"];
    function setView(next) {
      if (adminViews.includes(next) && !authed.value) next = "login";
      if (next === "login" && authed.value) next = "devices";
      if (!views.includes(next)) next = "home";
      view.value = next;
      if (location.hash.slice(1) !== next) location.hash = next;
    }
    function syncFromHash() {
      setView(location.hash.slice(1) || "home");
    }
    window.addEventListener("hashchange", syncFromHash);

    // Device-management dashboard summary.
    const nowSeconds = () => Math.floor(Date.now() / 1000);
    const isOnline = (device) => nowSeconds() - device.last_seen < 300; // 5 min
    const latestByBoard = computed(() => {
      const map = {};
      for (const b of boards) {
        const list = history.value[b.id] || [];
        if (list.length) map[b.id] = list[0].version;
      }
      return map;
    });
    const deviceRows = computed(() =>
      devices.value.map((d) => {
        const latest = latestByBoard.value[d.board_type];
        return {
          ...d,
          online: isOnline(d),
          latest,
          outdated: latest ? latest !== d.firmware_version : false,
        };
      }),
    );
    const stats = computed(() => {
      const rows = deviceRows.value;
      return {
        total: rows.length,
        online: rows.filter((d) => d.online).length,
        boards: new Set(rows.map((d) => d.board_type)).size,
        outdated: rows.filter((d) => d.outdated).length,
      };
    });

    // Search / filter / pagination for large fleets. Filtering is client-side;
    // the stat cards double as quick filters. (For very large deployments the
    // /admin/devices endpoint would move to server-side paging.)
    const search = ref("");
    const statusFilter = ref("all"); // all | online | offline | outdated
    const page = ref(1);
    const pageSize = 20;
    const setFilter = (value) => {
      statusFilter.value = statusFilter.value === value ? "all" : value;
    };
    const filteredRows = computed(() => {
      const q = search.value.trim().toLowerCase();
      return deviceRows.value.filter((d) => {
        if (statusFilter.value === "online" && !d.online) return false;
        if (statusFilter.value === "offline" && d.online) return false;
        if (statusFilter.value === "outdated" && !d.outdated) return false;
        if (!q) return true;
        const hay = [
          d.device_id,
          d.board_type,
          boardName(d.board_type),
          d.firmware_version,
          d.ip_address,
          d.metadata?.nrl_callsign,
          d.metadata?.nrl_ssid,
        ]
          .filter(Boolean)
          .join(" ")
          .toLowerCase();
        return hay.includes(q);
      });
    });
    const pageCount = computed(() => Math.max(1, Math.ceil(filteredRows.value.length / pageSize)));
    const pagedRows = computed(() => {
      const start = (page.value - 1) * pageSize;
      return filteredRows.value.slice(start, start + pageSize);
    });
    const pageFrom = computed(() => (filteredRows.value.length ? (page.value - 1) * pageSize + 1 : 0));
    const pageTo = computed(() => Math.min(page.value * pageSize, filteredRows.value.length));
    const goPage = (delta) => {
      page.value = Math.min(pageCount.value, Math.max(1, page.value + delta));
    };
    watch([search, statusFilter], () => {
      page.value = 1;
    });

    watch(
      language,
      (value) => {
        localStorage.otaLanguage = value;
        document.documentElement.lang = value === "zh" ? "zh-CN" : "en";
        document.title = t("title");
      },
      { immediate: true },
    );

    loadHistory();
    loadFlasher();
    restoreSession().finally(syncFromHash);

    return {
      language,
      view,
      setView,
      authed,
      sessionUser,
      username,
      password,
      history,
      devices,
      deviceRows,
      stats,
      search,
      statusFilter,
      setFilter,
      pagedRows,
      filteredRows,
      page,
      pageCount,
      pageFrom,
      pageTo,
      goPage,
      loadError,
      loginError,
      flasherReady,
      secureContext,
      webSerialAvailable,
      board,
      version,
      channel,
      notes,
      firmware,
      publishMessage,
      t,
      setLanguage,
      boardName,
      localizedBoards,
      loadHistory,
      login,
      logout,
      refreshDevices,
      upload,
      formatTime,
      formatSize,
    };
  },
  template: `
    <div class="app">
      <header class="navbar">
        <div class="nav-brand" @click="setView('home')">
          <span class="nav-logo">◉</span><span class="nav-title">{{ t('brandName') }}</span>
        </div>
        <nav class="nav-menu">
          <button :class="{ active: view === 'home' }" @click="setView('home')">{{ t('navHome') }}</button>
          <button :class="{ active: view === 'firmware' }" @click="setView('firmware')">{{ t('navFirmware') }}</button>
          <button :class="{ active: view === 'flash' }" @click="setView('flash')">{{ t('navFlash') }}</button>
          <button v-if="authed" :class="{ active: view === 'devices' }" @click="setView('devices')">{{ t('navDevices') }}</button>
          <button v-if="authed" :class="{ active: view === 'publish' }" @click="setView('publish')">{{ t('navPublish') }}</button>
        </nav>
        <div class="nav-right">
          <div class="language" :aria-label="t('language')">
            <button :class="{ active: language === 'zh' }" @click="setLanguage('zh')">中</button>
            <button :class="{ active: language === 'en' }" @click="setLanguage('en')">EN</button>
          </div>
          <template v-if="authed">
            <span class="user-chip"><span class="user-avatar">{{ sessionUser.slice(0, 1).toUpperCase() }}</span>{{ sessionUser }}</span>
            <button class="ghost" @click="logout">{{ t('logout') }}</button>
          </template>
          <button v-else class="primary" @click="setView('login')">{{ t('adminLogin') }}</button>
        </div>
      </header>

      <main class="content">
        <!-- Home: board introductions -->
        <section v-if="view === 'home'" class="view">
          <div class="view-head">
            <h1>{{ t('title') }}</h1>
            <p class="subtitle">{{ t('subtitle') }}</p>
          </div>
          <h2 class="section-h">{{ t('boardsHeading') }}</h2>
          <div class="board-grid">
            <article v-for="b in localizedBoards" :key="b.id" class="board-card">
              <img class="board-image" :src="b.image" :alt="b.name" />
              <div class="board-card-head">
                <h3>{{ b.name }}</h3>
                <span class="chip">{{ b.chip }}</span>
              </div>
              <p class="tagline">{{ b.tagline }}</p>
              <ul class="features">
                <li v-for="feature in b.features" :key="feature">{{ feature }}</li>
              </ul>
              <code class="board-id">{{ b.id }}</code>
            </article>
          </div>
        </section>

        <!-- Firmware download / history -->
        <section v-else-if="view === 'firmware'" class="view">
          <div class="view-head row">
            <h1>{{ t('navFirmware') }}</h1>
            <button class="ghost" @click="loadHistory">{{ t('refresh') }}</button>
          </div>
          <p v-if="loadError" class="error">{{ loadError }}</p>
          <div v-for="b in localizedBoards" :key="b.id" class="panel board-history">
            <div class="board-history-head"><h3>{{ b.name }}</h3><code class="board-id">{{ b.id }}</code></div>
            <div class="table-scroll" v-if="(history[b.id] || []).length">
              <table>
                <thead>
                  <tr><th>{{ t('version') }}</th><th>{{ t('channel') }}</th><th>{{ t('size') }}</th><th class="notes-col">{{ t('notes') }}</th><th>{{ t('releasedAt') }}</th><th></th></tr>
                </thead>
                <tbody>
                  <tr v-for="(release, index) in history[b.id]" :key="release.version + release.channel">
                    <td><strong>{{ release.version }}</strong> <span v-if="index === 0" class="badge latest">{{ t('latest') }}</span></td>
                    <td><span class="badge" :class="release.channel">{{ release.channel === 'stable' ? t('stable') : t('beta') }}</span></td>
                    <td>{{ formatSize(release.size) }}</td>
                    <td class="notes-col">{{ release.notes || t('noNotes') }}</td>
                    <td>{{ formatTime(release.created_at) }}</td>
                    <td><a class="download" :href="release.url">{{ t('download') }}</a></td>
                  </tr>
                </tbody>
              </table>
            </div>
            <p v-else class="empty">{{ t('noReleases') }}</p>
          </div>
        </section>

        <!-- USB web flashing -->
        <section v-else-if="view === 'flash'" class="view">
          <div class="view-head">
            <h1>{{ t('flashHeading') }}</h1>
            <p class="subtitle">{{ t('flashIntro') }}</p>
          </div>
          <div class="board-grid">
            <article v-for="b in localizedBoards" :key="b.id" class="board-card flash-card">
              <div class="board-card-head">
                <h3>{{ b.name }}</h3>
                <span class="chip">{{ b.chip }}</span>
              </div>
              <template v-if="b.flashable">
                <p class="tagline">{{ t('flashReady') }}</p>
                <template v-if="flasherReady[b.id]">
                  <p v-if="!secureContext" class="unsupported">{{ t('flashNeedsHttps') }}</p>
                  <p v-else-if="!webSerialAvailable" class="unsupported">{{ t('flashUnsupported') }}</p>
                  <template v-else>
                    <esp-web-install-button :manifest="apiURL('/flasher/manifest-' + b.id + '.json')">
                      <button slot="activate" class="flash-btn primary">{{ t('flashButton') }}</button>
                      <span slot="unsupported" class="unsupported">{{ t('flashUnsupported') }}</span>
                      <span slot="not-allowed" class="unsupported">{{ t('flashNotAllowed') }}</span>
                    </esp-web-install-button>
                    <p class="flash-tip">{{ t('flashTip') }}</p>
                  </template>
                </template>
                <p v-else class="empty">{{ t('flashUnavailable') }}</p>
              </template>
              <p v-else class="tagline serial-only">{{ t('flashSerialOnly') }}</p>
              <code class="board-id">{{ b.id }}</code>
            </article>
          </div>
        </section>

        <!-- Admin login -->
        <section v-else-if="view === 'login'" class="view login-view">
          <div class="login-card">
            <div class="login-logo">◉</div>
            <h2>{{ t('loginTitle') }}</h2>
            <p class="hint">{{ t('adminHint') }}</p>
            <label>{{ t('username') }}<input v-model="username" autocomplete="username" @keyup.enter="login"></label>
            <label>{{ t('password') }}<input v-model="password" type="password" autocomplete="current-password" @keyup.enter="login"></label>
            <button class="primary block" @click="login">{{ t('login') }}</button>
            <p v-if="loginError" class="error">{{ loginError }}</p>
          </div>
        </section>

        <!-- Device management dashboard -->
        <section v-else-if="view === 'devices'" class="view">
          <div class="view-head row">
            <h1>{{ t('dashboardTitle') }}</h1>
            <button class="ghost" @click="refreshDevices">{{ t('refresh') }}</button>
          </div>
          <div class="stat-grid">
            <button type="button" class="stat-card" :class="{ active: statusFilter === 'all' }" @click="statusFilter = 'all'">
              <span class="stat-num">{{ stats.total }}</span><span class="stat-label">{{ t('statTotal') }}</span>
            </button>
            <button type="button" class="stat-card" :class="{ active: statusFilter === 'online' }" @click="setFilter('online')">
              <span class="stat-num accent-green">{{ stats.online }}</span><span class="stat-label">{{ t('statOnline') }}</span>
            </button>
            <div class="stat-card static">
              <span class="stat-num">{{ stats.boards }}</span><span class="stat-label">{{ t('statBoards') }}</span>
            </div>
            <button type="button" class="stat-card" :class="{ active: statusFilter === 'outdated' }" @click="setFilter('outdated')">
              <span class="stat-num accent-amber">{{ stats.outdated }}</span><span class="stat-label">{{ t('statOutdated') }}</span>
            </button>
          </div>
          <p v-if="loginError" class="error">{{ loginError }}</p>
          <div class="panel table-panel">
            <div class="table-toolbar">
              <input class="search" v-model="search" type="search" :placeholder="t('searchPlaceholder')">
              <span class="muted-sm">{{ t('onlineHint') }}</span>
            </div>
            <div class="table-scroll sticky" v-if="filteredRows.length">
              <table>
                <thead>
                  <tr><th>{{ t('status') }}</th><th>{{ t('deviceId') }}</th><th>{{ t('board') }}</th><th>{{ t('firmware') }}</th><th>{{ t('callsign') }}</th><th>{{ t('ssid') }}</th><th>{{ t('ipAddress') }}</th><th>{{ t('lastSeen') }}</th></tr>
                </thead>
                <tbody>
                  <tr v-for="d in pagedRows" :key="d.device_id">
                    <td><span class="status"><span class="dot" :class="d.online ? 'on' : 'off'"></span>{{ d.online ? t('statusOnline') : t('statusOffline') }}</span></td>
                    <td class="mono">{{ d.device_id }}</td>
                    <td>{{ boardName(d.board_type) }}</td>
                    <td>{{ d.firmware_version }} <span v-if="d.outdated" class="badge beta">{{ t('updateAvailable') }}</span><span v-else-if="d.latest" class="badge stable">{{ t('upToDate') }}</span></td>
                    <td>{{ d.metadata?.nrl_callsign || '-' }}</td>
                    <td>{{ d.metadata?.nrl_ssid ?? '-' }}</td>
                    <td class="mono">{{ d.ip_address }}</td>
                    <td>{{ formatTime(d.last_seen) }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
            <p v-else-if="deviceRows.length" class="empty">{{ t('noMatch') }}</p>
            <p v-else class="empty">{{ t('noDevices') }}</p>
            <div class="pager" v-if="pageCount > 1">
              <span class="muted-sm">{{ t('pageInfo', { from: pageFrom, to: pageTo, total: filteredRows.length }) }}</span>
              <div class="pager-controls">
                <button class="ghost" :disabled="page <= 1" @click="goPage(-1)">{{ t('prevPage') }}</button>
                <span class="muted-sm">{{ t('pageOf', { page: page, count: pageCount }) }}</span>
                <button class="ghost" :disabled="page >= pageCount" @click="goPage(1)">{{ t('nextPage') }}</button>
              </div>
            </div>
          </div>
        </section>

        <!-- Publish firmware -->
        <section v-else-if="view === 'publish'" class="view">
          <div class="view-head"><h1>{{ t('publishHeading') }}</h1></div>
          <div class="panel">
            <div class="publish-grid">
              <label>{{ t('boardType') }}<select v-model="board"><option v-for="b in localizedBoards" :key="b.id" :value="b.id">{{ b.name }}</option></select></label>
              <label>{{ t('firmwareVersion') }}<input v-model="version" :placeholder="t('firmwareVersion')"></label>
              <label>{{ t('channel') }}<select v-model="channel"><option value="stable">{{ t('stable') }}</option><option value="beta">{{ t('beta') }}</option></select></label>
              <label>{{ t('firmwareFile') }}<input ref="firmware" type="file" accept=".bin"></label>
              <label class="wide">{{ t('releaseNotes') }}<textarea v-model="notes" :placeholder="t('releaseNotes')"></textarea></label>
              <div class="actions wide"><button class="primary" @click="upload">{{ t('publish') }}</button><span class="message" aria-live="polite">{{ publishMessage }}</span></div>
            </div>
          </div>
        </section>
      </main>

      <footer class="app-footer">{{ t('brandName') }} · {{ t('subtitle') }}</footer>
    </div>`,
});

// <esp-web-install-button> is a custom element defined by esp-web-tools; tell the
// Vue template compiler not to treat it as a Vue component.
app.config.compilerOptions.isCustomElement = (tag) => tag.startsWith("esp-web-");
app.mount("#app");

// Load the esp-web-tools web component at runtime (kept out of the Vite bundle so
// its own relative chunk imports resolve against /esp-web-tools/).
if (!document.querySelector("script[data-esp-web-tools]")) {
  const script = document.createElement("script");
  script.type = "module";
  script.src = "/esp-web-tools/install-button.js";
  script.dataset.espWebTools = "1";
  document.head.appendChild(script);
}

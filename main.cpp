// 3drag - Windows 三指拖拽工具（CLI）
// 仅依赖 Win32 + HID API，MinGW-w64 单文件编译。
//
// 用法：
//   3drag start    后台启动 daemon
//   3drag stop     停止 daemon
//   3drag reload   重新加载 3drag.ini
//   3drag status   查询是否在运行
//   3drag autostart enabled|disabled
//   3drag help     帮助
//
// 内部：
//   3drag --daemon  实际的守护进程（由 start 用 DETACHED_PROCESS 派生，不要手动调）
//
// 工作原理：
//   1. 隐藏 message-only 窗口 + RegisterRawInputDevices 订阅 PTP (UsagePage 0x0D / Usage 0x05)
//   2. 解析每帧 HID 报告，按 LinkCollection 取出每根手指的 X/Y/TipSwitch/ContactId
//   3. 状态机：3 指落下 -> LEFTDOWN；3 指移动 -> SendInput MOVE + SetCursorPos；任一指抬起 -> LEFTUP

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdarg.h>

extern "C" {
#include <hidsdi.h>
}
#include <shellapi.h>

// ============================================================================
// 常量
// ============================================================================

#define MAX_FINGERS        16   // 同时跟踪的最大触点数（按 ContactId 索引）
#define MAX_DEVICES        8    // 同时缓存的最大触摸板设备数
#define MAX_LINKS          16   // 每个设备最多解析的 finger LinkCollection 数
#define DRAG_FINGERS       3    // 触发拖拽要求的手指数（严格相等：4 指及以上留给其它手势）

// HID_USAGE_PAGE_GENERIC / HID_USAGE_GENERIC_X/Y / HID_USAGE_PAGE_DIGITIZER /
// HID_USAGE_DIGITIZER_TIP_SWITCH 已由 <hidusage.h> 提供。
// 这里仅补一个标准没给出的常量：Contact Identifier
#define HID_USAGE_DIGITIZER_CONTACT_ID ((USAGE)0x51)

// 跨进程 IPC 标识
static const wchar_t* WINDOW_CLASS_W   = L"3dragMsgWnd";
static const wchar_t* MUTEX_NAME_W     = L"Local\\3drag_single_instance_mutex_v1";
static const wchar_t* INI_NAME_W       = L"3drag.ini";
static const wchar_t* LOG_NAME_W       = L"3drag.log";
static const wchar_t* APPDIR_NAME_W    = L"3drag";   // 子目录名（%APPDATA% 下）
static const wchar_t* AUTOSTART_KEY_W  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* AUTOSTART_VAL_W  = L"3drag";

// 首次 start 时若 ini 不存在，写入这份带注释的模板
static const char* DEFAULT_INI_TEMPLATE =
    "; 3drag config. Edit and run: 3drag reload\r\n"
    ";\r\n"
    "; sensitivity : 100 = 1:1; lower = slower, higher = faster\r\n"
    "; dt_clamp    : single-frame spike threshold in touchpad units; 0 disables\r\n"
    "; debug       : 1 = write 3drag.log next to this ini (large volume)\r\n"
    "; autostart   : 1 = register in HKCU Run (runs '3drag start' at logon)\r\n"
    "\r\n"
    "[general]\r\n"
    "sensitivity = 120\r\n"
    "dt_clamp    = 50\r\n"
    "debug       = 0\r\n"
    "autostart   = 1\r\n";

#define WM_APP_RELOAD  (WM_APP + 1)

// ============================================================================
// 数据结构
// ============================================================================

// 每个 finger collection 的 X/Y 量程信息
struct FingerLink {
    USHORT lc;          // HID LinkCollection 索引
    LONG   xMin, xMax;
    LONG   yMin, yMax;
    bool   hasX, hasY;
};

// 每个 HID 设备的缓存信息
struct DeviceCache {
    HANDLE                hDevice;
    PHIDP_PREPARSED_DATA  preparsed;
    FingerLink            links[MAX_LINKS];
    int                   linkCount;
};

// 当前帧每根手指的状态
struct Finger {
    bool down;
    LONG x, y;
};

// 用户可配置项
struct AppConfig {
    int sensitivity;   // 灵敏度百分比；100 = 1.0
    int dt_clamp;      // 孤立尖刺钳位阈值；0 = 关闭
    int debug;         // 0/1：1 时写日志到 exe 同目录 3drag.log，含全部高频事件
    int autostart;     // 0/1：1 时写入 HKCU Run 注册表项
};

// ============================================================================
// 全局状态（运行期零堆分配，全部在 .bss）
// ============================================================================

// 灵敏度系数：1.0 = 全触摸板对应一倍主屏宽。由 ApplyConfig 从 g_cfg.sensitivity 写入。
static float g_sens = 1.2f;

// 单帧 HID 报告的"假位移"阈值（触摸板坐标单位）。由 ApplyConfig 从 g_cfg.dt_clamp 写入。
// 判定方式是 **孤立尖刺检测**：当前 dt > DT_CLAMP **并且** 上一帧 dt < DT_CLAMP/2 → 视为尖刺
// 连续大 dt（真实甩动）→ 全部放行。
static int   DT_CLAMP = 50;

// 设备/手指
static DeviceCache g_devices[MAX_DEVICES];
static int         g_deviceCount = 0;
static Finger      g_fingers[MAX_FINGERS];

// 状态机
static bool g_dragging   = false;
static int  g_primaryCid = -1;
static LONG g_lastTouchX = 0;
static LONG g_lastTouchY = 0;
static LONG g_curLogMaxX = 1;
static LONG g_curLogMaxY = 1;

// 诊断用：上一条 [move] 日志的时间戳
static ULONGLONG g_lastMoveLogMs = 0;

// 时间控制
static ULONGLONG g_lastSaw3FingersMs   = 0;  // 最近一次看到 n==3 的时间戳（LEFTUP 去抖）
static ULONGLONG g_threeFingersSinceMs = 0;  // n 进入 ==3 状态的时间戳（LEFTDOWN 去抖）
static HWND      g_hwnd                = nullptr;

// LEFTUP 去抖：n 偏离 3 之后再观察这段时间，期间若 n 又回到 3 视为偶发干扰、继续保持拖拽。
static const ULONGLONG RELEASE_DEBOUNCE_MS = 50;
static const UINT_PTR  TIMER_RELEASE       = 1;
// LEFTDOWN 去抖：n 必须稳定 ==3 持续这么久才触发拖拽，挡住 4/5 指手势途经 n=3 的瞬态。
static const ULONGLONG START_DEBOUNCE_MS   = 20;

// 亚像素累加器：本帧 dt*k 的分数部分留下，下一帧继续累加，攒够 1px 才下发。
static double g_subX = 0.0;
static double g_subY = 0.0;
// 上一帧（钳位后）的 max(|dx|,|dy|)，用于尖刺检测。LEFTDOWN 时清零。
static LONG   g_prevDtMag = 0;

// 配置（默认值由 ConfigDefaults 写入，运行期由 ApplyConfig 应用到上面那些 g_sens / DT_CLAMP / g_debug）
static AppConfig g_cfg = { 120, 50, 0, 1 };

// 路径（启动时各算一次）
static wchar_t g_iniPath[MAX_PATH] = {};
static wchar_t g_logPath[MAX_PATH] = {};

// 日志
static HANDLE g_logFile         = INVALID_HANDLE_VALUE;
static int    g_lastFingerCount = -1;
static bool   g_debug           = false;

// ============================================================================
// CLI 输出（写到 stdout；ASCII 即可，所有用户可见信息都是英文）
// ============================================================================

static void Print(const char* s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, s, lstrlenA(s), &written, nullptr);
}

static void PrintLine(const char* fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    Print(buf);
    Print("\r\n");
}

// ============================================================================
// 调试日志（仅 debug=1 时启用）
// ============================================================================

static void Log(const char* s)
{
    if (!g_debug || !s) return;
    OutputDebugStringA(s);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(g_logFile, s, lstrlenA(s), &wr, nullptr);
        WriteFile(g_logFile, "\r\n", 2, &wr, nullptr);
    }
}

static void LogF(const char* fmt, ...)
{
    if (!g_debug) return;
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    Log(buf);
}

static void OpenLog()
{
    if (g_logFile != INVALID_HANDLE_VALUE) return;
    if (g_logPath[0] == 0) return;
    g_logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void CloseLog()
{
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

// ============================================================================
// 路径助手
// ============================================================================

static void BuildPath(wchar_t* out, size_t cap, const wchar_t* dir, const wchar_t* name)
{
    lstrcpynW(out, dir, (int)cap);
    int dirLen = lstrlenW(out);
    int nameLen = lstrlenW(name);
    if (dirLen + nameLen + 1 < (int)cap) {
        lstrcatW(out, name);
    }
}

// 解析 %APPDATA%\3drag\ 并按需创建该目录。out 末尾带反斜杠，便于直接拼文件名。
static bool ResolveUserDir(wchar_t* out, size_t cap)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    int baseLen = lstrlenW(base);
    int subLen  = lstrlenW(APPDIR_NAME_W);
    // base + '\' + sub + '\' + '\0'
    if (baseLen + 1 + subLen + 1 + 1 > (int)cap) return false;

    lstrcpynW(out, base, (int)cap);
    if (baseLen > 0 && out[baseLen - 1] != L'\\') {
        out[baseLen] = L'\\';
        out[baseLen + 1] = 0;
    }
    lstrcatW(out, APPDIR_NAME_W);
    lstrcatW(out, L"\\");

    CreateDirectoryW(out, nullptr);   // 已存在返回 FALSE/ERROR_ALREADY_EXISTS，忽略
    return true;
}

// 解析 %APPDATA%\3drag\3drag.ini 和 .log 路径，写入 g_iniPath / g_logPath。
static bool InitUserPaths()
{
    wchar_t userDir[MAX_PATH];
    if (!ResolveUserDir(userDir, MAX_PATH)) return false;
    BuildPath(g_iniPath, MAX_PATH, userDir, INI_NAME_W);
    BuildPath(g_logPath, MAX_PATH, userDir, LOG_NAME_W);
    return true;
}

// 若 ini 不存在则写入默认模板。返回 true 表示本次写入了新文件。
static bool WriteDefaultIniIfMissing(const wchar_t* path)
{
    if (path[0] == 0) return false;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return false;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    WriteFile(h, DEFAULT_INI_TEMPLATE, lstrlenA(DEFAULT_INI_TEMPLATE), &wr, nullptr);
    CloseHandle(h);
    return true;
}

// ============================================================================
// 配置：加载 / 应用 / 开机启动
// ============================================================================

static int ReadBoolFromIni(const wchar_t* section, const wchar_t* key, int def, const wchar_t* path)
{
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 32, path);
    if (buf[0] == 0) return def;
    for (wchar_t* p = buf; *p; ++p) {
        if (*p >= L'A' && *p <= L'Z') *p = (wchar_t)(*p + 32);
    }
    if (lstrcmpW(buf, L"1") == 0 || lstrcmpW(buf, L"true") == 0
        || lstrcmpW(buf, L"yes") == 0 || lstrcmpW(buf, L"on") == 0) return 1;
    if (lstrcmpW(buf, L"0") == 0 || lstrcmpW(buf, L"false") == 0
        || lstrcmpW(buf, L"no") == 0 || lstrcmpW(buf, L"off") == 0) return 0;
    return def;
}

static void LoadConfigFromIni(const wchar_t* path, AppConfig& cfg)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;   // 没有 ini，保持默认
    cfg.sensitivity = (int)GetPrivateProfileIntW(L"general", L"sensitivity", cfg.sensitivity, path);
    cfg.dt_clamp    = (int)GetPrivateProfileIntW(L"general", L"dt_clamp",    cfg.dt_clamp,    path);
    cfg.debug       = ReadBoolFromIni(L"general", L"debug",     cfg.debug,     path);
    cfg.autostart   = ReadBoolFromIni(L"general", L"autostart", cfg.autostart, path);

    // 兜底防呆
    if (cfg.sensitivity < 1)     cfg.sensitivity = 1;
    if (cfg.sensitivity > 1000)  cfg.sensitivity = 1000;
    if (cfg.dt_clamp < 0)        cfg.dt_clamp = 0;
    if (cfg.dt_clamp > 100000)   cfg.dt_clamp = 100000;
}

static void WriteIntToIni(const wchar_t* section, const wchar_t* key, int val, const wchar_t* path)
{
    wchar_t buf[16];
    wsprintfW(buf, L"%d", val);
    WritePrivateProfileStringW(section, key, buf, path);
}

static bool ReadAutostartEnabled()
{
    HKEY hKey;
    LSTATUS rc = RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY_W,
                                0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    rc = RegQueryValueExW(hKey, AUTOSTART_VAL_W, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

static void SetAutostart(bool enable)
{
    HKEY hKey;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY_W,
                                  0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS) return;

    if (enable) {
        wchar_t exePath[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            // 注册表值：`"C:\path\3drag.exe" start`
            wchar_t value[MAX_PATH + 32];
            wsprintfW(value, L"\"%s\" start", exePath);
            DWORD bytes = (DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t));
            RegSetValueExW(hKey, AUTOSTART_VAL_W, 0, REG_SZ, (const BYTE*)value, bytes);
        }
    } else {
        RegDeleteValueW(hKey, AUTOSTART_VAL_W);
    }
    RegCloseKey(hKey);
}

// 从 ini 同步 HKCU Run（reload / autostart 命令在 CLI 进程调用，不依赖 daemon）
static void ApplyAutostartFromIni()
{
    if (!InitUserPaths()) return;
    AppConfig cfg = { 120, 50, 0, 1 };
    LoadConfigFromIni(g_iniPath, cfg);
    SetAutostart(cfg.autostart != 0);
}

static void ApplyConfig(const AppConfig& cfg)
{
    g_sens   = (float)cfg.sensitivity / 100.0f;
    DT_CLAMP = cfg.dt_clamp;

    bool wantDebug = (cfg.debug != 0);
    if (g_debug && !wantDebug) {
        Log("[config] debug -> off, closing log");
        g_debug = false;
        CloseLog();
    } else {
        g_debug = wantDebug;
        if (g_debug && g_logFile == INVALID_HANDLE_VALUE) {
            OpenLog();
        }
    }

    SetAutostart(cfg.autostart != 0);
}

// ============================================================================
// 设备缓存：首次见到某 HID 设备时解析 ValueCaps，按 LinkCollection 建立 finger 表。
// ============================================================================

static DeviceCache* GetDeviceCache(HANDLE hDevice)
{
    for (int i = 0; i < g_deviceCount; ++i) {
        if (g_devices[i].hDevice == hDevice) return &g_devices[i];
    }
    if (g_deviceCount >= MAX_DEVICES) return nullptr;

    DeviceCache d;
    d.hDevice   = hDevice;
    d.preparsed = nullptr;
    d.linkCount = 0;

    UINT size = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) != 0) return nullptr;
    if (size == 0) return nullptr;

    d.preparsed = (PHIDP_PREPARSED_DATA)HeapAlloc(GetProcessHeap(), 0, size);
    if (!d.preparsed) return nullptr;

    UINT got = size;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, d.preparsed, &got) == (UINT)-1) {
        HeapFree(GetProcessHeap(), 0, d.preparsed);
        return nullptr;
    }

    HIDP_CAPS caps;
    if (HidP_GetCaps(d.preparsed, &caps) != HIDP_STATUS_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, d.preparsed);
        return nullptr;
    }

    USHORT nValueCaps = caps.NumberInputValueCaps;
    if (nValueCaps == 0) {
        HeapFree(GetProcessHeap(), 0, d.preparsed);
        return nullptr;
    }

    HIDP_VALUE_CAPS* vcaps =
        (HIDP_VALUE_CAPS*)HeapAlloc(GetProcessHeap(), 0, sizeof(HIDP_VALUE_CAPS) * nValueCaps);
    if (!vcaps) {
        HeapFree(GetProcessHeap(), 0, d.preparsed);
        return nullptr;
    }

    if (HidP_GetValueCaps(HidP_Input, vcaps, &nValueCaps, d.preparsed) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < nValueCaps; ++i) {
            const HIDP_VALUE_CAPS& vc = vcaps[i];
            if (vc.UsagePage != HID_USAGE_PAGE_GENERIC) continue;

            USAGE usage = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            if (usage != HID_USAGE_GENERIC_X && usage != HID_USAGE_GENERIC_Y) continue;

            FingerLink* link = nullptr;
            for (int k = 0; k < d.linkCount; ++k) {
                if (d.links[k].lc == vc.LinkCollection) { link = &d.links[k]; break; }
            }
            if (!link) {
                if (d.linkCount >= MAX_LINKS) continue;
                link = &d.links[d.linkCount++];
                link->lc    = vc.LinkCollection;
                link->hasX  = false;
                link->hasY  = false;
                link->xMin  = 0; link->xMax = 0;
                link->yMin  = 0; link->yMax = 0;
            }
            if (usage == HID_USAGE_GENERIC_X) {
                link->xMin = vc.LogicalMin;
                link->xMax = vc.LogicalMax;
                link->hasX = true;
            } else {
                link->yMin = vc.LogicalMin;
                link->yMax = vc.LogicalMax;
                link->hasY = true;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, vcaps);

    // 仅保留同时有 X 和 Y 的 link
    int kept = 0;
    for (int k = 0; k < d.linkCount; ++k) {
        if (d.links[k].hasX && d.links[k].hasY) {
            if (kept != k) d.links[kept] = d.links[k];
            ++kept;
        }
    }
    d.linkCount = kept;

    if (d.linkCount == 0) {
        LogF("[device] hDevice=0x%X has no usable finger link (skipped)",
             (unsigned)(ULONG_PTR)hDevice);
        HeapFree(GetProcessHeap(), 0, d.preparsed);
        return nullptr;
    }

    g_devices[g_deviceCount] = d;
    LogF("[device] hDevice=0x%X registered: linkCount=%d xMax=%d yMax=%d",
         (unsigned)(ULONG_PTR)hDevice, d.linkCount,
         (int)d.links[0].xMax, (int)d.links[0].yMax);
    for (int k = 0; k < d.linkCount; ++k) {
        LogF("  link[%d] lc=%u xMax=%d yMax=%d", k,
             (unsigned)d.links[k].lc, (int)d.links[k].xMax, (int)d.links[k].yMax);
    }
    return &g_devices[g_deviceCount++];
}

// ============================================================================
// 鼠标输入注入
// ============================================================================

static void SendMouseFlag(DWORD flag)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof(in));
}

// 屏幕像素 -> SendInput 归一化绝对坐标（0..65536，覆盖整个虚拟桌面）。
static LONG ScreenToNormAbs(LONG pos, int smOrigin, int smSize)
{
    int origin = GetSystemMetrics(smOrigin);
    int size   = GetSystemMetrics(smSize);
    if (size <= 0) return 0;
    return MulDiv(pos - origin, 65536, size);
}

// 把光标移到指定屏幕像素坐标。
//
// 必须走 SendInput(MOUSEEVENTF_MOVE)：任务栏图标重排、OLE 拖放（资源管理器文件、
// Windows Terminal 选中文本等）只认输入队列里的 WM_MOUSEMOVE，SetCursorPos 不发消息。
// 绝对坐标 + VIRTUALDESK：不受 EnhancePointerPrecision 影响，多显示器用虚拟桌面坐标。
// SetCursorPos 再钉到目标像素，抵消 65536 量化误差，下一帧 GetCursorPos+delta 不漂移。
static void MoveCursorTo(LONG screenX, LONG screenY)
{
    INPUT in = {};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = ScreenToNormAbs(screenX, SM_XVIRTUALSCREEN, SM_CXVIRTUALSCREEN);
    in.mi.dy      = ScreenToNormAbs(screenY, SM_YVIRTUALSCREEN, SM_CYVIRTUALSCREEN);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
    SendInput(1, &in, sizeof(in));
    SetCursorPos((int)screenX, (int)screenY);
}

// 在当前 g_fingers 中选 cid 最小的 down=true 那根作为主导手指；找不到返回 -1。
static int PickPrimaryCid()
{
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (g_fingers[i].down) return i;
    }
    return -1;
}

// 设主导手指：记录当前触摸板位置，作为下一帧计算位移的基准。
static void SetPrimary(int cid)
{
    g_primaryCid = cid;
    g_lastTouchX = g_fingers[cid].x;
    g_lastTouchY = g_fingers[cid].y;
}

// ============================================================================
// WM_INPUT 处理：解析 HID 报告 -> 更新手指状态 -> 跑状态机
// ============================================================================

static void HandleRawInput(HRAWINPUT hRawInput)
{
    UINT size = 0;
    if (GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) return;
    if (size == 0 || size > 8192) return;

    BYTE buffer[8192];
    UINT got = GetRawInputData(hRawInput, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
    if (got == (UINT)-1 || got == 0) return;

    RAWINPUT* ri = (RAWINPUT*)buffer;
    if (ri->header.dwType != RIM_TYPEHID) return;

    DeviceCache* d = GetDeviceCache(ri->header.hDevice);
    if (!d || !d->preparsed || d->linkCount == 0) return;

    DWORD reportSize  = ri->data.hid.dwSizeHid;
    DWORD reportCount = ri->data.hid.dwCount;
    BYTE* reports     = ri->data.hid.bRawData;
    if (reportSize == 0 || reportCount == 0) return;

    if (g_debug) {
        LogF("[raw] reportCount=%u reportSize=%u reportId=0x%02X",
             (unsigned)reportCount, (unsigned)reportSize,
             reportSize > 0 ? (unsigned)reports[0] : 0u);
    }

    for (DWORD r = 0; r < reportCount; ++r) {
        BYTE* report = reports + (size_t)r * reportSize;

        for (int k = 0; k < d->linkCount; ++k) {
            const FingerLink& link = d->links[k];

            ULONG x = 0, y = 0;
            NTSTATUS sX = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, link.lc,
                                             HID_USAGE_GENERIC_X, &x,
                                             d->preparsed, (PCHAR)report, reportSize);
            NTSTATUS sY = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, link.lc,
                                             HID_USAGE_GENERIC_Y, &y,
                                             d->preparsed, (PCHAR)report, reportSize);

            // Contact Identifier：用来给每根手指做稳定索引
            ULONG cid = (ULONG)k;
            NTSTATUS sC = HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_DIGITIZER, link.lc,
                                             HID_USAGE_DIGITIZER_CONTACT_ID, &cid,
                                             d->preparsed, (PCHAR)report, reportSize);

            // Tip Switch：=1 表示真实接触触摸板
            USAGE usages[16];
            ULONG nUsages = 16;
            bool tipSwitch = false;
            NTSTATUS sT = HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_DIGITIZER, link.lc,
                                         usages, &nUsages,
                                         d->preparsed, (PCHAR)report, reportSize);
            if (sT == HIDP_STATUS_SUCCESS) {
                for (ULONG u = 0; u < nUsages; ++u) {
                    if (usages[u] == HID_USAGE_DIGITIZER_TIP_SWITCH) { tipSwitch = true; break; }
                }
            }

            if (g_debug) {
                LogF("  link[%d] lc=%u sX=0x%X sY=0x%X x=%u y=%u sC=0x%X cid=%u sT=0x%X nU=%u tip=%d",
                     k, (unsigned)link.lc, (unsigned)sX, (unsigned)sY,
                     (unsigned)x, (unsigned)y, (unsigned)sC, (unsigned)cid,
                     (unsigned)sT, (unsigned)nUsages, tipSwitch ? 1 : 0);
            }

            if (sX != HIDP_STATUS_SUCCESS || sY != HIDP_STATUS_SUCCESS) continue;

            // 跳过 PTP 设备的 "inactive finger slot"（无 Digitizer button 处于 ON 状态、cid=0）
            if (nUsages == 0) continue;
            if (cid >= MAX_FINGERS) continue;

            g_fingers[cid].down = tipSwitch;
            g_fingers[cid].x    = (LONG)x;
            g_fingers[cid].y    = (LONG)y;

            if (link.xMax > 0) g_curLogMaxX = link.xMax;
            if (link.yMax > 0) g_curLogMaxY = link.yMax;
        }
    }

    // 统计当前按下的手指
    int n = 0;
    LONG sx = 0, sy = 0;
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (g_fingers[i].down) { ++n; sx += g_fingers[i].x; sy += g_fingers[i].y; }
    }

    if (n != g_lastFingerCount) {
        LogF("[input] fingers=%d (was %d)  avg=(%d,%d)  logMax=(%d,%d)",
             n, g_lastFingerCount,
             n ? (int)(sx / n) : 0, n ? (int)(sy / n) : 0,
             (int)g_curLogMaxX, (int)g_curLogMaxY);
        g_lastFingerCount = n;
    }

    // ----------------- 状态机 -----------------
    ULONGLONG now = GetTickCount64();
    if (n == DRAG_FINGERS) {
        g_lastSaw3FingersMs = now;
        if (g_threeFingersSinceMs == 0) g_threeFingersSinceMs = now;  // 进入 ==3 起点
    } else {
        g_threeFingersSinceMs = 0;                                    // 离开 ==3 立即清零
    }

    if (!g_dragging && n == DRAG_FINGERS
        && (now - g_threeFingersSinceMs) >= START_DEBOUNCE_MS) {
        // 3 指稳定 ≥ 20ms 后才落下左键。4/5 指手势从 0 升到目标手指数时
        // 必然短暂经过 n==3，那段不到 20ms，所以不会触发。
        int cid = PickPrimaryCid();
        if (cid >= 0) {
            SetPrimary(cid);
            SendMouseFlag(MOUSEEVENTF_LEFTDOWN);
            g_dragging = true;

            g_subX = 0.0;
            g_subY = 0.0;
            g_prevDtMag = 0;

            POINT p0;
            if (GetCursorPos(&p0)) {
                LogF("[drag] LEFTDOWN  primary cid=%d  touch=(%d,%d)  cursor=(%d,%d)",
                     cid, (int)g_lastTouchX, (int)g_lastTouchY, (int)p0.x, (int)p0.y);
            } else {
                LogF("[drag] LEFTDOWN  primary cid=%d  touch=(%d,%d)",
                     cid, (int)g_lastTouchX, (int)g_lastTouchY);
            }
        }
    }
    else if (g_dragging && n == DRAG_FINGERS) {
        if (g_hwnd) KillTimer(g_hwnd, TIMER_RELEASE);

        // 主导指被报为抬起 -> 切换到剩余手指中 cid 最小的那根。
        // 关键：切换帧只更新 g_lastTouchX/Y（新的基准位置），不动光标。
        if (g_primaryCid < 0 || !g_fingers[g_primaryCid].down) {
            int cid = PickPrimaryCid();
            if (cid < 0) return;
            SetPrimary(cid);
            LogF("[drag] primary switched to cid=%d", cid);
            return;
        }

        // 增量驱动：dt 钳位过滤掉驱动假数据后，剩下都是真实位移，
        // 直接换算成屏幕像素 1:1 下发到光标，光标完全跟手。
        LONG dx_touch = g_fingers[g_primaryCid].x - g_lastTouchX;
        LONG dy_touch = g_fingers[g_primaryCid].y - g_lastTouchY;
        g_lastTouchX  = g_fingers[g_primaryCid].x;
        g_lastTouchY  = g_fingers[g_primaryCid].y;

        if (dx_touch == 0 && dy_touch == 0) return;

        // dt 钳位（孤立尖刺检测）：
        // 当前帧位移 > DT_CLAMP 且上一帧位移 < DT_CLAMP/2 → 判定为驱动假突发，丢弃该轴。
        // 连续大 dt（真实甩动）不会触发，因为上一帧 dt 也大。
        LONG absDx = dx_touch >= 0 ? dx_touch : -dx_touch;
        LONG absDy = dy_touch >= 0 ? dy_touch : -dy_touch;
        bool clampedX = false, clampedY = false;
        if (DT_CLAMP > 0 && g_prevDtMag < DT_CLAMP / 2) {
            clampedX = absDx > DT_CLAMP;
            clampedY = absDy > DT_CLAMP;
            if (clampedX) { dx_touch = 0; absDx = 0; }
            if (clampedY) { dy_touch = 0; absDy = 0; }
        }
        g_prevDtMag = absDx > absDy ? absDx : absDy;

        // 统一缩放因子：触摸板 X 全长 = 主屏宽度 * g_sens
        int sw = GetSystemMetrics(SM_CXSCREEN);
        double k = (double)sw / (g_curLogMaxX > 0 ? g_curLogMaxX : 1) * (double)g_sens;

        // 浮点位移塞进亚像素累加器，整数部分本帧下发，分数部分留到下一帧。
        g_subX += (double)dx_touch * k;
        g_subY += (double)dy_touch * k;
        LONG stepX = (LONG)g_subX;
        LONG stepY = (LONG)g_subY;
        g_subX -= (double)stepX;
        g_subY -= (double)stepY;

        if (stepX != 0 || stepY != 0) {
            POINT pt;
            if (GetCursorPos(&pt)) MoveCursorTo(pt.x + stepX, pt.y + stepY);
        }

        if (g_debug) {
            const char* clampTag = clampedX && clampedY ? "XY"
                                 : clampedX             ? "X"
                                 : clampedY             ? "Y" : "-";
            ULONGLONG nowMs = GetTickCount64();
            LONG dms = g_lastMoveLogMs ? (LONG)(nowMs - g_lastMoveLogMs) : -1;
            g_lastMoveLogMs = nowMs;

            LogF("[move] dms=%d cid=%d n=%d touch=(%d,%d) dt=(%d,%d) clamp=%s step=(%d,%d)",
                 (int)dms,
                 g_primaryCid, n,
                 (int)g_fingers[g_primaryCid].x, (int)g_fingers[g_primaryCid].y,
                 (int)dx_touch, (int)dy_touch,
                 clampTag,
                 (int)stepX, (int)stepY);
        }
    }
    else if (g_dragging && n != DRAG_FINGERS) {
        // 不立即 LEFTUP，启 50ms 去抖 timer。涵盖两类瞬时偏离：
        //   n < 3：硬件偶发漏报某指
        //   n > 3：手掌/手腕擦碰被识别成第 4/5 指
        if (g_hwnd) SetTimer(g_hwnd, TIMER_RELEASE, (UINT)RELEASE_DEBOUNCE_MS, nullptr);
    }
}

// 由 WM_TIMER 调用：若已经超过去抖时长仍未看到 n==3，确认释放左键。
static void CheckReleaseTimeout()
{
    if (g_hwnd) KillTimer(g_hwnd, TIMER_RELEASE);
    if (!g_dragging) return;
    if (GetTickCount64() - g_lastSaw3FingersMs >= RELEASE_DEBOUNCE_MS) {
        SendMouseFlag(MOUSEEVENTF_LEFTUP);
        g_dragging      = false;
        g_primaryCid    = -1;
        g_lastMoveLogMs = 0;
        for (int i = 0; i < MAX_FINGERS; ++i) g_fingers[i].down = false;
        Log("[drag] LEFTUP (debounced)");
    }
}

// ============================================================================
// 窗口过程（守护进程）
// ============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT:
        HandleRawInput((HRAWINPUT)lParam);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_RELEASE) {
            CheckReleaseTimeout();
            return 0;
        }
        break;

    case WM_APP_RELOAD:
        Log("[config] reload requested");
        LoadConfigFromIni(g_iniPath, g_cfg);
        ApplyConfig(g_cfg);
        LogF("[config] reloaded sens=%d%% dt_clamp=%d debug=%d autostart=%d",
             g_cfg.sensitivity, g_cfg.dt_clamp, g_cfg.debug, g_cfg.autostart);
        return 0;

    case WM_DESTROY:
        // 防止异常关闭时鼠标左键一直按住
        if (g_dragging) {
            SendMouseFlag(MOUSEEVENTF_LEFTUP);
            g_dragging = false;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// 守护进程主循环（由 `3drag start` 用 DETACHED_PROCESS 派生，内部 --daemon 触发）
// ============================================================================

static int runDaemon()
{
    // 路径解析：把 ini/log 放到 %APPDATA%/3drag 目录
    // 失败也继续（用内置默认值跑，仅日志无落盘）
    if (InitUserPaths()) {
        WriteDefaultIniIfMissing(g_iniPath);  // 首次 start 自动落盘模板
    }

    // 单实例：mutex 保险 + 后面 FindWindow 保险
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME_W);
    if (!hMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // 加载并应用配置（debug 此时才决定是否 OpenLog）
    LoadConfigFromIni(g_iniPath, g_cfg);
    ApplyConfig(g_cfg);

    LogF("[boot] daemon start pid=%u sens=%d%% dt_clamp=%d autostart=%d",
         (unsigned)GetCurrentProcessId(), g_cfg.sensitivity, g_cfg.dt_clamp, g_cfg.autostart);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WINDOW_CLASS_W;
    if (!RegisterClassExW(&wc)) {
        LogF("[boot] RegisterClassExW failed err=%u", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, WINDOW_CLASS_W, L"3drag",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        LogF("[boot] CreateWindowExW failed err=%u", GetLastError());
        return 1;
    }
    g_hwnd = hwnd;

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x0D;             // Digitizers
    rid.usUsage     = 0x05;             // Touch Pad
    rid.dwFlags     = RIDEV_INPUTSINK;  // 后台也接收输入
    rid.hwndTarget  = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        LogF("[boot] RegisterRawInputDevices failed err=%u", GetLastError());
        return 1;
    }
    Log("[boot] RegisterRawInputDevices(0x0D/0x05) OK, entering message loop");

    // 枚举 HID 设备便于诊断（debug 模式）
    if (g_debug && g_logFile != INVALID_HANDLE_VALUE) {
        UINT cnt = 0;
        if (GetRawInputDeviceList(nullptr, &cnt, sizeof(RAWINPUTDEVICELIST)) == 0 && cnt > 0) {
            RAWINPUTDEVICELIST* list = (RAWINPUTDEVICELIST*)HeapAlloc(GetProcessHeap(), 0,
                                                                     sizeof(RAWINPUTDEVICELIST) * cnt);
            if (list && GetRawInputDeviceList(list, &cnt, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1) {
                for (UINT i = 0; i < cnt; ++i) {
                    if (list[i].dwType != RIM_TYPEHID) continue;
                    RID_DEVICE_INFO info = {};
                    info.cbSize = sizeof(info);
                    UINT sz = sizeof(info);
                    if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz)
                        != (UINT)-1) {
                        LogF("[enum] HID dev=0x%X usagePage=0x%04X usage=0x%04X",
                             (unsigned)(ULONG_PTR)list[i].hDevice,
                             info.hid.usUsagePage, info.hid.usUsage);
                    }
                }
            }
            if (list) HeapFree(GetProcessHeap(), 0, list);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        DispatchMessageW(&msg);
    }

    Log("[boot] message loop exited, bye");
    CloseLog();
    return 0;
}

// ============================================================================
// CLI 子命令
// ============================================================================

static HWND FindDaemonWindow()
{
    // message-only 窗口必须通过 HWND_MESSAGE 父查找
    return FindWindowExW(HWND_MESSAGE, nullptr, WINDOW_CLASS_W, nullptr);
}

static DWORD WindowPid(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

static int cmd_start()
{
    HWND existing = FindDaemonWindow();
    if (existing) {
        PrintLine("3drag is already running (pid %u)", (unsigned)WindowPid(existing));
        return 0;
    }

    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        PrintLine("failed to start: cannot resolve exe path");
        return 1;
    }

    // CreateProcess 要求 lpCommandLine 可写
    wchar_t cmdLine[MAX_PATH + 64];
    wsprintfW(cmdLine, L"\"%s\" --daemon", exePath);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        exePath, cmdLine, nullptr, nullptr, FALSE,
        DETACHED_PROCESS | CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);
    if (!ok) {
        PrintLine("failed to start: CreateProcess error %u", (unsigned)GetLastError());
        return 1;
    }
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // 等 daemon 注册窗口（最多 1s）
    for (int i = 0; i < 20; ++i) {
        Sleep(50);
        if (FindDaemonWindow()) {
            PrintLine("3drag is running now (pid %u)", (unsigned)pid);
            return 0;
        }
    }
    PrintLine("3drag started but did not initialize within 1s (pid %u)", (unsigned)pid);
    return 1;
}

static int cmd_stop()
{
    HWND h = FindDaemonWindow();
    if (!h) {
        PrintLine("3drag is not running");
        return 0;   // 期望终态已达成
    }
    DWORD pid = WindowPid(h);
    PostMessageW(h, WM_CLOSE, 0, 0);

    // 等进程退出最多 2s（一般 < 50ms）
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (proc) {
        WaitForSingleObject(proc, 2000);
        CloseHandle(proc);
    }
    PrintLine("3drag stopped (pid %u)", (unsigned)pid);
    return 0;
}

static int cmd_reload()
{
    ApplyAutostartFromIni();

    HWND h = FindDaemonWindow();
    if (!h) {
        PrintLine("3drag is not running");
        return 1;
    }
    DWORD pid = WindowPid(h);
    PostMessageW(h, WM_APP_RELOAD, 0, 0);
    PrintLine("3drag config reloaded (pid %u)", (unsigned)pid);
    return 0;
}

static int cmd_autostart(bool enable)
{
    if (!InitUserPaths()) {
        PrintLine("failed to resolve %%APPDATA%%");
        return 1;
    }
    WriteDefaultIniIfMissing(g_iniPath);
    WriteIntToIni(L"general", L"autostart", enable ? 1 : 0, g_iniPath);
    SetAutostart(enable);

    HWND h = FindDaemonWindow();
    if (h) {
        PostMessageW(h, WM_APP_RELOAD, 0, 0);
    }
    PrintLine("autostart: %s", enable ? "enabled" : "disabled");
    return 0;
}

static int cmd_status()
{
    HWND h = FindDaemonWindow();
    if (h) {
        PrintLine("3drag is running (pid %u)", (unsigned)WindowPid(h));
    } else {
        PrintLine("3drag is not running");
    }
    PrintLine("autostart: %s", ReadAutostartEnabled() ? "enabled" : "disabled");
    return h ? 0 : 1;
}

static int cmd_config()
{
    if (!InitUserPaths()) {
        PrintLine("failed to resolve %%APPDATA%%");
        return 1;
    }
    if (WriteDefaultIniIfMissing(g_iniPath)) {
        PrintLine("created default config");
    }
    PrintLine("opening: %ws", g_iniPath);

    // 先按 ini 的关联程序打开；若没关联（罕见），回落到 notepad
    HINSTANCE rc = ShellExecuteW(nullptr, nullptr, g_iniPath, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        rc = ShellExecuteW(nullptr, L"open", L"notepad.exe", g_iniPath, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)rc <= 32) {
            PrintLine("failed to launch editor (rc=%d)", (int)(INT_PTR)rc);
            return 1;
        }
    }
    return 0;
}

static int cmd_help()
{
    Print(
        "Usage: 3drag <command>\r\n"
        "\r\n"
        "Commands:\r\n"
        "  start    Start the 3drag daemon in the background\r\n"
        "  stop     Stop the running daemon\r\n"
        "  reload   Reload config from 3drag.ini\r\n"
        "  status   Show whether 3drag is running\r\n"
        "  config   Open 3drag.ini in the default editor (creates one if missing)\r\n"
        "  autostart enabled|disabled   Toggle logon autostart (updates ini + registry)\r\n"
        "  help     Print this message\r\n"
        "\r\n"
        "Files:\r\n"
        "  config : %APPDATA%\\3drag\\3drag.ini   (auto-created on first start)\r\n"
        "  log    : %APPDATA%\\3drag\\3drag.log   (only when debug = 1)\r\n"
        "\r\n"
        "Config keys (under [general]):\r\n"
        "  sensitivity = 120     ; 100 = 1:1, lower = slower, higher = faster\r\n"
        "  dt_clamp    = 50      ; 0 to disable spike filter\r\n"
        "  debug       = 0       ; 1 = write 3drag.log\r\n"
        "  autostart   = 1       ; 0 to disable; runs '3drag start' at logon via HKCU Run\r\n"
    );
    return 0;
}

// 当从 logon/explorer 直接派生 3drag（如 autostart）时，进程会被分配一个新 console
// 一闪而过。这里检测：如果本进程是该 console 的唯一所有者（== 没有父终端），就藏掉它。
// 真从 cmd/PowerShell 调用时 count >= 2，不会藏。
static void HideConsoleIfStandalone()
{
    DWORD list[2];
    DWORD count = GetConsoleProcessList(list, 2);
    if (count == 1) {
        HWND console = GetConsoleWindow();
        if (console) ShowWindow(console, SW_HIDE);
    }
}

// ============================================================================
// 入口
// ============================================================================

int wmain(int argc, wchar_t** argv)
{
    HideConsoleIfStandalone();

    if (argc < 2) {
        cmd_help();
        return 0;
    }

    const wchar_t* sub = argv[1];

    if (lstrcmpiW(sub, L"--daemon") == 0) return runDaemon();
    if (lstrcmpiW(sub, L"start")    == 0) return cmd_start();
    if (lstrcmpiW(sub, L"stop")     == 0) return cmd_stop();
    if (lstrcmpiW(sub, L"reload")   == 0) return cmd_reload();
    if (lstrcmpiW(sub, L"status")   == 0) return cmd_status();
    if (lstrcmpiW(sub, L"config")   == 0) return cmd_config();
    if (lstrcmpiW(sub, L"autostart") == 0) {
        if (argc < 3) {
            PrintLine("usage: 3drag autostart enabled|disabled");
            return 2;
        }
        if (lstrcmpiW(argv[2], L"enabled") == 0)  return cmd_autostart(true);
        if (lstrcmpiW(argv[2], L"disabled") == 0) return cmd_autostart(false);
        PrintLine("unknown autostart option: %ws", argv[2]);
        PrintLine("usage: 3drag autostart enabled|disabled");
        return 2;
    }
    if (lstrcmpiW(sub, L"help")     == 0 ||
        lstrcmpiW(sub, L"--help")   == 0 ||
        lstrcmpiW(sub, L"-h")       == 0) {
        cmd_help();
        return 0;
    }

    PrintLine("unknown command: %ws", sub);
    PrintLine("run '3drag help' for usage");
    return 2;
}

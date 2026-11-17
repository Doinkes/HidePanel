// ============================================================================
//  HidePanel - мгновенно скрываемая панель задач Windows
// ----------------------------------------------------------------------------
//  Идея: нативная авто-скрываемая панель задач Windows всегда анимируется
//  (выезжает/уезжает) и имеет задержку. Эта программа вместо анимации делает
//  ShowWindow(SW_HIDE) / SetWindowPos(SWP_SHOWWINDOW) окна Shell_TrayWnd,
//  поэтому появление и исчезновение панели происходят моментально.
//
//  Для этого нативное авто-скрытие временно отключается (ABM_SETSTATE),
//  чтобы оболочка не "боролась" с нами, а исходное состояние восстанавливается
//  при выходе. Рабочая область при этом не меняется: место под панель остаётся
//  зарезервированным, поэтому окна не прыгают.
//
//  Лицензия: MIT
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ---------------------------------------------------------------------------
//  Константы
// ---------------------------------------------------------------------------

static const wchar_t* const kAppName        = L"HidePanel";
static const wchar_t* const kVersion        = L"1.0.0";
static const wchar_t* const kMsgWndClass    = L"HidePanel.MessageWindow";
static const wchar_t* const kMutexName      = L"Local\\HidePanel.SingleInstance";
static const wchar_t* const kTrayClass      = L"Shell_TrayWnd";
static const wchar_t* const kSecondaryTray  = L"Shell_SecondaryTrayWnd";

static const UINT WM_HP_QUIT   = WM_APP + 0x101;   // просьба завершиться
static const UINT WM_HP_TASKCREATED = WM_APP + 0x102;

static const int  kHotkeyId = 0x4850;

// ---------------------------------------------------------------------------
//  Настройки
// ---------------------------------------------------------------------------

struct Options
{
    int  edgePx        = 2;      // толщина "горячей" полосы у края экрана
    int  pollMs        = 5;      // период опроса курсора, мс
    bool restoreOnExit = true;   // вернуть исходное авто-скрытие при выходе
    bool logEnabled    = false;  // писать лог
    bool fullscreenGuard = true; // не показывать панель поверх fullscreen-приложений
    UINT hotkeyMods    = MOD_CONTROL | MOD_ALT;
    UINT hotkeyVk      = 'Q';

    std::wstring exeDir;      // каталог exe
    std::wstring iniPath;     // настройки
    std::wstring dataDir;     // %LOCALAPPDATA%\HidePanel
    std::wstring logPath;
    std::wstring statePath;   // файл состояния (для восстановления после сбоя)
};

struct CliOptions
{
    bool restore = false;
    bool quit    = false;
    bool help    = false;
    bool version = false;
    bool debug   = false;
    bool noFullscreenGuard = false;
    int  edge    = -1;
    int  poll    = -1;
};

static Options    g_opt;
static CliOptions g_cli;

static HINSTANCE  g_hInst   = nullptr;
static HWND       g_hwnd    = nullptr;
static HANDLE     g_mutex   = nullptr;
static HANDLE     g_thread  = nullptr;
static volatile LONG g_running = 1;
static volatile LONG g_forceRefresh = 0;

static bool g_originalAutoHide = true;   // состояние штатного авто-скрытия до запуска

static UINT g_msgTaskbarCreated = 0;

// ---------------------------------------------------------------------------
//  Логирование
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_logLock;
static bool             g_logReady = false;

static std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (n == 0) return L".";
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

static std::wstring GetLocalAppDataDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
        return std::wstring(buf);
    return GetExeDir();
}

static void EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty()) return;
    for (size_t i = 2; i < dir.size(); ++i)
    {
        if (dir[i] != L'\\') continue;
        std::wstring part = dir.substr(0, i);
        if (part.size() > 2) CreateDirectoryW(part.c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static void LogInit()
{
    InitializeCriticalSection(&g_logLock);
    if (!g_opt.logEnabled) return;

    EnsureDirectory(g_opt.dataDir);

    // ротация: слишком большой лог просто пересоздаём
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(g_opt.logPath.c_str(), GetFileExInfoStandard, &fad))
    {
        ULARGE_INTEGER sz;
        sz.LowPart  = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > 512 * 1024)
            DeleteFileW(g_opt.logPath.c_str());
    }
    g_logReady = true;
}

static void LogW(const wchar_t* fmt, ...)
{
    if (!g_logReady) return;

    wchar_t msg[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(msg, _countof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[1300] = {};
    StringCchPrintfW(line, _countof(line),
                     L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    EnterCriticalSection(&g_logLock);
    HANDLE f = CreateFileW(g_opt.logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER zero = {};
        if (GetFileSizeEx(f, &zero) && zero.QuadPart == 0)
        {
            const wchar_t bom = 0xFEFF;
            DWORD wr = 0;
            WriteFile(f, &bom, sizeof(bom), &wr, nullptr);
        }
        DWORD wr = 0;
        WriteFile(f, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wr, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static void LogShutdown()
{
    if (g_logReady)
    {
        g_logReady = false;
        DeleteCriticalSection(&g_logLock);
    }
}

// ---------------------------------------------------------------------------
//  INI / файл состояния
// ---------------------------------------------------------------------------

static std::wstring IniString(const wchar_t* section, const wchar_t* key,
                              const wchar_t* def)
{
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, def, buf, _countof(buf),
                             g_opt.iniPath.c_str());
    return std::wstring(buf);
}

static int IniInt(const wchar_t* section, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(section, key, def, g_opt.iniPath.c_str());
}

// ---------------------------------------------------------------------------
//  Разбор горячей клавиши
// ---------------------------------------------------------------------------

static bool ParseHotkey(const std::wstring& text, UINT& mods, UINT& vk)
{
    mods = 0;
    vk   = 0;

    std::wstring s = text;
    size_t start = 0;
    bool haveKey = false;

    while (start <= s.size())
    {
        size_t plus = s.find(L'+', start);
        std::wstring tok = s.substr(start, (plus == std::wstring::npos ? s.size() : plus) - start);
        start = (plus == std::wstring::npos) ? s.size() + 1 : plus + 1;

        // trim
        while (!tok.empty() && iswspace(tok.front())) tok.erase(tok.begin());
        while (!tok.empty() && iswspace(tok.back()))  tok.pop_back();
        if (tok.empty()) continue;

        for (auto& c : tok) c = (wchar_t)towupper(c);

        if (tok == L"CTRL" || tok == L"CONTROL") { mods |= MOD_CONTROL; continue; }
        if (tok == L"ALT")                        { mods |= MOD_ALT;     continue; }
        if (tok == L"SHIFT")                      { mods |= MOD_SHIFT;   continue; }
        if (tok == L"WIN" || tok == L"WINDOWS")   { mods |= MOD_WIN;     continue; }

        // сама клавиша
        if (tok.size() == 1)
        {
            wchar_t c = tok[0];
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            {
                vk = (UINT)c;
                haveKey = true;
                continue;
            }
        }
        if (tok.size() >= 2 && tok[0] == L'F')
        {
            int n = _wtoi(tok.c_str() + 1);
            if (n >= 1 && n <= 24) { vk = (UINT)(VK_F1 + n - 1); haveKey = true; continue; }
        }
        if (tok == L"SPACE")  { vk = VK_SPACE;  haveKey = true; continue; }
        if (tok == L"PAUSE")  { vk = VK_PAUSE;  haveKey = true; continue; }
        if (tok == L"INSERT") { vk = VK_INSERT; haveKey = true; continue; }
        if (tok == L"HOME")   { vk = VK_HOME;   haveKey = true; continue; }
        if (tok == L"END")    { vk = VK_END;    haveKey = true; continue; }

        return false; // непонятный токен
    }

    if (!haveKey) return false;
    if (mods == 0) mods = MOD_CONTROL | MOD_ALT; // без модификатора не регистрируем
    return true;
}

static std::wstring HotkeyToString(UINT mods, UINT vk)
{
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";

    if (vk >= VK_F1 && vk <= VK_F24)
    {
        wchar_t b[8] = {};
        StringCchPrintfW(b, _countof(b), L"F%u", vk - VK_F1 + 1);
        s += b;
    }
    else if (vk >= 'A' && vk <= 'Z')
    {
        s += (wchar_t)vk;
    }
    else
    {
        wchar_t b[16] = {};
        StringCchPrintfW(b, _countof(b), L"VK_%u", vk);
        s += b;
    }
    return s;
}

// ---------------------------------------------------------------------------
//  Нативное состояние панели задач
// ---------------------------------------------------------------------------

static HWND PrimaryTrayWindow()
{
    return FindWindowW(kTrayClass, nullptr);
}

static DWORD GetShellTaskbarState()
{
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    return (DWORD)SHAppBarMessage(ABM_GETSTATE, &abd);
}

static bool IsNativeAutoHideOn()
{
    return (GetShellTaskbarState() & ABS_AUTOHIDE) != 0;
}

// Включает/выключает нативное авто-скрытие.
// autohideOn == false  -> панель постоянно на месте (мы сами её прячем)
static bool SetNativeAutoHide(bool autohideOn)
{
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    abd.hWnd   = PrimaryTrayWindow();

    // hWnd не обязателен, но некоторые сборки Windows его ожидают
    abd.lParam = autohideOn ? (ABS_AUTOHIDE | ABS_ALWAYSONTOP) : ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &abd);

    // Проверяем результат (оболочке нужно мгновение)
    for (int i = 0; i < 20; ++i)
    {
        if (IsNativeAutoHideOn() == autohideOn) return true;
        Sleep(25);
        abd.hWnd = PrimaryTrayWindow();
        SHAppBarMessage(ABM_SETSTATE, &abd);
    }
    return IsNativeAutoHideOn() == autohideOn;
}

// ---------------------------------------------------------------------------
//  Перечисление панелей задач и "горячих" зон
// ---------------------------------------------------------------------------

enum TrayEdge { EDGE_BOTTOM = 0, EDGE_TOP = 1, EDGE_LEFT = 2, EDGE_RIGHT = 3 };

struct Zone
{
    RECT mon;       // границы монитора
    RECT bar;       // "спокойное" положение панели
    int  edge;      // к какому краю монитора прилегает
};

struct TraySet
{
    std::vector<HWND> hwnds;
    std::vector<Zone> zones;
    bool              visible     = true;  // наше намерение
    ULONGLONG         lastRefresh = 0;
};

static TraySet g_tray;

static BOOL CALLBACK EnumTrayProc(HWND hwnd, LPARAM lp)
{
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, _countof(cls)) > 0)
    {
        if (_wcsicmp(cls, kTrayClass) == 0 || _wcsicmp(cls, kSecondaryTray) == 0)
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
    }
    return TRUE;
}

static bool PointInside(const RECT& r, POINT p)
{
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

static bool SameZone(const Zone& a, const Zone& b)
{
    return EqualRect(&a.mon, &b.mon) && EqualRect(&a.bar, &b.bar) && a.edge == b.edge;
}

// Строит зону по монитору: место панели определяется по рабочей области,
// это надёжнее, чем GetWindowRect (окно может быть сдвинуто авто-скрытием).
static bool BuildZone(const MONITORINFO& mi, Zone& z)
{
    z.mon  = mi.rcMonitor;
    z.bar  = mi.rcMonitor;
    z.edge = EDGE_BOTTOM;

    const RECT& m = mi.rcMonitor;
    const RECT& w = mi.rcWork;

    if (w.bottom < m.bottom)      { z.edge = EDGE_BOTTOM; z.bar.top    = w.bottom; }
    else if (w.top > m.top)       { z.edge = EDGE_TOP;    z.bar.bottom = w.top;    }
    else if (w.left > m.left)     { z.edge = EDGE_LEFT;   z.bar.right  = w.left;   }
    else if (w.right < m.right)   { z.edge = EDGE_RIGHT;  z.bar.left   = w.right;  }
    else
    {
        // Рабочая область не урезана. Такого с существующей панелью быть не
        // должно, но на всякий случай берём нижние 48 px.
        z.edge = EDGE_BOTTOM;
        z.bar.top = m.bottom - 48;
        return false;
    }
    return true;
}

static void RefreshTrays(bool logChanges)
{
    std::vector<HWND> found;
    EnumWindows(EnumTrayProc, reinterpret_cast<LPARAM>(&found));

    // отбрасываем протухшие окна (перезапуск explorer)
    found.erase(std::remove_if(found.begin(), found.end(),
                               [](HWND h) { return !IsWindow(h); }),
                found.end());

    bool hwndsChanged = (found != g_tray.hwnds);

    std::vector<Zone> zones;
    std::vector<HMONITOR> seen;
    for (HWND h : found)
    {
        HMONITOR hm = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (std::find(seen.begin(), seen.end(), hm) != seen.end()) continue;
        seen.push_back(hm);

        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hm, &mi)) continue;

        Zone z;
        if (!BuildZone(mi, z))
            LogW(L"warning: monitor %ld,%ld has taskbar window but not reduced work area",
                 mi.rcMonitor.left, mi.rcMonitor.top);
        zones.push_back(z);
    }

    bool zonesChanged = zones.size() != g_tray.zones.size() ||
        !std::equal(zones.begin(), zones.end(), g_tray.zones.begin(), SameZone);

    g_tray.hwnds = found;
    g_tray.zones = zones;

    // Пишем в лог только изменения, иначе при log=1 файл быстро разрастётся.
    if (logChanges && hwndsChanged)
    {
        LogW(L"taskbar windows: %zu", zones.size());
        for (HWND h : found)
        {
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, _countof(cls));
            LogW(L"  hwnd=%p class=%s", h, cls);
        }
    }
    if (logChanges && zonesChanged)
    {
        for (const Zone& z : zones)
            LogW(L"zone: monitor %ld,%ld-%ld,%ld  bar %ld,%ld-%ld,%ld  edge=%d",
                 z.mon.left, z.mon.top, z.mon.right, z.mon.bottom,
                 z.bar.left, z.bar.top, z.bar.right, z.bar.bottom, z.edge);
    }
}

// Курсор в "горячей" полосе у края или над самой панелью?
static bool WantsVisible(POINT p)
{
    for (const Zone& z : g_tray.zones)
    {
        if (PointInside(z.bar, p)) return true;

        RECT strip = z.mon;
        switch (z.edge)
        {
        case EDGE_BOTTOM: strip.top    = z.mon.bottom - g_opt.edgePx; break;
        case EDGE_TOP:    strip.bottom = z.mon.top    + g_opt.edgePx; break;
        case EDGE_LEFT:   strip.right  = z.mon.left   + g_opt.edgePx; break;
        case EDGE_RIGHT:  strip.left   = z.mon.right  - g_opt.edgePx; break;
        }
        if (PointInside(strip, p)) return true;
    }
    return false;
}

// Полноэкранное приложение на переднем плане? Тогда панель не показываем.
// Используем системный признак (SHQueryUserNotificationState): он корректно
// определяет игры/F11-видео и не путает с обычным развёрнутым окном.
static bool IsFullscreenForeground()
{
    typedef HRESULT (WINAPI *PFN_QueryState)(int*);
    static PFN_QueryState pfn = (PFN_QueryState)
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHQueryUserNotificationState");
    if (!pfn) return false;

    int st = 0;                       // QUERY_USER_NOTIFICATION_STATE
    if (FAILED(pfn(&st))) return false;

    // 1 = NOT_PRESENT (локирован/скринсейвер), 2 = BUSY (fullscreen app),
    // 3 = RUNNING_D3D_FULL_SCREEN, 4 = PRESENTATION_MODE
    return st >= 1 && st <= 4;
}

static void ShowTrays(bool show)
{
    for (HWND h : g_tray.hwnds)
    {
        if (!IsWindow(h)) continue;

        if (show)
        {
            // SWP_SHOWWINDOW без активации + гарантия, что панель выше окон
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        else
        {
            ShowWindow(h, SW_HIDE);
        }
    }
}

// Приводит фактическую видимость к желаемой. Дешёвая проверка на каждом тике
// работает как watchdog: если оболочка сама показала панель — снова спрячем.
static void ApplyVisible(bool want)
{
    bool changed = (want != g_tray.visible);
    g_tray.visible = want;

    if (changed)
    {
        LogW(want ? L"show taskbar" : L"hide taskbar");
        ShowTrays(want);
        return;
    }

    for (HWND h : g_tray.hwnds)
    {
        if (!IsWindow(h)) continue;
        bool vis = IsWindowVisible(h) != FALSE;
        if (vis != want) ShowTrays(want);
    }
}

// ---------------------------------------------------------------------------
//  Рабочий поток: опрос курсора
// ---------------------------------------------------------------------------

static bool g_fullscreenCacheValue = false;
static ULONGLONG g_fullscreenCacheTime = 0;

static bool FullscreenCached()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_fullscreenCacheTime > 400)
    {
        g_fullscreenCacheValue = IsFullscreenForeground();
        g_fullscreenCacheTime  = now;
    }
    return g_fullscreenCacheValue;
}

static DWORD WINAPI WorkerProc(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Точный таймер без timeBeginPeriod: не поднимаем глобальную
    // дискретность таймера системы (важно для ноутбуков).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -10000LL;                    // 1 мс
        SetWaitableTimer(timer, &due, g_opt.pollMs, nullptr, nullptr, FALSE);
    }

    ULONGLONG lastRefresh = 0;
    ULONGLONG lastAutoHideCheck = 0;

    while (InterlockedCompareExchange(&g_running, 1, 1) == 1)
    {
        ULONGLONG now = GetTickCount64();

        // Раз в 500 мс перечитываем список окон панели (перезапуск explorer).
        // Дополнительно — сразу по сообщению TaskbarCreated.
        if (now - lastRefresh >= 500 || InterlockedExchange(&g_forceRefresh, 0) != 0)
        {
            RefreshTrays(true);
            lastRefresh = now;
        }

        // Раз в секунду следим, чтобы пользователь/система не включили
        // нативное авто-скрытие обратно — иначе начнётся "драка" анимаций.
        if (now - lastAutoHideCheck >= 1000)
        {
            if (IsNativeAutoHideOn())
            {
                LogW(L"native auto-hide was re-enabled, turning it off again");
                SetNativeAutoHide(false);
            }
            lastAutoHideCheck = now;
        }

        POINT pt = {};
        if (GetCursorPos(&pt))
        {
            bool want = WantsVisible(pt);

            static bool suppressedLogged = false;
            if (want && g_opt.fullscreenGuard && FullscreenCached())
            {
                if (!suppressedLogged)
                    LogW(L"hover at %ld,%ld ignored: fullscreen app is active", pt.x, pt.y);
                suppressedLogged = true;
                want = false;
            }
            else
            {
                suppressedLogged = false;
            }

            static bool lastWant = false;
            if (want != lastWant)
            {
                LogW(L"cursor %ld,%ld -> %s", pt.x, pt.y, want ? L"show" : L"hide");
                lastWant = want;
            }

            ApplyVisible(want);
        }

        if (timer) WaitForSingleObject(timer, INFINITE);
        else       Sleep((DWORD)g_opt.pollMs);
    }

    if (timer) CloseHandle(timer);
    return 0;
}

// ---------------------------------------------------------------------------
//  Окно сообщений
// ---------------------------------------------------------------------------

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_HOTKEY:
        LogW(L"exit hotkey pressed");
        DestroyWindow(hwnd);
        return 0;

    case WM_HP_QUIT:
        LogW(L"quit requested by another instance");
        DestroyWindow(hwnd);
        return 0;

    case WM_HP_TASKCREATED:
        LogW(L"TaskbarCreated broadcast");
        InterlockedExchange(&g_forceRefresh, 1);
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp)
        {
            InterlockedExchange(&g_running, 0);
            if (g_thread) { WaitForSingleObject(g_thread, 1500); }
            if (g_opt.restoreOnExit)
            {
                ShowTrays(true);
                SetNativeAutoHide(g_originalAutoHide);
            }
            DeleteFileW(g_opt.statePath.c_str());
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated)
        {
            LogW(L"TaskbarCreated broadcast");
            InterlockedExchange(&g_forceRefresh, 1);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static bool CreateMessageWindow()
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MsgWndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = kMsgWndClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             kMsgWndClass, kAppName, WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
    if (!g_hwnd) return false;

    if (!RegisterHotKey(g_hwnd, kHotkeyId, g_opt.hotkeyMods | MOD_NOREPEAT, g_opt.hotkeyVk))
        LogW(L"warning: RegisterHotKey failed (%lu)", GetLastError());
    return true;
}

// ---------------------------------------------------------------------------
//  Файл состояния (страховка от падения)
// ---------------------------------------------------------------------------

static bool ReadStateFile(bool& autohide)
{
    if (GetFileAttributesW(g_opt.statePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;

    int v = (int)GetPrivateProfileIntW(L"state", L"autohide", -1, g_opt.statePath.c_str());
    if (v != 0 && v != 1) return false;

    autohide = (v == 1);
    return true;
}

static void WriteStateFile(bool autohide)
{
    EnsureDirectory(g_opt.dataDir);
    WritePrivateProfileStringW(L"state", L"autohide", autohide ? L"1" : L"0",
                               g_opt.statePath.c_str());
    wchar_t pid[32] = {};
    StringCchPrintfW(pid, _countof(pid), L"%lu", GetCurrentProcessId());
    WritePrivateProfileStringW(L"state", L"pid", pid, g_opt.statePath.c_str());
}

// ---------------------------------------------------------------------------
//  Настройки / аргументы командной строки
// ---------------------------------------------------------------------------

static void LoadConfig()
{
    if (GetFileAttributesW(g_opt.iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    g_opt.edgePx = IniInt(L"settings", L"edge_px", g_opt.edgePx);
    g_opt.pollMs = IniInt(L"settings", L"poll_ms", g_opt.pollMs);
    g_opt.restoreOnExit = IniInt(L"settings", L"restore_on_exit", g_opt.restoreOnExit ? 1 : 0) != 0;
    g_opt.logEnabled   = IniInt(L"settings", L"log", g_opt.logEnabled ? 1 : 0) != 0;
    g_opt.fullscreenGuard = IniInt(L"settings", L"fullscreen_guard", g_opt.fullscreenGuard ? 1 : 0) != 0;

    std::wstring hk = IniString(L"settings", L"exit_hotkey", L"");
    if (!hk.empty())
    {
        UINT m = 0, v = 0;
        if (ParseHotkey(hk, m, v)) { g_opt.hotkeyMods = m; g_opt.hotkeyVk = v; }
    }

    if (g_opt.edgePx < 1)   g_opt.edgePx = 1;
    if (g_opt.edgePx > 64)  g_opt.edgePx = 64;
    if (g_opt.pollMs < 1)   g_opt.pollMs = 1;
    if (g_opt.pollMs > 200) g_opt.pollMs = 200;
}

static void ParseCommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        for (auto& c : a) c = (wchar_t)towlower(c);

        if (a == L"--restore")       g_cli.restore = true;
        else if (a == L"--quit")     g_cli.quit    = true;
        else if (a == L"--debug")    g_cli.debug   = true;
        else if (a == L"--no-fullscreen-guard") g_cli.noFullscreenGuard = true;
        else if (a == L"--help" || a == L"-h" || a == L"/?")  g_cli.help = true;
        else if (a == L"--version" || a == L"-v")             g_cli.version = true;
        else if ((a == L"--edge" || a == L"--poll") && i + 1 < argc)
        {
            int v = _wtoi(argv[++i]);
            if (a == L"--edge") g_cli.edge = v;
            else                g_cli.poll = v;
        }
    }
    LocalFree(argv);
}

static void ShowHelp()
{
    std::wstring text =
        L"HidePanel " + std::wstring(kVersion) + L"\n\n"
        L"Панель задач скрыта постоянно и появляется мгновенно, когда курсор\n"
        L"оказывается в самой нижней полосе экрана.\n\n"
        L"Параметры:\n"
        L"  --restore        Показать панель задач и вернуть нативное авто-скрытие\n"
        L"  --quit           Остановить уже запущенный экземпляр\n"
        L"  --edge N         Толщина горячей полосы у края, px (по умолчанию 2)\n"
        L"  --poll N         Период опроса курсора, мс (по умолчанию 5)\n"
        L"  --no-fullscreen-guard  Показывать панель даже поверх fullscreen-приложений\n"
        L"  --debug          Подробный лог\n"
        L"  --version        Версия\n"
        L"  --help           Эта справка\n\n"
        L"Выход: " + HotkeyToString(g_opt.hotkeyMods, g_opt.hotkeyVk) +
        L"  (или: HidePanel.exe --quit)\n\n"
        L"Настройки: HidePanel.ini рядом с HidePanel.exe\n"
        L"Лог: " + g_opt.logPath;
    MessageBoxW(nullptr, text.c_str(), L"HidePanel", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
//  Восстановление оболочки
// ---------------------------------------------------------------------------

static void RestoreShell(bool useStateFile)
{
    bool autohide   = g_originalAutoHide;
    bool haveState  = true;

    if (useStateFile)
    {
        haveState = ReadStateFile(autohide);
        if (haveState)
        {
            g_originalAutoHide = autohide;
            LogW(L"restoring saved auto-hide state: %d", autohide ? 1 : 0);
        }
        else
        {
            LogW(L"no saved state, only showing the taskbar");
        }
    }
    else
    {
        LogW(L"restore: auto-hide=%d", g_originalAutoHide ? 1 : 0);
    }

    ShowTrays(true);

    // Дать панели чуть-чуть времени реально показаться перед сменой режима
    Sleep(30);
    if (haveState)
        SetNativeAutoHide(g_originalAutoHide);

    DeleteFileW(g_opt.statePath.c_str());
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_hInst = hInst;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_opt.exeDir   = GetExeDir();
    g_opt.iniPath  = g_opt.exeDir + L"\\HidePanel.ini";
    g_opt.dataDir  = GetLocalAppDataDir() + L"\\HidePanel";
    g_opt.logPath  = g_opt.dataDir + L"\\HidePanel.log";
    g_opt.statePath = g_opt.dataDir + L"\\state.ini";

    ParseCommandLine();
    LoadConfig();

    if (g_cli.edge > 0) g_opt.edgePx = std::min(64, std::max(1, g_cli.edge));
    if (g_cli.poll > 0) g_opt.pollMs = std::min(200, std::max(1, g_cli.poll));
    if (g_cli.debug)    g_opt.logEnabled = true;
    if (g_cli.noFullscreenGuard) g_opt.fullscreenGuard = false;

    LogInit();

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // ---- --quit: попросить запущенный экземпляр выйти ----------------------
    if (g_cli.quit)
    {
        HWND w = FindWindowW(kMsgWndClass, nullptr);
        if (w) PostMessageW(w, WM_HP_QUIT, 0, 0);
        LogW(L"--quit: window %p", w);
        LogShutdown();
        return 0;
    }

    // ---- --restore: аварийное восстановление -------------------------------
    if (g_cli.restore)
    {
        RefreshTrays(true);
        RestoreShell(true);
        LogShutdown();
        return 0;
    }

    if (g_cli.help)
    {
        ShowHelp();
        LogShutdown();
        return 0;
    }

    if (g_cli.version)
    {
        MessageBoxW(nullptr, L"HidePanel " L"1.0.0", L"HidePanel",
                    MB_OK | MB_ICONINFORMATION);
        LogShutdown();
        return 0;
    }

    // ---- один экземпляр ----------------------------------------------------
    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g_mutex)
    {
        MessageBoxW(nullptr, L"Не удалось создать объект синхронизации.",
                    kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND w = FindWindowW(kMsgWndClass, nullptr);
        if (w) PostMessageW(w, WM_HP_QUIT, 0, 0);
        LogW(L"already running, requested exit");
        LogShutdown();
        CloseHandle(g_mutex);
        return 0;
    }

    LogW(L"---- HidePanel %s starting (edge=%d px, poll=%d ms, hotkey=%s) ----",
         kVersion, g_opt.edgePx, g_opt.pollMs,
         HotkeyToString(g_opt.hotkeyMods, g_opt.hotkeyVk).c_str());

    if (!CreateMessageWindow())
    {
        LogW(L"failed to create message window: %lu", GetLastError());
        MessageBoxW(nullptr, L"Не удалось создать служебное окно.",
                    kAppName, MB_OK | MB_ICONERROR);
        LogShutdown();
        return 1;
    }

    // ---- состояние оболочки ------------------------------------------------
    if (!ReadStateFile(g_originalAutoHide))
        g_originalAutoHide = IsNativeAutoHideOn();
    else
        LogW(L"found stale state file, previous run did not exit cleanly");

    WriteStateFile(g_originalAutoHide);

    RefreshTrays(true);

    if (!g_tray.hwnds.empty())
    {
        if (!g_originalAutoHide)
            LogW(L"native auto-hide already off");

        if (!SetNativeAutoHide(false))
            LogW(L"warning: could not disable native auto-hide");

        // Спокойное положение панели могло измениться -> перечитываем зоны
        Sleep(120);
        RefreshTrays(false);

        // Начальное состояние: показана только если курсор уже внизу
        POINT pt = {};
        bool want = GetCursorPos(&pt) ? WantsVisible(pt) : false;
        g_tray.visible = true;         // сейчас панель точно видна
        ApplyVisible(want);
    }
    else
    {
        LogW(L"no taskbar windows found");
    }

    g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);

    // ---- цикл сообщений ----------------------------------------------------
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- завершение --------------------------------------------------------
    InterlockedExchange(&g_running, 0);
    if (g_thread)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    LogW(L"shutting down, restoreOnExit=%d", g_opt.restoreOnExit ? 1 : 0);

    if (g_opt.restoreOnExit)
    {
        RestoreShell(false);
    }
    else
    {
        ShowTrays(true);
    }

    if (g_hwnd) { UnregisterHotKey(g_hwnd, kHotkeyId); DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }

    LogW(L"---- HidePanel stopped ----");
    LogShutdown();
    return 0;
}

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

#include "resource.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

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
    bool fullWorkArea  = true;   // рабочая область = весь экран (панель поверх окон)
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
//  Локализация
// ---------------------------------------------------------------------------

enum Lang { LANG_RU = 0, LANG_EN = 1 };

struct UiStrings
{
    const wchar_t* languageLabel;
    const wchar_t* langRu;
    const wchar_t* langEn;

    const wchar_t* edgeLabel;
    const wchar_t* edgeDesc;
    const wchar_t* px;

    const wchar_t* pollLabel;
    const wchar_t* pollDesc;
    const wchar_t* ms;

    const wchar_t* workAreaLabel;
    const wchar_t* workAreaDesc;

    const wchar_t* guardLabel;
    const wchar_t* guardDesc;

    const wchar_t* restoreLabel;
    const wchar_t* restoreDesc;

    const wchar_t* saveBtn;
    const wchar_t* cancelBtn;
    const wchar_t* defaultsBtn;

    const wchar_t* errTitle;
    const wchar_t* errRange;
    const wchar_t* errSave;

    const wchar_t* trayTip;
    const wchar_t* menuSettings;
    const wchar_t* menuAutostart;
    const wchar_t* menuExit;

    const wchar_t* helpBody;
    const wchar_t* helpExit;
    const wchar_t* helpSettings;
    const wchar_t* helpLog;
};

static const UiStrings kStringsRu =
{
    L"Язык интерфейса",
    L"Русский",
    L"English",

    L"Толщина горячей полосы",
    L"Высота невидимой полосы у нижнего края экрана, при касании которой появляется панель.",
    L"px",

    L"Период опроса курсора",
    L"Как часто проверяется положение курсора: 5 мс — примерно 200 раз в секунду. Меньше — отзывчивее, больше — экономнее для процессора.",
    L"мс",

    L"Разворачивать окна на весь экран",
    L"Окна разворачиваются на весь монитор, а панель рисуется поверх них. Если выключить — внизу резервируется полоса под панель.",

    L"Не показывать поверх полноэкранных приложений",
    L"Панель не будет появляться поверх игр и полноэкранного видео.",

    L"Восстанавливать авто-скрытие при выходе",
    L"Вернуть исходную настройку Windows, когда программа закрывается.",

    L"Сохранить",
    L"Отмена",
    L"По умолчанию",

    L"Настройки",
    L"Проверьте значения: толщина 1–64 px, период опроса 1–200 мс.",
    L"Не удалось записать файл настроек.",

    L"HidePanel — панель задач скрыта",
    L"Настройки…",
    L"Запускать при входе в Windows",
    L"Выход",

    L"Панель задач скрыта и появляется мгновенно, когда курсор оказывается\n"
    L"в самой нижней полосе экрана.\n\n"
    L"Параметры командной строки:\n"
    L"  --restore        показать панель и вернуть штатное авто-скрытие\n"
    L"  --quit           остановить запущенный экземпляр\n"
    L"  --edge N         толщина горячей полосы, px\n"
    L"  --poll N         период опроса курсора, мс\n"
    L"  --debug          подробный лог\n"
    L"  --version        версия\n",

    L"Выход",
    L"Настройки",
    L"Лог",
};

static const UiStrings kStringsEn =
{
    L"Interface language",
    L"Русский",
    L"English",

    L"Hot zone thickness",
    L"Height of the invisible strip at the bottom edge that reveals the taskbar.",
    L"px",

    L"Cursor polling interval",
    L"How often the cursor position is checked: 5 ms is about 200 times per second. Lower is more responsive, higher saves CPU.",
    L"ms",

    L"Maximize windows to full screen",
    L"Windows maximize to the whole monitor and the taskbar is drawn on top. Turn off to reserve a strip for the taskbar.",

    L"Never show over fullscreen apps",
    L"The taskbar will not appear over games and fullscreen video.",

    L"Restore auto-hide on exit",
    L"Put the original Windows setting back when the program quits.",

    L"Save",
    L"Cancel",
    L"Defaults",

    L"Settings",
    L"Check the values: thickness 1–64 px, polling interval 1–200 ms.",
    L"Could not write the settings file.",

    L"HidePanel — taskbar hidden",
    L"Settings…",
    L"Start with Windows",
    L"Exit",

    L"The taskbar is hidden and appears instantly when the cursor reaches\n"
    L"the bottom edge of the screen.\n\n"
    L"Command line:\n"
    L"  --restore        show the taskbar and restore the native auto-hide\n"
    L"  --quit           stop the running instance\n"
    L"  --edge N         hot zone thickness, px\n"
    L"  --poll N         cursor polling interval, ms\n"
    L"  --debug          verbose log\n"
    L"  --version        version\n",

    L"Exit",
    L"Settings",
    L"Log",
};

static Lang g_lang = LANG_RU;
static const UiStrings& S() { return (g_lang == LANG_EN) ? kStringsEn : kStringsRu; }

static Lang DetectSystemLanguage()
{
    LANGID id = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(id) == LANG_RUSSIAN) ? LANG_RU : LANG_EN;
}

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

// Строит зону по реальному окну панели. Рабочая область может быть уже
// расширена нами до полного экрана, поэтому опираться на неё нельзя.
static bool BuildZone(HWND hwnd, const MONITORINFO& mi, Zone& z)
{
    z.mon = mi.rcMonitor;

    RECT r = {};
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top)
    {
        // запасной вариант — нижние 48 px монитора
        z.bar = mi.rcMonitor;
        z.bar.top = mi.rcMonitor.bottom - 48;
        z.edge = EDGE_BOTTOM;
        return false;
    }
    z.bar = r;

    // Горизонтальная панель (шире, чем выше) — сверху или снизу.
    const RECT& m = mi.rcMonitor;
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;

    if (w >= h)
        z.edge = (cy < (m.top + m.bottom) / 2) ? EDGE_TOP : EDGE_BOTTOM;
    else
        z.edge = (cx < (m.left + m.right) / 2) ? EDGE_LEFT : EDGE_RIGHT;

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
        if (!BuildZone(h, mi, z))
            LogW(L"warning: cannot read taskbar rect, using bottom 48 px");
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

// Курсор в "горячей" полосе у края экрана (показ) или над самой панелью
// (удержание, только когда она уже показана)?
static bool WantsVisible(POINT p)
{
    for (const Zone& z : g_tray.zones)
    {
        RECT strip = z.mon;
        switch (z.edge)
        {
        case EDGE_BOTTOM: strip.top    = z.mon.bottom - g_opt.edgePx; break;
        case EDGE_TOP:    strip.bottom = z.mon.top    + g_opt.edgePx; break;
        case EDGE_LEFT:   strip.right  = z.mon.left   + g_opt.edgePx; break;
        case EDGE_RIGHT:  strip.left   = z.mon.right  - g_opt.edgePx; break;
        }

        // Узкая полоса у края — единственный "триггер" появления.
        if (PointInside(strip, p)) return true;

        // Панель уже показана — держим, пока курсор на ней.
        if (g_tray.visible && PointInside(z.bar, p)) return true;
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

// ---------------------------------------------------------------------------
//  Рабочая область = весь экран
// ---------------------------------------------------------------------------
//  Windows не даёт "включить" панель, но не резервировать под неё место.
//  Поэтому после отключения авто-скрытия мы сами возвращаем рабочую область
//  к полному размеру монитора: SPI_SETWORKAREA без SPIF_SENDCHANGE. Такой
//  вызов оболочка не откатывает и не рассылает WM_SETTINGCHANGE, поэтому
//  уже развёрнутые окна не дёргаются, а новые разворачиваются на весь экран.

static RECT      g_primaryMon   = {};
static bool      g_workAreaOwned = false;

static void InitWorkAreaInfo()
{
    POINT pt = { 0, 0 };
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi))
        g_primaryMon = mi.rcMonitor;
}

static RECT GetWorkArea()
{
    RECT r = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

// Возвращает true, если рабочая область была расширена этим вызовом.
static bool ApplyFullWorkArea()
{
    if (g_primaryMon.right <= g_primaryMon.left) return false;

    RECT cur = GetWorkArea();
    if (EqualRect(&cur, &g_primaryMon)) return false;

    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &g_primaryMon, 0))
        return false;

    g_workAreaOwned = true;
    return true;
}

// Штатная рабочая область, когда панель не автоскрыта: монитор минус панель.
static void BroadcastWorkAreaChange();

static RECT GetReservedWorkArea()
{
    RECT target = g_primaryMon;
    if (!g_tray.hwnds.empty())
    {
        RECT bar = {};
        if (GetWindowRect(g_tray.hwnds[0], &bar) && bar.top > g_primaryMon.top)
            target.bottom = bar.top;
    }
    return target;
}

// Переключает режим рабочей области на лету (из окна настроек).
static void SetWorkAreaMode(bool fullScreen)
{
    RECT target = fullScreen ? g_primaryMon : GetReservedWorkArea();

    RECT cur = GetWorkArea();
    if (EqualRect(&cur, &target)) return;

    SystemParametersInfoW(SPI_SETWORKAREA, 0, &target, 0);
    g_workAreaOwned = true;
    BroadcastWorkAreaChange();
}

// Возвращает штатное значение рабочей области (при выходе).
static void RestoreWorkArea(bool autohideOn, bool force = false)
{
    if (!g_workAreaOwned && !force) return;

    // При включённом авто-скрытии Windows место под панель не резервирует,
    // иначе — резервирует полосу внизу.
    RECT target = autohideOn ? g_primaryMon : GetReservedWorkArea();

    RECT cur = GetWorkArea();
    if (!EqualRect(&cur, &target))
    {
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &target, 0);
        BroadcastWorkAreaChange();          // вернуть окна в штатные размеры
    }
    g_workAreaOwned = false;
}

// Однократное уведомление приложений (чтобы уже развёрнутые окна заняли
// весь экран). Дальше расширяем рабочую область молча.
static void BroadcastWorkAreaChange()
{
    DWORD_PTR res = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"WorkArea"),
                        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 200, &res);
}

// ---------------------------------------------------------------------------
//  Показ/скрытие панели
// ---------------------------------------------------------------------------

// Скрытие панели через пустой регион окна вместо SW_HIDE.
// Так окно остаётся "видимым" для оболочки (она не пересчитывает рабочую
// область и не пытается вернуть место под панель), но не рисуется и не
// перехватывает мышь. Показ/скрытие — одно мгновенное изменение региона.
static void SetTrayRegionHidden(HWND h, bool hidden)
{
    if (hidden)
    {
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);   // пустой регион
        if (!rgn) return;
        if (!SetWindowRgn(h, rgn, TRUE))
            DeleteObject(rgn);                   // при ошибке регион остаётся нашим
    }
    else
    {
        SetWindowRgn(h, nullptr, TRUE);
    }
}

static bool IsTrayRegionHidden(HWND h)
{
    static HRGN probe = CreateRectRgn(0, 0, 1, 1);
    if (!probe) return false;
    return GetWindowRgn(h, probe) == NULLREGION;
}

static void ShowTrays(bool show)
{
    for (HWND h : g_tray.hwnds)
    {
        if (!IsWindow(h)) continue;

        if (show)
        {
            SetTrayRegionHidden(h, false);
            // без активации + гарантия, что панель выше окон
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        else
        {
            SetTrayRegionHidden(h, true);
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
        // На случай, если оболочка успела вернуть место под панель.
        if (g_opt.fullWorkArea) ApplyFullWorkArea();
        return;
    }

    // Watchdog: оболочка/explorer могли сбросить регион или создать новое окно.
    for (HWND h : g_tray.hwnds)
    {
        if (!IsWindow(h)) continue;
        bool hidden = IsTrayRegionHidden(h);
        if (hidden == want)          // хотим показать, а скрыто (и наоборот)
            ShowTrays(want);
    }
}

// ---------------------------------------------------------------------------
//  Рабочий поток: опрос курсора
// ---------------------------------------------------------------------------

static void PromoteTrayIcon();   // определена ниже

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
    ULONGLONG lastWorkAreaCheck = 0;
    ULONGLONG started = GetTickCount64();
    bool promoted = false;

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

        // Оболочка время от времени сама возвращает место под панель.
        if (g_opt.fullWorkArea && now - lastWorkAreaCheck >= 500)
        {
            ApplyFullWorkArea();
            lastWorkAreaCheck = now;
        }

        // Через пару секунд после старта значок уже зарегистрирован в трее —
        // при первом запуске выносим его из "скрытых" на панель.
        if (!promoted && now - started > 2500)
        {
            PromoteTrayIcon();
            promoted = true;
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
//  Иконка в области уведомлений
// ---------------------------------------------------------------------------

static UINT WM_TRAYICON = WM_APP + 0x300;

enum TrayMenuId
{
    IDM_TRAY_EXIT = 0x701,
    IDM_TRAY_AUTOSTART,
    IDM_TRAY_SETTINGS,
};

static const wchar_t* const kRunKeyPath   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* const kRunValueName = L"HidePanel";

static NOTIFYICONDATAW g_nid = {};
static bool g_trayIconAdded = false;

static std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf));
    return std::wstring(buf, n);
}

static bool IsAutostartEnabled()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValueName,
                        RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS;
}

static bool SetAutostart(bool on)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (on)
    {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        ok = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    else
    {
        LONG rc = RegDeleteValueW(key, kRunValueName);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

static void ShowSettingsWindow();   // определена ниже

static void TrayUpdateTip()
{
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), S().trayTip);
    if (g_trayIconAdded) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayAdd()
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON),
                                    IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), S().trayTip);
    g_trayIconAdded = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
    if (!g_trayIconAdded) LogW(L"warning: Shell_NotifyIcon(NIM_ADD) failed");
}

static void TrayRemove()
{
    if (!g_trayIconAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayIconAdded = false;
}

// Windows 11 кладёт новые значки в "скрытые" (^). Один раз просим оболочку
// показывать наш значок на панели — но только если пользователь сам ещё
// ничего не выбирал (значения IsPromoted нет).
static void PromoteTrayIcon()
{
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                      KEY_READ | KEY_WRITE, &root) != ERROR_SUCCESS)
        return;

    std::wstring exe = ExePath();
    wchar_t name[256] = {};

    for (DWORD i = 0; ; ++i)
    {
        DWORD cch = _countof(name);
        if (RegEnumKeyExW(root, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        HKEY item = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | KEY_WRITE, &item) != ERROR_SUCCESS)
            continue;

        wchar_t path[1024] = {};
        DWORD sz = sizeof(path);
        bool ours = RegQueryValueExW(item, L"ExecutablePath", nullptr, nullptr,
                                     reinterpret_cast<BYTE*>(path), &sz) == ERROR_SUCCESS
                    && _wcsicmp(path, exe.c_str()) == 0;

        if (ours)
        {
            DWORD v = 0, vsz = sizeof(v);
            if (RegQueryValueExW(item, L"IsPromoted", nullptr, nullptr,
                                 reinterpret_cast<BYTE*>(&v), &vsz) == ERROR_FILE_NOT_FOUND)
            {
                DWORD one = 1;
                RegSetValueExW(item, L"IsPromoted", 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&one), sizeof(one));
                LogW(L"tray icon promoted to the visible area");
            }
            RegCloseKey(item);
            break;
        }
        RegCloseKey(item);
    }
    RegCloseKey(root);
}

static void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    wchar_t title[64] = {};
    StringCchPrintfW(title, _countof(title), L"HidePanel %s", kVersion);
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, title);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, S().menuSettings);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                IDM_TRAY_AUTOSTART, S().menuAutostart);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, S().menuExit);

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
//  Окно настроек
// ---------------------------------------------------------------------------

static const wchar_t* const kSettingsClass = L"HidePanel.SettingsWindow";

enum SettingsId
{
    IDC_LANG_RU = 0x901,
    IDC_LANG_EN,
    IDC_EDGE,
    IDC_EDGE_UNIT,
    IDC_POLL,
    IDC_POLL_UNIT,
    IDC_WORKAREA,
    IDC_GUARD,
    IDC_RESTORE,
    IDC_SAVE,
    IDC_CANCEL,
    IDC_DEFAULTS,
    IDC_DESC_FIRST = 0xA00,     // описания (серый текст); ID > этого значения
};

static HWND  g_settingsWnd = nullptr;
static HFONT g_fontUi     = nullptr;
static HFONT g_fontBold   = nullptr;
static HFONT g_fontSmall  = nullptr;
static int   g_dpi        = 96;
static Lang  g_langOnOpen = LANG_RU;
static bool  g_settingsOpen = false;

static int Scale(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT MakeFont(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void EnsureFonts()
{
    if (g_fontUi) return;
    g_fontUi    = MakeFont(9, FW_NORMAL);
    g_fontBold  = MakeFont(9, FW_SEMIBOLD);
    g_fontSmall = MakeFont(8, FW_NORMAL);
}

static void DestroyFonts()
{
    if (g_fontUi)    { DeleteObject(g_fontUi);    g_fontUi = nullptr; }
    if (g_fontBold)  { DeleteObject(g_fontBold);  g_fontBold = nullptr; }
    if (g_fontSmall) { DeleteObject(g_fontSmall); g_fontSmall = nullptr; }
}

static HWND MakeCtrl(const wchar_t* cls, const wchar_t* text, DWORD style,
                     int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_settingsWnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             g_hInst, nullptr);
    if (c && font) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return c;
}

static int TextHeight(const wchar_t* text, int width, HFONT font)
{
    HDC hdc = GetDC(g_settingsWnd);
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    RECT r = { 0, 0, width, 0 };
    DrawTextW(hdc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
    ReleaseDC(g_settingsWnd, hdc);
    return (r.bottom - r.top) + Scale(2);
}

static bool IsChecked(int id)
{
    return SendDlgItemMessageW(g_settingsWnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void SetChecked(int id, bool on)
{
    SendDlgItemMessageW(g_settingsWnd, id, BM_SETCHECK,
                        on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Собирает окно заново (в текущем языке и с текущими значениями).
static void BuildSettingsUi()
{
    if (!g_settingsWnd) return;

    EnumChildWindows(g_settingsWnd, [](HWND c, LPARAM) -> BOOL {
        DestroyWindow(c);
        return TRUE;
    }, 0);

    EnsureFonts();
    g_dpi = GetDpiForWindow(g_settingsWnd);

    {
        wchar_t title[128] = {};
        StringCchPrintfW(title, _countof(title), L"HidePanel %s — %s", kVersion, S().menuSettings);
        SetWindowTextW(g_settingsWnd, title);
    }

    const int M       = Scale(18);
    const int clientW = Scale(620);
    const int cw      = clientW - 2 * M;

    int y = M;

    // --- язык -------------------------------------------------------------
    MakeCtrl(L"STATIC", S().languageLabel, SS_LEFT, M, y, Scale(150), Scale(20), -1, g_fontBold);
    MakeCtrl(L"BUTTON", S().langRu, BS_AUTORADIOBUTTON | BS_LEFT | WS_GROUP,
             M + Scale(160), y - Scale(2), Scale(110), Scale(22), IDC_LANG_RU, g_fontUi);
    MakeCtrl(L"BUTTON", S().langEn, BS_AUTORADIOBUTTON | BS_LEFT,
             M + Scale(275), y - Scale(2), Scale(110), Scale(22), IDC_LANG_EN, g_fontUi);
    SetChecked(IDC_LANG_RU, g_lang == LANG_RU);
    SetChecked(IDC_LANG_EN, g_lang == LANG_EN);
    y += Scale(30);

    MakeCtrl(L"STATIC", nullptr, SS_ETCHEDHORZ, M, y, cw, Scale(1), -1, nullptr);
    y += Scale(14);

    // --- числовые/текстовые строки ----------------------------------------
    struct Row
    {
        const wchar_t* label;
        const wchar_t* desc;
        int  id;
        int  value;
        int  minV, maxV;
        const wchar_t* unit;
    };
    Row rows[] = {
        { S().edgeLabel, S().edgeDesc, IDC_EDGE,   g_opt.edgePx, 1,  64,  S().px },
        { S().pollLabel, S().pollDesc, IDC_POLL,   g_opt.pollMs, 1, 200,  S().ms },
    };

    for (const Row& r : rows)
    {
        MakeCtrl(L"STATIC", r.label, SS_LEFT, M, y, cw - Scale(130), Scale(20), -1, g_fontBold);

        wchar_t buf[32] = {};
        StringCchPrintfW(buf, _countof(buf), L"%d", r.value);
        MakeCtrl(L"EDIT", buf, ES_NUMBER | ES_RIGHT | WS_BORDER,
                 M + cw - Scale(110), y - Scale(3), Scale(70), Scale(24), r.id, g_fontUi);
        MakeCtrl(L"STATIC", r.unit, SS_LEFT, M + cw - Scale(32), y, Scale(32), Scale(20), -1, g_fontUi);
        y += Scale(26);

        int dh = TextHeight(r.desc, cw, g_fontSmall);
        MakeCtrl(L"STATIC", r.desc, SS_LEFT, M, y, cw, dh, IDC_DESC_FIRST + r.id, g_fontSmall);
        y += dh + Scale(12);
    }

    // --- галочки ----------------------------------------------------------
    struct BoolRow
    {
        const wchar_t* label;
        const wchar_t* desc;
        int  id;
        bool value;
    };
    BoolRow bools[] = {
        { S().workAreaLabel, S().workAreaDesc, IDC_WORKAREA, g_opt.fullWorkArea },
        { S().guardLabel,    S().guardDesc,    IDC_GUARD,    g_opt.fullscreenGuard },
        { S().restoreLabel,  S().restoreDesc,  IDC_RESTORE,  g_opt.restoreOnExit },
    };

    for (const BoolRow& b : bools)
    {
        MakeCtrl(L"BUTTON", b.label, BS_AUTOCHECKBOX | BS_LEFT,
                 M - Scale(4), y - Scale(2), cw, Scale(24), b.id, g_fontBold);
        SetChecked(b.id, b.value);
        y += Scale(26);

        int dh = TextHeight(b.desc, cw - Scale(20), g_fontSmall);
        MakeCtrl(L"STATIC", b.desc, SS_LEFT, M + Scale(16), y, cw - Scale(20), dh,
                 IDC_DESC_FIRST + b.id, g_fontSmall);
        y += dh + Scale(10);
    }

    // --- кнопки -----------------------------------------------------------
    const int btnW = Scale(110);
    const int btnH = Scale(28);
    int by = y + Scale(2);

    MakeCtrl(L"BUTTON", S().defaultsBtn, BS_PUSHBUTTON,
             M, by, Scale(140), btnH, IDC_DEFAULTS, g_fontUi);
    MakeCtrl(L"BUTTON", S().cancelBtn, BS_PUSHBUTTON,
             M + cw - btnW * 2 - Scale(10), by, btnW, btnH, IDC_CANCEL, g_fontUi);
    MakeCtrl(L"BUTTON", S().saveBtn, BS_DEFPUSHBUTTON,
             M + cw - btnW, by, btnW, btnH, IDC_SAVE, g_fontUi);

    int clientH = by + btnH + M;

    // --- подгоняем размер окна -------------------------------------------
    RECT wr = { 0, 0, clientW, clientH };
    DWORD style = (DWORD)GetWindowLongPtrW(g_settingsWnd, GWL_STYLE);
    DWORD ex    = (DWORD)GetWindowLongPtrW(g_settingsWnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&wr, style, FALSE, ex);

    int wndW = wr.right - wr.left;
    int wndH = wr.bottom - wr.top;

    RECT cur = {};
    GetWindowRect(g_settingsWnd, &cur);
    if ((cur.right - cur.left) != wndW || (cur.bottom - cur.top) != wndH)
    {
        HMONITOR hm = MonitorFromWindow(g_settingsWnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hm, &mi);

        int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - wndW) / 2;
        int cy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wndH) / 2;
        if (cy < mi.rcWork.top) cy = mi.rcWork.top;

        SetWindowPos(g_settingsWnd, nullptr, x, cy, wndW, wndH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static bool SaveConfigFile()
{
    bool ok = true;
    wchar_t buf[64] = {};
    auto put = [&](const wchar_t* key, const wchar_t* value) {
        ok = WritePrivateProfileStringW(L"settings", key, value, g_opt.iniPath.c_str()) && ok;
    };

    StringCchPrintfW(buf, _countof(buf), L"%d", g_opt.edgePx);
    put(L"edge_px", buf);
    StringCchPrintfW(buf, _countof(buf), L"%d", g_opt.pollMs);
    put(L"poll_ms", buf);
    put(L"full_work_area",    g_opt.fullWorkArea    ? L"1" : L"0");
    put(L"fullscreen_guard",  g_opt.fullscreenGuard ? L"1" : L"0");
    put(L"restore_on_exit",   g_opt.restoreOnExit   ? L"1" : L"0");
    put(L"language",          g_lang == LANG_EN ? L"en" : L"ru");

    return ok;
}

static void ApplySettings()
{
    UINT edge = GetDlgItemInt(g_settingsWnd, IDC_EDGE, nullptr, FALSE);
    UINT poll = GetDlgItemInt(g_settingsWnd, IDC_POLL, nullptr, FALSE);
    if (edge < 1 || edge > 64 || poll < 1 || poll > 200)
    {
        MessageBoxW(g_settingsWnd, S().errRange, S().errTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    bool wasFullWorkArea = g_opt.fullWorkArea;

    g_opt.edgePx          = (int)edge;
    g_opt.pollMs          = (int)poll;
    g_opt.fullWorkArea    = IsChecked(IDC_WORKAREA);
    g_opt.fullscreenGuard = IsChecked(IDC_GUARD);
    g_opt.restoreOnExit   = IsChecked(IDC_RESTORE);

    // рабочая область — сразу
    if (g_opt.fullWorkArea != wasFullWorkArea)
        SetWorkAreaMode(g_opt.fullWorkArea);

    if (!SaveConfigFile())
    {
        MessageBoxW(g_settingsWnd, S().errSave, S().errTitle, MB_OK | MB_ICONERROR);
        return;
    }

    LogW(L"settings saved (edge=%d, poll=%d, workArea=%d, guard=%d, restore=%d, log=%d, lang=%d)",
         g_opt.edgePx, g_opt.pollMs, g_opt.fullWorkArea ? 1 : 0,
         g_opt.fullscreenGuard ? 1 : 0, g_opt.restoreOnExit ? 1 : 0,
         g_opt.logEnabled ? 1 : 0, (int)g_lang);

    TrayUpdateTip();
    DestroyWindow(g_settingsWnd);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CTLCOLORSTATIC:
    {
        HWND ctl = reinterpret_cast<HWND>(lp);
        int id = GetDlgCtrlID(ctl);
        if (id >= IDC_DESC_FIRST)
        {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_LANG_RU:
            if (g_lang != LANG_RU) { g_lang = LANG_RU; BuildSettingsUi(); }
            return 0;
        case IDC_LANG_EN:
            if (g_lang != LANG_EN) { g_lang = LANG_EN; BuildSettingsUi(); }
            return 0;
        case IDC_SAVE:
            ApplySettings();
            return 0;
        case IDC_CANCEL:
            g_lang = g_langOnOpen;          // отмена — язык не меняем
            DestroyWindow(hwnd);
            return 0;
        case IDC_DEFAULTS:
            SetDlgItemInt(hwnd, IDC_EDGE, 2, FALSE);
            SetDlgItemInt(hwnd, IDC_POLL, 5, FALSE);
            SetChecked(IDC_WORKAREA, true);
            SetChecked(IDC_GUARD, true);
            SetChecked(IDC_RESTORE, true);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        g_lang = g_langOnOpen;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_settingsWnd = nullptr;
        g_settingsOpen = false;
        DestroyFonts();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSettingsWindow()
{
    if (g_settingsWnd && IsWindow(g_settingsWnd))
    {
        ShowWindow(g_settingsWnd, SW_SHOW);
        SetForegroundWindow(g_settingsWnd);
        return;
    }

    static bool classRegistered = false;
    if (!classRegistered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = SettingsWndProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kSettingsClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        classRegistered = true;
    }

    g_langOnOpen = g_lang;
    g_settingsOpen = true;

    wchar_t title[128] = {};
    StringCchPrintfW(title, _countof(title), L"HidePanel %s — %s", kVersion, S().menuSettings);

    // размер уточним в BuildSettingsUi
    g_settingsWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                    kSettingsClass, title,
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                    CW_USEDEFAULT, CW_USEDEFAULT, Scale(640), Scale(600),
                                    nullptr, nullptr, g_hInst, nullptr);
    if (!g_settingsWnd) { g_settingsOpen = false; return; }

    BuildSettingsUi();
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
    LogW(L"settings window opened");
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

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_TRAY_EXIT:
            LogW(L"exit from tray menu");
            DestroyWindow(hwnd);
            return 0;
        case IDM_TRAY_AUTOSTART:
            SetAutostart(!IsAutostartEnabled());
            return 0;
        case IDM_TRAY_SETTINGS:
            ShowSettingsWindow();
            return 0;
        }
        return 0;

    default:
        if (msg == WM_TRAYICON)
        {
            switch (LOWORD(lp))
            {
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowTrayMenu();
                return 0;
            }
            return 0;
        }
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

    if (g_opt.hotkeyVk != 0 &&
        !RegisterHotKey(g_hwnd, kHotkeyId, g_opt.hotkeyMods | MOD_NOREPEAT, g_opt.hotkeyVk))
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
    // По умолчанию — язык системы; файл настроек может его переопределить.
    g_lang = DetectSystemLanguage();

    if (GetFileAttributesW(g_opt.iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    g_opt.edgePx = IniInt(L"settings", L"edge_px", g_opt.edgePx);
    g_opt.pollMs = IniInt(L"settings", L"poll_ms", g_opt.pollMs);
    g_opt.restoreOnExit = IniInt(L"settings", L"restore_on_exit", g_opt.restoreOnExit ? 1 : 0) != 0;
    g_opt.logEnabled   = IniInt(L"settings", L"log", g_opt.logEnabled ? 1 : 0) != 0;
    g_opt.fullscreenGuard = IniInt(L"settings", L"fullscreen_guard", g_opt.fullscreenGuard ? 1 : 0) != 0;
    g_opt.fullWorkArea    = IniInt(L"settings", L"full_work_area", g_opt.fullWorkArea ? 1 : 0) != 0;

    std::wstring hk = IniString(L"settings", L"exit_hotkey", L"");
    if (!hk.empty())
    {
        if (_wcsicmp(hk.c_str(), L"none") == 0 ||
            _wcsicmp(hk.c_str(), L"off") == 0 ||
            _wcsicmp(hk.c_str(), L"no") == 0)
        {
            g_opt.hotkeyVk = 0;                     // без горячей клавиши
        }
        else
        {
            UINT m = 0, v = 0;
            if (ParseHotkey(hk, m, v)) { g_opt.hotkeyMods = m; g_opt.hotkeyVk = v; }
        }
    }

    std::wstring lang = IniString(L"settings", L"language", L"auto");
    if (_wcsicmp(lang.c_str(), L"ru") == 0)      g_lang = LANG_RU;
    else if (_wcsicmp(lang.c_str(), L"en") == 0) g_lang = LANG_EN;
    else                                         g_lang = DetectSystemLanguage();

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
    std::wstring hkText = (g_opt.hotkeyVk != 0)
        ? HotkeyToString(g_opt.hotkeyMods, g_opt.hotkeyVk)
        : (g_lang == LANG_EN ? std::wstring(L"none") : std::wstring(L"нет"));

    std::wstring text =
        L"HidePanel " + std::wstring(kVersion) + L"\n\n" +
        S().helpBody +
        L"\n" + S().helpExit + L": " + hkText +
        L"   (" + L"HidePanel.exe --quit" + L")\n" +
        S().helpSettings + L": HidePanel.ini\n" +
        S().helpLog + L": " + g_opt.logPath;
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

    // Если авто-скрытие было выключено — вернуть зарезервированное место
    // (при включённом авто-скрытии оболочка пересчитает всё сама).
    if (haveState && !g_originalAutoHide)
        RestoreWorkArea(false, true);

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

    InitWorkAreaInfo();

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
        wchar_t v[64] = {};
        StringCchPrintfW(v, _countof(v), L"HidePanel %s", kVersion);
        MessageBoxW(nullptr, v, L"HidePanel", MB_OK | MB_ICONINFORMATION);
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
    InitWorkAreaInfo();

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

        // Рабочая область = весь экран, панель рисуется поверх окон.
        if (g_opt.fullWorkArea)
        {
            if (ApplyFullWorkArea())
                BroadcastWorkAreaChange();   // подтянуть уже развёрнутые окна
        }
    }
    else
    {
        LogW(L"no taskbar windows found");
    }

    TrayAdd();

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
        ShowTrays(true);
        RestoreWorkArea(g_originalAutoHide);
        RestoreShell(false);
    }
    else
    {
        ShowTrays(true);
        RestoreWorkArea(g_originalAutoHide);
    }

    TrayRemove();

    if (g_hwnd) { UnregisterHotKey(g_hwnd, kHotkeyId); DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }

    LogW(L"---- HidePanel stopped ----");
    LogShutdown();
    return 0;
}

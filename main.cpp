/*
 * latency.exe  —  Windows 10 Latency Optimizer
 * ──────────────────────────────────────────────────────────────────────────
 * Personal use, CMD-based. No GUI. No installer. Just run it.
 *
 * Usage:
 *   latency.exe apply           Apply ALL tweaks (requires Admin)
 *   latency.exe restore         Restore Windows defaults
 *   latency.exe status          Show current state
 *   latency.exe timer start     Start 0.5ms timer daemon (background)
 *   latency.exe timer stop      Kill timer daemon
 *   latency.exe timer status    Show timer resolution info
 *   latency.exe help
 *
 * Individual module flags (apply just one module):
 *   latency.exe apply --boot       bcdedit boot tweaks only
 *   latency.exe apply --vbs        VBS/HVCI only
 *   latency.exe apply --spectre    Spectre/Meltdown only
 *
 * Build:
 *   g++ main.cpp -O2 -std=c++17 -Wall -static -o latency.exe
 *       -ladvapi32 -lpowrprof -lshlwapi -lole32
 *
 * Modules in 'apply' (all run by default):
 *   [1]  Registry      — MMCSS, GameDVR, mouse, TCP, FTH, telemetry,
 *                        NetworkThrottlingIndex, Win32PrioritySeparation,
 *                        DisablePagingExecutive, HAGS, timer coalescing,
 *                        SvcHostSplitThreshold, Psched, crash dump, kill timeouts
 *   [2]  Services      — SysMain, DiagTrack, Xbox, WSearch, lfsvc, WerSvc,
 *                        DoSvc, CDPSvc, PcaSvc, Sensors, Fax, PrintNotify, etc.
 *   [3]  Network       — Interrupt Moderation, EEE, Flow Control, RSC (via PS)
 *   [4]  Network Stack — TCP autotuning, RSS, chimney, ECN, timestamps, IPv6
 *   [5]  Power         — Ultimate Performance + USB suspend
 *   [6]  CPU Power     — Core parking, idle states, boost mode
 *   [7]  MSI Mode      — GPU + NIC interrupt routing              (restart req.)
 *   [8]  GPU           — NVIDIA PowerMizer lock, HAGS registry
 *   [9]  Visual FX     — Disable animations, transparency, menu delay
 *   [10] Boot          — bcdedit tweaks                           (restart req.)
 *   [11] VBS/HVCI      — Disable virtualization-based security    (restart req.)
 *   [12] Spectre       — Disable Spectre/Meltdown mitigations     (restart req.)
 *   [13] Timer Daemon  — 0.5ms NtSetTimerResolution daemon
 */

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winsvc.h>
#include <powrprof.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <conio.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <functional>
#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

// ─────────────────────────────────────────────────────────────────────────────
//  NT API typedefs  (ntdll, undocumented but stable since XP)
// ─────────────────────────────────────────────────────────────────────────────
typedef LONG NTSTATUS;
typedef NTSTATUS(WINAPI* pfnNtSetTimerResolution)  (ULONG, BOOLEAN, PULONG);
typedef NTSTATUS(WINAPI* pfnNtQueryTimerResolution)(PULONG, PULONG, PULONG);

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────
static const char*  DAEMON_MUTEX    = "Global\\LatencyOptimizerDaemon_v1";
static const char*  DAEMON_FLAG     = "--timer-daemon-internal";
static const char*  DAEMON_PID_FILE = "daemon.pid";
static const ULONG  TIMER_TARGET    = 5000;   // 0.5ms in 100ns units

// Ultimate Performance power plan GUID
static const GUID GUID_ULTIMATE = {
    0xe9a42b02, 0xd5df, 0x448d,
    { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 }
};

static std::string g_selfPath;   // full path to this exe
static std::string g_selfDir;    // directory containing this exe

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 1  —  Console helpers
// ─────────────────────────────────────────────────────────────────────────────
enum Color {
    C_DEFAULT  = 7,
    C_DARK     = 8,
    C_RED      = 12,
    C_GREEN    = 10,
    C_YELLOW   = 14,
    C_CYAN     = 11,
    C_MAGENTA  = 13,
    C_WHITE    = 15
};

static void SetCol(Color c) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)c);
}

static void Print(const std::string& s, Color c = C_DEFAULT) {
    SetCol(c); std::cout << s; SetCol(C_DEFAULT);
}
static void Println(const std::string& s, Color c = C_DEFAULT) {
    Print(s + "\n", c);
}

static void OK   (const std::string& s) { Print("  [", C_DARK); Print("+", C_GREEN);   Print("] ", C_DARK); Println(s); }
static void FAIL (const std::string& s) { Print("  [", C_DARK); Print("!", C_RED);     Print("] ", C_DARK); Println(s); }
static void INFO (const std::string& s) { Print("  [", C_DARK); Print("*", C_CYAN);    Print("] ", C_DARK); Println(s); }
static void WARN (const std::string& s) { Print("  [", C_DARK); Print("~", C_YELLOW);  Print("] ", C_DARK); Println(s); }
static void SKIP (const std::string& s) { Print("  [-] ", C_DARK); Println(s, C_DARK); }

static void Section(const std::string& title) {
    std::cout << "\n";
    Print("  --- ", C_DARK);
    Print(title, C_CYAN);
    Print(" ---------------------\n", C_DARK);
}

static void Banner() {
    Print(
        "\n"
        "   _       _       _   ___ _  _ _____   __\n"
        "  | |     /_\\     | | | __| \\| |  __ \\ \\ \\   v1.2\n"
        "  | |__  / _ \\  __| | | _|| .` | |__/ /  > \\  Win10\n"
        "  |____|/_/ \\_\\|____| |___|_|\\_|_| ___/  /_/\n"
        "\n", C_CYAN);
    Print("  Windows 10 Latency Optimizer", C_WHITE);
    Print("  --  personal tool by LO\n\n", C_DARK);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 2  —  Utilities
// ─────────────────────────────────────────────────────────────────────────────
static bool IsElevated() {
    BOOL el = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te{};
        DWORD len = sizeof(te);
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &len))
            el = te.TokenIsElevated;
        CloseHandle(tok);
    }
    return el != FALSE;
}

static void RequireAdmin() {
    if (!IsElevated()) {
        FAIL("Requires Administrator. Re-run from an elevated prompt.");
        exit(1);
    }
}

static int RunCmd(const std::string& cmd) {
    std::string full = "cmd.exe /c \"" + cmd + "\" >nul 2>&1";
    return system(full.c_str());
}

static int RunPS(const std::string& script) {
    // Write the script to a temp .ps1 file — cleanest approach, no quoting hell
    char tmpPath[MAX_PATH]{}, tmpFile[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmpPath);
    GetTempFileNameA(tmpPath, "lopt", 0, tmpFile);

    {
        std::ofstream f(tmpFile);
        if (!f) {
            // Fallback: direct command (may fail with complex quoting)
            std::string cmd = "powershell.exe -NonInteractive -NoProfile -Command \"" +
                              script + "\" >nul 2>&1";
            return system(cmd.c_str());
        }
        f << script;
    }

    std::string cmd = std::string("powershell.exe -NonInteractive -NoProfile"
                                  " -ExecutionPolicy Bypass -File \"") +
                      tmpFile + "\" >nul 2>&1";
    int r = system(cmd.c_str());
    DeleteFileA(tmpFile);
    return r;
}

static DWORD GetWinBuild() {
    HKEY hk;
    char buf[16] = "0";
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD sz = sizeof(buf);
        RegQueryValueExA(hk, "CurrentBuildNumber", nullptr, nullptr, (LPBYTE)buf, &sz);
        RegCloseKey(hk);
    }
    return (DWORD)atoi(buf);
}

static std::string GuidStr(const GUID& g) {
    char b[40];
    snprintf(b, sizeof(b),
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (unsigned long)g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 3  —  Timer Resolution
// ─────────────────────────────────────────────────────────────────────────────

// Query current timer state
struct TimerState {
    ULONG minRes;   // highest interval (worst),  100ns units
    ULONG maxRes;   // lowest interval  (best),   100ns units
    ULONG curRes;   // current resolution,         100ns units
};

static TimerState QueryTimerState() {
    TimerState ts{};
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto fn = (pfnNtQueryTimerResolution)GetProcAddress(ntdll, "NtQueryTimerResolution");
    if (fn) fn(&ts.minRes, &ts.maxRes, &ts.curRes);
    return ts;
}

static bool SetTimer(ULONG res, BOOLEAN enable) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto fn = (pfnNtSetTimerResolution)GetProcAddress(ntdll, "NtSetTimerResolution");
    if (!fn) return false;
    ULONG cur;
    return fn(res, enable, &cur) == 0L; // STATUS_SUCCESS
}

// ── Timer Daemon ──────────────────────────────────────────────────────────────
// This process holds NtSetTimerResolution(0.5ms, TRUE) forever.
// Win10 2004+ changed timer to per-process, so we need this daemon alive.
static void RunTimerDaemon() {
    // Named mutex: prevents duplicate daemons
    HANDLE hMtx = CreateMutexA(nullptr, TRUE, DAEMON_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMtx) CloseHandle(hMtx);
        return;
    }

    // Write our PID so Stop() can find us
    std::string pidFile = g_selfDir + "\\" + DAEMON_PID_FILE;
    {
        std::ofstream f(pidFile);
        if (f) f << GetCurrentProcessId();
    }

    // Set and hold
    SetTimer(TIMER_TARGET, TRUE);

    // Re-apply every second (defensive against anything resetting it)
    while (true) {
        SetTimer(TIMER_TARGET, TRUE);
        Sleep(1000);
    }
}

static bool IsDaemonRunning() {
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, DAEMON_MUTEX);
    if (h) { CloseHandle(h); return true; }
    return false;
}

static void StartDaemon() {
    if (IsDaemonRunning()) { INFO("Timer daemon already running."); return; }

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::string cmd = "\"" + g_selfPath + "\" " + DAEMON_FLAG;
    if (CreateProcessA(nullptr, &cmd[0],
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS,
        nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(400);
        OK("Timer daemon started  (0.5ms / 2000Hz)");
    } else {
        FAIL("Failed to start daemon: " + std::to_string(GetLastError()));
    }
}

static void StopDaemon() {
    // Try PID file first
    std::string pidFile = g_selfDir + "\\" + DAEMON_PID_FILE;
    std::ifstream f(pidFile);
    if (f) {
        DWORD pid = 0;
        f >> pid;
        f.close();
        if (pid) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) {
                TerminateProcess(h, 0);
                CloseHandle(h);
                DeleteFileA(pidFile.c_str());
                OK("Timer daemon stopped. Resolution back to Windows default.");
                return;
            }
        }
        DeleteFileA(pidFile.c_str());
    }

    // Fallback: scan process list
    bool found = false;
    std::string selfName = PathFindFileNameA(g_selfPath.c_str());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe{ sizeof(pe) };
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, selfName.c_str()) == 0
                && pe.th32ProcessID != GetCurrentProcessId()) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); found = true; }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    if (found) OK("Timer daemon stopped.");
    else       WARN("Daemon was not running.");
}

static void CmdTimerStatus() {
    auto ts = QueryTimerState();
    Section("Timer Resolution");
    INFO("Min (worst) : " + std::to_string(ts.minRes / 10) + " us  (" + std::to_string(ts.minRes) + " x100ns)");
    INFO("Max (best)  : " + std::to_string(ts.maxRes / 10) + " us  (" + std::to_string(ts.maxRes) + " x100ns)");
    std::string cs = std::to_string(ts.curRes / 10) + " us";
    if (ts.curRes <= 5000) OK ("Current     : " + cs + "  <- optimized");
    else                   WARN("Current     : " + cs + "  <- default  (run: timer start)");
    INFO("Daemon      : " + std::string(IsDaemonRunning() ? "RUNNING" : "stopped"));

    DWORD build = GetWinBuild();
    if (build >= 19041)
        WARN("Build " + std::to_string(build) + ": timer is per-process — daemon MUST stay alive");
    else
        INFO("Build " + std::to_string(build) + ": timer is global (affects all processes)");
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 4  —  Registry helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool RegDWORD(HKEY root, const char* path, const char* name, DWORD val) {
    HKEY hk;
    if (RegCreateKeyExA(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr)
        != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExA(hk, name, 0, REG_DWORD, (LPBYTE)&val, sizeof(val)) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return ok;
}

static bool RegSZ(HKEY root, const char* path, const char* name, const char* val) {
    HKEY hk;
    if (RegCreateKeyExA(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr)
        != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExA(hk, name, 0, REG_SZ, (LPBYTE)val, (DWORD)strlen(val) + 1) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return ok;
}

static bool RegDel(HKEY root, const char* path, const char* name) {
    HKEY hk;
    if (RegOpenKeyExA(root, path, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return false;
    bool ok = RegDeleteValueA(hk, name) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return ok;
}

static DWORD RegRead(HKEY root, const char* path, const char* name, DWORD def = 0) {
    HKEY hk;
    DWORD v = def, sz = sizeof(v);
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, name, nullptr, nullptr, (LPBYTE)&v, &sz);
        RegCloseKey(hk);
    }
    return v;
}

static std::string RegReadSZ(HKEY root, const char* path, const char* name, const char* def = "") {
    HKEY hk;
    char buf[64] = {};
    DWORD sz = sizeof(buf);
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, name, nullptr, nullptr, (LPBYTE)buf, &sz);
        RegCloseKey(hk);
        return buf;
    }
    return def;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 5  —  Registry Tweaks
// ─────────────────────────────────────────────────────────────────────────────
struct Tweak {
    const char* label;
    std::function<bool()> apply;
    std::function<bool()> restore;
};

static std::vector<Tweak> BuildTweaks() {
    return {
        // ── MMCSS ──────────────────────────────────────────────────────────
        {
            "MMCSS SystemResponsiveness = 0 (100% CPU to high-priority tasks)",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                "SystemResponsiveness", 0); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                "SystemResponsiveness", 20); }
        },
        {
            "MMCSS NoLazyMode = 1 (removes idle sleep chains from scheduler)",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                "NoLazyMode", 1); },
            []{ return RegDel(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                "NoLazyMode"); }
        },
        {
            "MMCSS Games: GPU Priority = 8",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
                "GPU Priority", 8); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
                "GPU Priority", 2); }
        },
        {
            "MMCSS Games: CPU Priority = 6",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
                "Priority", 6); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
                "Priority", 2); }
        },
        {
            "MMCSS Games: Scheduling Category = High",
            []{ return RegSZ(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
                "Scheduling Category", "High"); },
            []{ return RegSZ(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
                "Scheduling Category", "Medium"); }
        },

        // ── Game DVR / Xbox Game Bar ────────────────────────────────────────
        {
            "GameDVR: disable recording overlay overhead",
            []{
                RegDWORD(HKEY_CURRENT_USER,  "System\\GameConfigStore", "GameDVR_Enabled", 0);
                RegDWORD(HKEY_CURRENT_USER,  "System\\GameConfigStore", "GameDVR_FSEBehavior", 2);
                RegDWORD(HKEY_CURRENT_USER,  "System\\GameConfigStore", "GameDVR_HonorUserFSEBehaviorMode", 1);
                RegDWORD(HKEY_CURRENT_USER,  "System\\GameConfigStore", "GameDVR_DXGIHonorFSEWindowsCompatible", 1);
                RegDWORD(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", "AllowGameDVR", 0);
                return true;
            },
            []{
                RegDWORD(HKEY_CURRENT_USER,  "System\\GameConfigStore", "GameDVR_Enabled", 1);
                RegDel  (HKEY_CURRENT_USER,  "System\\GameConfigStore", "GameDVR_FSEBehavior");
                RegDel  (HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", "AllowGameDVR");
                return true;
            }
        },
        {
            "GameBar PresenceWriter: disable background activation",
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\WindowsRuntime\\ActivatableClassId\\"
                    "Windows.Gaming.GameBar.PresenceServer.Internal.PresenceWriter",
                    "ActivationType", 0);
            },
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\WindowsRuntime\\ActivatableClassId\\"
                    "Windows.Gaming.GameBar.PresenceServer.Internal.PresenceWriter",
                    "ActivationType", 1);
            }
        },

        // ── Mouse / Input ───────────────────────────────────────────────────
        {
            "Mouse: disable acceleration (raw 1:1 input)",
            []{
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseSpeed",      "0");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseThreshold1", "0");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseThreshold2", "0");
                return true;
            },
            []{
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseSpeed",      "1");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseThreshold1", "6");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseThreshold2", "10");
                return true;
            }
        },
        {
            "StickyKeys: disable popup hotkey",
            []{
                return RegSZ(HKEY_CURRENT_USER,
                    "Control Panel\\Accessibility\\StickyKeys", "Flags", "506");
            },
            []{
                return RegSZ(HKEY_CURRENT_USER,
                    "Control Panel\\Accessibility\\StickyKeys", "Flags", "510");
            }
        },

        // ── Boot / Power ────────────────────────────────────────────────────
        {
            "Fast Startup: disable (avoids stale driver state on cold boot)",
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                    "HiberbootEnabled", 0);
            },
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                    "HiberbootEnabled", 1);
            }
        },

        // ── Background / Overhead ───────────────────────────────────────────
        {
            "Background apps: disable for all UWP apps",
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
                    "LetAppsRunInBackground", 2);
            },
            []{
                return RegDel(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
                    "LetAppsRunInBackground");
            }
        },
        {
            "Fault Tolerant Heap: disable (FTH adds jitter to allocations)",
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\FTH", "Enabled", 0);
            },
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\FTH", "Enabled", 1);
            }
        },
        {
            "Automatic Maintenance: disable scheduler interference",
            []{
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\Maintenance",
                    "MaintenanceDisabled", 1);
            },
            []{
                return RegDel(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\Maintenance",
                    "MaintenanceDisabled");
            }
        },
        {
            "Transparency effects: disable (less DWM GPU overhead)",
            []{
                return RegDWORD(HKEY_CURRENT_USER,
                    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    "EnableTransparency", 0);
            },
            []{
                return RegDWORD(HKEY_CURRENT_USER,
                    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    "EnableTransparency", 1);
            }
        },

        // ── Telemetry ───────────────────────────────────────────────────────
        {
            "Telemetry: cap to minimum level (reduces disk/net background activity)",
            []{
                RegDWORD(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection",
                    "AllowTelemetry", 0);
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Services\\DiagTrack",
                    "Start", 4);
            },
            []{
                RegDel(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection",
                    "AllowTelemetry");
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Services\\DiagTrack",
                    "Start", 2);
            }
        },

        // ── Scheduler / Memory ──────────────────────────────────────────────
        {
            "Win32PrioritySeparation = 38 (short quanta, high foreground boost)",
            // 38 decimal = 0x26 = Short | Variable | High foreground boost
            // Shorter scheduler quanta: foreground app gets CPU time faster, more responsive
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
                "Win32PrioritySeparation", 38); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
                "Win32PrioritySeparation", 2); }
        },
        {
            "DisablePagingExecutive = 1 (kernel + drivers pinned in RAM)",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                "DisablePagingExecutive", 1); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                "DisablePagingExecutive", 0); }
        },
        {
            "NetworkThrottlingIndex = 0xFFFFFFFF (remove MMCSS 10pkt/ms network cap)",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                "NetworkThrottlingIndex", 0xFFFFFFFF); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                "NetworkThrottlingIndex", 10); }
        },
        {
            "HAGS: HwSchMode = 2 (Hardware Accelerated GPU Scheduling, Win10 2004+)",
            // GPU manages its own queue -> lower scheduling latency, better frame pacing
            // Requires WDDM 2.7+ driver (most post-2020 drivers). Needs restart.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                "HwSchMode", 2); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                "HwSchMode", 1); }
        },

        // ── TCP Network ─────────────────────────────────────────────────────
        {
            "TCP: TcpAckFrequency=1 + TCPNoDelay=1 (disable Nagle for TCP games/apps)",
            []{
                // Apply to every NIC interface
                HKEY hBase;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces",
                    0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hBase) != ERROR_SUCCESS) return false;
                char sub[256]; DWORD idx = 0, len = sizeof(sub);
                while (RegEnumKeyExA(hBase, idx++, sub, &len,
                    nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    std::string p = std::string(
                        "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + sub;
                    RegDWORD(HKEY_LOCAL_MACHINE, p.c_str(), "TcpAckFrequency", 1);
                    RegDWORD(HKEY_LOCAL_MACHINE, p.c_str(), "TCPNoDelay",      1);
                    len = sizeof(sub);
                }
                RegCloseKey(hBase);
                return true;
            },
            []{
                HKEY hBase;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces",
                    0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hBase) != ERROR_SUCCESS) return false;
                char sub[256]; DWORD idx = 0, len = sizeof(sub);
                while (RegEnumKeyExA(hBase, idx++, sub, &len,
                    nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    std::string p = std::string(
                        "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + sub;
                    RegDel(HKEY_LOCAL_MACHINE, p.c_str(), "TcpAckFrequency");
                    RegDel(HKEY_LOCAL_MACHINE, p.c_str(), "TCPNoDelay");
                    len = sizeof(sub);
                }
                RegCloseKey(hBase);
                return true;
            }
        },

        // ── Timer Coalescing ────────────────────────────────────────────────
        {
            "Timer Coalescing: disabled (CoalescingTimerInterval=0)",
            // Prevents CPU from grouping timer events together, reduces scheduling jitter
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                "CoalescingTimerInterval", 0); },
            []{ return RegDel(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                "CoalescingTimerInterval"); }
        },

        // ── SvcHost Split Threshold ─────────────────────────────────────────
        {
            "SvcHostSplitThresholdInKB = <RAM_in_KB> (reduce svchost process count)",
            // Forces Windows to group services back into shared svchost processes
            // (reduces per-process overhead). Detect actual RAM and set accordingly.
            []{
                MEMORYSTATUSEX memStatus{};
                memStatus.dwLength = sizeof(memStatus);
                DWORD threshold = 33554432; // 32 GB fallback in KB
                if (GlobalMemoryStatusEx(&memStatus)) {
                    DWORDLONG kb = memStatus.ullTotalPhys / 1024;
                    // Clamp to 32GB in KB as a reasonable max
                    DWORDLONG maxKB = (DWORDLONG)33554432;
                    if (kb > maxKB) kb = maxKB;
                    threshold = (DWORD)kb;
                }
                return RegDWORD(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Control",
                    "SvcHostSplitThresholdInKB", threshold);
            },
            []{ return RegDel(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control",
                "SvcHostSplitThresholdInKB"); }
        },

        // ── Psched Timer Resolution ─────────────────────────────────────────
        {
            "Psched TimerResolution = 1us (stabilizes ndis.sys DPC)",
            // 1 microsecond packet scheduler precision — stabilizes ndis.sys DPC latency
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Policies\\Microsoft\\Windows\\Psched",
                "TimerResolution", 1); },
            []{ return RegDel(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Policies\\Microsoft\\Windows\\Psched",
                "TimerResolution"); }
        },

        // ── PageFile Clear on Shutdown ──────────────────────────────────────
        {
            "PageFile: don't clear on shutdown (faster shutdown)",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                "ClearPageFileAtShutdown", 0); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                "ClearPageFileAtShutdown", 0); }
        },

        // ── App / Service Kill Timeouts ─────────────────────────────────────
        {
            "App/Service kill timeout: 2s (faster cleanup on crash)",
            // AutoEndTasks, WaitToKillAppTimeout, HungAppTimeout, WaitToKillServiceTimeout
            []{
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "AutoEndTasks",           "1");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "WaitToKillAppTimeout",   "2000");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "HungAppTimeout",         "1000");
                RegSZ(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Control",
                    "WaitToKillServiceTimeout", "2000");
                return true;
            },
            []{
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "AutoEndTasks",           "0");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "WaitToKillAppTimeout",   "20000");
                RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "HungAppTimeout",         "5000");
                RegSZ(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Control",
                    "WaitToKillServiceTimeout", "5000");
                return true;
            }
        },

        // ── Crash Dump ──────────────────────────────────────────────────────
        {
            "Crash dump: disabled (no kernel dump overhead)",
            // Don't write dump files — removes kernel overhead on crash/BSOD
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\CrashControl",
                "CrashDumpEnabled", 0); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\CrashControl",
                "CrashDumpEnabled", 7); }
        },

        // ── Windows Error Reporting ─────────────────────────────────────────
        {
            "Windows Error Reporting: disabled via registry",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting",
                "Disabled", 1); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting",
                "Disabled", 0); }
        },

        // ── NTFS File System ────────────────────────────────────────────────
        {
            "NTFS: DisableLastAccessUpdate = 0x80000001 (no disk write on every file read)",
            // Every file open triggers a last-access timestamp write by default.
            // 0x80000001 = disable + persist flag (Win8+ extended format).
            // Eliminates unnecessary I/O jitter during heavy game asset streaming.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                "NtfsDisableLastAccessUpdate", 0x80000001); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                "NtfsDisableLastAccessUpdate", 0x00000000); }
        },
        {
            "NTFS: Disable8dot3NameCreation = 1 (no 8.3 filename generation overhead)",
            // Eliminates the creation of legacy 8.3 short names on every new file.
            // Measurable reduction in file creation time on large directories.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                "NtfsDisable8dot3NameCreation", 1); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                "NtfsDisable8dot3NameCreation", 0); }
        },
        {
            "NTFS: NtfsMemoryUsage = 2 (more paged pool for NTFS metadata cache)",
            // Value 2 tells NTFS to use more paged pool RAM for internal caches.
            // Reduces disk reads on repeated access to the same directories.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                "NtfsMemoryUsage", 2); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                "NtfsMemoryUsage", 1); }
        },

        // ── Global Timer Resolution ─────────────────────────────────────────
        {
            "GlobalTimerResolutionRequests = 1 (any process raises global timer — Win10 2004+)",
            // Win10 build 19041+ made NtSetTimerResolution per-process by default.
            // This key reverts to the old global behavior: when any process requests
            // a higher timer frequency, it lifts the resolution system-wide.
            // Without it, our daemon only helps itself — games still get 15ms ticks.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel",
                "GlobalTimerResolutionRequests", 1); },
            []{ return RegDel(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel",
                "GlobalTimerResolutionRequests"); }
        },

        // ── Input Device Queue Sizes ────────────────────────────────────────
        {
            "Mouse: MouseDataQueueSize = 16 (tighter queue, faster input dispatch)",
            // Windows default is 100 packets — deep queue buffers old positions.
            // 16 forces stale events to be discarded sooner, polling feels tighter.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\mouclass\\Parameters",
                "MouseDataQueueSize", 16); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\mouclass\\Parameters",
                "MouseDataQueueSize", 100); }
        },
        {
            "Keyboard: KeyboardDataQueueSize = 16 (tighter keyboard buffer)",
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\kbdclass\\Parameters",
                "KeyboardDataQueueSize", 16); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\kbdclass\\Parameters",
                "KeyboardDataQueueSize", 100); }
        },

        // ── Network Server IRP Stack ────────────────────────────────────────
        {
            "LanmanServer: IrpStackSize = 30 (larger server IRP stack)",
            // Prevents buffer overflow errors in SMB and local loopback networking.
            // Also stabilizes some game anti-cheat drivers that use named pipes.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                "IrpStackSize", 30); },
            []{ return RegDel(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
                "IrpStackSize"); }
        },

        // ── GPU TDR (Timeout Detection and Recovery) ────────────────────────
        {
            "GPU TdrDelay = 60 (prevents driver reset during heavy GPU load)",
            // Default TDR timeout is 2 seconds — a heavy scene or shader compile
            // can spike past it and trigger an unnecessary driver reset + stutter.
            // 60s gives headroom without disabling recovery entirely.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                "TdrDelay", 60); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                "TdrDelay", 2); }
        },

        // ── MMCSS Games task: full configuration ────────────────────────────
        {
            "MMCSS Games: Affinity=0, Background Only=False, SFIO=High, ClockRate=10000",
            // Affinity=0 means use all CPUs. Background Only=False allows foreground
            // boosting. SFIO Priority=High gives storage I/O priority. Clock Rate=10000
            // (1ms in 100ns units) = scheduler resolution for this task class.
            []{
                const char* p =
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
                    "\\Multimedia\\SystemProfile\\Tasks\\Games";
                RegDWORD(HKEY_LOCAL_MACHINE, p, "Affinity",        0);
                RegSZ   (HKEY_LOCAL_MACHINE, p, "Background Only", "False");
                RegDWORD(HKEY_LOCAL_MACHINE, p, "Clock Rate",      10000);
                RegSZ   (HKEY_LOCAL_MACHINE, p, "SFIO Priority",   "High");
                return true;
            },
            []{
                const char* p =
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
                    "\\Multimedia\\SystemProfile\\Tasks\\Games";
                RegDWORD(HKEY_LOCAL_MACHINE, p, "Affinity",        0);
                RegSZ   (HKEY_LOCAL_MACHINE, p, "Background Only", "True");
                RegDWORD(HKEY_LOCAL_MACHINE, p, "Clock Rate",      10000);
                RegSZ   (HKEY_LOCAL_MACHINE, p, "SFIO Priority",   "Normal");
                return true;
            }
        },

        // ── Memory Manager ──────────────────────────────────────────────────
        {
            "LargeSystemCache = 0 (memory manager favors application working sets)",
            // 0 = workstation mode: OS gives priority to application RAM over disk cache.
            // Default on Win10 but worth enforcing — some tuning guides accidentally set it to 1.
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                "LargeSystemCache", 0); },
            []{ return RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                "LargeSystemCache", 0); }
        },
    };
}

static void ApplyTweaks(bool apply) {
    Section(apply ? "Registry Tweaks" : "Restoring Registry");
    auto tweaks = BuildTweaks();
    for (auto& t : tweaks) {
        bool ok = apply ? t.apply() : t.restore();
        if (ok) OK  (t.label);
        else    FAIL(std::string(t.label) + "  [write failed]");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 6  —  Services
// ─────────────────────────────────────────────────────────────────────────────
struct SvcTweak {
    const char* name;
    const char* label;
    DWORD       offStart;   // disabled state
    DWORD       onStart;    // original/restore state
};

static const SvcTweak g_svcs[] = {
    { "SysMain",           "SysMain (Superfetch preloader)",               4, 2 },
    { "DiagTrack",         "DiagTrack (connected telemetry)",              4, 2 },
    { "XboxGipSvc",        "XboxGipSvc (Game Input Protocol)",             4, 3 },
    { "XblAuthManager",    "XblAuthManager (Xbox Live Auth)",              4, 3 },
    { "XblGameSave",       "XblGameSave (Xbox Live Save sync)",            4, 3 },
    { "lfsvc",             "lfsvc (Geolocation service)",                  4, 3 },
    { "MapsBroker",        "MapsBroker (Downloaded Maps)",                 4, 3 },
    { "RetailDemo",        "RetailDemo (store kiosk service)",             4, 4 },
    { "WerSvc",            "WerSvc — Windows Error Reporting",             4, 3 },
    { "DoSvc",             "DoSvc — Delivery Optimization (P2P updates)",  4, 3 },
    { "CDPSvc",            "CDPSvc — Connected Devices Platform",          4, 3 },
    { "WSearch",           "WSearch — Windows Search indexer",             4, 2 },
    { "PcaSvc",            "PcaSvc — Program Compatibility Assistant",     4, 3 },
    { "SensorService",     "SensorService — Sensor data manager",          4, 3 },
    { "SensorDataService", "SensorDataService — Sensor acquisition",       4, 3 },
    { "SensrSvc",          "SensrSvc — Sensor Monitoring",                 4, 3 },
    { "Fax",               "Fax service",                                  4, 4 },
    { "PrintNotify",       "PrintNotify — Print notification",             4, 3 },
    { "TabletInputService","TabletInputService — Tablet PC Input Service",  4, 3 },
    { "RemoteRegistry",    "RemoteRegistry — remote registry access",       4, 4 },
    { "WMPNetworkSvc",     "WMPNetworkSvc — WMP Network Sharing Service",   4, 3 },
    { "SSDPSRV",           "SSDPSRV — SSDP Discovery (UPnP detection)",     4, 3 },
    { "upnphost",          "upnphost — UPnP Device Host",                   4, 3 },
    { "fdPHost",           "fdPHost — Function Discovery Provider Host",     4, 3 },
    { "FDResPub",          "FDResPub — Function Discovery Resource Pub",     4, 3 },
    { "WbioSrvc",          "WbioSrvc — Windows Biometric Service",          4, 3 },
    { "stisvc",            "stisvc — Windows Image Acquisition (scanner)",   4, 3 },
    { "SharedAccess",      "SharedAccess — Internet Connection Sharing",     4, 4 },
};

static bool SvcSetStart(const char* name, DWORD start) {
    SC_HANDLE hSCM = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;
    SC_HANDLE hSvc = OpenServiceA(hSCM, name, SERVICE_CHANGE_CONFIG | SERVICE_STOP);
    bool ok = false;
    if (hSvc) {
        SERVICE_STATUS ss{};
        ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
        ok = ChangeServiceConfigA(hSvc, SERVICE_NO_CHANGE, start,
            SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr);
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hSCM);
    return ok;
}

static void ApplySvcTweaks(bool apply) {
    Section(apply ? "Services" : "Restoring Services");
    for (auto& s : g_svcs) {
        DWORD target = apply ? s.offStart : s.onStart;
        if (SvcSetStart(s.name, target))
            OK  (std::string(s.label) + (apply ? "  -> disabled" : "  -> restored"));
        else
            SKIP(std::string(s.label) + "  (not found / access denied)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 7  —  Network Adapter  (via PowerShell netadapter cmdlets)
// ─────────────────────────────────────────────────────────────────────────────
static void ApplyNetTweaks(bool apply) {
    Section(apply ? "Network Adapter" : "Restoring Network Adapter");

    struct NicTweak {
        const char* label;
        const char* keyword;
        const char* applyVal;
        const char* restoreVal;
    };

    static const NicTweak nics[] = {
        {
            "Interrupt Moderation OFF — each packet fires an immediate CPU interrupt "
            "instead of batching N packets; eliminates NIC-side receive latency",
            "*InterruptModeration", "0", "1"
        },
        {
            "Energy Efficient Ethernet OFF — prevents NIC from micro-sleeping during "
            "low-traffic gaps; kills the wake latency spike when traffic resumes",
            "*EEE", "0", "1"
        },
        {
            "Flow Control OFF — removes Ethernet pause-frame handshake overhead; "
            "NIC never halts transmit waiting for the remote to drain its buffer",
            "*FlowControl", "0", "3"
        },
        {
            "Receive Segment Coalescing OFF — TCP segments delivered individually "
            "to the stack instead of being merged; trades throughput for lower latency",
            "*RSC", "0", "1"
        },
    };

    for (auto& n : nics) {
        std::string val = apply ? n.applyVal : n.restoreVal;
        std::string ps =
            "Get-NetAdapter -Physical | "
            "Set-NetAdapterAdvancedProperty "
            "-RegistryKeyword '" + std::string(n.keyword) + "' "
            "-RegistryValue " + val;
        int r = RunPS(ps);
        if (r == 0) OK  (std::string(n.label));
        else        SKIP(std::string(n.label) + "  [adapter doesn't support this property]");
    }

    // Power Management: stop Windows from sleeping the NIC
    if (apply) {
        std::string ps =
            "Get-NetAdapter -Physical | ForEach-Object {"
            "  $path = \"HKLM:\\SYSTEM\\CurrentControlSet\\Enum\\\" + $_.PnPDeviceID;"
            "  if (Test-Path $path) {"
            "    Set-ItemProperty -Path $path "
            "      -Name 'WakeOnMagicPacketOnly' -Value 0 -ErrorAction SilentlyContinue"
            "  }"
            "}";
        RunPS(ps);
        OK("NIC power wake flags cleared");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 7b  —  Network Stack  (TCP global settings + IPv6)
// ─────────────────────────────────────────────────────────────────────────────
static void ApplyNetworkStack(bool apply) {
    Section(apply ? "Network Stack" : "Restoring Network Stack");

    if (apply) {
        // 1. TCP AutoTuning — disabled reduces receive buffer bloat, lowers latency
        if (RunCmd("netsh int tcp set global autotuninglevel=disabled") == 0)
            OK("TCP AutoTuning: disabled");
        else
            WARN("TCP AutoTuning: command failed (may need elevated netsh)");

        // 2. RSS — Receive Side Scaling: distribute NIC interrupts across cores
        if (RunCmd("netsh int tcp set global rss=enabled") == 0)
            OK("RSS: enabled");
        else
            WARN("RSS: command failed");

        // 3. Chimney offload — disabled reduces CPU/NIC sync overhead
        if (RunCmd("netsh int tcp set global chimney=disabled") == 0)
            OK("Chimney offload: disabled (reduces CPU/NIC sync overhead)");
        else
            SKIP("Chimney offload: not supported on this adapter");

        // 4. ECN — Explicit Congestion Notification: off avoids router compatibility issues
        if (RunCmd("netsh int tcp set global ecncapability=disabled") == 0)
            OK("ECN: disabled");
        else
            WARN("ECN: command failed");

        // 5. TCP Timestamps — disabled reduces per-packet overhead
        if (RunCmd("netsh int tcp set global timestamps=disabled") == 0)
            OK("TCP timestamps: disabled");
        else
            WARN("TCP timestamps: command failed");

        // 6. IPv6 deprioritization — prefer IPv4 without fully disabling IPv6
        // 0x20 = prefer IPv4 over IPv6 in prefix policies
        if (RegDWORD(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters",
                "DisabledComponents", 0x20))
            OK("IPv6: deprioritized (IPv4 preferred)");
        else
            WARN("IPv6 deprioritize: registry write failed");

        // 7. TCP heuristics: disabled — removes dynamic receive-window tuning
        // which can cause erratic buffer growth under latency-sensitive conditions.
        if (RunCmd("netsh interface tcp set heuristics disabled") == 0)
            OK("TCP heuristics: disabled");
        else
            SKIP("TCP heuristics: command not supported on this build");

        // 8. CTCP (Compound TCP): higher throughput with better latency under load.
        // Uses additive-increase/multiplicative-decrease with a delay component.
        // Only available on Win10 certain builds; silently no-ops if unsupported.
        if (RunCmd("netsh int tcp set supplemental internet congestionprovider=ctcp") == 0)
            OK("TCP: CTCP congestion provider active");
        else
            SKIP("TCP CTCP: not supported on this build (normal)");

        // 9. InitialRto = 2000ms — halves the default initial retransmit timeout
        // so TCP connection failures surface and retry faster.
        if (RunCmd("netsh int tcp set global initialRto=2000") == 0)
            OK("TCP InitialRto: 2000ms (faster connection failure detection)");
        else
            SKIP("TCP InitialRto: not configurable");

        // 10. MinRto = 300ms — reduces minimum retransmit timeout for low-latency LANs
        if (RunCmd("netsh int tcp set global minRto=300") == 0)
            OK("TCP MinRto: 300ms");
        else
            SKIP("TCP MinRto: not configurable");

    } else {
        // Restore defaults
        if (RunCmd("netsh int tcp set global autotuninglevel=normal") == 0)
            OK("TCP AutoTuning: restored to normal");
        else
            WARN("TCP AutoTuning restore: command failed");

        if (RunCmd("netsh int tcp set global rss=enabled") == 0)
            OK("RSS: enabled (default)");
        else
            WARN("RSS restore: command failed");

        if (RunCmd("netsh int tcp set global chimney=enabled") == 0)
            OK("Chimney offload: restored");
        else
            SKIP("Chimney restore: not supported");

        if (RunCmd("netsh int tcp set global ecncapability=enabled") == 0)
            OK("ECN: restored");
        else
            WARN("ECN restore: command failed");

        if (RunCmd("netsh int tcp set global timestamps=disabled") == 0)
            OK("TCP timestamps: restored (disabled is default)");
        else
            WARN("TCP timestamps restore: command failed");

        if (RunCmd("netsh interface tcp set heuristics enabled") == 0)
            OK("TCP heuristics: restored");
        else
            SKIP("TCP heuristics restore: command not supported");

        // Restore IPv6 components to default (0 = all components enabled)
        if (RegDel(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters",
                "DisabledComponents"))
            OK("IPv6: restored to default");
        else
            SKIP("IPv6 restore: value not present");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 8  —  Power Plan
// ─────────────────────────────────────────────────────────────────────────────
static GUID g_prevPlan{};
static bool g_prevPlanSaved = false;

static void ApplyPower(bool apply) {
    Section(apply ? "Power Plan" : "Restoring Power Plan");

    if (apply) {
        // Save current plan so restore() can bring it back
        GUID* cur = nullptr;
        if (PowerGetActiveScheme(nullptr, &cur) == ERROR_SUCCESS && cur) {
            g_prevPlan = *cur;
            g_prevPlanSaved = true;
            LocalFree(cur);
        }

        // Try activating Ultimate Performance directly
        if (PowerSetActiveScheme(nullptr, &GUID_ULTIMATE) == ERROR_SUCCESS) {
            OK("Power plan: Ultimate Performance activated");
        } else {
            // Not present yet — duplicate it
            RunCmd("powercfg /duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61 >nul 2>&1");
            if (PowerSetActiveScheme(nullptr, &GUID_ULTIMATE) == ERROR_SUCCESS)
                OK("Power plan: Ultimate Performance created and activated");
            else
                WARN("Power plan: Ultimate Performance unavailable, current plan unchanged");
        }

        // Disable USB selective suspend (kills controller power gating latency)
        RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
               "2a737441-1930-4402-8d77-b2bebba308a3 "
               "48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0");
        RunCmd("powercfg /setactive SCHEME_CURRENT");
        OK("USB selective suspend: disabled");

    } else {
        if (g_prevPlanSaved)
            PowerSetActiveScheme(nullptr, &g_prevPlan);
        else
            RunCmd("powercfg /setactive 381b4222-f694-41f0-9685-ff5bb260df2e"); // Balanced
        OK("Power plan: restored to Balanced");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 9  —  CPU Power Tweaks
// ─────────────────────────────────────────────────────────────────────────────
// Core parking, idle states, boost mode — all via powercfg on SCHEME_CURRENT.
// Applied after power plan switch so they land on Ultimate Performance.
static void ApplyCpuPower(bool apply) {
    Section(apply ? "CPU Power" : "Restoring CPU Power");

    if (apply) {
        // ── Core Parking: min cores = 100% (all cores always active) ───────
        // Prevents cores from parking when load spikes — kills the stutter
        // that happens when a parked core has to spin back up mid-frame.
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "54533251-82be-4824-96c1-47b60b740d00 "
                   "0cc5b647-c1df-4637-891a-dec35c318583 100") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("Core parking: disabled (CPMINCORES = 100%)");
        else
            FAIL("Core parking tweak failed");

        // ── CPU Idle Disable ──────────────────────────────────────────────
        // IDLEDISABLE = 1: CPU spins instead of entering C-states when idle.
        // Eliminates C-state exit latency. Uses more power. Desktop only.
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "54533251-82be-4824-96c1-47b60b740d00 "
                   "5d76a2ca-e8c0-402f-a133-2158492d58ad 1") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("CPU idle states: disabled (no C-state transitions)");
        else
            WARN("CPU idle disable: not available on this hardware/plan");

        // ── Processor Performance Boost Mode = Aggressive ─────────────────
        // PERFBOOSTMODE = 3 (Aggressive): CPU boosts immediately without
        // waiting for a utilization threshold.
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "54533251-82be-4824-96c1-47b60b740d00 "
                   "be337238-0d82-4146-a960-4f3749d470c7 3") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("CPU boost mode: Aggressive (instant boost, no ramp-up delay)");
        else
            WARN("CPU boost mode: not configurable on this plan");

        // ── Processor Idle Check: minimum interval ─────────────────────────
        // IDLECHECK = 0ms: OS checks idle state as frequently as possible
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "54533251-82be-4824-96c1-47b60b740d00 "
                   "7b224883-b3cc-4d79-819f-8374152cbe7c 0") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("CPU idle check interval: 0ms (most responsive)");
        else
            SKIP("CPU idle check interval: not configurable");

        // ── Processor Frequency: prevent throttling below max ──────────────
        // PROCTHROTTLEMIN = 100%: CPU never drops below max frequency
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "54533251-82be-4824-96c1-47b60b740d00 "
                   "893dee8e-2bef-41e0-89c6-b55d0929964c 100") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("Processor min frequency: 100% (no downclocking)");
        else
            SKIP("Processor min frequency: not configurable");

        // ── PCI Express Link State Power Management: Off ───────────────────
        // Prevents GPU and NIC from entering PCIe L0s/L1 link power states
        // which incur a wake latency penalty on every access burst.
        // Sub GUID: 19caa947-263e-4b40-8c56-a74db7c38f14 = PCI Express
        // Setting:  ee12f906-d277-404b-b6da-e5fa1a576df5 = ASPM policy
        // Value 0 = Link Power Management: Off
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "19caa947-263e-4b40-8c56-a74db7c38f14 "
                   "ee12f906-d277-404b-b6da-e5fa1a576df5 0") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("PCIe LSPM: Off (no link power-state latency on GPU/NIC)");
        else
            SKIP("PCIe LSPM: not configurable on this plan");

        // ── Adaptive Control: Disable processor performance adaptive algorithm ──
        // PERFAUTONOMOUS = 0: use fixed perf policy, no autonomous frequency scaling
        if (RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
                   "54533251-82be-4824-96c1-47b60b740d00 "
                   "8baa4a8a-14c6-4451-8e8b-14bdbd197537 0") == 0 &&
            RunCmd("powercfg /setactive SCHEME_CURRENT") == 0)
            OK("CPU adaptive performance: Off (fixed perf policy)");
        else
            SKIP("CPU adaptive performance: not configurable");

    } else {
        // Restore defaults
        RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
               "54533251-82be-4824-96c1-47b60b740d00 "
               "0cc5b647-c1df-4637-891a-dec35c318583 0");
        OK("Core parking: restored to default");

        RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
               "54533251-82be-4824-96c1-47b60b740d00 "
               "5d76a2ca-e8c0-402f-a133-2158492d58ad 0");
        OK("CPU idle states: restored");

        RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
               "54533251-82be-4824-96c1-47b60b740d00 "
               "be337238-0d82-4146-a960-4f3749d470c7 2");
        OK("CPU boost mode: restored to Efficient Aggressive");

        RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
               "54533251-82be-4824-96c1-47b60b740d00 "
               "893dee8e-2bef-41e0-89c6-b55d0929964c 5");
        OK("Processor min frequency: restored to 5%");

        // Restore PCIe LSPM to Moderate (default Windows value)
        RunCmd("powercfg /setacvalueindex SCHEME_CURRENT "
               "19caa947-263e-4b40-8c56-a74db7c38f14 "
               "ee12f906-d277-404b-b6da-e5fa1a576df5 1");
        OK("PCIe LSPM: restored to Moderate");

        RunCmd("powercfg /setactive SCHEME_CURRENT");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 10  —  MSI Mode  (Message Signaled Interrupts)
// ─────────────────────────────────────────────────────────────────────────────
// Sets MSISupported=1 in registry for GPU and NIC PCI devices.
// Requires restart to take effect.
static void ApplyMSI(bool apply) {
    Section(apply ? "MSI Mode (GPU + NIC)" : "Restoring MSI Mode");

    INFO("MSI mode routes device interrupts through the CPU Local APIC instead of");
    INFO("sharing legacy IRQ lines — eliminates interrupt contention latency on GPU/NIC.");

    const char* base = "SYSTEM\\CurrentControlSet\\Enum\\PCI";
    HKEY hBase;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base, 0,
        KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hBase) != ERROR_SUCCESS) {
        FAIL("Cannot open PCI registry key (need Admin)");
        return;
    }

    int hits = 0;
    char devId[256];
    DWORD di = 0, dl = sizeof(devId);
    while (RegEnumKeyExA(hBase, di++, devId, &dl,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {

        std::string devPath = std::string(base) + "\\" + devId;
        HKEY hDev;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, devPath.c_str(), 0,
            KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hDev) == ERROR_SUCCESS) {

            char instId[256];
            DWORD ii = 0, il = sizeof(instId);
            while (RegEnumKeyExA(hDev, ii++, instId, &il,
                nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {

                std::string instPath = devPath + "\\" + instId;
                char cls[64] = {};
                DWORD cl = sizeof(cls);
                HKEY hInst;

                bool isGPU = false, isNIC = false;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, instPath.c_str(), 0,
                    KEY_READ, &hInst) == ERROR_SUCCESS) {
                    RegQueryValueExA(hInst, "Class", nullptr, nullptr,
                        (LPBYTE)cls, &cl);
                    if (_stricmp(cls, "Display") == 0) isGPU = true;
                    if (_stricmp(cls, "Net")     == 0) isNIC = true;
                    RegCloseKey(hInst);
                }

                if (isGPU || isNIC) {
                    std::string msiPath = instPath +
                        "\\Device Parameters\\Interrupt Management"
                        "\\MessageSignaledInterruptProperties";
                    DWORD msiVal = apply ? 1 : 0;
                    if (RegDWORD(HKEY_LOCAL_MACHINE, msiPath.c_str(), "MSISupported", msiVal)) {
                        std::string tag = isGPU ? "[GPU] " : "[NIC] ";
                        OK(tag + std::string(instId) +
                            (apply ? "  MSISupported=1" : "  MSISupported=0 (reverted)"));
                        hits++;

                        // GPU: cap message count to 1 for cleaner interrupt routing
                        if (isGPU && apply)
                            RegDWORD(HKEY_LOCAL_MACHINE, msiPath.c_str(), "MessageNumberLimit", 1);
                    }
                }
                il = sizeof(instId);
            }
            RegCloseKey(hDev);
        }
        dl = sizeof(devId);
    }
    RegCloseKey(hBase);

    if (hits == 0)
        WARN("No Display/Net PCI devices found — check Admin permissions.");
    else
        WARN("MSI changes take effect after RESTART.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 11  —  GPU Tweaks  (NVIDIA PowerMizer + HAGS verification)
// ─────────────────────────────────────────────────────────────────────────────
// NVIDIA PowerMizer: prevents GPU from downclocking when load drops momentarily.
// Particularly noticeable in competitive games where GPU load fluctuates rapidly.
static void ApplyGpuTweaks(bool apply) {
    Section(apply ? "GPU Tweaks (NVIDIA PowerMizer)" : "Restoring GPU Tweaks");

    // Find NVIDIA display adapter keys under
    // HKLM\SYSTEM\CurrentControlSet\Control\Video\{GUID}\0000
    const char* videoBase = "SYSTEM\\CurrentControlSet\\Control\\Video";
    HKEY hBase;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, videoBase, 0,
        KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hBase) != ERROR_SUCCESS) {
        SKIP("GPU: cannot open Video registry key");
        return;
    }

    char guid[256];
    DWORD gi = 0, gl = sizeof(guid);
    int hits = 0;

    while (RegEnumKeyExA(hBase, gi++, guid, &gl,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {

        // Open the \0000 subkey (first adapter instance)
        std::string inst = std::string(videoBase) + "\\" + guid + "\\0000";
        HKEY hInst;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, inst.c_str(), 0,
            KEY_READ | KEY_SET_VALUE, &hInst) == ERROR_SUCCESS) {

            // Check if it's an NVIDIA adapter by reading DriverDesc
            char desc[128] = {};
            DWORD dl = sizeof(desc);
            RegQueryValueExA(hInst, "DriverDesc", nullptr, nullptr, (LPBYTE)desc, &dl);
            RegCloseKey(hInst);

            bool isNvidia = (strstr(desc, "NVIDIA") != nullptr ||
                             strstr(desc, "GeForce") != nullptr);

            if (_stricmp(desc, "") != 0 && isNvidia) {
                if (apply) {
                    // PerfLevelSrc = 0x2222: use fixed perf level for both battery and AC
                    RegDWORD(HKEY_LOCAL_MACHINE, inst.c_str(), "PerfLevelSrc",     0x2222);
                    // PowerMizerEnable = 1: enable the PowerMizer override
                    RegDWORD(HKEY_LOCAL_MACHINE, inst.c_str(), "PowerMizerEnable", 1);
                    // PowerMizerLevel = 1: highest performance level (no power saving)
                    RegDWORD(HKEY_LOCAL_MACHINE, inst.c_str(), "PowerMizerLevel",  1);
                    // PowerMizerLevelAC = 1: same on AC power
                    RegDWORD(HKEY_LOCAL_MACHINE, inst.c_str(), "PowerMizerLevelAC",1);
                    OK(std::string("[NVIDIA] ") + desc + "  -> PowerMizer locked to max perf");
                } else {
                    RegDel(HKEY_LOCAL_MACHINE, inst.c_str(), "PerfLevelSrc");
                    RegDel(HKEY_LOCAL_MACHINE, inst.c_str(), "PowerMizerEnable");
                    RegDel(HKEY_LOCAL_MACHINE, inst.c_str(), "PowerMizerLevel");
                    RegDel(HKEY_LOCAL_MACHINE, inst.c_str(), "PowerMizerLevelAC");
                    OK(std::string("[NVIDIA] ") + desc + "  -> PowerMizer restored to driver default");
                }
                hits++;
            }

        }
        gl = sizeof(guid);
    }
    RegCloseKey(hBase);

    if (hits == 0)
        INFO("No NVIDIA GPU found (Intel iGPU — PowerMizer not applicable)");

    // HAGS status note (registry key was already set in BuildTweaks)
    DWORD hags = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", 0);
    if (hags == 2) INFO("HAGS: HwSchMode=2 (enabled) — restart required");
    else           INFO("HAGS: HwSchMode=" + std::to_string(hags));
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 12  —  Visual Effects
// ─────────────────────────────────────────────────────────────────────────────
// Kills all animations and transparency effects that add DWM rendering overhead.
static void ApplyVisualEffects(bool apply) {
    Section(apply ? "Visual Effects" : "Restoring Visual Effects");

    if (apply) {
        // VisualFXSetting = 2: Custom — lets us control individual settings
        // (0=best appearance, 1=best performance, 2=custom, 3=let Windows decide)
        if (RegDWORD(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
                "VisualFXSetting", 2))
            OK("VisualFXSetting = 2 (custom — full animation control)");
        else
            FAIL("VisualFXSetting write failed");

        // No full-window drag — show outline only while dragging
        if (RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "DragFullWindows", "0"))
            OK("DragFullWindows = 0 (outline drag only)");
        else
            FAIL("DragFullWindows write failed");

        // Zero menu show delay — menus appear instantly
        if (RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "MenuShowDelay", "0"))
            OK("MenuShowDelay = 0 (instant menu)");
        else
            FAIL("MenuShowDelay write failed");

        // Disable window minimize/maximize animations
        if (RegSZ(HKEY_CURRENT_USER,
                "Control Panel\\Desktop\\WindowMetrics", "MinAnimate", "0"))
            OK("MinAnimate = 0 (no minimize/maximize animation)");
        else
            FAIL("MinAnimate write failed");

        // Disable taskbar animations
        if (RegDWORD(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                "TaskbarAnimations", 0))
            OK("TaskbarAnimations = 0 (no taskbar animation)");
        else
            FAIL("TaskbarAnimations write failed");

        // Disable listview drop shadow
        if (RegDWORD(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                "ListviewShadow", 0))
            OK("ListviewShadow = 0 (no desktop icon shadow)");
        else
            FAIL("ListviewShadow write failed");

        // Disable listview alpha selection highlight
        if (RegDWORD(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                "ListviewAlphaSelect", 0))
            OK("ListviewAlphaSelect = 0 (no translucent selection)");
        else
            FAIL("ListviewAlphaSelect write failed");

    } else {
        // Restore Windows defaults
        if (RegDWORD(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
                "VisualFXSetting", 0))
            OK("VisualFXSetting = 0 (best appearance, default)");

        if (RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "DragFullWindows", "1"))
            OK("DragFullWindows = 1 (restored)");

        if (RegSZ(HKEY_CURRENT_USER, "Control Panel\\Desktop", "MenuShowDelay", "400"))
            OK("MenuShowDelay = 400 (restored default)");

        if (RegSZ(HKEY_CURRENT_USER,
                "Control Panel\\Desktop\\WindowMetrics", "MinAnimate", "1"))
            OK("MinAnimate = 1 (animations restored)");

        if (RegDWORD(HKEY_CURRENT_USER,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                "TaskbarAnimations", 1))
            OK("TaskbarAnimations = 1 (restored)");

        // ListviewShadow and ListviewAlphaSelect not strictly needed to restore
        // but do it for completeness
        RegDWORD(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            "ListviewShadow", 1);
        RegDWORD(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            "ListviewAlphaSelect", 1);
        OK("Listview shadow + alpha select: restored");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 13  —  Boot Tweaks  (bcdedit)
// ─────────────────────────────────────────────────────────────────────────────
static void ApplyBoot(bool apply) {
    Section(apply ? "Boot Tweaks (bcdedit)" : "Restoring Boot Tweaks");
    WARN("These require a restart to take effect.");

    if (apply) {
        // disabledynamictick: prevents idle timer coalescing, keeps tick rate steady
        if (RunCmd("bcdedit /set disabledynamictick yes") == 0)
            OK("disabledynamictick = yes");
        else
            FAIL("disabledynamictick failed (check Admin)");

        // useplatformclock false: use TSC instead of HPET (lower latency on modern hardware)
        if (RunCmd("bcdedit /set useplatformclock false") == 0)
            OK("useplatformclock = false  (TSC over HPET)");
        else
            FAIL("useplatformclock failed");

        // tscsyncpolicy Enhanced: forces enhanced TSC synchronization across CPU cores.
        // Prevents per-core timer skew which causes DPC latency spikes on multi-core CPUs.
        // Options: Default | Legacy | Enhanced. Enhanced = best on Ryzen/modern Intel.
        if (RunCmd("bcdedit /set tscsyncpolicy Enhanced") == 0)
            OK("tscsyncpolicy = Enhanced  (better multicore TSC sync, reduces DPC jitter)");
        else
            WARN("tscsyncpolicy: not supported on this firmware/platform");

        // quietboot: suppress boot progress UI — minor but removes DWM animation overhead
        // during session startup and keeps kernel boot path leaner.
        if (RunCmd("bcdedit /set quietboot yes") == 0)
            OK("quietboot = yes  (no boot animation)");
        else
            SKIP("quietboot: no effect on UEFI systems with firmware splash");

        // useplatformtick: delete if present — force TSC, not platform tick source
        RunCmd("bcdedit /deletevalue useplatformtick");

    } else {
        RunCmd("bcdedit /deletevalue disabledynamictick");
        OK("disabledynamictick removed");
        RunCmd("bcdedit /deletevalue useplatformclock");
        OK("useplatformclock removed");
        RunCmd("bcdedit /deletevalue tscsyncpolicy");
        OK("tscsyncpolicy removed");
        RunCmd("bcdedit /deletevalue quietboot");
        OK("quietboot removed");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 14  —  VBS / HVCI disable
// ─────────────────────────────────────────────────────────────────────────────
// 5–15% performance impact on older Intel/Ryzen (~2019 and earlier).
// Security trade-off: disables kernel code integrity verification.
// Requires restart.
static void ApplyVBS(bool apply) {
    Section(apply ? "VBS/HVCI (Virtualization-Based Security) OFF" : "Restoring VBS/HVCI");
    WARN("Security trade-off! VBS protects against kernel exploits.");

    if (apply) {
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            "EnableVirtualizationBasedSecurity", 0);
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            "HypervisorEnforcedCodeIntegrity", 0);
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            "Enabled", 0);
        RunCmd("bcdedit /set hypervisorlaunchtype off");
        OK("VBS disabled. Performance gain: 5-15% on pre-2020 CPUs.");
        WARN("RESTART REQUIRED.");
    } else {
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            "EnableVirtualizationBasedSecurity", 1);
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            "HypervisorEnforcedCodeIntegrity", 1);
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            "Enabled", 1);
        RunCmd("bcdedit /set hypervisorlaunchtype auto");
        OK("VBS restored.");
        WARN("RESTART REQUIRED.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 15  —  Spectre/Meltdown mitigations
// ─────────────────────────────────────────────────────────────────────────────
// Real performance gain on older Intel CPUs (4–10% syscall overhead reduction).
// Security trade-off: leaves CPU vulnerable to speculative execution attacks.
// Only meaningful on older hardware. Modern CPUs have silicon-level mitigations.
static void ApplySpectre(bool disable) {
    Section(disable ? "Spectre/Meltdown Mitigations OFF" : "Restoring Spectre Mitigations");
    WARN("Security trade-off! Only use on air-gapped / gaming-only machines.");

    if (disable) {
        // FeatureSettingsOverride = 3: disable Spectre variant 2 + Meltdown
        // FeatureSettingsOverrideMask = 3: apply to both variants
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            "FeatureSettingsOverride", 3);
        RegDWORD(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            "FeatureSettingsOverrideMask", 3);
        OK("Spectre/Meltdown mitigations disabled.");
        WARN("RESTART REQUIRED.");
    } else {
        RegDel(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            "FeatureSettingsOverride");
        RegDel(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            "FeatureSettingsOverrideMask");
        OK("Spectre/Meltdown mitigations restored to Windows defaults.");
        WARN("RESTART REQUIRED.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 16  —  Status
// ─────────────────────────────────────────────────────────────────────────────
static void ShowStatus() {
    Section("System Latency Status");

    // Timer
    auto ts = QueryTimerState();
    std::string cs = std::to_string(ts.curRes / 10) + " us";
    if (ts.curRes <= 5000) OK  ("Timer Resolution    : " + cs + "  [optimized]");
    else                   WARN("Timer Resolution    : " + cs + "  [default ~15600us]");
    INFO("Timer Daemon        : " + std::string(IsDaemonRunning() ? "RUNNING" : "stopped"));

    DWORD build = GetWinBuild();
    INFO("Windows Build       : " + std::to_string(build) +
        (build >= 19041 ? "  (per-process timer — daemon required)" :
                          "  (global timer — daemon optional)"));

    // MMCSS
    DWORD sr = RegRead(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        "SystemResponsiveness", 20);
    if (sr == 0) OK  ("MMCSS SysResp       : 0  [optimized]");
    else         WARN("MMCSS SysResp       : " + std::to_string(sr) + "  [default 20]");

    DWORD nlm = RegRead(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        "NoLazyMode", 0);
    if (nlm == 1) OK  ("MMCSS NoLazyMode    : 1  [optimized]");
    else          WARN("MMCSS NoLazyMode    : 0  [default]");

    // GameDVR
    DWORD dvr = RegRead(HKEY_CURRENT_USER, "System\\GameConfigStore", "GameDVR_Enabled", 1);
    if (dvr == 0) OK  ("Game DVR            : disabled");
    else          WARN("Game DVR            : enabled  [overhead]");

    // Mouse accel
    std::string mspd = RegReadSZ(HKEY_CURRENT_USER, "Control Panel\\Mouse", "MouseSpeed", "1");
    if (mspd == "0") OK  ("Mouse Accel         : disabled  [raw input]");
    else             WARN("Mouse Accel         : enabled");

    // Fast Startup
    DWORD fb = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        "HiberbootEnabled", 1);
    if (fb == 0) OK  ("Fast Startup        : disabled");
    else         WARN("Fast Startup        : enabled");

    // FTH
    DWORD fth = RegRead(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\FTH", "Enabled", 1);
    if (fth == 0) OK  ("Fault Tol. Heap     : disabled");
    else          WARN("Fault Tol. Heap     : enabled  [adds jitter]");

    // Power plan
    GUID* activePlan = nullptr;
    if (PowerGetActiveScheme(nullptr, &activePlan) == ERROR_SUCCESS && activePlan) {
        bool isUlt = (memcmp(activePlan, &GUID_ULTIMATE, sizeof(GUID)) == 0);
        if (isUlt) OK  ("Power Plan          : Ultimate Performance");
        else       WARN("Power Plan          : " + GuidStr(*activePlan));
        LocalFree(activePlan);
    }

    // Scheduler
    DWORD w32ps = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
        "Win32PrioritySeparation", 2);
    if (w32ps == 38) OK  ("Win32PrioritySep    : 38 (short quanta, high FG boost)");
    else             WARN("Win32PrioritySep    : " + std::to_string(w32ps) + "  [default 2]");

    DWORD dpe = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
        "DisablePagingExecutive", 0);
    if (dpe == 1) OK  ("PagingExecutive     : disabled (kernel in RAM)");
    else          WARN("PagingExecutive     : enabled (default)");

    // Network throttling
    DWORD nti = RegRead(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
        "NetworkThrottlingIndex", 10);
    if (nti == 0xFFFFFFFF) OK  ("Net Throttle        : disabled (0xFFFFFFFF)");
    else                   WARN("Net Throttle        : " + std::to_string(nti) + "  [default 10]");

    // HAGS
    DWORD hags = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", 1);
    if (hags == 2) OK  ("HAGS                : enabled (HwSchMode=2)");
    else           WARN("HAGS                : off/default (HwSchMode=" + std::to_string(hags) + ")");

    // VBS
    DWORD vbs = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
        "EnableVirtualizationBasedSecurity", 1);
    if (vbs == 0) OK  ("VBS/HVCI            : disabled (perf gain active)");
    else          INFO("VBS/HVCI            : enabled (disabled by default in apply)");

    // Spectre
    DWORD spec = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
        "FeatureSettingsOverride", 0xDEAD);
    if (spec == 3)         OK  ("Spectre/Meltdown    : mitigations OFF");
    else if (spec == 0xDEAD) INFO("Spectre/Meltdown    : default (Windows managed)");
    else                   INFO("Spectre/Meltdown    : custom value=" + std::to_string(spec));

    // ── New status checks ────────────────────────────────────────────────────

    // Timer Coalescing
    DWORD cti = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        "CoalescingTimerInterval", 0xDEAD);
    if (cti == 0)          OK  ("Timer Coalescing    : disabled (CoalescingTimerInterval=0)");
    else if (cti == 0xDEAD) WARN("Timer Coalescing    : default (value not set)");
    else                   WARN("Timer Coalescing    : " + std::to_string(cti) + " (non-zero, coalescing active)");

    // SvcHostSplitThresholdInKB
    DWORD svcThresh = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control",
        "SvcHostSplitThresholdInKB", 0);
    if (svcThresh > 0)  OK  ("SvcHostSplitKB      : " + std::to_string(svcThresh) + " KB (optimized — grouped svchosts)");
    else                WARN("SvcHostSplitKB      : not set (default — many svchost processes)");

    // VisualFXSetting
    DWORD vfx = RegRead(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
        "VisualFXSetting", 0xDEAD);
    if (vfx == 2)          OK  ("VisualFXSetting     : 2 (custom/optimized — animations off)");
    else if (vfx == 0xDEAD) WARN("VisualFXSetting     : not set (default appearance)");
    else                   WARN("VisualFXSetting     : " + std::to_string(vfx) + " (not optimized)");

    // TCP TcpAckFrequency (proxy for whether TCP tweaks were applied)
    // Check the first interface that has the value set
    {
        bool tcpTweaked = false;
        HKEY hBase;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces",
                0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &hBase) == ERROR_SUCCESS) {
            char sub[256]; DWORD idx = 0, len = sizeof(sub);
            while (RegEnumKeyExA(hBase, idx++, sub, &len,
                    nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                std::string p = std::string(
                    "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + sub;
                DWORD ackFreq = RegRead(HKEY_LOCAL_MACHINE, p.c_str(), "TcpAckFrequency", 0xDEAD);
                if (ackFreq != 0xDEAD) { tcpTweaked = true; break; }
                len = sizeof(sub);
            }
            RegCloseKey(hBase);
        }
        if (tcpTweaked) OK  ("TCP (Nagle/AckFreq) : tweaked (TcpAckFrequency set)");
        else            WARN("TCP (Nagle/AckFreq) : not tweaked (default)");
    }

    // GlobalTimerResolutionRequests
    DWORD gtrr = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel",
        "GlobalTimerResolutionRequests", 0xDEAD);
    if (gtrr == 1)          OK  ("GlobalTimerReq      : 1 (any process raises global timer)");
    else if (gtrr == 0xDEAD) WARN("GlobalTimerReq      : not set (default per-process only)");
    else                    WARN("GlobalTimerReq      : " + std::to_string(gtrr));

    // NTFS Last Access Update
    DWORD lau = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
        "NtfsDisableLastAccessUpdate", 0);
    if (lau == 0x80000001 || lau == 1) OK  ("NTFS LastAccess     : disabled (no extra disk writes)");
    else                               WARN("NTFS LastAccess     : enabled (default — extra I/O)");

    // Mouse queue size
    DWORD mqsz = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Services\\mouclass\\Parameters",
        "MouseDataQueueSize", 100);
    if (mqsz <= 16) OK  ("Mouse Queue         : " + std::to_string(mqsz) + " (tight — optimized)");
    else            WARN("Mouse Queue         : " + std::to_string(mqsz) + " (default 100)");

    // GPU TDR delay
    DWORD tdr = RegRead(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "TdrDelay", 2);
    if (tdr >= 30) OK  ("GPU TdrDelay        : " + std::to_string(tdr) + "s (extended — no spurious resets)");
    else           WARN("GPU TdrDelay        : " + std::to_string(tdr) + "s (default 2s — may TDR on heavy load)");

    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 17  —  Help
// ─────────────────────────────────────────────────────────────────────────────
static void ShowHelp() {
    std::cout << "\n";
    Print("  Usage: ", C_DARK); Println("latency.exe <command> [flags]", C_WHITE);
    std::cout << "\n";
    Print("  Commands:\n", C_CYAN);
    Println("    apply              Apply ALL tweaks — all 13 modules run by default");
    Println("    restore            Undo all tweaks, restore Windows defaults");
    Println("    status             Full system latency state");
    Println("    timer start        Start 0.5ms timer daemon (background)");
    Println("    timer stop         Kill timer daemon");
    Println("    timer status       Show timer resolution info");
    Println("    help               This screen");
    std::cout << "\n";
    Print("  Single-module flags (run only that module):\n", C_CYAN);
    Println("    apply --boot       Only run bcdedit boot tweaks     (restart req.)");
    Println("    apply --vbs        Only disable VBS/HVCI            (restart req., security tradeoff)");
    Println("    apply --spectre    Only disable Spectre/Meltdown    (restart req., security tradeoff)");
    Println("    restore --boot     Only undo boot tweaks            (restart req.)");
    Println("    restore --vbs      Only re-enable VBS/HVCI          (restart req.)");
    Println("    restore --spectre  Only re-enable Spectre           (restart req.)");
    std::cout << "\n";
    Print("  Modules in 'apply' (all run by default):\n", C_CYAN);
    Println("    [1]  Registry      MMCSS (full task config), GameDVR, mouse queue,");
    Println("                       Win32PrioritySep, DisablePagingExecutive,");
    Println("                       NetworkThrottlingIndex, HAGS, TCP NoDelay, FTH,");
    Println("                       telemetry, CoalescingTimerInterval, SvcHostSplit,");
    Println("                       Psched, crash dump, kill timeouts, WER, NTFS (3),");
    Println("                       GlobalTimerResolutionRequests, mouse/kbd queues,");
    Println("                       IrpStackSize, TdrDelay, LargeSystemCache");
    Println("    [2]  Services      SysMain, DiagTrack, Xbox (3), WSearch, lfsvc,");
    Println("                       WerSvc, DoSvc, CDPSvc, PcaSvc, Sensors (3), Fax,");
    Println("                       PrintNotify, TabletInput, RemoteRegistry, WMP,");
    Println("                       SSDP, UPnP, FDResPub, fdPHost, WbioSrvc, STI,");
    Println("                       SharedAccess  (30 services total)");
    Println("    [3]  Network       Interrupt Moderation, EEE, Flow Control, RSC");
    Println("    [4]  Net Stack     TCP autotuning, RSS, chimney, ECN, timestamps,");
    Println("                       heuristics, CTCP, InitialRto, MinRto, IPv6 deprior.");
    Println("    [5]  Power         Ultimate Performance + USB suspend");
    Println("    [6]  CPU Power     Core parking, idle states, boost, PCIe LSPM off,");
    Println("                       adaptive perf off, idle check interval, min freq");
    Println("    [7]  MSI Mode      GPU + NIC MSI interrupts          (restart req.)");
    Println("    [8]  GPU           NVIDIA PowerMizer locked to max performance level");
    Println("    [9]  Visual FX     Kill animations, menu delay, drag, listview effects");
    Println("    [10] Boot          disabledynamictick, useplatformclock, tscsyncpolicy,");
    Println("                       quietboot                          (restart req.)");
    Println("    [11] VBS/HVCI      Disable virtualization-based security");
    Println("    [12] Spectre       Disable Spectre/Meltdown mitigations");
    Println("    [13] Timer Daemon  0.5ms NtSetTimerResolution daemon");
    std::cout << "\n";
    Print("  Notes:\n", C_YELLOW);
    Println("    Always run from an ELEVATED (Admin) command prompt.");
    Println("    MSI mode, HAGS, boot, VBS, Spectre all need a restart.");
    Println("    VBS and Spectre are security trade-offs — desktop/gaming-only.");
    Println("    Timer daemon is a hidden process; kill with 'timer stop'.");
    Println("    RESTART required after apply for full effect.");
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 18  —  Interactive Menu
// ─────────────────────────────────────────────────────────────────────────────
static void DoApplyAll() {
    ApplyTweaks      (true);
    ApplySvcTweaks   (true);
    ApplyNetTweaks   (true);
    ApplyNetworkStack(true);
    ApplyPower       (true);
    ApplyCpuPower    (true);
    ApplyMSI         (true);
    ApplyGpuTweaks   (true);
    ApplyVisualEffects(true);
    ApplyBoot        (true);
    ApplyVBS         (true);
    ApplySpectre     (true);
    Section("Timer Daemon");
    StartDaemon();
}

static void DoRestoreAll() {
    ApplySpectre     (false);
    ApplyVBS         (false);
    ApplyBoot        (false);
    ApplyVisualEffects(false);
    ApplyGpuTweaks   (false);
    ApplyMSI         (false);
    ApplyCpuPower    (false);
    ApplyPower       (false);
    ApplyNetworkStack(false);
    ApplyNetTweaks   (false);
    ApplySvcTweaks   (false);
    ApplyTweaks      (false);
}

static void WaitKey() {
    std::cout << "\n";
    Print("  Presiona cualquier tecla para volver al menu...\n", C_DARK);
    _getch();
}

static void ShowMenu() {
    system("cls");
    Banner();
    Print("  --- Menu ----------------------------------------------\n\n", C_DARK);

    bool admin = IsElevated();
    std::string adm = admin ? "" : "  [necesita Admin]";

    Print("    [1]  ", C_CYAN); Println("Aplicar TODOS los tweaks" + adm,      admin ? C_WHITE  : C_YELLOW);
    Print("    [2]  ", C_CYAN); Println("Restaurar Windows defaults" + adm,   admin ? C_WHITE  : C_YELLOW);
    Print("    [3]  ", C_CYAN); Println("Status - ver estado actual",           C_WHITE);
    Print("    [4]  ", C_CYAN); Println("Timer: iniciar daemon 0.5ms",          C_WHITE);
    Print("    [5]  ", C_CYAN); Println("Timer: detener daemon",                C_WHITE);
    Print("    [6]  ", C_CYAN); Println("Timer: ver estado",                    C_WHITE);
    Print("    [7]  ", C_CYAN); Println("Boot tweaks solamente" + adm,          admin ? C_WHITE  : C_YELLOW);
    Print("    [8]  ", C_CYAN); Println("Deshabilitar VBS/HVCI" + adm,          admin ? C_WHITE  : C_YELLOW);
    Print("    [9]  ", C_CYAN); Println("Deshabilitar mitigaciones Spectre" + adm, admin ? C_WHITE : C_YELLOW);
    std::cout << "\n";
    Print("    [0]  ", C_DARK); Println("Salir", C_DARK);
    std::cout << "\n";

    if (!admin)
        WARN("No corres como Admin - opciones en amarillo van a fallar.");

    std::cout << "\n";
    Print("  Elige > ", C_CYAN);
}

static void InteractiveMenu() {
    while (true) {
        ShowMenu();

        int c = _getch();
        std::cout << "\n\n";

        if (c == '0' || c == 27 /* ESC */ || c == 'q' || c == 'Q') break;

        bool admin = IsElevated();

        switch (c) {
            case '1':
                if (!admin) { FAIL("Necesita Admin. Cierra y abre como Administrador."); }
                else        { Print("\n  Aplicando todos los tweaks...\n\n", C_YELLOW); DoApplyAll(); WARN("Reinicia para efecto completo."); }
                break;
            case '2':
                if (!admin) { FAIL("Necesita Admin. Cierra y abre como Administrador."); }
                else        { Print("\n  Restaurando Windows defaults...\n\n", C_YELLOW); DoRestoreAll(); }
                break;
            case '3':
                ShowStatus();
                break;
            case '4':
                Section("Timer Daemon");
                StartDaemon();
                break;
            case '5':
                Section("Timer Daemon");
                StopDaemon();
                break;
            case '6':
                CmdTimerStatus();
                break;
            case '7':
                if (!admin) { FAIL("Necesita Admin."); }
                else        { ApplyBoot(true); }
                break;
            case '8':
                if (!admin) { FAIL("Necesita Admin."); }
                else        { ApplyVBS(true); }
                break;
            case '9':
                if (!admin) { FAIL("Necesita Admin."); }
                else        { ApplySpectre(true); }
                break;
            default:
                WARN("Opcion no valida. Elige entre 0-9.");
                break;
        }

        WaitKey();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SECTION 19  —  main()
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {

    // Force UTF-8 output so Spanish text and box chars render correctly
    // in any CMD/PowerShell regardless of the system code page.
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    // Resolve own path and directory
    {
        char buf[MAX_PATH]{};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        g_selfPath = buf;
        g_selfDir  = buf;
        // trim filename
        size_t slash = g_selfDir.find_last_of("\\/");
        if (slash != std::string::npos) g_selfDir.resize(slash);
    }

    // -- Internal timer daemon mode - must check BEFORE banner
    if (argc >= 2 && strcmp(argv[1], DAEMON_FLAG) == 0) {
        RunTimerDaemon();
        return 0;
    }

    Banner();

    if (argc < 2) { InteractiveMenu(); return 0; }

    std::string cmd  = argv[1];

    // Collect all flags from remaining argv
    auto hasFlag = [&](const char* flag) -> bool {
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], flag) == 0) return true;
        return false;
    };

    std::string arg2 = (argc >= 3) ? argv[2] : "";

    // Individual module flags — only run that one module if specified
    bool onlyBoot    = hasFlag("--boot");
    bool onlyVBS     = hasFlag("--vbs");
    bool onlySpectre = hasFlag("--spectre");
    bool singleModule = onlyBoot || onlyVBS || onlySpectre;

    // ── timer ────────────────────────────────────────────────────────────────
    if (cmd == "timer") {
        if      (arg2 == "start")                  StartDaemon();
        else if (arg2 == "stop")                   StopDaemon();
        else if (arg2 == "status" || arg2.empty()) CmdTimerStatus();
        else { FAIL("Unknown timer subcommand: " + arg2); }
        std::cout << "\n";
        return 0;
    }

    // ── status ───────────────────────────────────────────────────────────────
    if (cmd == "status") { ShowStatus(); return 0; }

    // ── help ─────────────────────────────────────────────────────────────────
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        ShowHelp(); return 0;
    }

    // ── apply / restore ──────────────────────────────────────────────────────
    if (cmd == "apply" || cmd == "restore") {
        RequireAdmin();
        bool apply = (cmd == "apply");

        // ── Single module shortcut ────────────────────────────────────────
        if (singleModule) {
            if (apply)
                Print("\n  Applying selected module only...\n", C_YELLOW);
            else
                Print("\n  Restoring selected module only...\n", C_YELLOW);

            if (onlyBoot)    ApplyBoot(apply);
            if (onlyVBS)     ApplyVBS(apply);
            if (onlySpectre) ApplySpectre(apply);

            std::cout << "\n";
            return 0;
        }

        // ── Full apply / restore sequence ─────────────────────────────────
        if (apply)
            Print("\n  Applying all latency optimizations...\n", C_YELLOW);
        else
            Print("\n  Restoring Windows defaults...\n", C_YELLOW);

        if (apply) {
            // Apply sequence:
            ApplyTweaks      (apply);   // [1]  Registry
            ApplySvcTweaks   (apply);   // [2]  Services
            ApplyNetTweaks   (apply);   // [3]  Network adapters
            ApplyNetworkStack(apply);   // [4]  TCP stack (NEW)
            ApplyPower       (apply);   // [5]  Power plan
            ApplyCpuPower    (apply);   // [6]  CPU parking, idle, boost
            ApplyMSI         (apply);   // [7]  MSI mode (restart req.)
            ApplyGpuTweaks   (apply);   // [8]  NVIDIA PowerMizer
            ApplyVisualEffects(apply);  // [9]  Visual effects (NEW)
            ApplyBoot        (apply);   // [10] bcdedit (NOW DEFAULT)
            ApplyVBS         (apply);   // [11] VBS/HVCI (NOW DEFAULT)
            ApplySpectre     (apply);   // [12] Spectre (NOW DEFAULT)
        } else {
            // Restore in reverse order:
            ApplySpectre     (apply);   // [12]
            ApplyVBS         (apply);   // [11]
            ApplyBoot        (apply);   // [10]
            ApplyVisualEffects(apply);  // [9]
            ApplyGpuTweaks   (apply);   // [8]
            ApplyMSI         (apply);   // [7]
            ApplyCpuPower    (apply);   // [6]
            ApplyPower       (apply);   // [5]
            ApplyNetworkStack(apply);   // [4]
            ApplyNetTweaks   (apply);   // [3]
            ApplySvcTweaks   (apply);   // [2]
            ApplyTweaks      (apply);   // [1]
        }

        // ── Timer daemon ─────────────────────────────────────────────────
        Section("Timer Daemon");
        if (apply) StartDaemon();
        else       StopDaemon();

        // ── Summary ───────────────────────────────────────────────────────
        Print("\n  ---------------------------------------------\n", C_DARK);
        if (apply) {
            OK("All tweaks applied.");
            WARN("RESTART required for: MSI mode + HAGS + boot tweaks + VBS + Spectre.");
        } else {
            OK("System restored to Windows defaults.");
            WARN("RESTART required to finalize boot/VBS/Spectre restoration.");
        }
        std::cout << "\n";
        return 0;
    }

    FAIL("Unknown command: " + cmd);
    ShowHelp();
    return 1;
}

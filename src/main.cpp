#include <windows.h>
#include "client.h"
#include "config.h"
#include "log.h"
#ifdef QD_BRIDGE
#include "../dev/bridge.h"
namespace bridge_ops { void Register(); }
#endif

#define QD_VERSION "1.1.1"

bool LoadProxyTargets();

static Config g_config;

static bool GameWindowFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool wasDown = false;
    for (;;) {
        bool down = (GetAsyncKeyState(g_config.hotkey) & 0x8000) != 0;
        if (down && !wasDown && GameWindowFocused())
            client::RequestDeposit(g_config);
        wasDown = down;
        Sleep(20);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    if (!LoadProxyTargets())
        return FALSE;

    // Only act inside the game itself, not tools that happen to load powrprof.dll from here.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!wcsstr(_wcslwr(exe), L"\\enshrouded.exe"))
        return TRUE;

    LogInit();
    g_config = LoadConfig();
    Log("Quick Deposit " QD_VERSION " loaded");
    if (!g_config.enabled) {
        Log("disabled in quick-deposit.ini");
        return TRUE;
    }
    // Hook now, before the game's code runs, so the patch never races a running frame.
    if (!client::Init()) {
        Log("inactive: this game version is not supported");
        return TRUE;
    }
    client::SetConfig(g_config);
#ifdef QD_BRIDGE
    bridge_ops::Register();
    bridge::Start(QD_VERSION "-dev");
#endif
    Log("ready: G twice in the inventory%s%s", g_config.doubleSortMs ? "" : " (off)", g_config.hotkey ? ", plus a hotkey" : "");
    if (g_config.hotkey) {
        if (HANDLE t = CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    return TRUE;
}

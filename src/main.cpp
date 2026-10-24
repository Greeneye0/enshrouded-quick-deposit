#include <windows.h>
#include <atomic>
#include "chat.h"
#include "client.h"
#include "config.h"
#include "inventory.h"
#include "hints.h"
#include "keep.h"
#include "log.h"
#include "../../common/greeneye.h"
#ifdef QD_BRIDGE
#include "../dev/bridge.h"
namespace bridge_ops { void Register(); }
#endif

#define QD_VERSION "1.3.6"

bool LoadProxyTargets();

static Config g_config;
static std::atomic<unsigned> g_hotkeyRun{0}; // a hotkey thread runs while this is its number
static HANDLE g_hotkeyThread = nullptr;

static bool GameWindowFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI HotkeyThread(LPVOID run)
{
    bool wasDown = false;
    while (g_hotkeyRun.load() == static_cast<unsigned>(reinterpret_cast<uintptr_t>(run))) {
        bool down = (GetAsyncKeyState(g_config.hotkey) & 0x8000) != 0;
        if (down && !wasDown && GameWindowFocused())
            client::RequestDeposit(g_config);
        wasDown = down;
        Sleep(20);
    }
    return 0;
}

static void StartHotkey()
{
    if (!g_config.hotkey)
        return;
    unsigned run = ++g_hotkeyRun;
    g_hotkeyThread = CreateThread(nullptr, 0, HotkeyThread, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(run)), 0, nullptr);
}

static void StopHotkey()
{
    ++g_hotkeyRun;
    if (g_hotkeyThread) {
        if (WaitForSingleObject(g_hotkeyThread, 500) != WAIT_OBJECT_0)
            Log("hotkey thread did not stop in time");
        CloseHandle(g_hotkeyThread);
        g_hotkeyThread = nullptr;
    }
}

// Settings, hooks, bridge and hotkey.
static void StartMod()
{
    g_config = LoadConfig();
    if (!g_config.enabled) {
        Log("disabled in quick-deposit.ini");
        return;
    }
    if (!client::Init()) {
        Log("inactive: this game version is not supported");
        return;
    }
    client::SetConfig(g_config);
    keep::Load(g_config.keepItems, g_config.keepKey);
    bool keepShown = keep::Init();
    if (!keepShown)
        Log("keep marks: kept items from quick-deposit.ini still stay, but are not shown");
    hints::Init(g_config.doubleSortMs > 0, keepShown ? g_config.keepKey : 0);
#ifdef QD_BRIDGE
    bridge_ops::Register();
    bridge::Start(QD_VERSION "-dev");
#endif
    Log("ready: G twice in the inventory%s%s", g_config.doubleSortMs ? "" : " (off)", g_config.hotkey ? ", plus a hotkey" : "");
    StartHotkey();
}

// Starts Quick Deposit once per process: the standalone powrprof.dll and the Greeneye Loader's
// copy may both be installed, and the first to start wins (a reload belongs to the first).
static bool StartQuickDeposit(const char* how, const GreeneyeHost* host)
{
    if (!greeneye::ClaimProcess(L"QuickDeposit", host))
        return false;
    LogInit(host && greeneye::HostHasHooks(host) && host->generation > 1);
    Log("Quick Deposit " QD_VERSION " loaded (%s)", how);
    StartMod();
    return true;
}

// Greeneye Loader (greeneye\quick-deposit\quick-deposit.dll): quick-deposit.ini, its log and
// report live in that folder; an existing quick-deposit.ini moves there from the game folder once.
// With the loader's API 2 every hook goes through the loader, so Quick Deposit can be reloaded
// while the game runs; what it learned from the game (chat sender, ECS registries, item
// databases) is kept by the loader across reloads.
GREENEYE_MOD_START
{
    if (!greeneye::HostOk(host) || host->side != kGreeneyeClient)
        return false;
    greeneye::UseHost(host);
    if (greeneye::Host()) {
        chat::UsePersistentState(static_cast<uint64_t*>(host->keep("quick-deposit.chat", 4 * sizeof(uint64_t))));
        inv::UseKept(host);
    }
    greeneye::SetDataDir(host->modDir);
    greeneye::Migrate("quick-deposit.ini");
    return StartQuickDeposit(host->generation > 1 ? "Greeneye Loader, reloaded" : "Greeneye Loader", host);
}

// Before a newer Quick Deposit replaces this one: not while a deposit runs; then the hotkey thread
// and the dev bridge stop. If the newer one fails to start, they start again.
static bool HotPrepare()
{
    if (client::DepositBusy())
        return false;
    StopHotkey();
#ifdef QD_BRIDGE
    bridge::Stop();
#endif
    Log("stopping for a reload");
    return true;
}

static bool HotReady()
{
    return true;
}

static void HotResume()
{
    StartHotkey();
#ifdef QD_BRIDGE
    bridge::Start(QD_VERSION "-dev");
#endif
    Log("the reload failed; carrying on");
}

static const GreeneyeHot kHot = {sizeof(GreeneyeHot), &HotPrepare, &HotReady, &HotResume};
GREENEYE_MOD_HOT
{
    return &kHot;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(instance);
    // Under any other name this is the Greeneye Loader's copy, started by GreeneyeModStart.
    if (!greeneye::LoadedAs(instance, L"powrprof.dll"))
        return TRUE;
    if (!LoadProxyTargets())
        return FALSE;

    // Only act inside the game itself, not tools that happen to load powrprof.dll from here.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!wcsstr(_wcslwr(exe), L"\\enshrouded.exe"))
        return TRUE;

    // Hook now, before the game's code runs, so the patch never races a running frame.
    greeneye::UseGameDirForData();
    StartQuickDeposit("standalone", nullptr);
    return TRUE;
}

// Which character is being played (character.h): read from the game's save manager, else the
// character save file the game writes while you are in a world.
//
// The save manager (build 1273043) is the client's field +0x88: 140844d65 "cmp dword [rcx+466b1c],
// 2; mov rbx, rcx; jne; mov rdx, [rcx+88h]; lea rcx, [rbp+f0h]; call 140dd1930", whose callee
// reads the character-load block at manager+0x10df28: +0 a busy flag, +4 the save asked for,
// +6 the save loaded, +8 the step (2: loaded), +0xc the loaded character's id. 140e01340, on the
// save thread, fills them when a character has loaded: "movzx eax, word [rsi+10df2c]; mov
// [rsi+10df2e], ax; mov eax, [rbp-78h]; movups [rsi+10df34], xmm0". A save is {u8 slot + 1, u8
// kind}: characterN.ksd is {N + 1, 1}.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "../../common/greeneye.h"
#include "character.h"
#include "log.h"
#include "scan.h"

namespace character {
namespace {

constexpr ULONGLONG kPollMs = 2000;
constexpr ULONGLONG kFindDirsMs = 60000; // look for the save folders again while none is found

std::vector<std::wstring> g_dirs; // folders holding characterN.ksd
ULONGLONG g_dirsAt = 0;
std::string g_current;  // id, 32 hex digits
bool g_confirmed = false;
bool g_inWorld = false;
unsigned g_visit = 0;
bool g_dirsLooked = false;
ULONGLONG g_enteredAt = 0;        // FILETIME of entering the world, as a number
ULONGLONG g_seenWrite = 0;        // the newest character file write seen so far this visit

// The save manager's character-load block (see above); found: all of it matched.
struct LoadBlock {
    bool found = false;
    int32_t manager = 0; // client + manager: the save manager
    int32_t loaded = 0;  // u16 save loaded
    int32_t step = 0;    // u8, 2: loaded
    int32_t id = 0;      // 16 bytes
} g_load;

void Confirm(const std::string& id, const char* how)
{
    if (id != g_current || !g_confirmed)
        Log("character: %s is played (%s)%s", id.c_str(), how,
            g_current.empty() || id == g_current ? "" : " - another character than before");
    if (id != g_current)
        WritePrivateProfileStringA("QuickDeposit", "lastCharacter", id.c_str(), greeneye::DataPath("quick-deposit.ini"));
    g_current = id;
    g_confirmed = true;
}

void FindLoadBlock()
{
    // The client's save manager field, and the check that reads the load block.
    uint8_t* use = FindUnique("83 B9 ?? ?? ?? ?? 02 48 8B D9 0F 85 ?? ?? ?? ?? 48 8B 91 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? E8");
    // Where a finished load puts the save and the character's id.
    uint8_t* fill = FindUnique("0F B7 86 ?? ?? ?? ?? 66 89 86 ?? ?? ?? ?? 8B 45 ?? 0F 11 86 ?? ?? ?? ??");
    if (!use || !fill) {
        Log("character: save manager not found - the character is known once the game saves it");
        return;
    }
    int32_t rel, busy, step, asked, loaded, id;
    memcpy(&rel, use + 31, 4);
    const uint8_t* check = use + 35 + rel;
    // 8B 82 <busy> 3C 01 74 ?? 80 BA <step> 03
    if (check[0] != 0x8B || check[1] != 0x82 || check[6] != 0x3C || check[10] != 0x80 || check[11] != 0xBA) {
        Log("character: save manager check not recognised - the character is known once the game saves it");
        return;
    }
    memcpy(&busy, check + 2, 4);
    memcpy(&step, check + 12, 4);
    memcpy(&asked, fill + 3, 4);
    memcpy(&loaded, fill + 10, 4);
    memcpy(&id, fill + 20, 4);
    if (asked != busy + 4 || loaded != busy + 6 || step != busy + 8 || id != busy + 12) {
        Log("character: save manager layout not recognised - the character is known once the game saves it");
        return;
    }
    memcpy(&g_load.manager, use + 19, 4);
    g_load.loaded = loaded;
    g_load.step = step;
    g_load.id = id;
    g_load.found = true;
}

// The loaded character's id from the save manager, or false.
bool ReadLoaded(void* client, std::string& out, unsigned& save)
{
    if (!g_load.found || !client)
        return false;
    uint8_t raw[16];
    __try {
        auto* manager = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(client) + g_load.manager);
        if (!manager || manager[g_load.step] != 2)
            return false;
        save = *reinterpret_cast<uint16_t*>(manager + g_load.loaded);
        memcpy(raw, manager + g_load.id, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    bool blank = true;
    for (uint8_t b : raw)
        blank &= b == 0;
    if (blank || (save & 0xff) == 0)
        return false;
    char hex[33];
    for (int i = 0; i < 16; ++i)
        sprintf_s(hex + i * 2, 3, "%02x", raw[i]);
    out = hex;
    return true;
}

ULONGLONG Number(const FILETIME& t)
{
    return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

bool HasCharacters(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\character*.ksd").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    return true;
}

// Steam's save folders of every account on this PC (userdata\<account>\1203620\remote), and the
// game's own folder in Saved Games.
void FindDirs()
{
    g_dirs.clear();
    wchar_t steam[MAX_PATH];
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam,
                     &size) == ERROR_SUCCESS) {
        std::wstring userdata = std::wstring(steam) + L"\\userdata";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((userdata + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                    std::wstring dir = userdata + L"\\" + fd.cFileName + L"\\1203620\\remote";
                    if (HasCharacters(dir))
                        g_dirs.push_back(dir);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    wchar_t profile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        std::wstring dir = std::wstring(profile) + L"\\Saved Games\\Enshrouded";
        if (HasCharacters(dir))
            g_dirs.push_back(dir);
    }
    for (const std::wstring& dir : g_dirs)
        Log("character saves in %ls", dir.c_str());
    if (g_dirs.empty())
        Log("character saves: none found - one keep list for every character");
}

// The id of a character save file: 16 bytes at +0x0c after "SV2C".
bool ReadId(const std::wstring& path, std::string& id)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    uint8_t head[0x1c];
    DWORD got = 0;
    bool ok = ReadFile(f, head, sizeof(head), &got, nullptr) && got == sizeof(head) && memcmp(head, "SV2C", 4) == 0;
    CloseHandle(f);
    if (!ok)
        return false;
    char hex[33];
    for (int i = 0; i < 16; ++i)
        sprintf_s(hex + i * 2, 3, "%02x", head[0x0c + i]);
    bool blank = true;
    for (int i = 0; i < 16; ++i)
        blank &= head[0x0c + i] == 0;
    if (blank)
        return false;
    id = hex;
    return true;
}

// The character file written last after entering the world, if any.
void Poll()
{
    std::wstring newest;
    ULONGLONG newestAt = g_seenWrite;
    for (const std::wstring& dir : g_dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\character*.ksd").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            // characterN.ksd only (the pattern also matches older short names).
            const wchar_t* name = fd.cFileName;
            size_t len = wcslen(name);
            if (len < 14 || _wcsnicmp(name, L"character", 9) != 0 || _wcsicmp(name + len - 4, L".ksd") != 0)
                continue;
            ULONGLONG at = Number(fd.ftLastWriteTime);
            if (at > g_enteredAt && at > newestAt) {
                newestAt = at;
                newest = dir + L"\\" + name;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (newest.empty())
        return;
    std::string id;
    if (!ReadId(newest, id))
        return; // being written; next poll
    g_seenWrite = newestAt;
    char how[MAX_PATH + 32];
    sprintf_s(how, "%ls was saved", wcsrchr(newest.c_str(), L'\\') + 1);
    Confirm(id, how);
}

} // namespace

void Init()
{
    char id[64] = "";
    GetPrivateProfileStringA("QuickDeposit", "lastCharacter", "", id, sizeof(id), greeneye::DataPath("quick-deposit.ini"));
    g_current = strlen(id) == 32 ? id : "";
    if (!g_current.empty())
        Log("character: the last one played is %s", id);
    FindLoadBlock();
}

void Tick(void* client, bool inWorld)
{
    static ULONGLONG lastPoll = 0;
    ULONGLONG now = GetTickCount64();
    if (inWorld != g_inWorld) {
        g_inWorld = inWorld;
        g_confirmed = false;
        if (inWorld) {
            ++g_visit;
            FILETIME t;
            GetSystemTimeAsFileTime(&t);
            // A little before now: a save that started as you entered still counts.
            g_enteredAt = Number(t) - 2 * 10000000ull;
            g_seenWrite = 0;
            lastPoll = 0;
        }
    }
    if (!inWorld)
        return;
    // The save manager knows at once; read every frame, so a character loaded later counts too.
    std::string id;
    unsigned save = 0;
    if (ReadLoaded(client, id, save)) {
        if (id != g_current || !g_confirmed) {
            char how[48];
            sprintf_s(how, "character%u.ksd, loaded", (save & 0xff) - 1);
            Confirm(id, how);
        }
        return;
    }
    if (g_confirmed || now - lastPoll < kPollMs)
        return;
    lastPoll = now;
    if (g_dirs.empty() && (g_dirsAt == 0 || now - g_dirsAt > kFindDirsMs)) {
        g_dirsAt = now;
        FindDirs();
        g_dirsLooked = true;
    }
    Poll();
}

std::string Current()
{
    return g_current;
}

bool Confirmed()
{
    return g_confirmed;
}

unsigned Visit()
{
    return g_visit;
}

bool InWorld()
{
    return g_inWorld;
}

bool Ready()
{
    return g_load.found || g_dirsLooked;
}

bool Tracking()
{
    return g_load.found || !g_dirs.empty();
}

} // namespace character

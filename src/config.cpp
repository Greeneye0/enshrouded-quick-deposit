#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "config.h"
#include "inventory.h"

static const char* kIniPath = ".\\quick-deposit.ini";
static const char* kDefaultCategories = "all";

static void WriteDefaults(const Config& c)
{
    FILE* f = fopen(kIniPath, "w");
    if (!f)
        return;
    fprintf(f,
        "; Quick Deposit for Enshrouded\n"
        "; Press G (Sort Inventory) twice in your inventory to deposit.\n"
        "; doubleSortMs: how quickly the second G must follow (0 turns double-G off)\n"
        "; hotkey: optional extra key, F1-F12 or a virtual-key code in hex (empty = none)\n"
        "; delayMs: pause between chests (raise it if some chests get skipped on a laggy server)\n"
        "; settleMs: wait for the server's changes before the next step and the report\n"
        "; tier2: off | dry | on - leftovers to a chest holding the same kind of item\n"
        ";   (dry = only write the plan to the report, move nothing)\n"
        "; tier2Categories: all, or the item categories tier 2 may move: Equipment, Weapons,\n"
        ";   Tools, Instrument, FishingRod, BuildTools, Consumables, Ammunition, Materials,\n"
        ";   Blueprints, Collectibles, AnimalFood, PetFood, WeaponGem\n"
        "; Chests whose name starts with -- are never used. excludeChests: more chest names the mod\n"
        ";   never uses, comma-separated (e.g. My Stuff)\n"
        "[QuickDeposit]\n"
        "enabled=1\n"
        "doubleSortMs=%d\n"
        "hotkey=\n"
        "delayMs=%d\n"
        "settleMs=%d\n"
        "tier2=on\n"
        "tier2Categories=%s\n"
        "excludeChests=\n",
        c.doubleSortMs, c.delayMs, c.settleMs, kDefaultCategories);
    fclose(f);
}

static int ParseKey(const char* s, int fallback)
{
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    char* end = nullptr;
    long v = strtol(s, &end, 0);
    return (end != s && v > 0 && v < 256) ? static_cast<int>(v) : fallback;
}

static uint32_t ParseCategories(const char* list)
{
    if (_stricmp(list, "all") == 0)
        return 0xFFFFFFFFu;
    uint32_t mask = 0;
    char buf[512];
    strncpy_s(buf, list, _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx))
        for (uint8_t c = 0; c < 32; ++c)
            if (_stricmp(tok, inv::CategoryName(c)) == 0)
                mask |= 1u << c;
    return mask;
}

static int ClampInt(int v, int lo, int hi, int fallback)
{
    return (v >= lo && v <= hi) ? v : fallback;
}

Config LoadConfig()
{
    Config c;
    if (GetFileAttributesA(kIniPath) == INVALID_FILE_ATTRIBUTES)
        WriteDefaults(c);

    char buf[512];
    c.enabled = GetPrivateProfileIntA("QuickDeposit", "enabled", 1, kIniPath) != 0;
    GetPrivateProfileStringA("QuickDeposit", "hotkey", "", buf, sizeof(buf), kIniPath);
    c.hotkey = ParseKey(buf, c.hotkey);
    c.doubleSortMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "doubleSortMs", c.doubleSortMs, kIniPath), 0, 2000, c.doubleSortMs);
    c.delayMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "delayMs", c.delayMs, kIniPath), 0, 2000, c.delayMs);
    c.settleMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "settleMs", c.settleMs, kIniPath), 200, 10000, c.settleMs);
    GetPrivateProfileStringA("QuickDeposit", "tier2", "on", buf, sizeof(buf), kIniPath);
    c.tier2 = _stricmp(buf, "dry") == 0 ? Tier2Mode::DryRun : _stricmp(buf, "off") == 0 ? Tier2Mode::Off : Tier2Mode::On;
    GetPrivateProfileStringA("QuickDeposit", "tier2Categories", kDefaultCategories, buf, sizeof(buf), kIniPath);
    c.tier2Categories = ParseCategories(buf);
    GetPrivateProfileStringA("QuickDeposit", "excludeChests", "", c.excludeChests, sizeof(c.excludeChests), kIniPath);
    return c;
}

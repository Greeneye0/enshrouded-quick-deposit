#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include "config.h"
#include "inventory.h"
#include "log.h"
#include "../../common/greeneye.h"

// quick-deposit.ini in the data folder: the game folder standalone, greeneye\quick-deposit\ under
// the Greeneye Loader (relative to the current folder in the hot reload runtime, as before).
static const char* IniPath()
{
    return greeneye::DataPath("quick-deposit.ini");
}
static const char* kDefaultCategories = "all";
static const char* kDefaultOrder = "item,kind,name,weapons,nearest";

static void WriteDefaults(const Config& c)
{
    FILE* f = fopen(IniPath(), "w");
    if (!f)
        return;
    fprintf(f,
        "; Quick Deposit for Enshrouded\n"
        "; Press G (Sort Inventory) twice in your inventory to deposit.\n"
        "; doubleSortMs: how quickly the second G must follow (0 turns double-G off)\n"
        "; hotkey: optional extra key to deposit (empty = none): a letter, digit, F1-F24, Mouse4, ...,\n"
        ";   with Shift+, Ctrl+, Alt+ in front, or a key code like 0x76; a controller button goes after a\n"
        ";   comma, Pad: and any of A B X Y LB RB LT RT LS RS View Menu Up Down Left Right joined with +\n"
        ";   (e.g. F6, Pad:LB+A). On a controller, Sort Inventory twice deposits anyway.\n"
        "; delayMs: pause between chests (raise it if some chests get skipped on a laggy server)\n"
        "; settleMs: the longest wait for the server's changes before the next step and the report\n"
        "; tier2: off | dry | on - leftovers to a chest holding the same kind of item\n"
        ";   (dry = only write the plan to the report, move nothing)\n"
        "; tier2Categories: all, or the item categories tier 2 may move: Equipment, Weapons,\n"
        ";   Tools, Instrument, FishingRod, BuildTools, Consumables, Ammunition, Materials,\n"
        ";   Blueprints, Collectibles, AnimalFood, PetFood, WeaponGem\n"
        "; tier2Order: how tier 2 picks a chest, first match wins: item (holds the same item), kind\n"
        ";   (holds the most items of the same kind), name (its name matches the item), weapons (holds\n"
        ";   the most weapons; weapons only), nearest. Leave a rule out to skip it; a stack none of\n"
        ";   them places stays in the backpack (e.g. name,item,kind: chest names first)\n"
        "; newItems: sort | stay - stay leaves items no chest holds yet in the backpack\n"
        "; Chests whose name starts with -- are never used. excludeChests: more chest names the mod\n"
        ";   never uses, comma-separated (e.g. My Stuff)\n"
        "; glowMs: how long the chests a deposit filled glow green after you close the inventory (0 = off)\n"
        "; keepKey: hover an item in your inventory and press it to keep that kind of item in the\n"
        ";   backpack (teal border); press it again to unmark. Same format as hotkey (empty = off);\n"
        ";   with no Pad: part a controller's left stick click marks the selected slot. hotkey and keepKey\n"
        ";   apply within two seconds, no restart needed.\n"
        "; keepItems: the kept items, comma-separated (keepKey writes it, e.g. Health Potion,Torch).\n"
        ";   Each character has its own list, in every world: [Keep.<character id>] keepItems. The\n"
        ";   list here is used until a character is known; the first character takes it over.\n"
        "; language: auto (the game's) or english, german, french, italian, spanish, brazilian, polish,\n"
        ";   russian, ukrainian, turkish, japanese, koreana, schinese, tchinese, thai\n"
        "; chat: full | short | off - deposit reports in your chat: every chest, one summary line, or\n"
        ";   none (only you see them; the report file always has everything)\n"
        "[QuickDeposit]\n"
        "enabled=1\n"
        "doubleSortMs=%d\n"
        "hotkey=\n"
        "delayMs=%d\n"
        "settleMs=%d\n"
        "tier2=on\n"
        "tier2Categories=%s\n"
        "tier2Order=%s\n"
        "newItems=sort\n"
        "excludeChests=\n"
        "glowMs=%d\n"
        "keepKey=K, Pad:LS\n"
        "keepItems=\n"
        "language=auto\n"
        "chat=full\n",
        c.doubleSortMs, c.delayMs, c.settleMs, kDefaultCategories, kDefaultOrder, c.glowMs);
    fclose(f);
}

// An ini from an older version lacks the newer settings, and one added by hand at the end of the
// file lands in the last [Keep.<character>] section, where it is never read. Writes every missing
// setting into [QuickDeposit], taking a value stranded in another section along (and removing it
// there), else the default.
static void AddMissingSettings(const Config& c)
{
    char num[3][16];
    sprintf_s(num[0], "%d", c.doubleSortMs);
    sprintf_s(num[1], "%d", c.delayMs);
    sprintf_s(num[2], "%d", c.settleMs);
    char glow[16];
    sprintf_s(glow, "%d", c.glowMs);
    const char* const settings[][2] = {
        {"enabled", "1"},          {"doubleSortMs", num[0]},  {"hotkey", ""},
        {"delayMs", num[1]},       {"settleMs", num[2]},      {"tier2", "on"},
        {"tier2Categories", kDefaultCategories}, {"tier2Order", kDefaultOrder}, {"newItems", "sort"},
        {"excludeChests", ""},     {"glowMs", glow},          {"keepKey", "K, Pad:LS"},
        {"language", "auto"},      {"chat", "full"},
    };
    static char sections[8192];
    DWORD len = GetPrivateProfileSectionNamesA(sections, sizeof(sections), IniPath());
    char buf[512];
    for (const auto& s : settings) {
        if (GetPrivateProfileStringA("QuickDeposit", s[0], "\x01", buf, sizeof(buf), IniPath()) != 1 || buf[0] != '\x01')
            continue;
        const char* value = s[1];
        for (const char* sec = sections; sec < sections + len && *sec; sec += strlen(sec) + 1) {
            if (_stricmp(sec, "QuickDeposit") == 0 ||
                (GetPrivateProfileStringA(sec, s[0], "\x01", buf, sizeof(buf), IniPath()) == 1 && buf[0] == '\x01'))
                continue;
            WritePrivateProfileStringA(sec, s[0], nullptr, IniPath());
            value = buf;
            break;
        }
        WritePrivateProfileStringA("QuickDeposit", s[0], value, IniPath());
    }
}

static int ParseKey(const char* s, int fallback);

// One key setting. A value only the older format knows (a key code without 0x) still counts.
static greeneye::input::Binding ReadKey(const char* name, const char* written, const greeneye::input::Binding& fallback)
{
    using namespace greeneye::input;
    char buf[128];
    GetPrivateProfileStringA("QuickDeposit", name, written, buf, sizeof(buf), IniPath());
    if (!buf[0])
        return Binding{}; // empty: off
    Binding out;
    if (ParseBinding(buf, fallback, out))
        return out;
    out = fallback;
    if (int vk = ParseKey(buf, 0)) {
        out.keys[0] = KeyChord{uint8_t(vk)};
        out.keyCount = 1;
    } else {
        Log("quickdeposit.ini: %s '%s' is not a key or button this mod knows", name, buf);
    }
    return out;
}

static void ReadKeys(Config& c)
{
    using namespace greeneye::input;
    c.hotkey = ReadKey("hotkey", "", Binding{});
    // Older ini files lack keepKey: as written for new ones.
    c.keepBinding = ReadKey("keepKey", "K, Pad:LS", MakeBinding("K, Pad:LS"));
    c.keepKey = c.keepBinding.keyCount ? c.keepBinding.keys[0].vk : 0;
}

bool RefreshKeys(Config& c)
{
    static greeneye::input::FileWatch watch;
    if (!watch.Changed(IniPath()))
        return false;
    ReadKeys(c);
    return true;
}

static int ParseKey(const char* s, int fallback)
{
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12)
            return VK_F1 + n - 1;
    }
    if (isalnum(static_cast<unsigned char>(s[0])) && !s[1])
        return toupper(static_cast<unsigned char>(s[0]));
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

// The list a test build before 1.0.1 wrote as its default (no BuildTools, Tools, Instrument or
// FishingRod, so roof tiles stayed in the backpack). Players who ran it never chose it.
static bool IsOldTestDefault(const char* list)
{
    static const char* const old = "Materials,Weapons,Equipment,Ammunition,Consumables,Collectibles,Blueprints,AnimalFood,PetFood,WeaponGem";
    const char* o = old;
    for (const char* p = list;; ++p) {
        if (*p == ' ')
            continue;
        if (tolower(static_cast<unsigned char>(*p)) != tolower(static_cast<unsigned char>(*o)))
            return false;
        if (!*p)
            return true;
        ++o;
    }
}

// tier2Order: rule names, comma-separated; unknown and repeated names are skipped. None known
// keeps the default order.
void ParseTier2Order(const char* list, Config& c)
{
    static const char* const names[kTier2Rules] = {"item", "kind", "name", "weapons", "nearest"};
    Tier2Rule order[kTier2Rules];
    int count = 0;
    char buf[256];
    strncpy_s(buf, list, _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx))
        for (int r = 0; r < kTier2Rules; ++r) {
            auto rule = static_cast<Tier2Rule>(r);
            if (_stricmp(tok, names[r]) == 0 && std::find(order, order + count, rule) == order + count)
                order[count++] = rule;
        }
    if (!count)
        return;
    std::copy(order, order + count, c.tier2Order);
    c.tier2RuleCount = count;
}

static int ClampInt(int v, int lo, int hi, int fallback)
{
    return (v >= lo && v <= hi) ? v : fallback;
}

Config LoadConfig()
{
    Config c;
    if (GetFileAttributesA(IniPath()) == INVALID_FILE_ATTRIBUTES)
        WriteDefaults(c);
    else
        AddMissingSettings(c);

    char buf[512];
    c.enabled = GetPrivateProfileIntA("QuickDeposit", "enabled", 1, IniPath()) != 0;
    ReadKeys(c);
    c.doubleSortMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "doubleSortMs", c.doubleSortMs, IniPath()), 0, 2000, c.doubleSortMs);
    c.delayMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "delayMs", c.delayMs, IniPath()), 0, 2000, c.delayMs);
    c.settleMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "settleMs", c.settleMs, IniPath()), 200, 10000, c.settleMs);
    GetPrivateProfileStringA("QuickDeposit", "tier2", "on", buf, sizeof(buf), IniPath());
    c.tier2 = _stricmp(buf, "dry") == 0 ? Tier2Mode::DryRun : _stricmp(buf, "off") == 0 ? Tier2Mode::Off : Tier2Mode::On;
    GetPrivateProfileStringA("QuickDeposit", "tier2Categories", kDefaultCategories, buf, sizeof(buf), IniPath());
    if (IsOldTestDefault(buf)) {
        WritePrivateProfileStringA("QuickDeposit", "tier2Categories", kDefaultCategories, IniPath());
        strcpy_s(buf, kDefaultCategories);
    }
    c.tier2Categories = ParseCategories(buf);
    GetPrivateProfileStringA("QuickDeposit", "tier2Order", kDefaultOrder, buf, sizeof(buf), IniPath());
    ParseTier2Order(buf, c);
    GetPrivateProfileStringA("QuickDeposit", "newItems", "sort", buf, sizeof(buf), IniPath());
    c.newItemsStay = _stricmp(buf, "stay") == 0;
    GetPrivateProfileStringA("QuickDeposit", "excludeChests", "", c.excludeChests, sizeof(c.excludeChests), IniPath());
    c.glowMs = ClampInt(GetPrivateProfileIntA("QuickDeposit", "glowMs", c.glowMs, IniPath()), 0, 60000, c.glowMs);
    GetPrivateProfileStringA("QuickDeposit", "keepItems", "", c.keepItems, sizeof(c.keepItems), IniPath());
    GetPrivateProfileStringA("QuickDeposit", "language", "auto", c.language, sizeof(c.language), IniPath());
    GetPrivateProfileStringA("QuickDeposit", "chat", "full", buf, sizeof(buf), IniPath());
    c.chat = _stricmp(buf, "short") == 0 ? chat::Mode::Short : _stricmp(buf, "off") == 0 ? chat::Mode::Off : chat::Mode::Full;
    return c;
}

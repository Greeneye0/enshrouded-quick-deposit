// Kept items, marked in the inventory (build 1273043).
//
// 140bf4180 doItemSlot(slot, style, ..) draws one inventory slot: *slot -> slot data {+0 page
// entity, +4 slot index, +8 item id (0: empty), +0x10 count}. Only slots of your backpack's
// storage pages are marked (not chests, not the hotbar). Its first image is the slot background, drawn through
// 14118ca30 DrawImageReal(widget, rect{x, y, w, h}, u32 id, u64 colour) (reached from the
// DrawImage thunk "mov r9, [r9]; mov r8d, [r8]; jmp DrawImageReal"). While a kept item's slot
// draws, its background is followed by the game's nine-slice glow boxes (FbUiPrimitives
// boxInnerGlow assets + 0xd4f0, glowBoxSmall + 0xd510) in teal, drawn with 140a3a1b0
// DrawNineSlice(widget, rect, ref, &borders[4], colour {u32 0xAABBGGRR, float alpha}). Find
// draws its gold search highlight the same way; the two chain on the same hooks.
// UI assets: *(*G + 200), G = the global loaded by "mov rax, [rip+x]; mov rcx, [rax+0xd0]".
// Rects are in UI units: 1080 high, as wide as the window's aspect makes it.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../common/greeneye.h"
#include "character.h"
#include "chat.h"
#include "inventory.h"
#include "keep.h"
#include "lang.h"
#include "log.h"
#include "scan.h"

namespace keep {
namespace {

using SlotFn = void (*)(uint8_t** slot, void* style, void* extra);
using DrawRealFn = void (*)(void* widget, const float* rect, uint32_t id, uint64_t colour);
using NineSliceFn = void (*)(void* widget, const float* rect, const void* ref, const float* borders, uint64_t colour);

SlotFn g_originalSlot = nullptr;
DrawRealFn g_originalDrawReal = nullptr;
NineSliceFn g_nineSlice = nullptr;
void** g_root = nullptr;

constexpr size_t kAssetBoxInnerGlow = 0xd3d0 + 0x120;
constexpr size_t kAssetGlowBoxSmall = 0xd3d0 + 0x140;
constexpr uint32_t kKeepColour = 0xFFA5CA5D; // teal (0xAABBGGRR), apart from Find's gold
constexpr int kMaxSlots = 256;
constexpr uint32_t kMaxExact = 5000;  // larger kept amounts skip the exact-sum search

struct Mark {
    std::string name; // item name, as in keepItems
    uint32_t amount;  // kAll: every one
    std::vector<uint64_t> prefer; // slots you pressed keepKey on (page entity << 8 | slot), kept first
    // kAll marks (stackable items): stacks you unmarked one by one go, the type stays marked and
    // new stacks arrive kept. free: how many may go. It only shrinks by what actually leaves the
    // backpack (seen: how many it held when free was last brought up to date), so the game's own
    // sorting, which merges stacks, changes nothing. freed: the slots you unmarked, which go first
    // and show as unmarked. Not saved: slot ids change between sessions.
    uint32_t free = 0;
    uint32_t seen = 0;
    std::vector<uint64_t> freed;
};
std::vector<Mark> g_marks;
std::unordered_map<uint32_t, int> g_cache; // item id -> index into g_marks, -1 unmarked
inv::Owner g_backpack;                     // pageCount 0 until read
struct KeptSlot {
    uint64_t key; // page entity << 8 | slot
    uint32_t itemId;
    uint32_t amount; // how many of the stack a deposit keeps
};
std::vector<KeptSlot> g_keptSlots;
// Your marks as item ids, for the host (read from its threads).
std::mutex g_snapshotLock;
std::vector<KeptItem> g_snapshot;
uint64_t g_snapshotVersion = 0;
uint32_t KeptHere(uint64_t key);
int g_key = 'K';
bool g_keyDown = false;

// The slot being drawn (game thread).
uint32_t g_slotItem = 0;
bool g_slotBackground = false; // its background is drawn
uint8_t g_slotData[32];
ULONGLONG g_chestSlotAt = 0; // when a slot of an inventory not yours last drew

// Slots drawn this frame and the last complete frame.
std::mutex g_slotsLock; // the bridge reads LastFrameSlots from its own op, on the game thread too
std::vector<SlotSeen> g_thisFrame, g_lastFrame;

uint8_t* Assets()
{
    void* g = g_root ? *g_root : nullptr;
    return g ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g) + 200) : nullptr;
}

uint64_t Colour(uint32_t rgba, float alpha)
{
    struct {
        uint32_t rgba;
        float alpha;
    } c{rgba, alpha};
    uint64_t v;
    memcpy(&v, &c, 8);
    return v;
}

// The name a new mark is saved under. Gear (items that do not stack) is marked piece by piece and
// the game gives some tiers one name (Eternal Ice Bolt I to IV are all "Spell Ice Bolt Eternal"),
// so its mark is pinned to the item id: "Spell Ice Bolt Eternal #1144190206". Item ids are the
// same on every PC. Items that stack keep a mark by name.
const char* NameOf(uint32_t itemId, char* buf, size_t size)
{
    inv::ItemDesc d = inv::Describe(itemId);
    if (!d.known)
        buf[0] = 0;
    else if (d.maxStack <= 1)
        _snprintf_s(buf, size, _TRUNCATE, "%s #%u", d.name, itemId);
    else
        _snprintf_s(buf, size, _TRUNCATE, "%s", d.name);
    return buf;
}

// A teal border around a kept item's slot, over its background (like Find's gold highlight).
void DrawKeepBorder(void* widget, const float* rect)
{
    uint8_t* assets = Assets();
    if (!assets || !g_nineSlice)
        return;
    float w = rect[2], h = rect[3];
    if (!(w > 4.0f && h > 4.0f) || w > 1000.0f || h > 1000.0f)
        return;
    float m = w < h ? w : h;
    float b = m * 0.3f;
    float borders[4] = {b, b, b, b};
    float grow = m * 0.08f;
    float outer[4] = {rect[0] - grow, rect[1] - grow, w + 2 * grow, h + 2 * grow};
    float ob = b + grow;
    float outerBorders[4] = {ob, ob, ob, ob};
    g_nineSlice(widget, outer, assets + kAssetGlowBoxSmall, outerBorders, Colour(kKeepColour, 0.55f));
    g_nineSlice(widget, rect, assets + kAssetBoxInnerGlow, borders, Colour(kKeepColour, 0.9f));
}

void DrawRealHook(void* widget, const float* rect, uint32_t id, uint64_t colour)
{
    greeneye::Lease lease;
    if (lease)
        inv::ResolveNames(); // the UI is being built: item names can be looked up
    if (!lease || !g_slotItem || g_slotBackground || !rect) {
        g_originalDrawReal(widget, rect, id, colour);
        return;
    }
    // The first image of an item's slot: its background.
    g_slotBackground = true;
    g_originalDrawReal(widget, rect, id, colour);
    {
        std::lock_guard<std::mutex> lock(g_slotsLock);
        if (g_thisFrame.size() < kMaxSlots) {
            SlotSeen s{};
            memcpy(s.rect, rect, sizeof(s.rect));
            s.itemId = g_slotItem;
            memcpy(s.data, g_slotData, sizeof(s.data));
            g_thisFrame.push_back(s);
        }
    }
    uint32_t page, index;
    memcpy(&page, g_slotData, 4);
    memcpy(&index, g_slotData + 4, 4);
    uint64_t key = static_cast<uint64_t>(page) << 8 | (index & 0xff);
    if (KeptHere(key))
        DrawKeepBorder(widget, rect);
}

// The slot's item id (0: empty or unreadable) and its first data bytes.
uint32_t ReadSlot(uint8_t** slot, uint8_t* data, size_t size)
{
    __try {
        uint8_t* d = slot ? *slot : nullptr;
        if (!d)
            return 0;
        memcpy(data, d, size);
        return *reinterpret_cast<uint32_t*>(d + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// A page of g_backpack that deposits empty: storage, not the hotbar.
bool IsBackpackRow(int p)
{
    const inv::Page& page = g_backpack.pages[p];
    return (page.kind == inv::PageKind::Generic || page.kind == inv::PageKind::Virtual) &&
           !inv::IsHotbarPage(g_backpack, page.entity);
}

void SlotHook(uint8_t** slot, void* style, void* extra)
{
    greeneye::Lease lease;
    if (!lease) {
        g_originalSlot(slot, style, extra);
        return;
    }
    uint8_t data[32] = {};
    uint32_t item = ReadSlot(slot, data, sizeof(data));
    uint32_t page;
    memcpy(&page, data, sizeof(page));
    bool backpack = false, yours = false;
    for (int p = 0; p < g_backpack.pageCount; ++p) {
        yours |= g_backpack.pages[p].entity == page;
        backpack |= g_backpack.pages[p].entity == page && IsBackpackRow(p);
    }
    if (page && g_backpack.pageCount && !yours)
        g_chestSlotAt = GetTickCount64();
    if (!backpack)
        item = 0; // a chest's or the hotbar's slot
    uint32_t outerItem = g_slotItem;
    bool outerBackground = g_slotBackground;
    uint8_t outerData[32];
    memcpy(outerData, g_slotData, sizeof(outerData));
    g_slotItem = item;
    g_slotBackground = false;
    memcpy(g_slotData, data, sizeof(g_slotData));
    g_originalSlot(slot, style, extra);
    g_slotItem = outerItem;
    g_slotBackground = outerBackground;
    memcpy(g_slotData, outerData, sizeof(g_slotData));
}

bool CursorInUi(float& x, float& y)
{
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground)
        GetWindowThreadProcessId(foreground, &pid);
    if (pid != GetCurrentProcessId())
        return false;
    POINT cursor{};
    RECT client{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(foreground, &cursor) || !GetClientRect(foreground, &client) ||
        client.bottom <= 0)
        return false;
    float scale = 1080.0f / static_cast<float>(client.bottom);
    x = cursor.x * scale;
    y = cursor.y * scale;
    return true;
}

// The item id a mark is pinned to ("Spell Ice Bolt Eternal #1144190206"), or 0 for a mark by name.
uint32_t PinOf(const std::string& name)
{
    size_t hash = name.rfind(" #");
    if (hash == std::string::npos || hash + 2 >= name.size())
        return 0;
    for (size_t i = hash + 2; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    return static_cast<uint32_t>(strtoul(name.c_str() + hash + 2, nullptr, 10));
}

int MarkOf(uint32_t itemId)
{
    if (!itemId || g_marks.empty())
        return -1;
    auto it = g_cache.find(itemId);
    if (it != g_cache.end())
        return it->second;
    inv::ItemDesc d = inv::Describe(itemId);
    int found = -1;
    for (size_t i = 0; i < g_marks.size() && found < 0 && d.known; ++i) {
        uint32_t pin = PinOf(g_marks[i].name);
        if (pin ? pin == itemId
                : _stricmp(g_marks[i].name.c_str(), d.name) == 0 || _stricmp(g_marks[i].name.c_str(), d.raw) == 0)
            found = static_cast<int>(i);
    }
    if (d.known) // unknown items may become known once the item database is seen
        g_cache[itemId] = found;
    return found;
}

// For each item type, which stacks make up its kept amount. markOf: index into amounts/prefer for
// an item type, -1 if not kept. onlyStorage: leave out pages that are not storage rows (the
// hotbar); the host's page lists hold only what a deposit may take.
template <class MarkOfFn>
void PlanCore(const inv::Owner& backpack, MarkOfFn markOf, const uint32_t* amounts,
              const std::vector<uint64_t>* const* prefer, bool onlyStorage, Plan& out,
              const uint32_t* frees = nullptr, const std::vector<uint64_t>* const* freed = nullptr)
{
    memset(&out, 0, sizeof(out));
    struct Stack {
        int page, slot;
        uint32_t count;
    };
    std::vector<uint32_t> done;
    for (int p0 = 0; p0 < backpack.pageCount; ++p0) {
        for (int s0 = 0; s0 < inv::kSlotsPerPage; ++s0) {
            uint32_t item = backpack.pages[p0].slots[s0].itemId;
            int mark = markOf(item);
            if (mark < 0 || std::find(done.begin(), done.end(), item) != done.end())
                continue;
            done.push_back(item);
            std::vector<Stack> stacks;
            for (int p = 0; p < backpack.pageCount; ++p) {
                const inv::Page& page = backpack.pages[p];
                if (onlyStorage && ((page.kind != inv::PageKind::Generic && page.kind != inv::PageKind::Virtual) ||
                                    inv::IsHotbarPage(backpack, page.entity)))
                    continue;
                for (int s = 0; s < inv::kSlotsPerPage; ++s)
                    if (page.slots[s].itemId == item && page.slots[s].count)
                        stacks.push_back({p, s, page.slots[s].count});
            }
            // Among stacks that would do as well, the ones you marked (two full stacks: the one you
            // pressed keepKey on). Earlier stacks are taken first below.
            if (const std::vector<uint64_t>* keys = prefer ? prefer[mark] : nullptr)
                std::stable_partition(stacks.begin(), stacks.end(), [&](const Stack& st) {
                    uint64_t key = static_cast<uint64_t>(backpack.pages[st.page].entity) << 8 | st.slot;
                    return std::find(keys->begin(), keys->end(), key) != keys->end();
                });
            uint32_t left = amounts[mark];
            if (left == kAll) {
                // Every one, less what you unmarked: first the stacks you unmarked, then from the
                // smallest.
                uint32_t go = frees ? frees[mark] : 0;
                for (const Stack& st : stacks)
                    out.keep[st.page][st.slot] = st.count;
                auto keyOf = [&](const Stack& st) {
                    return static_cast<uint64_t>(backpack.pages[st.page].entity) << 8 | st.slot;
                };
                if (const std::vector<uint64_t>* keys = freed ? freed[mark] : nullptr)
                    for (uint64_t key : *keys)
                        for (const Stack& st : stacks)
                            if (go && keyOf(st) == key && out.keep[st.page][st.slot] && st.count <= go) {
                                out.keep[st.page][st.slot] = 0;
                                go -= st.count;
                            }
                std::vector<Stack> bySize = stacks;
                std::stable_sort(bySize.begin(), bySize.end(), [](const Stack& a, const Stack& b) { return a.count < b.count; });
                for (const Stack& st : bySize) {
                    uint32_t& k = out.keep[st.page][st.slot];
                    uint32_t take = go < k ? go : k;
                    k -= take;
                    go -= take;
                }
                continue;
            }
            // Whole stacks that add up to exactly the amount: the stacks you marked, most likely.
            // reach[i][sum]: the first i stacks can make sum.
            if (left <= kMaxExact && stacks.size() <= 64) {
                const size_t n = stacks.size();
                std::vector<std::vector<bool>> reach(n + 1, std::vector<bool>(left + 1, false));
                reach[0][0] = true;
                for (size_t i = 0; i < n; ++i)
                    for (uint32_t sum = 0; sum <= left; ++sum)
                        reach[i + 1][sum] = reach[i][sum] || (sum >= stacks[i].count && reach[i][sum - stacks[i].count]);
                if (reach[n][left]) {
                    uint32_t sum = left;
                    for (size_t i = n; i > 0 && sum; --i) {
                        if (reach[i - 1][sum])
                            continue; // made without stack i - 1
                        const Stack& st = stacks[i - 1];
                        out.keep[st.page][st.slot] = st.count;
                        sum -= st.count;
                    }
                    continue;
                }
            }
            std::stable_sort(stacks.begin(), stacks.end(), [](const Stack& a, const Stack& b) { return a.count > b.count; });
            for (const Stack& st : stacks) {
                if (left && st.count <= left) {
                    out.keep[st.page][st.slot] = st.count;
                    left -= st.count;
                }
            }
            // The rest from part of the smallest larger stack; on a tie the first, which is the
            // preferred one (the stack kept so far), so the mark does not hop between equal stacks.
            const Stack* part = nullptr;
            for (const Stack& st : stacks)
                if (left && !out.keep[st.page][st.slot] && st.count > left && (!part || st.count < part->count))
                    part = &st;
            if (part)
                out.keep[part->page][part->slot] = left;
        }
    }
}


// The marked item types in your backpack, as ids and amounts.
void PublishSnapshot()
{
    std::vector<KeptItem> now;
    for (int p = 0; p < g_backpack.pageCount; ++p)
        for (const inv::Stack& st : g_backpack.pages[p].slots) {
            int mark = st.itemId ? MarkOf(st.itemId) : -1;
            if (mark < 0 || std::any_of(now.begin(), now.end(), [&](const KeptItem& k) { return k.itemId == st.itemId; }))
                continue;
            uint32_t amount = g_marks[mark].amount;
            if (amount == kAll && g_marks[mark].free) {
                uint64_t total = 0;
                for (const KeptSlot& k : g_keptSlots)
                    total += k.itemId == st.itemId ? k.amount : 0;
                amount = static_cast<uint32_t>(total);
            }
            now.push_back({st.itemId, amount});
        }
    std::sort(now.begin(), now.end(), [](const KeptItem& x, const KeptItem& y) { return x.itemId < y.itemId; });
    std::lock_guard<std::mutex> lock(g_snapshotLock);
    if (now.size() == g_snapshot.size() &&
        std::equal(now.begin(), now.end(), g_snapshot.begin(),
                   [](const KeptItem& x, const KeptItem& y) { return x.itemId == y.itemId && x.amount == y.amount; }))
        return;
    g_snapshot = std::move(now);
    ++g_snapshotVersion;
}

// How many of a mark's item the backpack's storage rows hold (the stacks a deposit may take).
uint32_t StoredFor(const inv::Owner& backpack, int mark)
{
    uint64_t n = 0;
    for (int p = 0; p < backpack.pageCount; ++p) {
        const inv::Page& page = backpack.pages[p];
        if ((page.kind != inv::PageKind::Generic && page.kind != inv::PageKind::Virtual) ||
            inv::IsHotbarPage(backpack, page.entity))
            continue;
        for (const inv::Stack& st : page.slots)
            if (st.itemId && st.count && MarkOf(st.itemId) == mark)
                n += st.count;
    }
    return static_cast<uint32_t>(n < kAll ? n : kAll - 1);
}

// What a kAll mark lets go now that the backpack holds total: what left since seen comes off.
uint32_t FreeNow(const Mark& m, uint32_t total)
{
    uint32_t gone = m.seen > total ? m.seen - total : 0;
    return m.free > gone ? m.free - gone : 0;
}

// Brings each kAll mark's free amount up to date with the backpack (what left comes off it; what
// arrived is kept).
void TrimFree()
{
    for (size_t i = 0; i < g_marks.size(); ++i) {
        Mark& m = g_marks[i];
        if (m.amount != kAll)
            continue;
        uint32_t total = StoredFor(g_backpack, static_cast<int>(i));
        m.free = FreeNow(m, total);
        m.seen = total;
        if (!m.free)
            m.freed.clear();
    }
}

void UpdateKeptSlots()
{
    g_keptSlots.clear();
    if (g_backpack.pageCount)
        TrimFree();
    if (!g_backpack.pageCount || g_marks.empty()) {
        if (g_backpack.pageCount)
            PublishSnapshot();
        return;
    }
    static Plan plan;
    PlanFor(g_backpack, plan);
    for (int p = 0; p < g_backpack.pageCount; ++p)
        for (int s = 0; s < inv::kSlotsPerPage; ++s)
            if (plan.keep[p][s])
                g_keptSlots.push_back({static_cast<uint64_t>(g_backpack.pages[p].entity) << 8 | s,
                                       g_backpack.pages[p].slots[s].itemId, plan.keep[p][s]});
    PublishSnapshot();
    // The kept stacks stay the preferred ones, so a stack you pick up that would do as well (a
    // second full stack) does not take the mark.
    for (size_t i = 0; i < g_marks.size(); ++i) {
        if (g_marks[i].amount == kAll)
            continue; // every stack is kept; freed says which ones go
        std::vector<uint64_t> keys;
        for (const KeptSlot& k : g_keptSlots)
            if (MarkOf(k.itemId) == static_cast<int>(i))
                keys.push_back(k.key);
        if (!keys.empty())
            g_marks[i].prefer = std::move(keys);
    }
}

// How many of the stack in this slot a deposit keeps (0: none).
uint32_t KeptHere(uint64_t key)
{
    for (const KeptSlot& k : g_keptSlots)
        if (k.key == key)
            return k.amount;
    return 0;
}

// Whose marks are loaded: a character's id (character.h), or "" for the shared list keepItems in
// [QuickDeposit] (before any character was known, or without character saves).
std::string g_character;
// Marks held back: in a world whose character is not known yet there are none, and what you mark
// meanwhile joins the character's list once it is known.
bool g_holding = false;

// The ini section of a character's marks.
std::string SectionOf(const std::string& id)
{
    return "Keep." + id;
}

void SaveNames()
{
    if (g_holding)
        return;
    std::string list;
    for (const Mark& m : g_marks) {
        if (!list.empty())
            list += ",";
        list += m.name;
        if (m.amount != kAll)
            list += " x" + std::to_string(m.amount);
    }
    const char* ini = greeneye::DataPath("quick-deposit.ini");
    if (g_character.empty())
        WritePrivateProfileStringA("QuickDeposit", "keepItems", list.c_str(), ini);
    else
        WritePrivateProfileStringA(SectionOf(g_character).c_str(), "keepItems", list.c_str(), ini);
}

// Marks may end on their own (gear that left you, gear put away) only once it is sure whose they
// are: not while the last character's marks stand in for one that has not been recognised yet.
bool MarksSettled()
{
    return g_character.empty() || character::Confirmed();
}

template <class T> bool Find(T& out, const char* name, const char* pattern)
{
    out = reinterpret_cast<T>(FindUnique(pattern));
    if (!out)
        Log("keep marks: signature not found: %s", name);
    return out != nullptr;
}

} // namespace

bool Init()
{
    uint8_t* slot;
    uint8_t* thunk;
    bool ok = true;
    // doItemSlot: mov rax, rsp; push rbp; push r15; lea rbp, [rax-228h] (13 bytes)
    ok &= Find(slot, "doItemSlot", "48 8B C4 55 41 57 48 8D A8 D8 FD FF FF");
    ok &= Find(thunk, "DrawImage", "4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC");
    ok &= Find(g_nineSlice, "DrawNineSlice", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 30 48 8B F2");
    g_root = reinterpret_cast<void**>(ResolveRipConsensus("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00", 3, 7));
    if (!ok || !g_root) {
        Log("keep marks: not drawn (game version?)");
        return false;
    }
    uint8_t* real = thunk + 11 + *reinterpret_cast<int32_t*>(thunk + 7);
    // DrawImageReal: mov rax, [rcx+8]; mov rcx, [rax+50h]; mov rcx, [rcx+18h] (12 bytes)
    static const uint8_t kReal[] = {0x48, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0x48, 0x50, 0x48, 0x8B, 0x49, 0x18};
    uint8_t canonical[sizeof(kReal)];
    if (const GreeneyeHost* host = greeneye::Host())
        host->read(real, canonical, sizeof(canonical));
    else
        memcpy(canonical, real, sizeof(canonical));
    if (memcmp(canonical, kReal, sizeof(kReal)) != 0 && !IsHookPatch(real)) {
        Log("keep marks: DrawImageReal not recognised");
        return false;
    }
    g_originalDrawReal = static_cast<DrawRealFn>(InstallHook(real, reinterpret_cast<void*>(&DrawRealHook), 12));
    g_originalSlot = static_cast<SlotFn>(InstallHook(slot, reinterpret_cast<void*>(&SlotHook), 13));
    if (!g_originalDrawReal || !g_originalSlot) {
        Log("keep marks: hook install failed");
        return false;
    }
    return true;
}

// Replaces the marks with a keepItems list.
void Parse(const char* list)
{
    g_marks.clear();
    g_cache.clear();
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',')
            ++p;
        const char* start = p;
        while (*p && *p != ',')
            ++p;
        const char* end = p;
        while (end > start && end[-1] == ' ')
            --end;
        // "Twigs x20": keep 20; a name alone keeps every one.
        uint32_t amount = kAll;
        const char* x = end;
        while (x > start && x[-1] >= '0' && x[-1] <= '9')
            --x;
        if (x < end && x - start >= 3 && (x[-1] == 'x' || x[-1] == 'X') && x[-2] == ' ') {
            amount = static_cast<uint32_t>(strtoul(std::string(x, end).c_str(), nullptr, 10));
            end = x - 2;
            while (end > start && end[-1] == ' ')
                --end;
        }
        if (end > start && amount)
            g_marks.push_back({std::string(start, end), amount});
    }
}

void Load(const char* list, int key)
{
    g_key = key;
    Parse(list);
    if (!g_marks.empty())
        Log("keep marks: %d item types kept", static_cast<int>(g_marks.size()));
    UpdateKeptSlots();
}

void UseCharacter(const std::string& id)
{
    if (id.empty() || id == g_character)
        return;
    std::vector<Mark> held;
    if (g_holding)
        held.swap(g_marks);
    g_holding = false;
    const char* ini = greeneye::DataPath("quick-deposit.ini");
    std::string section = SectionOf(id);
    static char list[32768];
    GetPrivateProfileStringA(section.c_str(), "keepItems", "<none>", list, sizeof(list), ini);
    bool own = strcmp(list, "<none>") != 0;
    if (!own) {
        // A character without a list of its own: the first one takes over the shared list from
        // before 1.3.14 (and what was marked since), the others start empty.
        GetPrivateProfileStringA("QuickDeposit", "keepItems", "", list, sizeof(list), ini);
        WritePrivateProfileStringA(section.c_str(), "keepItems", list, ini);
        if (list[0]) {
            WritePrivateProfileStringA("QuickDeposit", "keepItems", nullptr, ini);
            Log("keep marks: the shared list now belongs to character %s: %s", id.c_str(), list);
        }
    }
    g_character = id;
    Parse(list);
    Log("keep marks: character %s, %d item types kept%s", id.c_str(), static_cast<int>(g_marks.size()),
        own ? "" : list[0] ? " (the shared list)" : " (a new list)");
    bool added = false;
    for (const Mark& m : held)
        if (std::none_of(g_marks.begin(), g_marks.end(), [&](const Mark& x) { return x.name == m.name; })) {
            g_marks.push_back({m.name, m.amount});
            added = true;
        }
    if (added) {
        g_cache.clear();
        SaveNames();
    }
    UpdateKeptSlots();
}

void FollowCharacter()
{
    if (!character::Ready() || !character::Tracking())
        return; // the character can't be told: one list for all
    if (character::Confirmed()) {
        UseCharacter(character::Current());
        return;
    }
    if (!character::InWorld() || g_holding)
        return;
    // In a world, not yet known whose marks count (only when the save manager can't be read):
    // none, and deposits wait (Waiting), so no one's kept items are deposited or unmarked.
    Log("keep marks: waiting until the game saves your character - no marks or deposits until then");
    g_holding = true;
    g_character.clear();
    Parse("");
    UpdateKeptSlots();
}

bool Waiting()
{
    return g_holding;
}

// How many of the item the owner holds, on any page (backpack, hotbar, worn).
uint64_t HeldAnywhere(const inv::Owner& owner, uint32_t itemId)
{
    uint64_t n = 0;
    for (int p = 0; p < owner.pageCount; ++p)
        for (const inv::Stack& st : owner.pages[p].slots)
            n += st.itemId == itemId ? st.count : 0;
    return n;
}

// Gear you kept and then let go of (sold, salvaged, dropped, put in a chest) loses its mark, so
// it does not keep the next copy you pick up. Those leave one at a time; dying empties the
// backpack into your grave at once, so when kDeathTypes or more item types leave together
// (a death, or a big move) no mark ends and the grave's items are still kept when you get them back.
// True if the owner holds any item the mark covers, on any page.
bool MarkHeld(const inv::Owner& owner, int mark)
{
    for (int p = 0; p < owner.pageCount; ++p)
        for (const inv::Stack& st : owner.pages[p].slots)
            if (st.itemId && st.count && MarkOf(st.itemId) == mark)
                return true;
    return false;
}

void ForgetLeftGear(const inv::Owner& before, const inv::Owner& now)
{
    constexpr size_t kDeathTypes = 3;
    if (!MarksSettled() || !before.pageCount || before.entity != now.entity || before.pageCount != now.pageCount)
        return;
    std::vector<uint32_t> left;
    for (int p = 0; p < before.pageCount; ++p)
        for (const inv::Stack& st : before.pages[p].slots)
            if (st.itemId && st.count && std::find(left.begin(), left.end(), st.itemId) == left.end() &&
                !HeldAnywhere(now, st.itemId))
                left.push_back(st.itemId);
    if (left.empty())
        return;
    if (left.size() >= kDeathTypes) {
        Log("keep marks: %d item types left at once (a death or a big move) - marks stay", static_cast<int>(left.size()));
        return;
    }
    bool ended = false;
    for (uint32_t id : left) {
        int mark = MarkOf(id);
        if (mark < 0 || g_marks[mark].amount == kAll || MarkHeld(now, mark))
            continue;
        inv::ItemDesc d = inv::Describe(id);
        if (!d.known || d.maxStack > 1)
            continue;
        g_marks.erase(g_marks.begin() + mark);
        g_cache.clear();
        char line[256];
        int n = _snprintf_s(line, _TRUNCATE, "Quick Deposit: ");
        _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, lang::T(lang::Str::NoLongerKeeping), d.shown);
        chat::Post(line);
        Log("keep marks: %s - it left your inventory (item %u)", line + 15, id);
        ended = true;
    }
    if (ended)
        SaveNames();
}

// Gear marks from before 1.3.14 match by name, and tiers can share one (Eternal Ice Bolt I to IV).
// Once you carry exactly one item such a mark matches, it is pinned to that item's id, so other
// tiers are no longer kept with it. While several match, it waits.
void PinOldGearMarks(const inv::Owner& owner)
{
    bool pinned = false;
    for (size_t i = 0; i < g_marks.size(); ++i) {
        Mark& m = g_marks[i];
        if (m.amount == kAll || PinOf(m.name))
            continue;
        uint32_t only = 0;
        bool several = false;
        for (int p = 0; p < owner.pageCount && !several; ++p)
            for (const inv::Stack& st : owner.pages[p].slots)
                if (st.itemId && st.count && st.itemId != only && MarkOf(st.itemId) == static_cast<int>(i)) {
                    several = only != 0;
                    only = st.itemId;
                }
        if (!only || several)
            continue;
        inv::ItemDesc d = inv::Describe(only);
        if (!d.known || d.maxStack > 1)
            continue;
        Log("keep marks: %s pinned to item %u (%s)", m.name.c_str(), only, d.shown);
        m.name += " #" + std::to_string(only);
        pinned = true;
    }
    if (pinned) {
        g_cache.clear();
        SaveNames();
    }
}

void SetBackpack(const inv::Owner& backpack)
{
    ForgetLeftGear(g_backpack, backpack);
    g_backpack = backpack;
    if (MarksSettled() && !g_holding)
        PinOldGearMarks(backpack);
    UpdateKeptSlots();
}

bool Any()
{
    return !g_marks.empty();
}

bool IsKept(uint32_t itemId)
{
    return MarkOf(itemId) >= 0;
}

void PlanFor(const inv::Owner& backpack, Plan& out)
{
    std::vector<uint32_t> amounts, frees;
    std::vector<const std::vector<uint64_t>*> prefer, freed;
    for (size_t i = 0; i < g_marks.size(); ++i) {
        const Mark& m = g_marks[i];
        amounts.push_back(m.amount);
        prefer.push_back(&m.prefer);
        // This backpack may be newer than the last read (a deposit's later pass): what left the
        // backpack since then comes off what may go.
        frees.push_back(m.amount == kAll ? FreeNow(m, StoredFor(backpack, static_cast<int>(i))) : 0);
        freed.push_back(&m.freed);
    }
    PlanCore(backpack, [](uint32_t item) { return MarkOf(item); }, amounts.data(), prefer.data(), true, out,
             frees.data(), freed.data());
}

void PlanForItems(const inv::Owner& backpack, const std::vector<KeptItem>& items, Plan& out)
{
    std::vector<uint32_t> amounts;
    for (const KeptItem& k : items)
        amounts.push_back(k.amount);
    auto markOf = [&](uint32_t item) {
        for (size_t i = 0; i < items.size(); ++i)
            if (item && items[i].itemId == item)
                return static_cast<int>(i);
        return -1;
    };
    PlanCore(backpack, markOf, amounts.data(), nullptr, false, out);
}

std::vector<KeptItem> Snapshot(uint64_t* version)
{
    std::lock_guard<std::mutex> lock(g_snapshotLock);
    if (version)
        *version = g_snapshotVersion;
    return g_snapshot;
}

// A chat line: "Quick Deposit: " and the text in the mod's language.
template <typename... Args>
void Say(char* line, size_t size, lang::Str s, Args... args)
{
    int n = _snprintf_s(line, size, _TRUNCATE, "Quick Deposit: ");
    if (n > 0)
        _snprintf_s(line + n, size - n, _TRUNCATE, lang::T(s), args...);
}

// How many of the item your backpack's storage rows hold (the stacks a deposit may take).
uint32_t StoredCount(uint32_t itemId)
{
    uint64_t n = 0;
    for (int p = 0; p < g_backpack.pageCount; ++p) {
        const inv::Page& page = g_backpack.pages[p];
        if ((page.kind != inv::PageKind::Generic && page.kind != inv::PageKind::Virtual) ||
            inv::IsHotbarPage(g_backpack, page.entity))
            continue;
        for (const inv::Stack& st : page.slots)
            n += st.itemId == itemId ? st.count : 0;
    }
    return static_cast<uint32_t>(n < kAll ? n : kAll - 1);
}

// K on a stack of an item that stacks: the whole type is kept, also what you pick up later. K on
// a kept stack lets just that stack go; K on the last kept one ends the mark. Old marks with an
// amount ("Straw x50") turn into this on the first K.
void ToggleType(uint32_t itemId, uint32_t keptHere, uint64_t key, const char* name, const char* shown)
{
    int mark = MarkOf(itemId);
    uint32_t total = StoredCount(itemId);
    char line[256];
    if (mark < 0) {
        g_marks.push_back({name, kAll});
        Say(line, sizeof(line), lang::Str::KeepingEvery, shown);
    } else {
        Mark& m = g_marks[mark];
        if (m.amount != kAll) { // an amount mark: what it does not keep goes, as before
            uint32_t kept = 0;
            for (const KeptSlot& k : g_keptSlots)
                kept += k.itemId == itemId ? k.amount : 0;
            m.amount = kAll;
            m.prefer.clear();
            m.free = total > kept ? total - kept : 0;
            m.seen = total;
            m.freed.clear();
        }
        uint32_t keptNow = total > FreeNow(m, total) ? total - FreeNow(m, total) : 0;
        if (keptHere && keptNow <= keptHere) {
            g_marks.erase(g_marks.begin() + mark);
            Say(line, sizeof(line), lang::Str::NoLongerKeeping, shown);
        } else if (keptHere) {
            m.free = FreeNow(m, total) + keptHere;
            m.seen = total;
            m.freed.erase(std::remove(m.freed.begin(), m.freed.end(), key), m.freed.end());
            m.freed.insert(m.freed.begin(), key);
            Say(line, sizeof(line), lang::Str::FreedStack, keptHere, shown);
        } else {
            // A stack that goes: kept again.
            uint32_t here = 0;
            for (int p = 0; p < g_backpack.pageCount; ++p)
                if (static_cast<uint64_t>(g_backpack.pages[p].entity) == key >> 8)
                    here = g_backpack.pages[p].slots[key & 0xff].count;
            uint32_t now = FreeNow(m, total);
            m.free = now > here ? now - here : 0;
            m.seen = total;
            m.freed.erase(std::remove(m.freed.begin(), m.freed.end(), key), m.freed.end());
            Say(line, sizeof(line), lang::Str::KeepingEvery, shown);
        }
    }
    g_cache.clear();
    SaveNames();
    UpdateKeptSlots();
    chat::Post(line);
    Log("keep marks: %s (item %u)", line + 15, itemId);
}

// K on a stack: a stack that is kept (in part) comes off the kept amount, any other is added to it.
bool ToggleStack(uint32_t itemId, uint32_t count, uint32_t keptHere, uint64_t key)
{
    char name[96];
    if (!*NameOf(itemId, name, sizeof(name)) || !count)
        return false;
    inv::ItemDesc desc = inv::Describe(itemId);
    const char* shown = desc.shown; // chat: the name as the game shows it
    if (desc.maxStack > 1) {
        ToggleType(itemId, keptHere, key, name, shown);
        return true;
    }
    int mark = MarkOf(itemId);
    uint32_t kept = 0; // what the mark keeps now
    for (const KeptSlot& k : g_keptSlots)
        kept += k.itemId == itemId ? k.amount : 0;
    // Adding keeps a mark's amount beyond what the backpack holds right now; unmarking counts
    // only what is kept here, so K on every kept stack always ends the mark.
    if (!keptHere && mark >= 0 && g_marks[mark].amount != kAll && g_marks[mark].amount > kept)
        kept = g_marks[mark].amount;
    uint32_t amount = keptHere ? kept - (keptHere < kept ? keptHere : kept) : kept + count;
    char line[256];
    if (!amount) {
        if (mark >= 0)
            g_marks.erase(g_marks.begin() + mark);
        Say(line, sizeof(line), lang::Str::NoLongerKeeping, shown);
    } else {
        if (mark < 0) {
            g_marks.push_back({name, amount});
            mark = static_cast<int>(g_marks.size()) - 1;
        }
        Mark& m = g_marks[mark];
        m.amount = amount;
        m.prefer.erase(std::remove(m.prefer.begin(), m.prefer.end(), key), m.prefer.end());
        if (!keptHere)
            m.prefer.push_back(key);
        Say(line, sizeof(line), lang::Str::KeepingCount, amount, shown);
    }
    g_cache.clear();
    SaveNames();
    UpdateKeptSlots();
    chat::Post(line);
    Log("keep marks: %s (item %u)", line + 15, itemId);
    return true;
}

bool AnyAmountMark()
{
    for (const Mark& m : g_marks)
        if (m.amount != kAll)
            return true;
    return false;
}

int UnmarkStored(const inv::Owner& player, const inv::Owner* chests, int count)
{
    int ended = 0;
    if (!MarksSettled())
        return 0;
    for (int c = 0; c < count; ++c)
        for (int p = 0; p < chests[c].pageCount; ++p)
            for (const inv::Stack& st : chests[c].pages[p].slots) {
                int mark = st.itemId ? MarkOf(st.itemId) : -1;
                if (mark < 0 || g_marks[mark].amount == kAll)
                    continue;
                inv::ItemDesc d = inv::Describe(st.itemId);
                if (!d.known || d.maxStack > 1)
                    continue;
                // A mark is by name, and items of different tiers can share it (Eternal Ice Bolt I
                // and IV): the mark stays while you hold any item it covers.
                if (MarkHeld(player, mark))
                    continue;
                g_marks.erase(g_marks.begin() + mark);
                g_cache.clear();
                char line[256];
                Say(line, sizeof(line), lang::Str::NoLongerKeeping, d.shown);
                chat::Post(line);
                Log("keep marks: %s - put away in chest %u (item %u)", line + 15, chests[c].entity, st.itemId);
                ++ended;
            }
    if (ended) {
        SaveNames();
        UpdateKeptSlots();
    }
    return ended;
}

bool Toggle(uint32_t itemId, uint32_t count)
{
    char name[96];
    if (!*NameOf(itemId, name, sizeof(name)))
        return false;
    inv::ItemDesc desc = inv::Describe(itemId);
    const char* shown = desc.shown; // chat: the name as the game shows it
    int mark = MarkOf(itemId);
    char line[256];
    if (mark >= 0) {
        g_marks.erase(g_marks.begin() + mark);
        Say(line, sizeof(line), lang::Str::NoLongerKeeping, shown);
    } else {
        uint32_t amount = count ? count : kAll;
        g_marks.push_back({name, amount});
        if (amount == kAll)
            Say(line, sizeof(line), lang::Str::KeepingEvery, shown);
        else
            Say(line, sizeof(line), lang::Str::KeepingCount, amount, shown);
    }
    g_cache.clear();
    SaveNames();
    UpdateKeptSlots();
    chat::Post(line);
    Log("keep marks: %s (item %u)", line + 15, itemId);
    return true;
}

void Tick()
{
    // The UI loop does not run exactly once per drawn frame: some ticks see no slots drawn since
    // the last one. The last slots drawn count until none have been drawn for kSlotsStaleMs.
    constexpr ULONGLONG kSlotsStaleMs = 250;
    static ULONGLONG lastDrawn = 0;
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_slotsLock);
        if (!g_thisFrame.empty()) {
            g_lastFrame.swap(g_thisFrame);
            g_thisFrame.clear();
            lastDrawn = now;
        } else if (now - lastDrawn > kSlotsStaleMs) {
            g_lastFrame.clear();
        }
    }
    if (!g_key || !g_originalSlot)
        return;
    // Bit 0: pressed since the last call, so a tap between two ticks still counts.
    SHORT state = GetAsyncKeyState(g_key);
    bool down = (state & 0x8000) != 0;
    bool pressed = (down && !g_keyDown) || (!down && (state & 1));
    g_keyDown = down;
    if (!pressed)
        return;
    // Only over an item slot of an open inventory.
    float x, y;
    if (g_lastFrame.empty() || !CursorInUi(x, y))
        return;
    for (const SlotSeen& s : g_lastFrame) {
        if (x >= s.rect[0] && x < s.rect[0] + s.rect[2] && y >= s.rect[1] && y < s.rect[1] + s.rect[3]) {
            uint32_t count;
            memcpy(&count, s.data + 0x10, sizeof(count));
            uint64_t key = static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(s.data)) << 8 | s.data[4];
            ToggleStack(s.itemId, count, KeptHere(key), key);
            return;
        }
    }
    Log("keep marks: key at %.0f, %.0f is over none of the %d backpack slots", x, y, static_cast<int>(g_lastFrame.size()));
}

bool ChestShown()
{
    return GetTickCount64() - g_chestSlotAt <= 250;
}

bool BackpackShown()
{
    std::lock_guard<std::mutex> lock(g_slotsLock);
    return !g_lastFrame.empty() || !g_thisFrame.empty();
}

std::vector<SlotSeen> LastFrameSlots()
{
    std::lock_guard<std::mutex> lock(g_slotsLock);
    return g_lastFrame;
}

} // namespace keep

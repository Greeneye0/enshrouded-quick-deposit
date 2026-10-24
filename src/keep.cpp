// Kept items, marked in the inventory (build 1273043).
//
// 140bf4180 doItemSlot(slot, style, ..) draws one inventory slot: *slot -> slot data {+0 page
// entity, +4 slot index, +8 item id (0: empty), +0x10 count}. Only slots of your backpack's
// storage pages are marked (not chests, not the hotbar). Its first image is the slot background, drawn through
// 14118ca30 DrawImageReal(widget, rect{x, y, w, h}, u32 id, u64 colour) (reached from the
// DrawImage thunk "mov r9, [r9]; mov r8d, [r8]; jmp DrawImageReal"). While a kept item's slot
// draws, its background is followed by the game's nine-slice glow boxes (FbUiPrimitives
// boxInnerGlow assets + 0xd370, glowBoxSmall + 0xd390) in teal, drawn with 140a3a1b0
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
#include "chat.h"
#include "inventory.h"
#include "keep.h"
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

constexpr size_t kAssetBoxInnerGlow = 0xd250 + 0x120;
constexpr size_t kAssetGlowBoxSmall = 0xd250 + 0x140;
constexpr uint32_t kKeepColour = 0xFFA5CA5D; // teal (0xAABBGGRR), apart from Find's gold
constexpr int kMaxSlots = 256;
constexpr uint32_t kMaxExact = 5000;  // larger kept amounts skip the exact-sum search

struct Mark {
    std::string name; // item name, as in keepItems
    uint32_t amount;  // kAll: every one
    std::vector<uint64_t> prefer; // slots you pressed keepKey on (page entity << 8 | slot), kept first
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
uint32_t KeptHere(uint64_t key);
int g_key = 'K';
bool g_keyDown = false;

// The slot being drawn (game thread).
uint32_t g_slotItem = 0;
bool g_slotBackground = false; // its background is drawn
uint8_t g_slotData[32];

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

const char* NameOf(uint32_t itemId, char* buf, size_t size)
{
    inv::ItemDesc d = inv::Describe(itemId);
    strncpy_s(buf, size, d.known ? d.name : "", _TRUNCATE);
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
    bool backpack = false;
    for (int p = 0; p < g_backpack.pageCount; ++p)
        backpack |= g_backpack.pages[p].entity == page && IsBackpackRow(p);
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

int MarkOf(uint32_t itemId)
{
    if (!itemId || g_marks.empty())
        return -1;
    auto it = g_cache.find(itemId);
    if (it != g_cache.end())
        return it->second;
    inv::ItemDesc d = inv::Describe(itemId);
    int found = -1;
    for (size_t i = 0; i < g_marks.size() && found < 0 && d.known; ++i)
        if (_stricmp(g_marks[i].name.c_str(), d.name) == 0 || _stricmp(g_marks[i].name.c_str(), d.raw) == 0)
            found = static_cast<int>(i);
    if (d.known) // unknown items may become known once the item database is seen
        g_cache[itemId] = found;
    return found;
}

void UpdateKeptSlots()
{
    g_keptSlots.clear();
    if (!g_backpack.pageCount || g_marks.empty())
        return;
    static Plan plan;
    PlanFor(g_backpack, plan);
    for (int p = 0; p < g_backpack.pageCount; ++p)
        for (int s = 0; s < inv::kSlotsPerPage; ++s)
            if (plan.keep[p][s])
                g_keptSlots.push_back({static_cast<uint64_t>(g_backpack.pages[p].entity) << 8 | s,
                                       g_backpack.pages[p].slots[s].itemId, plan.keep[p][s]});
    // The kept stacks stay the preferred ones, so a stack you pick up that would do as well (a
    // second full stack) does not take the mark.
    for (size_t i = 0; i < g_marks.size(); ++i) {
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

void SaveNames()
{
    std::string list;
    for (const Mark& m : g_marks) {
        if (!list.empty())
            list += ",";
        list += m.name;
        if (m.amount != kAll)
            list += " x" + std::to_string(m.amount);
    }
    WritePrivateProfileStringA("QuickDeposit", "keepItems", list.c_str(), greeneye::DataPath("quick-deposit.ini"));
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

void Load(const char* list, int key)
{
    g_marks.clear();
    g_cache.clear();
    g_key = key;
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
    if (!g_marks.empty())
        Log("keep marks: %d item types kept", static_cast<int>(g_marks.size()));
    UpdateKeptSlots();
}

void SetBackpack(const inv::Owner& backpack)
{
    g_backpack = backpack;
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
    memset(&out, 0, sizeof(out));
    struct Stack {
        int page, slot;
        uint32_t count;
    };
    std::vector<uint32_t> done;
    for (int p0 = 0; p0 < backpack.pageCount; ++p0) {
        for (int s0 = 0; s0 < inv::kSlotsPerPage; ++s0) {
            uint32_t item = backpack.pages[p0].slots[s0].itemId;
            int mark = MarkOf(item);
            if (mark < 0 || std::find(done.begin(), done.end(), item) != done.end())
                continue;
            done.push_back(item);
            std::vector<Stack> stacks;
            for (int p = 0; p < backpack.pageCount; ++p) {
                const inv::Page& page = backpack.pages[p];
                if ((page.kind != inv::PageKind::Generic && page.kind != inv::PageKind::Virtual) ||
                    inv::IsHotbarPage(backpack, page.entity))
                    continue;
                for (int s = 0; s < inv::kSlotsPerPage; ++s)
                    if (page.slots[s].itemId == item && page.slots[s].count)
                        stacks.push_back({p, s, page.slots[s].count});
            }
            // Among stacks that would do as well, the ones you marked (two full stacks: the one you
            // pressed keepKey on). Earlier stacks are taken first below.
            const std::vector<uint64_t>& prefer = g_marks[mark].prefer;
            std::stable_partition(stacks.begin(), stacks.end(), [&](const Stack& st) {
                uint64_t key = static_cast<uint64_t>(backpack.pages[st.page].entity) << 8 | st.slot;
                return std::find(prefer.begin(), prefer.end(), key) != prefer.end();
            });
            uint32_t left = g_marks[mark].amount;
            if (left == kAll) {
                for (const Stack& st : stacks)
                    out.keep[st.page][st.slot] = st.count;
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

// K on a stack: a stack that is kept (in part) comes off the kept amount, any other is added to it.
bool ToggleStack(uint32_t itemId, uint32_t count, uint32_t keptHere, uint64_t key)
{
    char name[64];
    if (!*NameOf(itemId, name, sizeof(name)) || !count)
        return false;
    int mark = MarkOf(itemId);
    uint32_t kept = 0; // what the mark keeps now
    for (const KeptSlot& k : g_keptSlots)
        kept += k.itemId == itemId ? k.amount : 0;
    // Adding keeps a mark's amount beyond what the backpack holds right now; unmarking counts
    // only what is kept here, so K on every kept stack always ends the mark.
    if (!keptHere && mark >= 0 && g_marks[mark].amount != kAll && g_marks[mark].amount > kept)
        kept = g_marks[mark].amount;
    uint32_t amount = keptHere ? kept - (keptHere < kept ? keptHere : kept) : kept + count;
    char line[128];
    if (!amount) {
        if (mark >= 0)
            g_marks.erase(g_marks.begin() + mark);
        _snprintf_s(line, _TRUNCATE, "Quick Deposit: no longer keeping %s", name);
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
        _snprintf_s(line, _TRUNCATE, "Quick Deposit: keeping %u %s", amount, name);
    }
    g_cache.clear();
    SaveNames();
    UpdateKeptSlots();
    chat::Post(line);
    Log("keep marks: %s (item %u)", line + 15, itemId);
    return true;
}

bool Toggle(uint32_t itemId, uint32_t count)
{
    char name[64];
    if (!*NameOf(itemId, name, sizeof(name)))
        return false;
    int mark = MarkOf(itemId);
    char line[128];
    if (mark >= 0) {
        g_marks.erase(g_marks.begin() + mark);
        _snprintf_s(line, _TRUNCATE, "Quick Deposit: no longer keeping %s", name);
    } else {
        uint32_t amount = count ? count : kAll;
        g_marks.push_back({name, amount});
        if (amount == kAll)
            _snprintf_s(line, _TRUNCATE, "Quick Deposit: keeping every %s", name);
        else
            _snprintf_s(line, _TRUNCATE, "Quick Deposit: keeping %u %s", amount, name);
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

// Key hints in the hint bar (build 1273043).
//
// 140cd5af0 doActionHints(span{entries, count}, options) draws the bar at the bottom of a menu
// screen; screens collect their entries in a list at *(G + 0xd0) + 0x32e18 (16 x 0x48 bytes,
// count byte at +0x480) and pass it as the span. Each entry is drawn by
// 140cd5300 doActionHint(entry, params):
//   +0x00 u8 input action: the key glyph comes from its binding record,
//         *(G + 0xd0) + 0x10 + action * 0xc4 + 0xf26c (keyboard: u16 device 1 at +0, u16 HID
//         usage code at +2, a modifier the same way at +0x0c)
//   +0x1a u8 bit 2: shown
//   +0x1b u8: use the text at +0x20 {utf8, length, fnv1a64 hash} (else the action's own name)
// Quick Deposit copies the game's own "Sort Inventory" entry (its action is the one bound to G)
// for its hints, so they look the same, and inserts them right after it. The chest screen has no
// such entry: there another enabled entry is copied and the hints go before the last two. A hint
// whose key differs from its copied action's (keepKey; G on the chest screen) gets it this way:
// while that one entry draws, the action's binding record says that key, and is put back right
// after. Only while your backpack is on screen.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../../common/greeneye.h"
#include "hints.h"
#include "keep.h"
#include "log.h"
#include "scan.h"

namespace hints {
namespace {

struct Span {
    uint8_t* data;
    uint64_t count;
};
using HintsFn = void (*)(Span* span, void* options);
using HintFn = uint64_t (*)(uint8_t* entry, void* params);

HintsFn g_originalHints = nullptr;
HintFn g_originalHint = nullptr;
void** g_root = nullptr;

constexpr size_t kEntry = 0x48;
constexpr int kMaxEntries = 16;
constexpr size_t kUiBindings = 0x10 + 0xf26c;
constexpr size_t kBinding = 0xc4;
constexpr uint16_t kHidG = 0x0a;

bool g_depositHint = false;
uint16_t g_keepHid = 0; // 0: no keep hint
alignas(16) uint8_t g_entries[(kMaxEntries + 2) * kEntry];
// Quick Deposit's hints among g_entries that show another key than their copied action's, while
// the bar draws.
struct Swap {
    uint8_t* entry;
    uint16_t hid;
};
Swap g_swaps[2];
int g_swapCount = 0;

struct Text {
    const char* utf8;
    size_t len;
    uint64_t hash;
};

Text MakeText(const char* utf8)
{
    Text t{utf8, strlen(utf8), 0xcbf29ce484222325ull};
    for (size_t i = 0; i < t.len; ++i)
        t.hash = (t.hash ^ static_cast<uint8_t>(utf8[i])) * 0x100000001b3ull;
    return t;
}

const Text& DepositText()
{
    static const Text t = MakeText("Twice: Quick Deposit");
    return t;
}

const Text& KeepText()
{
    static const Text t = MakeText("Keep in Backpack");
    return t;
}

// The HID usage code the game's bindings use for a virtual key: letters, digits, F1-F12.
uint16_t HidOf(int vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<uint16_t>(0x04 + vk - 'A');
    if (vk >= '1' && vk <= '9')
        return static_cast<uint16_t>(0x1e + vk - '1');
    if (vk == '0')
        return 0x27;
    if (vk >= VK_F1 && vk <= VK_F12)
        return static_cast<uint16_t>(0x3a + vk - VK_F1);
    return 0;
}

uint8_t* Ui()
{
    void* g = g_root ? *g_root : nullptr;
    uint8_t* ui = g ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g) + 0xd0) : nullptr;
    return ui && *reinterpret_cast<void**>(ui) ? ui : nullptr;
}

uint8_t* Binding(uint8_t action)
{
    uint8_t* ui = Ui();
    return ui ? ui + kUiBindings + action * kBinding : nullptr;
}

// The game's "Sort Inventory" entry: an entry shown with the action bound to G alone.
int SortEntry(const Span* span)
{
    for (uint64_t i = 0; i < span->count && i < kMaxEntries; ++i) {
        const uint8_t* e = span->data + i * kEntry;
        const uint8_t* b = Binding(e[0]);
        uint16_t device, key, modifier;
        if (!b || !(e[0x1a] & 4))
            continue;
        memcpy(&device, b, 2);
        memcpy(&key, b + 2, 2);
        memcpy(&modifier, b + 0x0c, 2);
        if (device == 1 && key == kHidG && !modifier)
            return static_cast<int>(i);
    }
    return -1;
}

// Without "Sort Inventory" (the chest screen sorts with small buttons): a shown, enabled entry
// with a keyboard key, to copy.
int TemplateEntry(const Span* span)
{
    for (uint64_t i = 0; i < span->count && i < kMaxEntries; ++i) {
        const uint8_t* e = span->data + i * kEntry;
        const uint8_t* b = Binding(e[0]);
        uint16_t device;
        if (!b || (e[0x1a] & 7) != 7)
            continue;
        memcpy(&device, b, 2);
        if (device == 1)
            return static_cast<int>(i);
    }
    return -1;
}

void SetText(uint8_t* entry, const Text& t)
{
    memcpy(entry + 0x20, &t, sizeof(t));
    entry[0x1b] = 1;
}

#ifdef QD_BRIDGE
char g_lastEntries[2048];

void Describe(const Span* span)
{
    int n = 0;
    for (uint64_t i = 0; i < span->count && i < kMaxEntries + 2; ++i) {
        const uint8_t* e = span->data + i * kEntry;
        Text t;
        memcpy(&t, e + 0x20, sizeof(t));
        char text[64] = "";
        if (e[0x1b] && t.utf8 && t.len < sizeof(text))
            memcpy(text, t.utf8, t.len);
        const uint8_t* b = Binding(e[0]);
        n += _snprintf_s(g_lastEntries + n, sizeof(g_lastEntries) - n, _TRUNCATE,
                         "action %u key %02x flags %02x/%02x text '%s'\n", e[0], b ? b[2] : 0, e[0x1a], e[0x1b], text);
        if (n < 0)
            break;
    }
}
#endif

void HintsBody(Span* span, void* options)
{
    g_swapCount = 0;
    bool usable = span && span->data && span->count && span->count <= kMaxEntries && keep::BackpackShown();
    int sort = usable ? SortEntry(span) : -1;
    int copy = sort >= 0 ? sort : usable ? TemplateEntry(span) : -1;
    if (copy < 0 || (!g_depositHint && !g_keepHid)) {
        g_originalHints(span, options);
        return;
    }
    // Right after "Sort Inventory"; without it before the last two (Show Tutorial, Close/Back).
    size_t insert = sort >= 0 ? sort + 1 : span->count >= 3 ? span->count - 2 : span->count;
    size_t before = insert * kEntry;
    size_t after = (span->count - insert) * kEntry;
    const uint8_t* model = span->data + copy * kEntry;
    memcpy(g_entries, span->data, before);
    uint8_t* at = g_entries + before;
    if (g_depositHint) {
        memcpy(at, model, kEntry);
        SetText(at, DepositText());
        if (sort < 0)
            g_swaps[g_swapCount++] = {at, kHidG};
        at += kEntry;
    }
    if (g_keepHid) {
        memcpy(at, model, kEntry);
        SetText(at, KeepText());
        g_swaps[g_swapCount++] = {at, g_keepHid};
        at += kEntry;
    }
    memcpy(at, span->data + before, after);
    at += after;
    Span mine{g_entries, static_cast<uint64_t>((at - g_entries) / kEntry)};
#ifdef QD_BRIDGE
    Describe(&mine);
#endif
    g_originalHints(&mine, options);
    g_swapCount = 0;
}

void HintsHook(Span* span, void* options)
{
    greeneye::Lease lease;
    if (!lease) {
        g_originalHints(span, options);
        return;
    }
    HintsBody(span, options);
}

uint64_t HintHook(uint8_t* entry, void* params)
{
    greeneye::Lease lease;
    uint16_t hid = 0;
    for (int i = 0; lease && entry && i < g_swapCount; ++i)
        if (g_swaps[i].entry == entry)
            hid = g_swaps[i].hid;
    uint8_t* b = hid ? Binding(entry[0]) : nullptr;
    if (!b)
        return g_originalHint(entry, params);
    // This hint's copied action shows its key for this one entry.
    uint8_t saved[kBinding];
    memcpy(saved, b, kBinding);
    uint16_t device = 1;
    memcpy(b, &device, 2);
    memcpy(b + 2, &hid, 2);
    memset(b + 0x0c, 0, 4); // no modifier
    uint64_t result = g_originalHint(entry, params);
    memcpy(b, saved, kBinding);
    return result;
}

} // namespace

bool Init(bool depositHint, int keepKey)
{
    g_depositHint = depositHint;
    g_keepHid = HidOf(keepKey);
    if (!g_depositHint && !g_keepHid)
        return true;
    // doActionHints: mov [rsp+20h], rbx; push rbp, rsi, rdi, r12, r13 (12 bytes)
    auto* bar = FindUnique("48 89 5C 24 20 55 56 57 41 54 41 55 48 8D 6C 24 C9 48 81 EC B0 00 00 00");
    // doActionHint: mov rax, rsp; push rbp, rsi, rdi, r14; lea rbp, [rax-228h] (15 bytes)
    auto* one = FindUnique("48 8B C4 55 56 57 41 56 48 8D A8 D8 FD FF FF 48 81 EC 08 03 00 00 F6 41 1A 04");
    g_root = reinterpret_cast<void**>(ResolveRipConsensus("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00", 3, 7));
    if (!bar || !one || !g_root) {
        Log("hints: signatures not found (%p %p %p) - no key hints", bar, one, reinterpret_cast<void*>(g_root));
        return false;
    }
    g_originalHint = static_cast<HintFn>(InstallHook(one, reinterpret_cast<void*>(&HintHook), 15));
    g_originalHints = static_cast<HintsFn>(InstallHook(bar, reinterpret_cast<void*>(&HintsHook), 12));
    if (!g_originalHint) {
        g_keepHid = 0; // the hints would show the copied action's key
        g_depositHint = false;
    }
    if (!g_originalHints) {
        Log("hints: hook install failed - no key hints");
        return false;
    }
    return true;
}

#ifdef QD_BRIDGE
const char* LastEntries()
{
    return g_lastEntries;
}
#endif

} // namespace hints

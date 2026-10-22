#include <windows.h>
#include <atomic>
#include <string>
#include <deque>
#include <mutex>
#include <cstdio>
#include "chat.h"
#include "../../common/greeneye.h"
#include "arealoot.h"
#include "log.h"
#include "scan.h"

namespace chat {
namespace {

using ChatAppendFn = uint8_t* (*)(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a,
                                  uint32_t b, uint32_t c, uint64_t d, uint64_t* time);

ChatAppendFn g_originalAppend = nullptr;
std::atomic<void*> g_client{nullptr};
constexpr size_t kClientChat = 0xf8;
constexpr uint8_t kTypePlayerText = 0; // measured: a typed message arrives as type 0

// Sender fields of this client, copied from the first system line in its chat.
std::atomic<bool> g_haveSender{false};
uint32_t g_senderC = 0;
uint64_t g_senderD = 0;
uint64_t g_lastTime = 0;
uint64_t* g_persist = nullptr; // hot reload: {haveSender, c, d, lastTime}, kept by the loader

#ifdef QD_BRIDGE
// Dev only (the bridge's area-loot experiments): a "/loot ..." line arriving at the host's chat
// (not this client's own log): answered in its place, so the command never reaches the others.
bool HostCommand(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b, uint32_t c,
                 uint64_t d, uint64_t* time)
{
    if (type != kTypePlayerText || !text || !text[0] || text[1] < 5)
        return false;
    std::string answer;
    if (!arealoot::HandleChat(reinterpret_cast<const char*>(text[0]), static_cast<size_t>(text[1]), d, answer))
        return false;
    uint64_t str[2] = {reinterpret_cast<uint64_t>(answer.c_str()), answer.size()};
    g_originalAppend(result, store, type, str, a, b, c, d, time);
    return true;
}

bool SafeHostCommand(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b,
                     uint32_t c, uint64_t d, uint64_t* time)
{
    __try {
        return HostCommand(result, store, type, text, a, b, c, d, time);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

#ifdef QD_BRIDGE
std::mutex g_recentLock;
std::deque<std::string> g_recent;

void Remember(void* store, uint8_t type, const uint64_t* text, uint64_t d)
{
    void* cl = g_client.load(std::memory_order_relaxed);
    bool mine = cl && store == *reinterpret_cast<void**>(static_cast<uint8_t*>(cl) + kClientChat);
    std::string body = text && text[0] ? std::string(reinterpret_cast<const char*>(text[0]), text[1] < 600 ? text[1] : 600) : "";
    char head[80];
    sprintf_s(head, "%s type=%u sender=%llX: ", mine ? "mine" : "host", type, static_cast<unsigned long long>(d));
    std::lock_guard<std::mutex> lock(g_recentLock);
    g_recent.push_back(head + body);
    if (g_recent.size() > 200)
        g_recent.pop_front();
}

void SafeRemember(void* store, uint8_t type, const uint64_t* text, uint64_t d)
{
    __try {
        Remember(store, type, text, d);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
#endif

// Quick Deposit's part of a chat line (the caller holds the lease). False: a host command
// answered it, keep it out of the chat.
bool ChatAppendOwn(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b, uint32_t c,
                   uint64_t d, uint64_t* time)
{
#ifdef QD_BRIDGE
    SafeRemember(store, type, text, d);
    {
        void* cl = g_client.load(std::memory_order_relaxed);
        bool clientLog = false;
        __try {
            clientLog = cl && store == *reinterpret_cast<void**>(static_cast<uint8_t*>(cl) + kClientChat);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!clientLog && SafeHostCommand(result, store, type, text, a, b, c, d, time))
            return false;
    }
#endif
    __try {
        void* cl = g_client.load(std::memory_order_relaxed);
        bool mine = cl && store == *reinterpret_cast<void**>(static_cast<uint8_t*>(cl) + kClientChat);
        if (mine && time && (time[1] & 0xFF))
            g_lastTime = time[0];
        // System lines (type != 0) carry this client's own sender fields; typed lines may come
        // from other players, so they are never used.
        if (mine && d && type != kTypePlayerText && !g_haveSender.load()) {
            g_senderC = c;
            g_senderD = d;
            g_haveSender = true;
            Log("chat: ready (sender %u/%llX)", c, static_cast<unsigned long long>(d));
        }
        if (g_persist && mine) {
            g_persist[1] = g_senderC;
            g_persist[2] = g_senderD;
            g_persist[3] = g_lastTime;
            g_persist[0] = g_haveSender.load();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return true;
}

uint8_t* ChatAppendHook(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b,
                        uint32_t c, uint64_t d, uint64_t* time)
{
    {
        // While it runs Quick Deposit's code (a hot reload waits for it); an older generation
        // just calls on.
        greeneye::Lease lease;
        if (lease && !ChatAppendOwn(result, store, type, text, a, b, c, d, time))
            return result; // answered by a /loot host command
    }
    return g_originalAppend(result, store, type, text, a, b, c, d, time);
}

} // namespace

#ifdef QD_BRIDGE
std::vector<std::string> RecentLines(size_t max)
{
    std::lock_guard<std::mutex> lock(g_recentLock);
    size_t from = g_recent.size() > max ? g_recent.size() - max : 0;
    return std::vector<std::string>(g_recent.begin() + from, g_recent.end());
}
#endif

bool Init()
{
    // Its first 13 bytes are what Auto Loot's hook replaces; the 6 after them let FindUnique find
    // it behind that hook (common/jumppatch.h FindBehindHook) whichever mod loads first.
    uint8_t* fn = FindUnique("40 55 56 41 54 41 55 41 57 48 83 EC 30 49 8B 41 08 33 ED");
    if (!fn) {
        Log("signature not found: ChatAppend");
        return false;
    }
    g_originalAppend = static_cast<ChatAppendFn>(InstallHook(fn, reinterpret_cast<void*>(&ChatAppendHook), 13));
    return g_originalAppend != nullptr;
}

void UsePersistentState(uint64_t* state)
{
    g_persist = state;
    if (state && state[0]) {
        g_senderC = static_cast<uint32_t>(state[1]);
        g_senderD = state[2];
        g_lastTime = state[3];
        g_haveSender = true;
        Log("chat: ready (sender %u/%llX, kept over the reload)", g_senderC, static_cast<unsigned long long>(g_senderD));
    }
}

void SetClient(void* client)
{
    g_client = client;
}

void Post(const char* text)
{
    Log("chat> %s", text);
    void* cl = g_client.load();
    if (!cl || !g_haveSender.load())
        return;
    __try {
        void* store = *reinterpret_cast<void**>(static_cast<uint8_t*>(cl) + kClientChat);
        if (!store)
            return;
        uint64_t str[2] = {reinterpret_cast<uint64_t>(text), static_cast<uint64_t>(lstrlenA(text))};
        // Current game time, as the client stamps received lines (140a3d9b0 reads it from
        // *(client + 0x18) + 0xe78); an old stamp keeps the chat from showing the line.
        uint64_t now = 0;
        if (auto* clock = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(cl) + 0x18))
            now = *reinterpret_cast<uint64_t*>(clock + 0xe78);
        if (!now)
            now = g_lastTime;
        uint64_t time[2] = {now, now ? 1u : 0u};
        uint8_t result[16] = {};
        g_originalAppend(result, store, kTypePlayerText, str, 0, 0, g_senderC, g_senderD, time);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("chat: posting failed");
    }
}

} // namespace chat

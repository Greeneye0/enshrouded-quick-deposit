#include <windows.h>
#include <atomic>
#include "chat.h"
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

uint8_t* ChatAppendHook(uint8_t* result, void* store, uint8_t type, uint64_t* text, uint32_t a, uint32_t b,
                        uint32_t c, uint64_t d, uint64_t* time)
{
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return g_originalAppend(result, store, type, text, a, b, c, d, time);
}

} // namespace

bool Init()
{
    uint8_t* fn = FindUnique("40 55 56 41 54 41 55 41 57 48 83 EC 30");
    if (!fn) {
        Log("signature not found: ChatAppend");
        return false;
    }
    g_originalAppend = static_cast<ChatAppendFn>(InstallHook(fn, reinterpret_cast<void*>(&ChatAppendHook), 13));
    return g_originalAppend != nullptr;
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

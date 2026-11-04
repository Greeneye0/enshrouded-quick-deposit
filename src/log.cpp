#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <share.h>
#include "log.h"
#include "../../common/greeneye.h"
#include "../../common/async_log.h"

static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static async_log::Writer g_writer;
static std::deque<LogLine> g_ring;
static uint64_t g_seq = 0;
constexpr size_t kRingSize = 2000;

void LogInit(bool reload)
{
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
    // Shared, so a reloaded Quick Deposit (hot reload) can append while the old one keeps its handle.
    FILE* file = _fsopen(greeneye::DataPath("quick-deposit.log"), reload ? "a" : "w", _SH_DENYNO);
    g_writer.Open(file);
}

void Log(const char* fmt, ...)
{
    if (!g_logReady)
        return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, args);
    va_end(args);

    EnterCriticalSection(&g_logLock);
    // On a thread of its own: a slow disk must not stall the caller.
    {
        const size_t length = strlen(line);
        if (length + 1 < sizeof(line)) {
            line[length] = '\n';
            g_writer.Write(line, length + 1);
            line[length] = 0;
        } else {
            g_writer.Write(line, length);
            g_writer.Write("\n", 1);
        }
    }
    g_ring.push_back({++g_seq, line});
    if (g_ring.size() > kRingSize)
        g_ring.pop_front();
    LeaveCriticalSection(&g_logLock);
}

uint64_t LogsSince(uint64_t since, size_t max, std::vector<LogLine>& out)
{
    if (!g_logReady)
        return 0;
    EnterCriticalSection(&g_logLock);
    for (const LogLine& l : g_ring)
        if (l.seq > since && out.size() < max)
            out.push_back(l);
    uint64_t latest = g_seq;
    LeaveCriticalSection(&g_logLock);
    return latest;
}

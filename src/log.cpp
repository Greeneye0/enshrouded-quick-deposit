#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <share.h>
#include "log.h"

static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static FILE* g_logFile = nullptr;
static std::deque<LogLine> g_ring;
static uint64_t g_seq = 0;
constexpr size_t kRingSize = 2000;

void LogInit()
{
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
    g_logFile = _fsopen("quick-deposit.log", "w", _SH_DENYWR);
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
    if (g_logFile) {
        fputs(line, g_logFile);
        fputc('\n', g_logFile);
        fflush(g_logFile);
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

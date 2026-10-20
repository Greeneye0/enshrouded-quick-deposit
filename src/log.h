#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Appends a timestamped line to quick-deposit.log in the game folder, and keeps the most
// recent lines in memory for the bridge's "logs" op.
void LogInit();
void Log(const char* fmt, ...);

struct LogLine {
    uint64_t seq;
    std::string text; // "[hh:mm:ss] message"
};

// Lines with a sequence number above since, oldest first, at most max. Returns the latest
// sequence number (pass it back as since next time).
uint64_t LogsSince(uint64_t since, size_t max, std::vector<LogLine>& out);

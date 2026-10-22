#pragma once
#include <string>
#include <vector>
#include <cstdint>

// The client's chat log (client + 0xf8), filled by 140e546e0 ChatAppend(result, store, type,
// &{text, length}, a, b, c, d, &{time, valid}). Lines posted here exist only on this client:
// nothing is sent to the server or other players.
namespace chat {

bool Init();

// Hot reload: keep the sender fields in these 4 values, which outlive the runtime, and take
// them over from the previous runtime. Call before Init.
void UsePersistentState(uint64_t* state);

// The client object of the current frame (call every frame from the UI loop).
void SetClient(void* client);

// Adds a line to your own chat window. Needs one chat line to have arrived first (any system
// line at join does), whose sender fields are copied; until then it only logs. UI thread only.
void Post(const char* text);

#ifdef QD_BRIDGE
// Dev only: the last chat lines this process appended (any store), oldest first, as
// "<mine|host> type=<t> sender=<hex>: <text>".
std::vector<std::string> RecentLines(size_t max);
#endif

} // namespace chat

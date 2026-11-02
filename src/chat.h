#pragma once
#include <string>
#include <vector>
#include <cstdint>

// The client's chat log (client + 0xf8), filled by 140e546e0 ChatAppend(result, store, type,
// &{text, length}, a, b, c, d, &{time, valid}). Lines posted here exist only on this client:
// nothing is sent to the server or other players.
namespace chat {

// What a line is, for chat= in quick-deposit.ini: Detail (where things went, what stayed: full
// only), Summary (one line per deposit: short only), Notice (keep marks, problems: full and short).
enum class Kind { Detail, Summary, Notice };
enum class Mode { Full, Short, Off };
void SetMode(Mode mode);

bool Init();

// Hot reload: keep the sender fields in these 5 values, which outlive the runtime, and take
// them over from the previous runtime. Call before Init.
void UsePersistentState(uint64_t* state);

// The client object of the current frame (call every frame from the UI loop).
void SetClient(void* client);

// Adds a line to your own chat window, drawn like the game's system notices (their line type
// and sender fields, copied from the first one to arrive; until then it only logs). Always
// logged; shown as chat= allows. UI thread only.
void Post(const char* text, Kind kind = Kind::Notice);

#ifdef QD_BRIDGE
// Dev only: the last chat lines this process appended (any store), oldest first, as
// "<mine|host> type=<t> sender=<hex>: <text>".
std::vector<std::string> RecentLines(size_t max);
#endif

} // namespace chat

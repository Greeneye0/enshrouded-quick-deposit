#pragma once
#include <cstdint>

// The client's chat log (client + 0xf8), filled by 140e546e0 ChatAppend(result, store, type,
// &{text, length}, a, b, c, d, &{time, valid}). Lines posted here exist only on this client:
// nothing is sent to the server or other players.
namespace chat {

bool Init();

// The client object of the current frame (call every frame from the UI loop).
void SetClient(void* client);

// Adds a line to your own chat window. Needs one chat line to have arrived first (any system
// line at join does), whose sender fields are copied; until then it only logs. UI thread only.
void Post(const char* text);

} // namespace chat

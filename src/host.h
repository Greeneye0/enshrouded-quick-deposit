#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The host's side of Quick Deposit: Quick Stack Stations leave kept items in the backpack.
//
// A station's area deposit runs on the host (a dedicated server, or the game of whoever hosts),
// which moves every matching stack. With Quick Deposit on the host, each player's kept amounts
// are set aside while it runs: the local player's marks directly, a remote player's from the
// list their Quick Deposit sends over hidden chat lines (see Protocol below).
//
// Protocol, chat entry type 255 (relayed by every server, drawn by no chat window):
//   client -> host  "/qdh1"                 hello: does the host run Quick Deposit?
//   host -> client  "/qds1 1"               yes (the host stamps the asker's handle on it)
//   client -> host  "/qdk1 id:n,id:n,..."   the kept items (n 0: every one), first part ("/qdk1"
//                                           alone: none)
//   client -> host  "/qdk1+ id:n,..."       more of the same list
namespace host {

constexpr uint8_t kChatType = 255;

// Hooks the per-chest area deposit and the server's receive loop (to tell players' accounts
// apart). Call before the game's code runs; on the dedicated server and in the game alike.
bool Init();

// The local player's entity, for a game that hosts its own world.
void SetLocalPlayer(uint32_t entity);

// The chests your own deposits on this host filled (a Quick Stack Station, "Deposit Stacks")
// since the last call.
std::vector<uint32_t> TakeFilled();

// Whole-base deposit on this host, at once, the way a Quick Stack Station deposits (stacks of
// items the chests already hold; kept amounts stay): true if this game hosts the world and the
// game's code matched.
bool WholeBaseReady();
// Asks for one for player into these chests; runs on the game's next frame. False if not ready.
bool RequestWholeBase(uint32_t player, const uint32_t* chests, int count);
// True once after you used a Quick Stack Station in the world this game hosts: the station did
// nothing, a whole Quick Deposit is due (UI thread).
bool TakeStationUse();
// How many chests the last one filled; -1 while it is pending.
int WholeBaseResult();
void CancelWholeBase();

// Client side, every UI frame: asks a remote host whether it runs Quick Deposit and sends it your
// kept items whenever they change. world: the client's world (null outside one).
void ClientTick(void* world, uint32_t localPlayer);

// A protocol line arriving in this client's own chat log.
void ClientLine(const char* text, size_t length, uint32_t senderHandle, uint32_t ownHandle);

// A chat line reaching the host's chat (not this client's own log). 0: not Quick Deposit's,
// 1: replace it with answer, 2: drop it.
int HostLine(const char* text, size_t length, uint64_t account, char* answer, size_t capacity);

} // namespace host

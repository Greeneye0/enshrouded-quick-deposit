#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "config.h"

// Deposits through the client UI, like the chest window's "Deposit Stacks" button, so it
// works on any world: hosted on this PC, a friend's game or a dedicated server.
namespace client {

// Finds the client UI functions and hooks the per-frame UI loop. Call before the game's code
// runs. Returns false (and logs why) when the game version does not match.
bool Init();

// On the next frames, sends "Deposit Stacks" to every chest of the base you stand in.
void RequestDeposit(const Config& cfg);

// Settings for deposits started from inside the game (G twice in the inventory).
void SetConfig(const Config& cfg);
const Config& CurrentConfig();

// --- for the dev bridge; game (UI) thread only ---

void* CurrentClient();

struct BaseInfo {
    bool inWorld = false;
    bool inBase = false;
    bool readable = false; // inventories readable
    uint32_t playerId = 0;
    float pos[3] = {};
    float lo[3] = {}, hi[3] = {};
    int magicCount = 0;               // the first magicCount chests are the base's own list
    std::vector<uint32_t> chests;     // storage chests a deposit would use
    std::vector<std::string> excluded; // names of skipped chests
};

// The base you stand in, the way a deposit sees it. Returns false (why says why) when you
// are not in a world or not inside a base.
bool DescribeBase(BaseInfo& out, std::string& why);

// Starts a deposit with these settings; false (why) if it can't start.
bool StartDeposit(const Config& cfg, std::string& why);
bool DepositBusy();
// Your player entity (0 outside a world). Game (UI) thread.
uint32_t LocalPlayerId();
// Sends a chat line of this entry type to the server (UI command 0x74), also where the server
// has text chat off. UI thread.
bool SendHiddenLine(const char* text, uint8_t type);
uint64_t DepositRunsFinished();
const std::vector<std::string>& LastReport();

// "Wands" for a named chest, else "Wood Logs chest".
std::string ChestLabelFor(uint32_t chest);

#ifdef QD_BRIDGE
// Dev only: log every UI command (id and first 48 bytes) passing the dispatcher.
void SetCommandTrace(bool on);
// Dev only: moves the stack in one inventory slot to another (UI command 0x2c, like a drag).
bool TransferSlot(uint32_t fromPage, int fromSlot, uint32_t toPage, int toSlot);
// Dev only: sends a chat line to the server as if typed (UI command 0x74). UI thread only.
bool SendChatLine(const char* text);
// Dev only: sends a raw 0x28-byte UI command (experiments with the transfer command's fields).
bool SendRawCommand(const uint8_t* cmd);
// Dev only: one of the three installed altar expansion recipes, amount one. The bridge
// must validate the test area and nearby altar identity first. No level/cost bypass.
bool SendAltarUpgrade(uint32_t altarEntity, uint32_t recipe);
// Dev only: the installed Flame1->2 or2->3 recipe at its native progress entity.
// The bridge must verify current level and a nearby altar in the test area.
bool SendFlameUpgrade(uint32_t progressEntity, uint32_t recipe);
#endif

// The app object the UI loop was last called with (dev inspection).
void* LastApp();

} // namespace client

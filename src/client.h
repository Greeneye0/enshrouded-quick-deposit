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
uint64_t DepositRunsFinished();
const std::vector<std::string>& LastReport();

// "Wands" for a named chest, else "Wood Logs chest".
std::string ChestLabelFor(uint32_t chest);

} // namespace client

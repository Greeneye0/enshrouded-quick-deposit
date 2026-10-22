#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Server-side auto loot: retunes the host's own "collect nearby loot" system (what holding E
// does) so it runs for every player all the time, in a larger radius, and optionally picks
// items up at once instead of pulling them in. Only has an effect where this process is the
// host (a hosted world or a dedicated server).
namespace arealoot {

// Finds the system and its patch sites (game build 1269579). False if anything doesn't match.
bool Init();

// radius in meters (clamped to 1..20); instant: pick up at once instead of pulling in;
// always: run without the player holding E.
bool Apply(float radius, bool instant, bool always);

// Also loot enemy corpses (and what they dropped into a loot container) within radius.
bool SetCorpses(bool on, float radius);

// Dev: which entities get "loot all": found by a spatial query for type name hash a (low 32 bits),
// and also by one for b if b != 0. Default: the corpse marker alone.
void SetLootAllQuery(uint32_t a, uint32_t b);
void Stats(uint32_t& foundA, uint32_t& foundB, uint32_t& sent);
void SetAcceptFirst(bool on, uint32_t offer);
uint64_t PermissionTable();

// Init, then load autoloot.ini (next to the game; created with defaults: 5 m, loose loot and
// corpses on, settings changeable by admins only) and apply it.
bool Start();

// A chat line arriving at the host: "/loot ..." commands are handled (true; answer filled in) and
// should not be passed on to the other players.
bool HandleChat(const char* text, size_t length, uint64_t sender, std::string& answer);

// Back to the game's own values.
void Restore();

} // namespace arealoot

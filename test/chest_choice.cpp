// Offline test of where a deposit's tier 2 puts items a base holds none of: the planner's own
// matching code (src/match.cpp) against a snapshot of a real base (test/snapshot_base.py) and the
// game's item table (tools/mcp/data/items.txt). Run it with test/run_chest_choice.py.
//
// chest_choice <base.tsv> <items.txt> <placement.tsv>
//   Writes, for every item of the categories below, the chest tier 2 picks and why. Items a chest
//   of the base already holds are listed with the chest holding the most of them (rule "item").
//
// Not modeled: weapons and equipment (they match by weapon type and equipment slot, which only
// the running game knows), and free slots changing during a deposit.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "match.h"

using namespace match;

namespace {

struct Chest {
    uint32_t entity;
    std::string label;
    bool named;
    float pos[3];
    int slotsTotal;
    std::map<uint32_t, uint64_t> held; // item id -> count
    int stacks = 0;
};

struct Item {
    uint32_t id;
    std::string raw, english, category;
    int maxStack;
};

const char* const kCategories[] = {"Customization", "Equipment", "Weapons", "Tools", "Instrument", "FishingRod",
                                   "BuildTools", "Consumables", "Ammunition", "Materials", "Blueprints", "Currency",
                                   "Collectibles", "AnimalFood", "PetFood", "WeaponGem", "ColorPalette"};

int CategoryOf(const std::string& name)
{
    for (int i = 0; i < static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0])); ++i)
        if (name == kCategories[i])
            return i;
    return -1;
}

std::vector<std::string> Split(const std::string& line)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            return out;
        start = tab + 1;
    }
}

bool ReadLines(const char* path, std::vector<std::string>& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    std::string cur;
    for (int c; (c = fgetc(f)) != EOF;) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(c);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    fclose(f);
    return true;
}

std::map<uint32_t, Item> g_items;

inv::ItemDesc Describe(uint32_t id)
{
    inv::ItemDesc d{};
    auto it = g_items.find(id);
    if (it == g_items.end())
        return d;
    int cat = CategoryOf(it->second.category);
    if (cat < 0)
        return d;
    d.known = true;
    d.category = static_cast<uint8_t>(cat);
    d.maxStack = static_cast<uint16_t>(it->second.maxStack);
    strncpy_s(d.raw, it->second.raw.c_str(), _TRUNCATE);
    inv::PrettyName(d.raw, d.name, sizeof(d.name));
    strncpy_s(d.shown, it->second.english.empty() ? d.name : it->second.english.c_str(), _TRUNCATE);
    // ammoType comes from the game's item record; the internal names tell it well enough here.
    if (cat == kCategoryAmmunition)
        d.ammoType = strstr(d.raw, "Arrow") ? 1 : strstr(d.raw, "FishingBait") ? 3 : 2;
    return d;
}

bool ContainsNoCase(const std::string& text, const char* part)
{
    if (!*part)
        return false;
    std::string t = text, p = part;
    for (char& c : t)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char& c : p)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return t.find(p) != std::string::npos;
}

// client.cpp NameScore without the game's equipment type names: the whole name, then the words.
int NameScore(const std::string& chestName, const inv::ItemDesc& d)
{
    if (ContainsNoCase(chestName, d.shown) || ContainsNoCase(chestName, d.name))
        return 1000;
    return WordScore(chestName.c_str(), d);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: chest_choice <base.tsv> <items.txt> <placement.tsv>\n");
        return 2;
    }
    std::vector<std::string> lines;
    if (!ReadLines(argv[2], lines)) {
        fprintf(stderr, "can't read %s\n", argv[2]);
        return 2;
    }
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#')
            continue;
        auto f = Split(line);
        if (f.size() < 9)
            continue;
        Item it{static_cast<uint32_t>(strtoul(f[0].c_str(), nullptr, 10)), f[1], f[2], f[5], atoi(f[8].c_str())};
        g_items[it.id] = it;
    }
    lines.clear();
    if (!ReadLines(argv[1], lines)) {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 2;
    }
    std::vector<Chest> chests;
    float me[3] = {};
    for (const std::string& line : lines) {
        auto f = Split(line);
        if (f[0] == "P" && f.size() >= 4) {
            for (int i = 0; i < 3; ++i)
                me[i] = static_cast<float>(atof(f[1 + i].c_str()));
        } else if (f[0] == "C" && f.size() >= 8) {
            Chest c{static_cast<uint32_t>(strtoul(f[1].c_str(), nullptr, 10)), f[2], f[3] == "1",
                    {static_cast<float>(atof(f[4].c_str())), static_cast<float>(atof(f[5].c_str())),
                     static_cast<float>(atof(f[6].c_str()))},
                    atoi(f[7].c_str())};
            chests.push_back(c);
        } else if (f[0] == "S" && f.size() >= 4 && !chests.empty()) {
            uint32_t id = static_cast<uint32_t>(strtoul(f[2].c_str(), nullptr, 10));
            chests.back().held[id] += strtoull(f[3].c_str(), nullptr, 10);
            ++chests.back().stacks;
        } else if (f[0] == "I" && f.size() >= 5) {
            // Items the base holds that the table (game 1.0) lacks: the game's own description.
            uint32_t id = static_cast<uint32_t>(strtoul(f[1].c_str(), nullptr, 10));
            if (!g_items.count(id))
                g_items[id] = Item{id, f[2], "", f[3], atoi(f[4].c_str())};
        }
    }

    // The chests' items, described once.
    std::vector<std::vector<inv::ItemDesc>> held(chests.size());
    for (size_t c = 0; c < chests.size(); ++c)
        for (const auto& [id, count] : chests[c].held)
            held[c].push_back(Describe(id));

    FILE* out = fopen(argv[3], "w");
    if (!out) {
        fprintf(stderr, "can't write %s\n", argv[3]);
        return 2;
    }
    fprintf(out, "category\titem\tinternal\tchest\trule\tsort\tstate\tword\tkind\n");
    // Optional 4th argument: an item name; prints every chest's fit for it instead.
    if (argc >= 5) {
        for (const auto& [id, item] : g_items) {
            inv::ItemDesc d = Describe(id);
            if (!d.known || _stricmp(d.shown, argv[4]) != 0)
                continue;
            printf("%s [%s] name '%s'\n", d.shown, item.raw.c_str(), d.name);
            for (size_t c = 0; c < chests.size(); ++c) {
                int score = chests[c].named ? NameScore(chests[c].label, d) : 0;
                KindFit f = FitByKind(d, held[c].data(), static_cast<int>(held[c].size()), score);
                if (f.kind || score)
                    printf("  %-24s sort %d state %d word %d kind %2d  name %d\n", chests[c].label.c_str(), f.sort,
                           f.state, f.word, f.kind, score);
            }
        }
        return 0;
    }
    const char* const tested[] = {"Materials", "Consumables", "Ammunition", "AnimalFood", "PetFood", "BuildTools",
                                  "Tools", "Collectibles", "WeaponGem", "Blueprints", "Instrument", "FishingRod"};
    int rows = 0;
    for (const auto& [id, item] : g_items) {
        bool want = false;
        for (const char* t : tested)
            want |= item.category == t;
        inv::ItemDesc d = Describe(id);
        if (!want || !d.known)
            continue;
        // As PlanTier2: among chests with a free slot, tier2Order item,kind,name,weapons,nearest.
        int sameItem = -1, kindChest = -1, nameChest = -1, nearChest = -1, bestName = 0;
        uint64_t sameCount = 0;
        KindFit bestFit;
        float bestDist = 1e30f;
        for (size_t c = 0; c < chests.size(); ++c) {
            const Chest& ch = chests[c];
            if (ch.stacks >= ch.slotsTotal)
                continue;
            auto h = ch.held.find(id);
            if (h != ch.held.end() && h->second > sameCount) {
                sameCount = h->second;
                sameItem = static_cast<int>(c);
            }
            int score = ch.named ? NameScore(ch.label, d) : 0;
            KindFit fit = FitByKind(d, held[c].data(), static_cast<int>(held[c].size()), score);
            if (fit.Any() && fit > bestFit) {
                bestFit = fit;
                kindChest = static_cast<int>(c);
            }
            if (score > bestName) {
                bestName = score;
                nameChest = static_cast<int>(c);
            }
            float dist = 0;
            for (int i = 0; i < 3; ++i)
                dist += (ch.pos[i] - me[i]) * (ch.pos[i] - me[i]);
            if (dist < bestDist) {
                bestDist = dist;
                nearChest = static_cast<int>(c);
            }
        }
        int chest = sameItem >= 0 ? sameItem : kindChest >= 0 ? kindChest : nameChest >= 0 ? nameChest : nearChest;
        const char* rule = sameItem >= 0 ? "item" : kindChest >= 0 ? "kind" : nameChest >= 0 ? "name" : "nearest";
        fprintf(out, "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\n", item.category.c_str(), d.shown, item.raw.c_str(),
                chest >= 0 ? chests[chest].label.c_str() : "(none)", rule, bestFit.sort, bestFit.state, bestFit.word,
                bestFit.kind);
        ++rows;
    }
    fclose(out);
    printf("%d items placed -> %s\n", rows, argv[3]);
    return 0;
}

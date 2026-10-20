// Offline check: every signature in src/game.cpp must match exactly once in the given exe.
// usage: sigtest.exe <path\to\enshrouded.exe>   (run from the project folder)
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include "../src/scan.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: sigtest <enshrouded.exe>\n");
        return 2;
    }
    HMODULE m = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        printf("cannot map %s\n", argv[1]);
        return 2;
    }
    // Image-resource handles carry flag bits in the low two bits.
    auto base = reinterpret_cast<uint8_t*>(reinterpret_cast<ULONG_PTR>(m) & ~ULONG_PTR(3));

    std::string src;
    for (const char* file : {"src/client.cpp", "src/inventory.cpp", "src/chat.cpp"}) {
        std::ifstream f(file);
        std::stringstream ss;
        ss << f.rdbuf();
        src += ss.str();
    }

    int bad = 0;
    uint8_t* system = nullptr;
    std::regex re(R"re(Find\((?:fn\.)?\w+, "(\w+)", "([^"]+)"\))re");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re); it != std::sregex_iterator(); ++it) {
        std::string name = (*it)[1], pattern = (*it)[2];
        uint8_t* a = FindUnique(pattern.c_str(), base);
        printf("%-18s %s +0x%llX\n", name.c_str(), a ? "ok" : "MISSING",
               a ? static_cast<unsigned long long>(a - base) : 0ull);
        bad += !a;
        if (name == "AreaDepositSystem")
            system = a;
    }
    if (system) {
        for (int off : {0x374, 0x3A6, 0x3C2}) {
            uint8_t* t = ResolveLea(system + off);
            printf("lea at +0x%X     %s +0x%llX\n", off, t ? "ok" : "MISSING",
                   t ? static_cast<unsigned long long>(t - base) : 0ull);
            bad += !t;
        }
    }
    printf(bad ? "%d FAILED\n" : "all signatures ok\n", bad);
    return bad;
}

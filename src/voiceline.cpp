// See voiceline.h. The sound files are greeneye\quick-deposit\voice\f.wav and m.wav, mono IMA
// ADPCM, decoded to 16-bit PCM and played through waveOut at the game's Master Volume x Player Voice
// (user_settings.json beside the character saves), the way Town Portal plays its voice lines. The
// voice pack comes from the newest characterN.ksd (voicepack.h): f1..f5 get f.wav, m1..m5 m.wav.
#define NOMINMAX // adpcm.h uses std::min
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../common/greeneye.h"
#include "adpcm.h"
#include "character.h"
#include "log.h"
#include "voiceline.h"
#include "voicepack.h"

namespace voiceline {
namespace {

constexpr int kPresses = 10;          // presses on one item ...
constexpr ULONGLONG kWindowMs = 4000; // ... within this time
constexpr ULONGLONG kCooldownMs = 60000;

bool g_enabled = true;
uint32_t g_item = 0;
ULONGLONG g_times[kPresses] = {};
int g_count = 0;
ULONGLONG g_sangAt = 0;

std::vector<uint8_t> ReadFile(const std::wstring& path, size_t max)
{
    std::vector<uint8_t> data;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        data.resize(max);
        data.resize(fread(data.data(), 1, data.size(), f));
        fclose(f);
    }
    return data;
}

// Master Volume x Player Voice from user_settings.json (values quoted: "0", "1", or a float's
// bits in hex); 1 when unreadable.
float VoiceGain(const std::wstring& folder)
{
    std::vector<uint8_t> raw = ReadFile(folder + L"user_settings.json", 32768);
    std::string text(raw.begin(), raw.end());
    auto value = [&](const char* key) {
        const size_t at = text.find(key);
        if (at == std::string::npos)
            return 1.0f;
        const size_t open = text.find('"', text.find(':', at + strlen(key)));
        if (open == std::string::npos)
            return 1.0f;
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos)
            return 1.0f;
        const std::string token = text.substr(open + 1, close - open - 1);
        float v = 1.0f;
        if (token.size() == 8 && token.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            const uint32_t bits = static_cast<uint32_t>(strtoul(token.c_str(), nullptr, 16));
            memcpy(&v, &bits, sizeof(v));
        } else {
            v = static_cast<float>(atof(token.c_str()));
        }
        return std::isfinite(v) ? (std::min)((std::max)(v, 0.0f), 1.0f) : 1.0f;
    };
    return value("\"masterVolume\"") * value("\"voiceVolume\"");
}

// Plays 16-bit mono PCM (a 44-byte-header WAV) through waveOut. The samples live on the process
// heap and no callback is used, so a hot reload mid-line unloads nothing the sound still needs.
void Play(const std::vector<uint8_t>& wav, float gain)
{
    static HWAVEOUT out = nullptr;
    static WAVEHDR header{};
    if (out) { // the last line, long over (cooldown)
        waveOutReset(out);
        waveOutUnprepareHeader(out, &header, sizeof(header));
        waveOutClose(out);
        HeapFree(GetProcessHeap(), 0, header.lpData);
        out = nullptr;
    }
    uint32_t rate;
    memcpy(&rate, wav.data() + 24, 4);
    const size_t bytes = wav.size() - 44;
    auto* samples = static_cast<int16_t*>(HeapAlloc(GetProcessHeap(), 0, bytes));
    if (!samples)
        return;
    memcpy(samples, wav.data() + 44, bytes);
    for (size_t i = 0; i < bytes / 2; ++i)
        samples[i] = static_cast<int16_t>(samples[i] * gain);
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, rate, rate * 2, 2, 16, 0};
    if (waveOutOpen(&out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        HeapFree(GetProcessHeap(), 0, samples);
        out = nullptr;
        return;
    }
    header = {};
    header.lpData = reinterpret_cast<LPSTR>(samples);
    header.dwBufferLength = static_cast<DWORD>(bytes);
    if (waveOutPrepareHeader(out, &header, sizeof(header)) != MMSYSERR_NOERROR ||
        waveOutWrite(out, &header, sizeof(header)) != MMSYSERR_NOERROR)
        Log("voice line: could not play");
}

void Speak()
{
    const std::wstring save = character::NewestSave();
    int voice = -1;
    if (!save.empty()) {
        std::vector<uint8_t> data = ReadFile(save, 1 << 16);
        voice = voicepack::VoiceIndex(voicepack::FromSave(data.data(), data.size()));
    }
    const bool female = voice < 0 || voicepack::Female(voice); // unknown: f.wav
    char path[MAX_PATH * 3];
    strcpy_s(path, greeneye::DataPath(female ? "voice\\f.wav" : "voice\\m.wav"));
    wchar_t wide[MAX_PATH * 3];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 3);
    std::vector<uint8_t> file = ReadFile(wide, 4 << 20);
    std::vector<uint8_t> wav = adpcm::ToPcm(file.data(), file.size());
    if (wav.size() <= 44) {
        Log("voice line: %s missing", path);
        return;
    }
    const size_t slash = save.find_last_of(L'\\');
    const float gain = slash == std::wstring::npos ? 1.0f : VoiceGain(save.substr(0, slash + 1));
    Log("voice line (voice %s, volume %.2f)", voice >= 0 ? voicepack::kNames[voice] : "unknown", gain);
    if (gain > 0.001f)
        Play(wav, gain);
}

} // namespace

void SetEnabled(bool on)
{
    g_enabled = on;
}

void Pressed(uint32_t itemId)
{
    if (!g_enabled || !itemId)
        return;
    const ULONGLONG now = GetTickCount64();
    if (itemId != g_item) {
        g_item = itemId;
        g_count = 0;
    }
    // The last kPresses presses on this item, oldest first.
    if (g_count == kPresses) {
        memmove(g_times, g_times + 1, sizeof(g_times) - sizeof(g_times[0]));
        --g_count;
    }
    g_times[g_count++] = now;
    if (g_count < kPresses || now - g_times[0] > kWindowMs || (g_sangAt && now - g_sangAt < kCooldownMs))
        return;
    g_sangAt = now;
    g_count = 0;
    Speak();
}

} // namespace voiceline

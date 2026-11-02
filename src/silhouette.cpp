// Lit chests through walls (build 1273043), from Find's silhouettes (FindMod/src/silhouette.cpp).
//
// Each lit chest's mesh is read from the game's content files (enshrouded.kfc lists where its
// data sits in enshrouded_NNN.dat), placed with the render instance's pose and rasterized on the
// CPU against its own depth only, so walls never hide it. The covered pixels become rectangles
// (an outline band, shaded faces, a soft halo) drawn with the UI's plain image call inside the
// HUD's spatial elements pass (the one the game's waypoint markers use).
//
// UI: root = *[rip + ...] (mov rax, [rip]; mov rcx, [rax+0D0h]); client = *(**(root + 0xd0) + 0x38);
// view matrix client + 0x1d20, projection client + 0x1d60, viewport *(root + 0x10) + 0x18.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include "../../common/greeneye.h"
#include "glow.h"
#include "log.h"
#include "projection_frame.h"
#include "scan.h"
#include "silhouette.h"
#include "../../common/highlight_work.h"
#include "../../common/smooth_highlight.h"
#include "../../common/ui_mesh.h"
#include "silhouette_mesh.h"
#include "silhouette_raster.h"
#include "silhouette_runs.h"

namespace ui {

void** g_root = nullptr;

void* Client()
{
    auto* g = g_root ? static_cast<uint8_t*>(*g_root) : nullptr;
    auto** ui = g ? *reinterpret_cast<uint8_t***>(g + 0xd0) : nullptr;
    return ui && *ui ? *reinterpret_cast<void**>(*ui + 0x38) : nullptr;
}

bool CaptureProjection(ProjectionFrame& frame)
{
    auto* root = g_root ? static_cast<uint8_t*>(*g_root) : nullptr;
    auto* client = static_cast<uint8_t*>(Client());
    if (!root || !client)
        return false;
    auto* window = *reinterpret_cast<uint8_t**>(root + 0x10);
    if (!window)
        return false;
    memcpy(frame.view, client + 0x1d20, sizeof(frame.view));
    memcpy(frame.projection, client + 0x1d60, sizeof(frame.projection));
    memcpy(frame.viewport, window + 0x18, sizeof(frame.viewport));
    return frame.viewport[2] >= 1 && frame.viewport[3] >= 1;
}

} // namespace ui

namespace silhouette {
#ifdef QD_BRIDGE
TestStats testStats;
SRWLOCK testStatsLock=SRWLOCK_INIT;
TestStats TestRead(bool reset) {
    AcquireSRWLockExclusive(&testStatsLock);
    auto result=testStats; if(reset)testStats={};
    ReleaseSRWLockExclusive(&testStatsLock); return result;
}
#endif
namespace {

using SpatialFn = void (*)(void* state, void* arg);
using BeginWidgetFn = void (*)(void* widget, uint64_t flags);
using EndWidgetFn = void (*)(void* widget);
using DrawRealFn = void (*)(void* widget, const float* rect, uint32_t id, uint64_t colour);

SpatialFn g_originalSpatial = nullptr;
BeginWidgetFn g_begin = nullptr;
EndWidgetFn g_end = nullptr;
DrawRealFn g_drawReal = nullptr;

constexpr uint32_t kGreen = 0xff6eff5au; // 0xAABBGGRR

struct Rect {
    float x, y, w, h;
};

uint64_t Colour(uint32_t rgba, float alpha)
{
    struct {
        uint32_t rgba;
        float alpha;
    } c{rgba, alpha};
    uint64_t v;
    memcpy(&v, &c, 8);
    return v;
}

// Plain rectangles in one widget of their own.
void DrawSolids(const Rect* rects, size_t count, uint32_t colour, float alpha)
{
    if (!count || count > 65536)
        return;
    alignas(16) uint8_t widget[0x80] = {};
    g_begin(widget, 0);
    uint32_t a = static_cast<uint32_t>((colour >> 24) * (std::max)(0.f, (std::min)(1.f, alpha)));
    for (size_t i = 0; i < count; ++i)
        if (rects[i].w > 0 && rects[i].h > 0)
            g_drawReal(widget, &rects[i].x, 0, Colour((colour & 0x00ffffffu) | a << 24, 1.f));
    g_end(widget);
}

std::filesystem::path GameDir()
{
    wchar_t p[32768];
    GetModuleFileNameW(nullptr, p, 32768);
    return std::filesystem::path(p).parent_path();
}

bool ReadFile(const std::filesystem::path& p, uint64_t at, uint32_t n, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size < 0 || at > uint64_t(size) || n > uint64_t(size) - at)
        return false;
    out.resize(n);
    f.seekg(at);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f);
}

// A content blob by its 16-byte hash: enshrouded.kfc's directory says file and offset.
bool Content(const uint8_t* hash, std::vector<uint8_t>& data)
{
    static std::vector<uint8_t> directory;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        auto p = GameDir() / "enshrouded.kfc";
        std::error_code ec;
        auto n = std::filesystem::file_size(p, ec);
        if (!ec && n >= 0x60 && n < 64 * 1024 * 1024)
            ReadFile(p, 0, static_cast<uint32_t>(n), directory);
    }
    if (directory.size() < 0x60 || memcmp(directory.data(), "KFC4", 4))
        return false;
    const auto* d = directory.data();
    uint64_t keys = 0x48ull + Read<uint32_t>(d + 0x48), values = 0x50ull + Read<uint32_t>(d + 0x50);
    uint32_t count = Read<uint32_t>(d + 0x4c), n = Read<uint32_t>(hash);
    if (count != Read<uint32_t>(d + 0x54) || keys + uint64_t(count) * 16 > directory.size() ||
        values + uint64_t(count) * 16 > directory.size() || n > 16 * 1024 * 1024)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!memcmp(d + keys + uint64_t(i) * 16, hash, 16)) {
            uint64_t entry = Read<uint64_t>(d + values + uint64_t(i) * 16);
            if (Read<uint64_t>(d + values + uint64_t(i) * 16 + 8))
                return false;
            wchar_t filename[64];
            swprintf_s(filename, L"enshrouded_%03u.dat", unsigned(entry >> 48));
            return ReadFile(GameDir() / filename, entry & 0xffffffffffffull, n, data);
        }
    return false;
}

struct Face {
    std::array<float, 3> normal{};
    unsigned char shade = 0;
    bool valid = false;
};
struct Prepared {
    const Mesh* mesh = nullptr;
    Spatial transform{};
    ui::ProjectionFrame camera{};
    bool imageValid = false;
    std::vector<std::array<float, 3>> worlds, points;
    std::vector<Face> faces;
    std::array<std::vector<Rect>, 7> strips;
};
std::map<std::string, Mesh> g_cache;
Prepared g_prepared[64];
struct Scratch {
    std::vector<uint8_t> mask, horizontal, expanded, labels;
    std::vector<SurfacePixel> surface;
    std::vector<RasterRect> rectangles;
} g_scratch;

// Every rectangle is one UI quad in the game's per-frame dynamic buffer, and running it out
// crashes the game: each chest coarsens its grid until it fits its share of kFrameRects.
constexpr size_t kChestRects = highlight_work::chestRects, kMinChestRects = 64, kFrameRects = highlight_work::frameRects;

size_t Count(const Prepared& prepared)
{
    size_t n = 0;
    for (int k = 1; k <= 6; ++k)
        n += prepared.strips[k].size();
    return n;
}

bool Submit(const Snapshot& s, const Prepared& prepared, size_t& budget)
{
    size_t n = Count(prepared);
    if (n > budget)
        return false;
    budget -= n;
    const auto& strips = prepared.strips;
    float pulse = .5f - .5f * std::cos(float(GetTickCount64() % 1200) / 1200.f * 6.283185307f);
    for (int k = 1; k <= 4; ++k)
        DrawSolids(strips[k].data(), strips[k].size(), kGreen, s.alpha * (.08f + k * .025f + (.08f + k * .025f) * pulse));
    DrawSolids(strips[6].data(), strips[6].size(), kGreen, s.alpha * (.08f + .10f * pulse));
    DrawSolids(strips[5].data(), strips[5].size(), kGreen, s.alpha * (.45f + .40f * pulse));
    return true;
}

void Fallback(const Snapshot& s, const ui::ProjectionFrame& camera) {
    float world[3]; for (int j=0;j<3;++j) world[j]=float(s.transform.position[j]/4294967296.0);
    float x,y,z;
    if (!camera.Project(world,x,y,z) || x<camera.viewport[0] || y<camera.viewport[1] ||
        x>camera.viewport[0]+camera.viewport[2] || y>camera.viewport[1]+camera.viewport[3]) return;
    const Rect marker[] = {{x-6,y-6,12,2},{x-6,y+4,12,2},{x-6,y-4,2,8},{x+4,y-4,2,8}};
    DrawSolids(marker,4,kGreen,s.alpha*.65f);
}

bool DrawOne(const Snapshot& s, const Mesh& mesh, const ui::ProjectionFrame& camera, Prepared& prepared, size_t& budget,
             size_t cap, highlight_work::Frame* work = nullptr)
{
    if (mesh.vertices.empty())
        return false;
    bool sameGeometry = prepared.mesh == &mesh && !memcmp(&prepared.transform, &s.transform, sizeof(s.transform));
    if (sameGeometry && prepared.imageValid && !memcmp(&prepared.camera, &camera, sizeof(camera))) {
        return Submit(s, prepared, budget);
    }
    prepared.imageValid = false;
    if (!budget || (work && !work->Rebuild())) return false;
    auto& points = prepared.points;
    auto& worlds = prepared.worlds;
    auto& faces = prepared.faces;
    points.clear();
    points.reserve(mesh.vertices.size());
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const auto& t = s.transform;
    double qnorm = 0;
    for (float q : t.rotation) {
        if (!std::isfinite(q))
            return false;
        qnorm += q * q;
    }
    if (std::fabs(qnorm - 1) > 0.02)
        return false;
    for (float scale : t.scale)
        if (!std::isfinite(scale) || std::fabs(scale) > 20)
            return false;
    if (!sameGeometry) {
        prepared.mesh = nullptr; // Interrupted geometry cannot be reused.
        worlds.clear();
        worlds.reserve(mesh.vertices.size());
        float qx = t.rotation[0], qy = t.rotation[1], qz = t.rotation[2], qw = t.rotation[3];
        for (auto p : mesh.vertices) {
            if (work && worlds.size() % 64 == 0 && !work->Available()) return false;
            for (int j = 0; j < 3; ++j)
                p[j] *= t.scale[j];
            float cx = 2 * (qy * p[2] - qz * p[1]), cy = 2 * (qz * p[0] - qx * p[2]), cz = 2 * (qx * p[1] - qy * p[0]);
            worlds.push_back({float(t.position[0] / 4294967296.0) + p[0] + qw * cx + qy * cz - qz * cy,
                              float(t.position[1] / 4294967296.0) + p[1] + qw * cy + qz * cx - qx * cz,
                              float(t.position[2] / 4294967296.0) + p[2] + qw * cz + qx * cy - qy * cx});
        }
        faces.assign(mesh.indices.size() / 3, {});
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            if (work && i % 96 == 0 && !work->Available()) return false;
            auto p = worlds[mesh.indices[i]], q = worlds[mesh.indices[i + 1]], r = worlds[mesh.indices[i + 2]];
            std::array<float, 3> a{}, b{};
            for (int j = 0; j < 3; ++j) {
                a[j] = q[j] - p[j];
                b[j] = r[j] - p[j];
            }
            auto& f = faces[i / 3];
            f.normal = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            auto& n = f.normal;
            float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length < 1e-8f)
                continue;
            for (float& v : n)
                v /= length;
            float light = std::fabs(n[0] * .36f + n[1] * .80f + n[2] * -.48f);
            f.shade = static_cast<unsigned char>((std::min)(3, int(light * 4)));
            f.valid = true;
        }
        prepared.mesh = &mesh;
        prepared.transform = s.transform;
    }
    for (const auto& world : worlds) {
        if (work && points.size() % 64 == 0 && !work->Available()) return false;
        float x, y, z;
        if (!camera.Project(world.data(), x, y, z) || std::fabs(x) > 20000 || std::fabs(y) > 20000)
            return false;
        points.push_back({x, y, 1.f / z});
        minx = (std::min)(minx, x);
        maxx = (std::max)(maxx, x);
        miny = (std::min)(miny, y);
        maxy = (std::max)(maxy, y);
    }
    // Entirely off screen: nothing to draw (a margin for the halo).
    if (maxx < camera.viewport[0] - 10 || maxy < camera.viewport[1] - 10 ||
        minx > camera.viewport[0] + camera.viewport[2] + 10 || miny > camera.viewport[1] + camera.viewport[3] + 10)
        return false;
    float step = (std::max)(1.f, (std::max)(maxx - minx, maxy - miny) / 250.f);
    if (maxx - minx < 1 || maxy - miny < 1 || step > 20)
        return false;
    const float originx = minx, originy = miny;
    auto& rectangles = g_scratch.rectangles;
    for (int attempt = 0;; ++attempt) {
        if (work && !work->Available()) return false;
        minx = originx;
        miny = originy;
        int w = int(std::ceil((maxx - minx) / step)) + 7, h = int(std::ceil((maxy - miny) / step)) + 7;
        minx -= 3 * step;
        miny -= 3 * step;
        auto& mask = g_scratch.mask;
        mask.assign(w * h, 0);
        auto& surface = g_scratch.surface;
        surface.assign(w * h, {});
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            if (work && i % 96 == 0 && !work->Available()) return false;
            const auto& face = faces[i / 3];
            if (!face.valid)
                continue;
            std::array<float, 3> p[3];
            for (int j = 0; j < 3; ++j) {
                auto v = points[mesh.indices[i + j]];
                p[j] = {(v[0] - minx) / step, (v[1] - miny) / step, v[2]};
            }
            RasterSurface(surface, w, h, p, face.normal, face.shade);
        }
        for (int i = 0; i < w * h; ++i)
            mask[i] = surface[i].inverseDepth > 0 ? 1 : 0;
        // A soft band around the outline (5x5 dilation, separable).
        auto& horizontal = g_scratch.horizontal;
        auto& expanded = g_scratch.expanded;
        auto& labels = g_scratch.labels;
        horizontal.assign(w * h, 0);
        expanded.assign(w * h, 0);
        labels.assign(w * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 2; x < w - 2; ++x)
                horizontal[y * w + x] = mask[y * w + x - 2] | mask[y * w + x - 1] | mask[y * w + x] | mask[y * w + x + 1] |
                                        mask[y * w + x + 2];
        for (int y = 2; y < h - 2; ++y)
            for (int x = 2; x < w - 2; ++x)
                expanded[y * w + x] = horizontal[(y - 2) * w + x] | horizontal[(y - 1) * w + x] | horizontal[y * w + x] |
                                      horizontal[(y + 1) * w + x] | horizontal[(y + 2) * w + x];
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x) {
                int at = y * w + x;
                if (!mask[at]) {
                    labels[at] = expanded[at] ? 6 : 0;
                    continue;
                }
                if (!mask[at - 1] || !mask[at + 1] || !mask[at - w] || !mask[at + w] ||
                    SurfaceCrease(surface[at], surface[at + 1]) || SurfaceCrease(surface[at], surface[at + w]))
                    labels[at] = 5;
                else
                    labels[at] = 1 + surface[at].shade;
            }
        if (work && !work->Available()) return false;
        MergeRuns(labels, w, h, rectangles);
        if (rectangles.size() <= cap)
            break;
        if (attempt >= 4)
            return false;
        step *= (std::max)(1.25f, std::sqrt(float(rectangles.size()) / cap) * 1.1f);
    }
    auto& strips = prepared.strips;
    for (auto& v : strips)
        v.clear();
    for (const auto& r : rectangles)
        strips[r.kind].push_back({minx + r.x * step, miny + r.y * step, r.w * step, r.h * step});
    prepared.camera = camera;
    prepared.imageValid = true;
    return Submit(s, prepared, budget);
}

void Draw(){
    auto testStart=std::chrono::steady_clock::now();
    static Snapshot snapshots[64];unsigned count=glow::Snapshots(snapshots,64);if(!count)return;
    ui::ProjectionFrame camera;if(!ui::CaptureProjection(camera))return;

    static std::map<std::string,std::vector<smooth_highlight::Point3>> supports;
    static smooth_highlight::Batch batch;batch.vertices.clear();batch.indices.clear();
    unsigned loads=0,drawn=0;
    float pulse=.65f+.35f*(.5f-.5f*std::cos(float(GetTickCount64()%1800)/1800.f*6.283185307f));
    for(unsigned i=0;i<count;++i){
        const auto& s=snapshots[i];
        std::string key(reinterpret_cast<const char*>(s.asset+0x110),16);key.append(reinterpret_cast<const char*>(s.mesh),56);
        auto found=supports.find(key);
        if(found==supports.end()){
            if(supports.size()>=64||loads++)continue;
            auto mesh=g_cache.find(key);
            if(mesh==g_cache.end()){
                Mesh decoded;std::vector<uint8_t> content;
                bool ok=Content(s.asset+0x110,content)&&Decode(content,s.asset,s.mesh,decoded);
                Log("smooth silhouette: decoded=%u vertices=%zu",ok?1:0,decoded.vertices.size());
                mesh=g_cache.emplace(key,std::move(decoded)).first;
            }
            found=supports.emplace(key,smooth_highlight::Support(mesh->second.vertices)).first;
        }
        drawn+=smooth_highlight::Project(found->second,s.transform,camera,kGreen,s.alpha*pulse,batch)?1:0;
    }
    if(!batch.vertices.empty()&&ui_mesh::Ready()){
        alignas(16) uint8_t widget[0x80]{};g_begin(widget,0);ui_mesh::Draw(widget,batch);g_end(widget);
    }
#ifdef QD_BRIDGE
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-testStart).count();
    AcquireSRWLockExclusive(&testStatsLock);
    ++testStats.frames;testStats.targets=count;testStats.details=drawn;testStats.markers=0;
    testStats.totalMs+=ms;testStats.maxMs=std::max(testStats.maxMs,ms);
    ReleaseSRWLockExclusive(&testStatsLock);
#endif
}

void SafeDraw()
{
    __try {
        Draw();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG logged;
        if (!InterlockedExchange(&logged, 1))
            Log("silhouette: drawing failed - chests do not show through walls");
    }
}

void SpatialHook(void* state, void* arg)
{
    greeneye::Lease lease;
    g_originalSpatial(state, arg);
    if (lease)
        SafeDraw();
}

} // namespace

bool Init()
{
    // The HUD's spatial elements: mov [rsp+10h], rdx; push rbp, rdi, r14; lea rbp, [rsp-200h] (17 bytes)
    uint8_t* spatial = FindUnique("48 89 54 24 10 55 57 41 56 48 8D AC 24 00 FE FF FF");
    g_begin = reinterpret_cast<BeginWidgetFn>(FindUnique("40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 45 33 C0"));
    g_end = reinterpret_cast<EndWidgetFn>(FindUnique("48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00"));
    uint8_t* thunk = FindUnique("4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC");
    ui::g_root = reinterpret_cast<void**>(ResolveRipConsensus("48 8B 05 ?? ?? ?? ?? 48 8B 88 D0 00 00 00", 3, 7));
    if (!spatial || !g_begin || !g_end || !thunk || !ui::g_root) {
        Log("silhouette: not found (%p %p %p %p %p) - chests do not show through walls", spatial, g_begin, g_end, thunk,
            ui::g_root);
        return false;
    }
    // DrawImage's thunk jumps to the real one (Quick Deposit's keep marks hook it and call on).
    g_drawReal = reinterpret_cast<DrawRealFn>(thunk + 11 + *reinterpret_cast<int32_t*>(thunk + 7));
    g_originalSpatial = static_cast<SpatialFn>(InstallHook(spatial, reinterpret_cast<void*>(&SpatialHook), 17));
    if (!g_originalSpatial) {
        Log("silhouette: hook install failed - chests do not show through walls");
        return false;
    }
    return true;
}

} // namespace silhouette

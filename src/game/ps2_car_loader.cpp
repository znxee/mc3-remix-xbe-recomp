// Opt-in: --mc3-ps2-cars=<folder> loads the PS2 version's car files in place of the Xbox ones.
//
// <folder> is a PS2 resources/vehicle tree from the user's own copy: <folder>/<car>/<car>.pck, <car>_o.pck,
// <car>_g.pck and the <car>/*.mesh.pck parts. Nothing is converted offline: mcResourceMgr::LoadResource (0x2105F0,
// a jump into 0x2126C0: open "<name>.xbck", read the 0x80-byte header, check version 1 and the type, allocate the
// body with 0x20DB20(size, 0x80) and read it) is answered here for a vehicle (type 0x38D030) or a mesh (22) whose
// PS2 file exists. ps2_vehicle.cpp parses the whole PS2 image and lays it out the way the Xbox loaders read it;
// the game's own relocators, shader factory, texture binder and renderer take it from there. A car with no PS2
// file, or one the parser refuses, loads from the Xbox assets as before.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "game/ps2_vehicle.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

X86_EXTERN_FUNC(sub_0020DB20);  // aligned operator new(size, alignment)

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kVehicle = 0x38D030, kMesh = 22;

struct Converted {
    std::vector<uint8_t> body;
    uint32_t base_va = 0;
};

std::mutex g_mutex;
std::map<std::wstring, Converted> g_cache;  // by host path: the same car is acquired by every variant and pool

const fs::path& Folder() {
    static const fs::path folder = [] {
        const wchar_t* v = Mc3DebugArg(L"--mc3-ps2-cars");
        return v && *v ? fs::path(v) : fs::path();
    }();
    return folder;
}

uint32_t GuestCall(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t a0, uint32_t a1) {
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    X86_PUSH32(a1);
    X86_PUSH32(a0);
    X86_PUSH32(0);  // return address: ret only pops it
    fn(ctx, base);
    const uint32_t r = ctx.eax;
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx, ctx.esp = esp;
    return r;
}

// "$/resources/vehicle/vp_jetta_03/vp_jetta_03_g" -> <folder>\vp_jetta_03\vp_jetta_03_g.pck
fs::path PathFor(const char* name) {
    static const char kPrefix[] = "$/resources/vehicle/";
    if (_strnicmp(name, kPrefix, sizeof kPrefix - 1) != 0) return {};
    std::string rest(name + sizeof kPrefix - 1);
    for (char& c : rest)
        if (c == '/') c = '\\';
    return Folder() / fs::path(rest + ".pck");
}

// The effects the game loaded at boot (0x1B4E30 -> 0x4FA80(index, name)): a 128-entry table at 0x500F28 of objects
// whose first word is their file name ("carpaint.fx"). PS2 shaders name a template; the Xbox shader keeps the index.
const ps2veh::Effects& Effects(uint8_t* base) {
    static ps2veh::Effects fx;
    if (!fx.empty()) return fx;
    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t e = X86_MEM32(0x500F28 + 4 * i);
        if (e < 0x10000) continue;
        const uint32_t name = X86_MEM32(e);
        if (name < 0x10000) continue;
        std::string s(reinterpret_cast<const char*>(base + name));
        if (const size_t slash = s.find_last_of("/\\"); slash != std::string::npos) s.erase(0, slash + 1);
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        fx.emplace(s, int(i));
    }
    LOGE("MC3PS2CAR the game's effect table holds %u effects", unsigned(fx.size()));
    return fx;
}

const Converted* Convert(uint8_t* base, const fs::path& path, uint32_t type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (auto it = g_cache.find(path.wstring()); it != g_cache.end()) return &it->second;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    Converted c;
    uint32_t out_type = 0;
    ps2veh::Report report;
    std::string error;
    if (!ps2veh::Convert(file, Effects(base), c.body, c.base_va, out_type, report, error) || out_type != type) {
        LOGE("MC3PS2CAR %ls: not converted (%s), the Xbox file loads instead", path.filename().c_str(),
             error.empty() ? "type mismatch" : error.c_str());
        return nullptr;
    }
    LOGE("MC3PS2CAR %ls: %s", path.filename().c_str(), report.Summary().c_str());
    for (const std::string& n : report.notes) LOGE("MC3PS2CAR   %s", n.c_str());
    return &g_cache.emplace(path.wstring(), std::move(c)).first->second;
}

}  // namespace

// Called first by the 0x2105F0 override (mc2_city.cpp). true when it answered the load itself.
bool Mc3Ps2CarLoad(X86Context& ctx, uint8_t* base) {
    if (Folder().empty()) return false;
    const uint32_t name_va = X86_MEM32(ctx.esp + 4), type = X86_MEM32(ctx.esp + 8);
    const uint32_t base_out = X86_MEM32(ctx.esp + 0xC), size_out = X86_MEM32(ctx.esp + 0x10);
    if ((type != kVehicle && type != kMesh) || name_va < 0x10000 || !base_out || !size_out) return false;
    const fs::path path = PathFor(reinterpret_cast<const char*>(base + name_va));
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec)) return false;
    const Converted* c = Convert(base, path, type);
    if (!c) return false;
    const uint32_t size = uint32_t(c->body.size());
    const uint32_t buffer = GuestCall(ctx, base, sub_0020DB20, size, 0x80);
    if (!buffer) {
        LOGE("MC3PS2CAR %ls: out of guest memory for %u bytes", path.filename().c_str(), size);
        return false;
    }
    std::memcpy(base + buffer, c->body.data(), size);
    X86_MEM32(base_out) = c->base_va;
    X86_MEM32(size_out) = size;
    ctx.eax = buffer;
    ctx.esp += 4;  // cdecl: pop the return address, the caller drops the arguments
    return true;
}

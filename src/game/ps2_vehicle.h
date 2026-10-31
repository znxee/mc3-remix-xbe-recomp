#pragma once

// Host-side reader for the PS2 version's car files (vp_*.pck, *_o/_g, *.mesh.pck parts), used by
// --mc3-ps2-cars. It parses the whole load-in-place image and rebuilds it in the layout the Xbox build's
// loaders expect, at load time, so the game's own relocators, shader factory and renderer consume it.
// No game code or data is referenced here: it depends only on the standard library.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps2veh {

constexpr uint32_t kTypeVehiclePs2 = 0x38E030;
constexpr uint32_t kTypeVehicleXbox = 0x38D030;
constexpr uint32_t kTypeMesh = 22;

// Byte accounting over the PS2 file. Every byte lands in exactly one bucket.
struct Report {
    uint32_t file_bytes = 0;
    uint32_t header = 0;   // the 0x80-byte load-in-place header
    uint32_t typed = 0;    // read through an explicit layout (geometry, shaders, textures, model, skeleton...)
    uint32_t graph = 0;    // objects whose layout is the same on both platforms, reached by pointer and kept
    uint32_t padding = 0;  // 0xCD allocation fill between objects
    uint32_t dead = 0;     // unreachable from the root through any pointer: data nothing owns
    uint32_t unknown = 0;  // reachable, but nothing above claims them
    uint32_t meshes = 0, batches = 0, triangles = 0, vertices = 0;
    uint32_t shaders = 0, textures = 0, texture_pixels = 0;
    uint32_t out_bytes = 0;
    std::vector<std::string> notes;  // anything that could not be carried over in full

    double ConsumedPercent() const;
    std::string Summary() const;
};

// The Xbox build's effect table: shaderlibx effect file name ("carpaint.fx") -> index. The game fills it at boot;
// the loader reads it from there (ps2_car_loader.cpp).
using Effects = std::map<std::string, int>;

// Converts a PS2 resource image (header included) into an Xbox one. On success `body` is the new body
// (what follows the 0x80-byte header), `base_va` the virtual address of its first byte (the PS2 one: pointers
// inside the body are kept in the PS2 address space and the game relocates them by buffer - base_va), and
// `type` the Xbox resource type the loader checks.
bool Convert(const std::vector<uint8_t>& file, const Effects& effects, std::vector<uint8_t>& body, uint32_t& base_va,
             uint32_t& type, Report& report, std::string& error);

// Converts one drwShaderModel image from a PS2 page file. PPF entries carry a
// 0x20-byte stream header before this body; callers pass the bytes after that
// header and the entry's PS2 virtual base. This is separate from Convert()
// because a page-file object has no PCK header of its own.
// A page-file model's skeleton (+0x68): its 0xC-byte header and the bytes [va, va + bytes.size()) holding its nodes
// and names. Some mod exporters leave a stale +0x68 that points into geometry; the PS2 tolerates it, but the Xbox
// constructor relocates the nodes it names, so such a model gets a copy of a valid skeleton of the same page file.
struct SkeletonTemplate {
    std::vector<uint8_t> header;
    std::vector<uint8_t> bytes;
    uint32_t va = 0;
};

// The skeleton of a page-file model (same arguments as ConvertShaderModel) when it is valid.
bool FindSkeleton(const std::vector<uint8_t>& source, uint32_t source_base, SkeletonTemplate& skeleton);

bool ConvertShaderModel(const std::vector<uint8_t>& source, uint32_t source_base, const Effects& effects,
                        std::vector<uint8_t>& body, Report& report, std::string& error,
                        const SkeletonTemplate* skeleton = nullptr);

}  // namespace ps2veh

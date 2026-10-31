// PS2 vp_*.pck -> Xbox load-in-place image, see ps2_vehicle.h.
//
// Both builds serialize the same C++ object graph with the same root, so most objects (tune, collision, audio,
// skeleton, bone tags, the model's LOD tables) have the same layout and stay where they are: the body is copied
// whole and keeps the PS2 virtual base, and the game relocates every pointer by buffer - base. What differs is
// rewritten. Objects that shrink on the Xbox are rewritten in place; what grows (vertex/index buffers, textures,
// shader parameters) is appended after the PS2 body, and its owner is pointed at it.
//
// Layout differences, measured on the 94 cars that exist on both platforms and read in the XBE's own loaders:
// - GCC puts a polymorphic class's vptr after its non-polymorphic base, MSVC at +0 (car model type, LOD table,
//   VehSim fluid); GCC aligns vec4/matrix members to 16 bytes, MSVC to 4 (car model type, window damage, turbo).
// - Shaders: the PS2 keeps a VU pass (state program, texture slots, waveforms); the Xbox keeps an effect index
//   (shaderlibx .fx, one per template) and a table of 0x44-byte parameters, the textures of the PS2 passes in order.
// - Geometry: VIF packets (UNPACK streams per VU batch, ADC bit in the normal) become a D3D vertex buffer
//   (float3 position, NORMPACKED3 normal, D3DCOLOR, float2 uv [, float2 uv]) and one indexed strip per group.
// - Textures: 8/4 bpp indexed, GS-swizzled, 0..0x80 alpha CLUT become swizzled A8R8G8B8 in a D3DTexture.

#include "game/ps2_vehicle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ps2veh {
namespace {

enum Tag : uint8_t { kFree = 0, kHeader, kTyped, kGraph };

// An internal container type used only by ConvertShaderModel.  It deliberately
// cannot collide with a game resource type: page-file drwShaderModel objects
// have no PCK header, so the public wrapper below supplies one for Converter.
constexpr uint32_t kTypeShaderModelImage = 0x4D534432u;  // "2DSM"

constexpr uint32_t kCd = 0xCDCDCDCDu;
constexpr uint32_t kXbNan = 0x7F800001u;  // what the Xbox exporter leaves in unused float slots
constexpr uint32_t kXbModelLodVtable = 0x005DEA34u;
constexpr uint32_t kXbStreamedTextureVtable = 0x005DDE34u;

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t Rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
float RdF(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

// The PS2 file, its pointers and the byte accounting.
class In {
public:
    explicit In(const std::vector<uint8_t>& f) : d_(f), tag_(f.size(), kFree) {
        if (d_.size() >= 0x80) {
            va0_ = Rd32(&d_[0]);
            type_ = Rd32(&d_[4]);
            version_ = Rd32(&d_[8]);
            body_ = Rd32(&d_[12]);
        }
    }
    bool Valid() const { return d_.size() >= 0x80 && version_ == 1 && size_t(body_) + 0x80 <= d_.size(); }
    uint32_t Type() const { return type_; }
    uint32_t Va0() const { return va0_; }
    uint32_t End() const { return 0x80 + body_; }
    uint32_t Size() const { return uint32_t(d_.size()); }

    // file offset a serialized pointer refers to, 0 when it is null or outside the body
    uint32_t Off(uint32_t va) const { return va >= va0_ && va - va0_ < body_ ? va - va0_ + 0x80 : 0; }
    uint32_t Va(uint32_t off) const { return va0_ + off - 0x80; }
    bool Has(uint32_t off, uint32_t n) const { return off >= 0x80 && off <= End() && n <= End() - off; }

    uint8_t U8(uint32_t o) const { return o < d_.size() ? d_[o] : 0; }
    uint16_t U16(uint32_t o) const { return o + 2 <= d_.size() ? Rd16(&d_[o]) : 0; }
    uint32_t U32(uint32_t o) const { return o + 4 <= d_.size() ? Rd32(&d_[o]) : 0; }
    float F32(uint32_t o) const { return o + 4 <= d_.size() ? RdF(&d_[o]) : 0.f; }
    uint32_t Ptr(uint32_t o) const { return Off(U32(o)); }
    const uint8_t* At(uint32_t o) const { return &d_[o]; }

    void Mark(uint32_t o, uint32_t n, Tag t) {
        if (o >= d_.size()) return;
        n = std::min<uint32_t>(n, uint32_t(d_.size()) - o);
        for (uint32_t i = 0; i < n; i++)
            if (tag_[o + i] == kFree || (t == kTyped && tag_[o + i] == kGraph)) tag_[o + i] = t;
    }
    Tag TagAt(uint32_t o) const { return o < tag_.size() ? Tag(tag_[o]) : kTyped; }

    // a NUL-terminated name, marked as read
    std::string Str(uint32_t o) {
        std::string s;
        if (!o) return s;
        for (uint32_t i = o; i < End() && d_[i]; i++) s.push_back(char(d_[i]));
        Mark(o, uint32_t(s.size()) + 1, kTyped);
        return s;
    }

    // Allocation extents for objects read without a layout: from a start to the next start any pointer names.
    void BuildTargets() {
        targets_.clear();
        for (uint32_t o = 0x80; o + 4 <= End(); o += 4) {
            if (TagAt(o) == kTyped && skip_typed_) continue;
            const uint32_t t = Off(U32(o));
            if (t) targets_.push_back(t);
        }
        for (uint32_t t : extra_starts_) targets_.push_back(t);
        std::sort(targets_.begin(), targets_.end());
        targets_.erase(std::unique(targets_.begin(), targets_.end()), targets_.end());
    }
    uint32_t Extent(uint32_t o) const {
        auto it = std::upper_bound(targets_.begin(), targets_.end(), o);
        return (it == targets_.end() ? End() : *it) - o;
    }
    void AddStart(uint32_t o) { extra_starts_.push_back(o); }

    // Bytes inside some object the root reaches through any chain of pointers (raw, no layout); the rest is
    // dead data: allocations the exporter (or a mod tool that appended and repointed) left without an owner.
    void Reach() {
        reach_.assign(d_.size(), 0);
        std::vector<uint32_t> stack{0x80};
        std::vector<uint8_t> seen(d_.size(), 0);
        while (!stack.empty()) {
            const uint32_t o = stack.back();
            stack.pop_back();
            if (o >= d_.size() || seen[o]) continue;
            seen[o] = 1;
            const uint32_t n = std::min<uint32_t>(Extent(o), End() - o);
            std::fill(reach_.begin() + o, reach_.begin() + o + n, 1);
            if (tag_[o] == kTyped && TypedPayload(o)) continue;
            for (uint32_t k = 0; k + 4 <= n; k += 4)
                if (const uint32_t t = Off(U32(o + k))) stack.push_back(t);
        }
    }
    void Count(Report& r) const {
        r.file_bytes = uint32_t(d_.size());
        for (size_t i = 0; i < d_.size(); i++) {
            switch (tag_[i]) {
                case kHeader: r.header++; break;
                case kTyped: r.typed++; break;
                case kGraph: r.graph++; break;
                default:
                    if (d_[i] == 0xCD) r.padding++;
                    else if (!reach_.empty() && !reach_[i]) r.dead++;
                    else r.unknown++;
            }
        }
    }
    // payloads (VIF packets, texels) whose words are data, never pointers
    void AddPayload(uint32_t o) { payload_.insert(o); }
    bool TypedPayload(uint32_t o) const { return payload_.count(o) != 0; }
    // the largest runs nothing claimed, for the report
    std::vector<std::pair<uint32_t, uint32_t>> UnknownRuns(size_t max) const {
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (uint32_t i = 0; i < d_.size();) {
            if (tag_[i] != kFree || d_[i] == 0xCD || (!reach_.empty() && !reach_[i])) {
                i++;
                continue;
            }
            uint32_t j = i;
            while (j < d_.size() && tag_[j] == kFree && d_[j] != 0xCD && (reach_.empty() || reach_[j])) j++;
            runs.push_back({i, j - i});
            i = j;
        }
        std::sort(runs.begin(), runs.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (runs.size() > max) runs.resize(max);
        return runs;
    }

private:
    const std::vector<uint8_t>& d_;
    std::vector<uint8_t> tag_;
    std::vector<uint32_t> targets_, extra_starts_;
    std::vector<uint8_t> reach_;
    std::set<uint32_t> payload_;
    bool skip_typed_ = true;
    uint32_t va0_ = 0, type_ = 0, version_ = 0, body_ = 0;
};

// The Xbox body. Offsets below 0x80 + PS2 body size are PS2 file offsets (body copied in place); appended objects
// get offsets past it. Every offset maps to the same virtual address space as the PS2 pointers.
class Out {
public:
    Out(const In& in) : va0_(in.Va0()) {
        b_.assign(in.At(0x80), in.At(0x80) + (in.End() - 0x80));
    }
    uint32_t Va(uint32_t off) const { return va0_ + off - 0x80; }
    uint8_t* At(uint32_t off) { return &b_[off - 0x80]; }
    void W8(uint32_t off, uint8_t v) { b_[off - 0x80] = v; }
    void W16(uint32_t off, uint16_t v) { std::memcpy(At(off), &v, 2); }
    void W32(uint32_t off, uint32_t v) { std::memcpy(At(off), &v, 4); }
    void WF(uint32_t off, float v) { std::memcpy(At(off), &v, 4); }
    void Fill(uint32_t off, uint32_t n, uint8_t v) { std::memset(At(off), v, n); }
    void Copy(uint32_t off, const uint8_t* src, uint32_t n) { std::memcpy(At(off), src, n); }
    // appended object, zero-filled; returns its offset
    uint32_t Alloc(uint32_t n, uint32_t align = 16) {
        uint32_t o = uint32_t(b_.size());
        const uint32_t pad = (align - (o % align)) % align;
        b_.insert(b_.end(), pad, 0xCD);
        o = uint32_t(b_.size());
        b_.insert(b_.end(), n, 0);
        return o + 0x80;
    }
    std::vector<uint8_t>& Bytes() { return b_; }

private:
    std::vector<uint8_t> b_;
    uint32_t va0_;
};

// ---------------------------------------------------------------------------------------------------------------
// VIF1 UNPACK, enough of it for the MC3 vehicle microcode packets.

struct VuMem {
    uint32_t q[1024][4];
};

class Vif {
public:
    struct Unpack {
        uint8_t cmd;
        uint16_t addr;
    };
    std::vector<Unpack> unpacks;  // of the current batch

    template <class OnBatch>
    bool Run(const uint8_t* p, uint32_t n, OnBatch&& on_batch, std::string& err) {
        uint32_t o = 0;
        while (o + 4 <= n) {
            const uint32_t v = Rd32(p + o);
            o += 4;
            const uint32_t cmd = (v >> 24) & 0x7F, num = (v >> 16) & 0xFF, imm = v & 0xFFFF;
            if (cmd >= 0x60) {
                if (!DoUnpack(p, n, o, cmd, num, imm)) {
                    err = "UNPACK runs past its packet";
                    return false;
                }
                continue;
            }
            switch (cmd) {
                case 0x00: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: break;
                case 0x01: cl_ = imm & 0xFF, wl_ = (imm >> 8) & 0xFF; break;
                case 0x02: ofst_ = imm & 0x3FF; break;
                case 0x03: base_ = imm & 0x3FF; break;
                case 0x04: itop_ = imm & 0x3FF; break;
                case 0x05: mode_ = imm & 3; break;
                case 0x20:
                    if (o + 4 > n) return false;
                    mask_ = Rd32(p + o), o += 4;
                    break;
                case 0x30: case 0x31:
                    if (o + 16 > n) return false;
                    for (int c = 0; c < 4; c++) (cmd == 0x30 ? row_ : col_)[c] = Rd32(p + o + 4 * c);
                    o += 16;
                    break;
                case 0x14: case 0x15: case 0x17:
                    on_batch(mem_, unpacks);
                    unpacks.clear();
                    break;
                default: {
                    char b[64];
                    std::snprintf(b, sizeof b, "unexpected VIF command %02X", cmd);
                    err = b;
                    return false;
                }
            }
        }
        return true;
    }

private:
    bool DoUnpack(const uint8_t* p, uint32_t n, uint32_t& o, uint32_t cmd, uint32_t num, uint32_t imm) {
        const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, masked = (cmd >> 4) & 1;
        const bool usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
        uint32_t addr = imm & 0x3FF;
        if (flg) addr += tops_;
        const uint32_t count = num ? num : 256, comps = vn + 1;
        const uint32_t esize = vl == 3 ? 2 : (vl == 0 ? 4 : vl == 1 ? 2 : 1) * comps;
        const uint32_t reads = wl_ <= cl_ ? count : (count / wl_) * cl_ + std::min(count % wl_, cl_);
        const uint32_t bytes = reads * esize;
        if (o + bytes > n) return false;
        const uint8_t* src = p + o;
        o += (bytes + 3) & ~3u;
        unpacks.push_back({uint8_t(cmd), uint16_t(addr)});
        uint32_t s = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t cyc, dst;
            bool has;
            if (wl_ <= cl_) {
                cyc = i % wl_, has = true, dst = addr + (i / wl_) * cl_ + cyc;
            } else {
                cyc = i % wl_, has = cyc < cl_, dst = addr + i;
            }
            uint32_t x[4] = {0, 0, 0, 0};
            if (has) {
                const uint8_t* e = src + esize * s++;
                if (vl == 3) {
                    const uint32_t w = Rd16(e);
                    x[0] = (w & 0x1F) << 3, x[1] = ((w >> 5) & 0x1F) << 3, x[2] = ((w >> 10) & 0x1F) << 3;
                    x[3] = ((w >> 15) & 1) << 7;
                } else {
                    for (uint32_t c = 0; c < comps; c++) {
                        if (vl == 0) x[c] = Rd32(e + 4 * c);
                        else if (vl == 1) x[c] = usn ? Rd16(e + 2 * c) : uint32_t(int32_t(int16_t(Rd16(e + 2 * c))));
                        else x[c] = usn ? e[c] : uint32_t(int32_t(int8_t(e[c])));
                    }
                }
            }
            uint32_t* q = mem_.q[dst & 0x3FF];
            for (int c = 0; c < 4; c++) {
                const uint32_t m = masked ? (mask_ >> (std::min<uint32_t>(cyc, 3) * 8 + c * 2)) & 3 : 0;
                if (m == 0) {
                    if (!has || (c >= int(comps) && vl != 3)) continue;
                    uint32_t val = x[c];
                    if (mode_ == 1) val += row_[c];
                    else if (mode_ == 2) val += row_[c], row_[c] = val;
                    q[c] = val;
                } else if (m == 1) {
                    q[c] = row_[c];
                } else if (m == 2) {
                    q[c] = col_[std::min<uint32_t>(cyc, 3)];
                }
            }
        }
        return true;
    }

    VuMem mem_ = {};
    uint32_t row_[4] = {}, col_[4] = {}, mask_ = 0, mode_ = 0, cl_ = 1, wl_ = 1, base_ = 0, ofst_ = 0, tops_ = 0,
             itop_ = 0;
};

struct Vtx {
    float pos[3];
    float nrm[3];
    uint32_t color;  // D3DCOLOR
    float uv[2], uv1[2];
    bool adc;
};

// ---------------------------------------------------------------------------------------------------------------
// Class layouts measured on the 94 cars both builds ship (see the notes at the top of the file).


// Xbox shader flags: the same word, except where the Xbox build has no dedicated class for the template and
// uses the generic effect shader (class 3).
uint32_t XboxShaderFlags(uint32_t flags, const std::string& tmpl) {
    if (tmpl == "licenseplate.shadert") return flags & ~0x80u;
    if (tmpl == "sprocket.shadert") return 3;
    if (tmpl == "rider_stealth.shadert") return 0x41;
    if (tmpl == "car_decal.shadert" || tmpl == "car_decal_chrome.shadert") return flags | 0x100;
    if ((flags & 0x7F) == 1) return (flags & ~0x7Fu) | 3;
    return flags;
}

// One run of a derived class's fields: Xbox byte offset <- PS2 byte offset.
struct Move {
    uint8_t xb, ps2, len;
};
struct ShaderClass {
    uint8_t type;
    uint8_t xb_size;   // total Xbox object size
    uint8_t nan_from, nan_to;  // Xbox float slots the exporter leaves as NaN
    Move moves[6];
};
constexpr ShaderClass kShaderClasses[] = {
    {0x04, 0x18, 0, 0, {{0x14, 0x30, 4}}},                                  // carpaint
    {0x05, 0x28, 0, 0, {{0x14, 0x30, 4}, {0x18, 0x38, 4}, {0x1C, 0x34, 4}, {0x20, 0x3C, 5}}},  // decal multi
    {0x06, 0x24, 0, 0, {{0x14, 0x30, 16}}},                                 // car decal: colour
    {0x07, 0x24, 0, 0, {{0x14, 0x30, 16}}},                                 // car decal chrome: colour
    {0x08, 0x24, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x30, 12}}},                // emissive taillight
    {0x09, 0x24, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x30, 12}}},                // emissive brakelight
    {0x0A, 0x24, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x30, 12}}},                // third brakelight
    {0x0B, 0x24, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x30, 12}}},                // reverselight
    {0x0C, 0x24, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x30, 12}}},                // headlight
    {0x0D, 0x28, 0, 0, {{0x14, 0x30, 12}, {0x20, 0x3C, 8}}},                // car window (+0x20: 2 textures)
    {0x0E, 0x38, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x50, 16}, {0x28, 0x30, 12}, {0x34, 0x40, 4}}},  // colored glass
    {0x0F, 0x1C, 0, 0, {{0x14, 0x30, 8}}},                                  // drop shadow
    {0x10, 0x34, 0, 0, {{0x14, 0x54, 1}, {0x18, 0x50, 4}, {0x1C, 0x40, 4}, {0x20, 0x30, 16}, {0x30, 0x55, 1}}},
    {0x11, 0x2C, 0x2C, 0x38, {{0x14, 0x40, 4}, {0x18, 0x30, 16}, {0x28, 0x50, 1}}},  // exhaust chrome
    {0x12, 0x38, 0x18, 0x24, {{0x14, 0x41, 4}, {0x24, 0x30, 16}, {0x34, 0x40, 1}}},  // masked chrome
    {0x19, 0x2C, 0, 0, {{0x14, 0x41, 4}, {0x18, 0x30, 16}, {0x28, 0x40, 1}}},  // more chrome wheel hi lod
    {0x21, 0x24, 0, 0, {{0x14, 0x30, 8}}},                                  // carbon fiber
    {0x22, 0x24, 0, 0, {{0x14, 0x30, 4}, {0x18, 0x38, 4}, {0x1C, 0x34, 4}, {0x20, 0x3C, 4}}},  // hood
    {0x23, 0x24, 0, 0, {{0x14, 0x30, 4}, {0x18, 0x38, 4}, {0x1C, 0x34, 4}, {0x20, 0x3C, 4}}},  // taillight casing
    {0x25, 0x24, 0, 0, {{0x14, 0x40, 1}, {0x18, 0x34, 12}}},                // coplight pass 1
    {0x26, 0x24, 0, 0, {{0x14, 0x3C, 4}, {0x18, 0x30, 12}}},                // coplight pass 2
    {0x28, 0x18, 0, 0, {{0x14, 0x30, 4}}},                                  // aa trim
    {0x29, 0x24, 0x18, 0x24, {{0x14, 0x30, 4}}},                            // aa chrome
    {0x3D, 0x18, 0, 0, {{0x14, 0x30, 4}}},                                  // distort
    {0x3E, 0x18, 0, 0, {{0x14, 0x30, 4}}},                                  // carpaint no vinyl
};

const ShaderClass* FindClass(uint32_t type) {
    for (const ShaderClass& c : kShaderClasses)
        if (c.type == type) return &c;
    return nullptr;
}

bool IsContainer(uint32_t type) { return type == 0x05 || type == 0x22 || type == 0x23; }

// ---------------------------------------------------------------------------------------------------------------

uint32_t PackNormal(const float n[3]) {
    auto q = [](float v, int bits) {
        const int m = (1 << (bits - 1)) - 1;
        int i = int(std::lround(std::max(-1.f, std::min(1.f, v)) * m));
        return uint32_t(i) & ((1u << bits) - 1);
    };
    return q(n[0], 11) | (q(n[1], 11) << 11) | (q(n[2], 10) << 22);
}

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) n++;
    return n;
}

// Xbox swizzle: x and y bits interleave from bit 0, x first; an axis drops out once used up
uint32_t Morton(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    uint32_t r = 0, bit = 0;
    for (uint32_t i = 0; i < lw || i < lh; i++) {
        if (i < lw) r |= ((x >> i) & 1) << bit++;
        if (i < lh) r |= ((y >> i) & 1) << bit++;
    }
    return r;
}

// GS PSMT8 swizzle: linear (x, y) -> index in the stored block (mc3_tex.py's _swizzle8_map)
uint32_t Psmt8Index(uint32_t x, uint32_t y, uint32_t w) {
    const uint32_t blk_y = (y & ~0xFu) * w;
    const uint32_t swap = (((y + 2) >> 2) & 1) * 4;
    const uint32_t pos_y = ((((y & ~3u) >> 1) + (y & 1)) & 7);
    const uint32_t col_y = pos_y * w * 2;
    const uint32_t byte_y = (y >> 1) & 1;
    const uint32_t block = blk_y + (x & ~0xFu) * 2;
    const uint32_t column = col_y + ((x + swap) & 7) * 4;
    return block + column + byte_y + ((x >> 2) & 2);
}

// PS2 CLUT: in each block of 32 colours the two middle groups of 8 swap
uint32_t ClutIndex(uint32_t i) { return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); }

// "carpaint.shadert" -> "carpaint.fx": the Xbox build loads one effect per PS2 shader template
std::string EffectName(const std::string& tmpl) {
    const size_t dot = tmpl.rfind('.');
    return (dot == std::string::npos ? tmpl : tmpl.substr(0, dot)) + ".fx";
}

// A skeleton at file offset s: u16 node count, nodes of 0x44 bytes at +8, their names, all inside the image.
bool ValidSkeleton(const In& in, uint32_t s) {
    if (!s || !in.Has(s, 0xC)) return false;
    const uint32_t n = in.U16(s), nodes = in.Ptr(s + 8);
    if (n == 0 || n > 256 || !nodes || !in.Has(nodes, n * 0x44)) return false;
    for (uint32_t i = 0; i < n; i++)
        if (in.U32(nodes + i * 0x44 + 0xC) && !in.Ptr(nodes + i * 0x44 + 0xC)) return false;
    return true;
}

class Converter {
public:
    Converter(const std::vector<uint8_t>& f, const Effects& fx, Report& r)
        : in_(f), out_(in_), fx_(fx), rep_(r), page_model_(in_.Type() == kTypeShaderModelImage) {}

    bool Run(std::vector<uint8_t>& body, uint32_t& base_va, uint32_t& type, std::string& err) {
        if (!in_.Valid()) {
            err = "not a load-in-place resource (version/size)";
            return false;
        }
        in_.Mark(0, 0x80, kHeader);
        if (in_.Type() == kTypeVehiclePs2) {
            type = kTypeVehicleXbox;
            Vehicle(0x80);
        } else if (in_.Type() == kTypeMesh) {
            type = kTypeMesh;
            // the resource's root is the mesh itself; its header is rewritten in place
            if (!Mesh(0x80)) {
                err = error_.empty() ? "mesh conversion failed" : error_;
                return false;
            }
        } else if (in_.Type() == kTypeShaderModelImage) {
            type = kTypeShaderModelImage;
            PpfShaderModel(0x80);
        } else {
            char b[64];
            std::snprintf(b, sizeof b, "unsupported resource type %X", in_.Type());
            err = b;
            return false;
        }
        if (!error_.empty()) {
            err = error_;
            return false;
        }
        // whatever the typed walk did not read, follow by pointer (same layout on both builds)
        in_.BuildTargets();
        for (auto [s, rest] : graph_roots_) Graph(s, rest);
        in_.Reach();
        in_.Count(rep_);
        for (auto [o, n] : in_.UnknownRuns(6)) {
            char b[96];
            std::snprintf(b, sizeof b, "unclaimed %u bytes at file +%X", n, o);
            rep_.notes.push_back(b);
        }
        base_va = in_.Va0();
        body.swap(out_.Bytes());
        rep_.out_bytes = uint32_t(body.size());
        return true;
    }

private:
    // ---- byte accounting helpers --------------------------------------------------------------------------
    void Typed(uint32_t o, uint32_t n) {
        if (o) in_.Mark(o, n, kTyped), in_.AddStart(o);
    }
    // objects read without a layout; `rest` also walks one whose header was typed
    void Later(uint32_t o, bool rest = false) {
        if (o) graph_roots_.push_back({o, rest});
    }
    void Graph(uint32_t o, bool rest) {
        std::vector<std::pair<uint32_t, bool>> stack{{o, rest}};
        while (!stack.empty()) {
            const auto [s, force] = stack.back();
            stack.pop_back();
            if (!s || !graph_seen_.insert(s).second) continue;
            if (in_.TagAt(s) == kTyped && !force) continue;
            const uint32_t n = std::min<uint32_t>(in_.Extent(s), 0x10000);
            in_.Mark(s, n, kGraph);
            for (uint32_t k = 0; k + 4 <= n; k += 4)
                if (const uint32_t t = in_.Ptr(s + k)) stack.push_back({t, false});
        }
    }
    void Note(const std::string& s) {
        if (std::find(rep_.notes.begin(), rep_.notes.end(), s) == rep_.notes.end()) rep_.notes.push_back(s);
    }
    // a GCC array cookie sits 0x10 before the array, an MSVC one 4 before: give the Xbox code its own
    void Cookie(uint32_t array, uint32_t count) {
        if (array >= 0x90 && in_.U32(array - 0x10) == count) Typed(array - 0x10, 4);
        if (array >= 0x84 && in_.U32(array - 4) == kCd) out_.W32(array - 4, count);
    }

    // ---- the vehicle --------------------------------------------------------------------------------------
    void Vehicle(uint32_t r) {
        Typed(r, 0x50);
        CarModelType(in_.Ptr(r + 0x08));
        Bound(in_.Ptr(r + 0x0C));
        Later(in_.Ptr(r + 0x10));  // physics material
        Later(in_.Ptr(r + 0x4C));  // rider animation set (bikes); null in cars
        for (uint32_t k : {0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x30u, 0x3Cu, 0x40u, 0x44u, 0x48u})
            NamedTune(in_.Ptr(r + k));
        Audio(in_.Ptr(r + 0x28));
        VehSim(in_.Ptr(r + 0x34));
        VehSim(in_.Ptr(r + 0x38));
    }

    void NamedTune(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0xC);
        in_.Str(in_.Ptr(o + 4));
        Later(o, true);
    }

    // VehSim: 0x94 bytes, then 16 subsystem pointers at +0x54
    void VehSim(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x94);
        in_.Str(in_.Ptr(o + 4));
        Later(o, true);
        for (uint32_t k = 0x54; k < 0x94; k += 4) {
            const uint32_t s = in_.Ptr(o + k);
            if (!s) continue;
            if (k == 0x78) Fluid(s);
            else if (k == 0x84) Turbo(s);
            else Later(s);
        }
    }

    // GCC keeps a base vptr at +0x0C of the fluid tune; MSVC puts it first
    void Fluid(uint32_t o) {
        if (!seen_.insert(o).second) return;
        Typed(o, 0x1C);
        uint8_t src[0x1C];
        std::memcpy(src, in_.At(o), 0x1C);
        out_.W32(o, 0);
        out_.Copy(o + 4, src, 0xC);
        out_.Copy(o + 0x10, src + 0x10, 0xC);
    }

    // super stop turbo: GCC aligns the smoke colours to 16 bytes (8 bytes of padding at +0x58)
    void Turbo(uint32_t o) {
        if (!seen_.insert(o).second) return;
        Typed(o, 0x80);
        uint8_t src[0x80];
        std::memcpy(src, in_.At(o), 0x80);
        out_.Copy(o + 0x58, src + 0x60, 0x20);
        out_.Fill(o + 0x78, 8, 0xCD);
    }

    void Audio(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0xB8);
        in_.Str(in_.Ptr(o + 4));
        for (uint32_t k = 0x18; k < 0xA8; k += 4) {
            const uint32_t b = in_.Ptr(o + k);
            const uint32_t slot = (k - 0x18) / 4;
            if (!b) continue;
            if (slot >= 18 && slot < 31) AudioBlock(b);
            else Later(b);
        }
    }

    void AudioBlock(uint32_t o) {
        if (!seen_.insert(o).second) return;
        Typed(o, 0xA0);
        if (const uint32_t ranges = in_.Ptr(o + 0x38)) Typed(ranges, 8 * 0x2C);
        for (uint32_t k = 0x88; k < 0x9C; k += 4) {
            const uint32_t c = in_.Ptr(o + k);
            if (!c || !seen_.insert(c).second) continue;
            Typed(c, 0x20);
            if (const uint32_t rows = in_.Ptr(c + 0x18)) Typed(rows, 8 * 0x24);
        }
    }

    void Bound(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x80);
        const uint32_t nv = in_.U32(o + 0x4C), np = in_.U32(o + 0x50), nm = in_.U8(o + 5);
        if (const uint32_t v = in_.Ptr(o + 0x68); v && nv < 65536) Typed(v, nv * 12);
        if (const uint32_t p = in_.Ptr(o + 0x6C); p && np < 65536) Typed(p, np * 0x20);
        if (const uint32_t m = in_.Ptr(o + 0x70); m) Typed(m, nm * 4);
    }

    // ---- the car model type (root+0x08), 0x3890 bytes on the PS2 and 0x3880 on the Xbox ------------------
    void CarModelType(uint32_t o) {
        if (!o || !in_.Has(o, 0x3890)) {
            if (o) error_ = "car model type outside the file";
            return;
        }
        Typed(o, 0x3890);
        std::vector<uint8_t> s(in_.At(o), in_.At(o) + 0x3890);
        out_.Fill(o, 0x3890, 0xCD);
        out_.W32(o, 0);                                  // vptr, written by its constructor
        out_.Copy(o + 0x04, &s[0x00], 0x1C);
        for (uint32_t k = 0x20; k < 0x2C; k += 4) out_.W32(o + k, kXbNan);
        out_.Copy(o + 0x2C, &s[0x2C], 0x390 - 0x2C);
        out_.Copy(o + 0x38C, &s[0x390], 0x3718 - 0x390);  // 16-byte aligned matrices on the PS2
        out_.Copy(o + 0x3714, &s[0x3720], 0x388C - 0x3720);

        in_.Str(in_.Ptr(o + 0x08));
        ShaderModel(in_.Ptr(o + 0xD8));
        Skeleton(in_.Ptr(o + 0xDC));
        WindowDamage(o + 0x315C);
        if (const uint32_t w = in_.Ptr(o + 0x3810)) Typed(w, std::min<uint32_t>(in_.U32(o + 0x3814), 8) * 0x1C);
        if (const uint32_t bits = in_.Ptr(o + 0x381C)) Typed(bits, in_.U16(o + 0x3820) * 4);
        if (const uint32_t sp = in_.Ptr(o + 0x3864)) Typed(sp, 0x64);
        ShaderGroup(in_.Ptr(o + 0x3868));
        for (uint32_t k = 0x386C; k <= 0x3874; k += 4) Shader(in_.Ptr(o + k));
        Occluders(in_.Ptr(o + 0x3888));
    }

    void WindowDamage(uint32_t o) {
        // the object is embedded (vptr at o); its windows are 0x60-byte records on the PS2, 0x50 on the Xbox
        Later(in_.Ptr(o + 4));
        const uint32_t w = in_.Ptr(o + 0xC), n = in_.U32(o + 0x14);
        if (!w || n > 8 || !in_.Has(w, n * 0x60)) return;
        Typed(w, n * 0x60);
        std::vector<uint8_t> s(in_.At(w), in_.At(w) + n * 0x60);
        out_.Fill(w, n * 0x60, 0xCD);
        for (uint32_t i = 0; i < n; i++) {
            out_.Copy(w + i * 0x50, &s[i * 0x60], 8);
            out_.Copy(w + i * 0x50 + 8, &s[i * 0x60 + 0x10], 0x48);
        }
    }

    // rmVehOccludeDataSet: groups of convex planes; each plane record loses 8 bytes of alignment on the Xbox
    void Occluders(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0xC);
        const uint32_t ng = in_.U32(o + 4), g = in_.Ptr(o + 8);
        if (!g || ng > 64) return;
        Typed(g, ng * 0x10);
        Cookie(g, ng);
        for (uint32_t i = 0; i < ng; i++) {
            const uint32_t np = in_.U32(g + i * 0x10 + 4), pl = in_.Ptr(g + i * 0x10 + 8);
            if (!pl || np > 64 || !in_.Has(pl, np * 0x20)) continue;
            Typed(pl, np * 0x20);
            std::vector<uint8_t> s(in_.At(pl), in_.At(pl) + np * 0x20);
            out_.Fill(pl, np * 0x20, 0xCD);
            for (uint32_t k = 0; k < np; k++) {
                out_.Copy(pl + k * 0x18, &s[k * 0x20], 0x18);
                Later(in_.Ptr(pl + k * 0x20 + 0x14));
            }
        }
    }

    // ---- drwShaderModel: same layout on both builds ------------------------------------------------------
    void ShaderModel(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x84);
        ShaderGroup(in_.Ptr(o + 0x08));
        for (uint32_t k = 0x10; k <= 0x1C; k += 4) Lod(in_.Ptr(o + k));
        Skeleton(in_.Ptr(o + 0x68));
        Later(in_.Ptr(o + 0x6C));
        Bound(in_.Ptr(o + 0x78));
        BonyData(in_.Ptr(o + 0x80));
    }

    // The shared wheel/exhaust PPF entries serialize drwShaderModel directly.
    // Its three model pointers are rmcModel LOD tables, not the drwLod form
    // used by a vehicle's shader model: each table starts with u16 count at
    // +2 and a rmcModelGeom* array at +8.  Treating those as drwLod makes the
    // name at the start of the table look like a bogus type byte and skips all
    // geometry.  This is intentionally a separate walker: ordinary vehicle
    // resources still use ShaderModel() above.
    void PpfShaderModel(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x1C);
        // PPF wheel/exhaust pages serialize the PS2 drwShaderModel vptr.  The
        // surrounding loader relocates the data pointers, but cannot turn a
        // PS2 dispatch target into the Xbox class vtable.  Retail Xbox wheel
        // and exhaust pages use this root vtable for the same object.
        out_.W32(o, 0x00608EC4u);
        ShaderGroup(in_.Ptr(o + 8));
        for (uint32_t k : {0x10u, 0x14u, 0x18u}) PpfModelLod(in_.Ptr(o + k));
        // A stale pointer can also land on an object this walk already claimed (and rewrites): the skeleton must not.
        const uint32_t skeleton = in_.Ptr(o + 0x68);
        if (ValidSkeleton(in_, skeleton) && Unclaimed(skeleton, 0xC) &&
            Unclaimed(in_.Ptr(skeleton + 8), in_.U16(skeleton) * 0x44)) {
            Skeleton(skeleton);
        } else if (in_.U32(o + 0x68) && skeleton_) {
            ReplaceSkeleton(o + 0x68, *skeleton_);
        } else if (in_.U32(o + 0x68)) {
            out_.W32(o + 0x68, 0);
            Note("invalid skeleton cleared (no valid one in the page file)");
        }
    }

    bool Unclaimed(uint32_t o, uint32_t n) const {
        for (uint32_t i = 0; i < n; i++)
            if (in_.TagAt(o + i) != kFree) return false;
        return true;
    }

    // A copy of `t` appended to the image, its internal pointers moved with it; `field` names the new header.
    void ReplaceSkeleton(uint32_t field, const SkeletonTemplate& t) {
        const uint32_t size = uint32_t(t.bytes.size());
        const uint32_t body = out_.Alloc(size, 16);
        const uint32_t shift = out_.Va(body) - t.va;
        out_.Copy(body, t.bytes.data(), size);
        for (uint32_t k = 0; k + 4 <= size; k += 4)
            if (const uint32_t w = Rd32(&t.bytes[k]); w >= t.va && w < t.va + size) out_.W32(body + k, w + shift);
        const uint32_t header = out_.Alloc(0xC, 16);
        out_.Copy(header, t.header.data(), 0xC);
        out_.W32(header + 8, Rd32(&t.header[8]) + shift);
        out_.W32(field, out_.Va(header));
        Note("invalid skeleton replaced with the page file's first valid one");
    }

    void PpfModelLod(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0xC);
        const uint32_t n = in_.U16(o + 2), meshes = in_.Ptr(o + 8);
        if (!meshes || n == 0 || n > 32 || !in_.Has(meshes, n * 4)) {
            Note("PPF model LOD has no valid mesh table");
            return;
        }
        // GCC serializes the non-polymorphic count word before the vptr;
        // MSVC puts the vptr first.  The page loader relocates pointers but
        // does not run a constructor for this embedded rmcModel table.
        out_.W32(o, kXbModelLodVtable);
        out_.W32(o + 4, in_.U32(o));
        Typed(meshes, n * 4);
        Cookie(meshes, n);
        for (uint32_t i = 0; i < n; i++) Mesh(in_.Ptr(meshes + i * 4));
    }

    void Skeleton(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0xC);
        const uint32_t n = in_.U16(o), nodes = in_.Ptr(o + 8);
        if (!nodes || n >= 4096 || !in_.Has(nodes, n * 0x44)) return;
        Typed(nodes, n * 0x44);
        for (uint32_t i = 0; i < n; i++) {
            in_.Str(in_.Ptr(nodes + i * 0x44 + 0xC));
            Later(in_.Ptr(nodes + i * 0x44 + 0x40));  // joint limits
        }
    }

    void HashTable(uint32_t o, bool names) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x18);
        const uint32_t nb = in_.U32(o + 8), b = in_.Ptr(o + 0x10);
        if (!b || nb > 4096) return;
        Typed(b, nb * 4);
        for (uint32_t i = 0; i < nb; i++)
            for (uint32_t e = in_.Ptr(b + i * 4), guard = 0; e && guard < 4096; e = in_.Ptr(e + 8), guard++) {
                if (!seen_.insert(e).second) break;
                Typed(e, 0xC);
                if (names) in_.Str(in_.Ptr(e));
            }
    }

    void BonyData(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x10);
        HashTable(in_.Ptr(o), true);
        HashTable(in_.Ptr(o + 4), true);
    }

    // ---- shaders --------------------------------------------------------------------------------------------
    void ShaderGroup(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x10);
        const uint32_t slots = in_.Ptr(o + 4), n = in_.U16(o + 8);
        if (slots && n < 4096) {
            Typed(slots, n * 4);
            Cookie(slots, n);
            for (uint32_t i = 0; i < n; i++) Shader(in_.Ptr(slots + i * 4));
        }
        for (uint32_t l = in_.Ptr(o + 0xC), guard = 0; l && guard < 1024; l = in_.Ptr(l), guard++) {
            if (!seen_.insert(l).second) break;
            Typed(l, 0xC);
            in_.Str(in_.Ptr(l + 4));
        }
    }

    // the render-state program of a VU pass: u8 opcodes, one-byte or four-byte operands, 0xFF ends
    void StateProgram(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        uint32_t p = o;
        while (p < in_.End()) {
            const uint8_t op = in_.U8(p);
            p += op == 0xFF ? 1 : (op & 0xC0) ? 5 : 2;
            if (op == 0xFF) break;
        }
        Typed(o, p - o);
    }

    void Shader(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        rep_.shaders++;
        const uint32_t flags = in_.U32(o + 4), type = flags & 0x7F;
        if (type == 0) {  // rmcShaderBasic: [vptr][flags][texture], same on both
            Typed(o, 0xC);
            Texture(in_.Ptr(o + 8));
            return;
        }
        // complex: inline first pass, linked passes, template name, mask, derived fields from +0x30
        std::vector<uint32_t> textures;
        for (uint32_t pass = o + 8, guard = 0; pass && guard < 8; guard++) {
            Typed(pass, 0x20);
            StateProgram(in_.Ptr(pass));
            const uint32_t slots = in_.Ptr(pass + 8), count = in_.U8(pass + 0x18);
            if (slots && count <= 16) {
                Typed(slots, count * 4);
                for (uint32_t t = 0; t < count; t++) {
                    textures.push_back(in_.U32(slots + t * 4));
                    Texture(in_.Ptr(slots + t * 4));
                }
            }
            Later(in_.Ptr(pass + 0xC));
            Later(in_.Ptr(pass + 0x10));
            if (const uint32_t tr = in_.Ptr(pass + 0x14)) {
                Typed(tr, 0x18);
                for (uint32_t k = 0; k < 0x18; k += 4) Later(in_.Ptr(tr + k));  // matrix, waveforms
            }
            pass = in_.Ptr(pass + 0x1C);
        }
        const std::string tmpl = in_.Str(in_.Ptr(o + 0x28));
        const ShaderClass* cls = FindClass(type);
        const bool xbox_unmapped = type == 1 || type == 0x2A || type == 0x20 || type == 0x24 || type == 0x2C ||
                                     type == 0x40 || type == 0x42 || type == 0x36;
        const uint32_t ps2_size = cls ? std::max<uint32_t>(0x30, MaxPs2(*cls)) : 0x30;
        Typed(o, ps2_size);
        const uint32_t xb_size = cls ? cls->xb_size : 0x14;
        if (xb_size > ps2_size + 0x10) {
            error_ = "Xbox shader larger than its PS2 object";
            return;
        }
        std::vector<uint8_t> s(0x60, 0xCD);
        std::memcpy(s.data(), in_.At(o), std::min<uint32_t>(0x60, in_.Size() - o));
        int effect = -1;
        if (!IsContainer(type)) {
            auto it = fx_.find(EffectName(tmpl));
            if (it == fx_.end()) it = fx_.find("black_matte.fx");
            if (it == fx_.end()) {
                error_ = "the game's effect table has no " + EffectName(tmpl) + " and no black_matte.fx";
                return;
            }
            if (it->first != EffectName(tmpl)) Note("no Xbox effect for '" + tmpl + "': drawn with black_matte.fx");
            effect = it->second;
        }
        if (!cls && !xbox_unmapped) {
            char b[96];
            std::snprintf(b, sizeof b, "shader class %02X (%s): using the generic Xbox effect shader", type,
                          tmpl.c_str());
            Note(b);
        }
        out_.Fill(o, xb_size, 0xCD);
        out_.W32(o, 0);
        uint32_t xbox_flags = XboxShaderFlags(flags, tmpl);
        if (!cls && !xbox_unmapped) xbox_flags = (xbox_flags & ~0x7Fu) | 3u;
        out_.W32(o + 4, xbox_flags);
        out_.W32(o + 8, uint32_t(effect));
        out_.W32(o + 0xC, 0);  // the effect pointer, looked up by index on load
        uint32_t params = 0;
        if (!IsContainer(type)) {
            const uint32_t n = uint32_t(textures.size());
            params = out_.Alloc((n + 2) * 0x44);
            out_.Fill(params, (n + 2) * 0x44, 0xCD);
            for (uint32_t i = 0; i < n + 2; i++) {
                const bool texture = i < n && !null_textures_.count(textures[i]);
                out_.W32(params + i * 0x44, texture ? 4 : 0);
                out_.W32(params + i * 0x44 + 4, texture ? textures[i] : 0);
            }
        }
        out_.W32(o + 0x10, params ? out_.Va(params) : 0);
        if (cls) {
            if (cls->nan_to)
                for (uint32_t k = cls->nan_from; k < cls->nan_to; k += 4) out_.W32(o + k, kXbNan);
            for (const Move& m : cls->moves)
                if (m.len) out_.Copy(o + m.xb, &s[m.ps2], m.len);
        }
        if (IsContainer(type)) {
            // children: a pointer array (decal multi keeps it inline at +0x48)
            const uint32_t kids = in_.Ptr(o + 0x30), n = in_.U32(o + 0x38);
            if (kids && n <= 8) {
                Typed(kids, n * 4);
                for (uint32_t i = 0; i < n; i++) Shader(in_.Ptr(kids + i * 4));
            }
        }
        if (type == 0x0D) {  // car window: two texture references at +0x3C on the PS2
            Texture(in_.Ptr(o + 0x3C));
            Texture(in_.Ptr(o + 0x40));
        }
    }

    static uint32_t MaxPs2(const ShaderClass& c) {
        uint32_t m = 0x30;
        for (const Move& mv : c.moves)
            if (mv.len) m = std::max<uint32_t>(m, mv.ps2 + mv.len);
        if (c.type == 0x05) m = std::max<uint32_t>(m, 0x50);
        return (m + 3) & ~3u;
    }

    // ---- textures -------------------------------------------------------------------------------------------
    void Texture(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        const uint8_t kind = in_.U8(o + 4);
        if (kind == 2) {  // reference by name
            Typed(o, 0x10);
            in_.Str(in_.Ptr(o + 8));
            return;
        }
        if (kind == 3) {  // dictionary reference
            Typed(o, 0x14);
            return;
        }
        if (kind != 0) {
            char b[64];
            std::snprintf(b, sizeof b, "texture kind %u at +%X left as is", kind, o);
            Note(b);
            Later(o);
            return;
        }
        rep_.textures++;
        Typed(o, 0xA0);
        const std::string name = in_.Str(in_.Ptr(o + 0x78));
        const uint64_t tex0 = uint64_t(in_.U32(o + 0x10)) | (uint64_t(in_.U32(o + 0x14)) << 32);
        const uint32_t psm = (tex0 >> 20) & 0x3F, tw = (tex0 >> 26) & 0xF, th = (tex0 >> 30) & 0xF;
        const uint32_t w = 1u << tw, h = 1u << th;
        const uint32_t mips = std::max<uint32_t>(1, in_.U8(o + 0x86));
        const uint32_t clamp = in_.U8(o + 0x85);
        const uint64_t tex1 = uint64_t(in_.U32(o + 0x88)) | (uint64_t(in_.U32(o + 0x8C)) << 32);
        const uint32_t level0 = in_.Ptr(o + 0x48);
        const uint32_t last = in_.Ptr(o + 0x48 + (std::min<uint32_t>(mips, 4) - 1) * 0xC);
        const bool p8 = psm == 0x13, p4 = psm == 0x14;
        if (!level0 || !last || (!p8 && !p4) || w > 1024 || h > 1024) {
            char b[96];
            std::snprintf(b, sizeof b, "texture '%s': PSM %02X %ux%u not converted", name.c_str(), psm, w, h);
            Note(b);
            if (page_model_ && in_.U32(o + 0x50)) {
                // A PPF texture keeps its texels in a separate page chunk.
                // PS2 stores the chunk reference at +0x4C/+0x50; the Xbox
                // rmcTexture layout moves it to +0x10/+0x14 and keeps the
                // dimensions inline. Convert the proxy here while the page
                // merger converts the referenced GS chunk to an Xbox chunk.
                const uint32_t flags = in_.U32(o + 4), chunk_offset = in_.U32(o + 0x4C);
                const uint32_t chunk_page = in_.U32(o + 0x50), name_va = in_.U32(o + 0x78);
                out_.Fill(o, 0xA0, 0xCD);
                out_.Fill(o, 0x80, 0);
                out_.W32(o, kXbStreamedTextureVtable);
                out_.W32(o + 4, flags);
                out_.W16(o + 8, uint16_t(w));
                out_.W16(o + 0xA, uint16_t(h));
                out_.W32(o + 0x10, chunk_offset);
                out_.W32(o + 0x14, chunk_page);
                out_.W32(o + 0x68, 0x01000001u);
                out_.W32(o + 0x70, name_va);
                return;
            }
            // A streamed PPF model's texels are another page-file chunk, not
            // part of this object image.  Name references cannot be created
            // during page-in because their dictionary is not active yet, so
            // page models put a null texture directly in the converted shader
            // parameters.  Ordinary vehicle resources keep using the name
            // reference path supported by their loader.
            out_.Fill(o, 0x10, 0);
            if (page_model_) {
                null_textures_.insert(in_.Va(o));
            } else {
                out_.W8(o + 4, 2);
                out_.W32(o + 8, in_.U32(o + 0x78));
            }
            return;
        }
        // level sizes as the GS packs them; the CLUT follows the last resident level
        auto level_bytes = [&](uint32_t l) { return std::max<uint32_t>(1, (w >> l) * (h >> l) * (p8 ? 8 : 4) / 8); };
        const uint32_t px = level0 + 0x90, clut = last + 0x90 + level_bytes(mips - 1);
        const uint32_t colors = p8 ? 256 : 16;
        if (!in_.Has(px, level_bytes(0)) || !in_.Has(clut, colors * 4)) {
            Note("texture '" + name + "': pixels outside the file");
            return;
        }
        for (uint32_t l = 0; l < std::min<uint32_t>(mips, 4); l++)
            if (const uint32_t b = in_.Ptr(o + 0x48 + l * 0xC)) Typed(b, 0x90 + level_bytes(l)), in_.AddPayload(b);
        Typed(clut, colors * 4);
        if (mips > 1) Note("texture '" + name + "': only the top mip level is converted");

        uint32_t pal[256];
        for (uint32_t i = 0; i < colors; i++) {
            const uint8_t* c = in_.At(clut + 4 * (p8 ? ClutIndex(i) : i));
            const uint32_t a = c[3] ? std::min(255, c[3] * 2 - 1) : 0;
            pal[i] = (a << 24) | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
        }
        const uint32_t lw = Log2(w), lh = Log2(h), bytes = w * h * 4;
        const uint32_t desc = out_.Alloc(0x80 + bytes, 0x80);
        out_.Fill(desc, 0x80, 0xCD);
        out_.W32(desc, 0);
        out_.W32(desc + 4, 0);                 // chunk tag, written by the loader
        out_.W32(desc + 8, out_.Va(o + 0xC));  // owner: the info's level record
        out_.W32(desc + 0xC, bytes + 0x100);
        out_.W32(desc + 0x14, 0xFFFF0024);
        out_.W32(desc + 0x18, 0);
        // D3DTexture: refcount 1 | texture, Data (rewritten on every bind), A8R8G8B8 swizzled, 1 level
        out_.W32(desc + 0x20, 0x00040001);
        out_.W32(desc + 0x24, out_.Va(desc + 0x80) - 0x80000000u);
        out_.W32(desc + 0x28, 0);
        out_.W32(desc + 0x2C, 0x29 | (0x06 << 8) | (1 << 16) | (lw << 20) | (lh << 24));
        out_.W32(desc + 0x30, 0);
        uint8_t* dst = out_.At(desc + 0x80);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t idx;
                if (p8) idx = in_.U8(px + Psmt8Index(x, y, w));
                else idx = (in_.U8(px + ((y * w + x) >> 1)) >> (((y * w + x) & 1) * 4)) & 0xF;
                std::memcpy(dst + Morton(x, y, lw, lh) * 4, &pal[idx], 4);
            }
        rep_.texture_pixels += w * h;

        // the info itself: 0xA0 -> 0x80 bytes, in place
        uint8_t keep[8];
        std::memcpy(keep, in_.At(o), 8);
        out_.Fill(o, 0xA0, 0xCD);
        out_.Fill(o, 0x80, 0);
        out_.Copy(o + 4, keep + 4, 4);  // kind, refcount
        out_.W16(o + 8, uint16_t(w));
        out_.W16(o + 0xA, uint16_t(h));
        out_.W32(o + 0xC, out_.Va(desc));
        out_.W8(o + 0x68, 1);
        out_.W8(o + 0x69, clamp & 1);
        out_.W8(o + 0x6A, (clamp >> 2) & 1);
        out_.W8(o + 0x6B, (tex1 >> 5) & 1);  // TEX1.MMAG: linear
        out_.W32(o + 0x70, in_.U32(o + 0x78));
    }

    // ---- LOD tables and meshes ------------------------------------------------------------------------------
    void Lod(uint32_t o) {
        if (!o || !seen_.insert(o).second) return;
        Typed(o, 0x14);
        const uint32_t w0 = in_.U32(o), n = w0 >> 16, kind = w0 & 0xFF;
        if (kind != 2) {
            char b[64];
            std::snprintf(b, sizeof b, "LOD kind %u at +%X", kind, o);
            Note(b);
        }
        uint8_t s[0x14];
        std::memcpy(s, in_.At(o), 0x14);
        out_.W32(o, 0);
        out_.W32(o + 4, Rd32(s));
        const uint32_t meshes = in_.Ptr(o + 8), names = in_.Ptr(o + 0xC), ids = in_.Ptr(o + 0x10);
        if (n >= 4096) return;
        if (names) {
            Typed(names, n * 4);
            for (uint32_t i = 0; i < n; i++) in_.Str(in_.Ptr(names + i * 4));
        }
        if (ids) Typed(ids, n * 4);
        if (!meshes) return;
        Typed(meshes, n * 4);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t m = in_.Ptr(meshes + i * 4);
            if (!m) continue;
            Mesh(m);
        }
    }

    // rmcModelGeom: decode the VIF packets of every group and rebuild them as the Xbox vertex/index buffers
    uint32_t Mesh(uint32_t o) {
        if (auto it = meshes_.find(o); it != meshes_.end()) return it->second;
        if (!in_.Has(o, 0x20)) {
            error_ = "mesh outside the file";
            return 0;
        }
        Typed(o, 0x20);
        const uint32_t piece = in_.U16(o + 6), ngroups = in_.U16(o + 8), slots = in_.U16(o + 0xA);
        const uint32_t mat = in_.Ptr(o + 0xC), groups = in_.Ptr(o + 0x10);
        if (slots || in_.U32(o + 0x14)) Note("a mesh carries damage/blend slots that are not converted");
        if (!groups || ngroups == 0 || ngroups > 256) {
            error_ = "mesh without groups";
            return 0;
        }
        if (mat) Typed(mat, ngroups * 2);
        Typed(groups, ngroups * 8);
        Cookie(groups, ngroups);
        rep_.meshes++;

        std::vector<std::vector<std::vector<Vtx>>> batches(ngroups);  // group -> batch -> vertices
        bool two_uv = false;
        for (uint32_t g = 0; g < ngroups; g++) {
            const uint32_t blocks = in_.Ptr(groups + g * 8), nb = in_.U16(groups + g * 8 + 4);
            if (!blocks || nb > 4096) continue;
            Typed(blocks, nb * 8);
            for (uint32_t b = 0; b < nb; b++) {
                const uint32_t pk = in_.Ptr(blocks + b * 8), qwc = in_.U16(blocks + b * 8 + 4);
                if (!pk || !in_.Has(pk, qwc * 16)) {
                    error_ = "VIF packet outside the file";
                    return 0;
                }
                Typed(pk, qwc * 16);
                in_.AddPayload(pk);
                std::vector<Vtx> streamed;
                // Compact packets are emitted by the shared rim/exhaust PPF
                // exporter.  Ordinary vehicle VIF data can contain the same
                // byte pattern by chance; treating it as compact data skips
                // its VU batches and leaves the car's body fragmented.
                if (page_model_ && PpfPacket(in_.At(pk), qwc * 16, streamed)) {
                    batches[g].push_back(std::move(streamed));
                    continue;
                }
                Vif vif;
                std::string err;
                const bool ok = vif.Run(in_.At(pk), qwc * 16, [&](const VuMem& m, const std::vector<Vif::Unpack>& u) {
                    Batch(m, u, batches[g], two_uv);
                }, err);
                if (!ok) {
                    char e[128];
                    std::snprintf(e, sizeof e, "VIF packet at +%X: %s", pk, err.c_str());
                    error_ = e;
                    return 0;
                }
            }
        }

        // vertex buffer: every batch's vertices in order; one strip per group joined by degenerate triangles
        const uint32_t stride = two_uv ? 36 : 28;
        std::vector<Vtx> verts;
        std::vector<std::vector<uint16_t>> strips(ngroups);
        for (uint32_t g = 0; g < ngroups; g++) {
            std::vector<uint16_t>& s = strips[g];
            for (const auto& bt : batches[g]) {
                const uint32_t first = uint32_t(verts.size());
                verts.insert(verts.end(), bt.begin(), bt.end());
                // runs without ADC breaks; a set ADC bit on vertex i skips the triangle closing at i
                uint32_t start = 0;
                auto flush = [&](uint32_t from, uint32_t to) {  // [from, to)
                    if (to - from < 3) return;
                    if (!s.empty()) {
                        s.push_back(s.back());
                        s.push_back(uint16_t(first + from));
                        // the run's first triangle (third vertex) must keep its parity within the batch
                        if ((s.size() & 1) != (from & 1)) s.push_back(uint16_t(first + from));
                    }
                    for (uint32_t i = from; i < to; i++) s.push_back(uint16_t(first + i));
                    for (uint32_t i = from + 2; i < to; i++) rep_.triangles++;
                };
                for (uint32_t i = 2; i < bt.size(); i++)
                    if (bt[i].adc) {
                        flush(start, i);
                        start = i - 1;
                    }
                flush(start, uint32_t(bt.size()));
            }
        }
        if (verts.size() > 65535) {
            error_ = "mesh with more than 65535 vertices";
            return 0;
        }
        rep_.vertices += uint32_t(verts.size());

        // The 0x20-byte header replaces the PS2 one in place (a .mesh resource's root is its mesh); appended:
        // material u16[n] (16-aligned) | u32 n | sets n x 0x18 | strip entries | VB | vertices | indices
        const uint32_t m = o;
        const uint32_t mat_size = (ngroups * 2 + 15) & ~15u;
        const uint32_t matx = out_.Alloc(mat_size + 4 + ngroups * 0x18 + ngroups * 8 + 0xC);
        out_.Fill(matx, mat_size, 0xCD);
        out_.Fill(matx + mat_size + 4 + ngroups * 0x18, ngroups * 8, 0xCD);
        const uint32_t sets = matx + mat_size + 4, entries = sets + ngroups * 0x18;
        const uint32_t vb = entries + ngroups * 8;
        out_.Fill(m, 0x20, 0);
        const uint32_t vdata = out_.Alloc(uint32_t(verts.size()) * stride, 16);
        uint32_t nidx = 0;
        for (auto& s : strips) nidx += uint32_t(s.size());
        const uint32_t idx = out_.Alloc(std::max<uint32_t>(nidx, 1) * 2, 4);

        out_.W8(m + 6, uint8_t(piece));
        out_.W8(m + 7, uint8_t(stride));
        out_.W16(m + 8, uint16_t(ngroups));
        out_.W32(m + 0xC, out_.Va(matx));
        out_.W32(m + 0x10, out_.Va(sets));
        out_.W32(m + 0x18, out_.Va(vb));
        out_.W32(m + 0x1C, out_.Va(idx));
        out_.W32(sets - 4, ngroups);
        uint32_t at = 0;
        for (uint32_t g = 0; g < ngroups; g++) {
            out_.W16(matx + g * 2, mat ? in_.U16(mat + g * 2) : 0);
            const uint32_t st = sets + g * 0x18, e = entries + g * 8;
            out_.W32(st, out_.Va(e));
            out_.W32(st + 4, 0);                     // vertex shader handle, created on load (0xE7790)
            out_.W16(st + 8, two_uv ? 0xD9 : 0x59);  // position, normal, diffuse, tex0 [, tex1]
            out_.W16(st + 0xA, uint16_t(stride));
            out_.W16(st + 0xC, 0x4A02);              // FLOAT3, NORMPACKED3, D3DCOLOR
            out_.W16(st + 0xE, two_uv ? 0x24 : 0x04);  // FLOAT2 [, FLOAT2]
            out_.W32(st + 0x10, 0);
            out_.W16(st + 0x14, 1);
            out_.W16(st + 0x16, 0xCDCD);
            out_.W16(e, uint16_t(at));
            out_.W16(e + 2, uint16_t(strips[g].size()));
            out_.W32(e + 4, 0);
            for (uint16_t i : strips[g]) out_.W16(idx + 2 * at++, i);
        }
        out_.W32(vb, 1);  // D3DVertexBuffer: refcount 1
        out_.W32(vb + 4, out_.Va(vdata) - 0x80000000u);
        out_.W32(vb + 8, 0);
        for (size_t i = 0; i < verts.size(); i++) {
            const Vtx& v = verts[i];
            const uint32_t p = vdata + uint32_t(i) * stride;
            out_.WF(p, v.pos[0]), out_.WF(p + 4, v.pos[1]), out_.WF(p + 8, v.pos[2]);
            out_.W32(p + 12, PackNormal(v.nrm));
            out_.W32(p + 16, v.color);
            out_.WF(p + 20, v.uv[0]), out_.WF(p + 24, v.uv[1]);
            if (two_uv) out_.WF(p + 28, v.uv1[0]), out_.WF(p + 32, v.uv1[1]);
        }
        meshes_[o] = m;
        return m;
    }

    // Wheel/exhaust page files use the compact VIF layout emitted by the
    // streamed-model exporter.  It has no VU header UNPACK (the normal vehicle
    // route below needs one), but its typed streams are stable across the
    // retail PPFs: position V4-16, optional UV V2-32, normal V3-8 and optional
    // diffuse V4-8.  Decode it before falling back to the full VIF interpreter.
    static int PpfTag(const uint8_t* p, uint32_t n, uint8_t a, uint8_t b, uint8_t command, uint8_t count) {
        for (uint32_t o = 0; o + 4 <= n; o += 4)
            if (p[o] == a && p[o + 1] == b && p[o + 2] == count && p[o + 3] == command) return int(o);
        return -1;
    }

    static bool PpfPacket(const uint8_t* p, uint32_t n, std::vector<Vtx>& out) {
        // Both exporter variants use one of the two tags below for the packed
        // signed-short position stream.  A positive count keeps coincidental
        // byte patterns in vertex data out of this decoder.
        int pos = -1;
        uint8_t count = 0;
        for (uint32_t o = 0; o + 4 <= n; o += 4) {
            if ((p[o] == 0xEE && p[o + 1] == 0x00) || (p[o] == 0x1B && p[o + 1] == 0x02)) {
                if (p[o + 2] && p[o + 3] == 0x69) {
                    pos = int(o);
                    count = p[o + 2];
                    break;
                }
            }
        }
        if (pos < 0 || uint32_t(pos) + 4 + uint32_t(count) * 6 > n) return false;
        const int uv = PpfTag(p, n, 0xC4, 0x00, 0x65, count) >= 0 ? PpfTag(p, n, 0xC4, 0x00, 0x65, count)
                                                                     : PpfTag(p, n, 0xF1, 0x01, 0x65, count);
        const int normal = PpfTag(p, n, 0x9A, 0x00, 0x6A, count) >= 0
                               ? PpfTag(p, n, 0x9A, 0x00, 0x6A, count)
                               : PpfTag(p, n, 0xC7, 0x01, 0x6A, count);
        const int diffuse = PpfTag(p, n, 0x18, 0x41, 0x6E, count) >= 0
                                ? PpfTag(p, n, 0x18, 0x41, 0x6E, count)
                                : PpfTag(p, n, 0x45, 0x42, 0x6E, count);
        if ((uv >= 0 && uint32_t(uv) + 4 + uint32_t(count) * 4 > n) ||
            (normal >= 0 && uint32_t(normal) + 4 + uint32_t(count) * 3 > n) ||
            (diffuse >= 0 && uint32_t(diffuse) + 4 + uint32_t(count) * 4 > n))
            return false;
        float scale = 1.f / 4096.f;
        if (n >= 8) {
            std::memcpy(&scale, p + 4, sizeof scale);
            if (!(scale > 0.f && scale < 100.f)) scale = 1.f / 4096.f;
        }
        out.reserve(out.size() + count);
        for (uint32_t i = 0; i < count; i++) {
            Vtx v{};
            const uint8_t* q = p + pos + 4 + i * 6;
            v.pos[0] = float(int16_t(Rd16(q))) * scale;
            v.pos[1] = float(int16_t(Rd16(q + 2))) * scale;
            v.pos[2] = float(int16_t(Rd16(q + 4))) * scale;
            if (uv >= 0) {
                const uint8_t* q = p + uv + 4 + i * 4;
                v.uv[0] = float(Rd16(q)) / 4096.f;
                v.uv[1] = float(Rd16(q + 2)) / 4096.f;
            }
            if (normal >= 0) {
                const uint8_t* q = p + normal + 4 + i * 3;
                v.nrm[0] = float(int8_t(q[0])) / 127.f;
                v.nrm[1] = float(int8_t(q[1])) / 127.f;
                v.nrm[2] = float(int8_t(q[2])) / 127.f;
                v.adc = (q[0] & 1) != 0;
            }
            if (diffuse >= 0) {
                const uint8_t* q = p + diffuse + 4 + i * 4;
                v.color = (uint32_t(q[3]) << 24) | (uint32_t(q[0]) << 16) | (uint32_t(q[1]) << 8) | q[2];
            } else {
                v.color = 0x80808000u;
            }
            out.push_back(v);
        }
        return true;
    }

    // one MSCAL: header at H, normals H+2, uv H+0x2C, positions H+0x56, diffuse H+0x80
    void Batch(const VuMem& m, const std::vector<Vif::Unpack>& u, std::vector<std::vector<Vtx>>& out, bool& two_uv) {
        int h = -1;
        bool uv16x4 = false;
        for (const auto& x : u) {
            if (x.cmd == 0x6C && h < 0) h = x.addr;
            if (x.cmd == 0x6D) uv16x4 = true;
        }
        if (h < 0) {
            error_ = "VU batch without a header";
            return;
        }
        rep_.batches++;
        two_uv |= uv16x4;
        const uint32_t n = m.q[h + 1][0];
        float scale;
        std::memcpy(&scale, &m.q[h][0], 4);
        std::vector<Vtx> st;
        for (uint32_t i = 0; i < n && i < 256; i++) {
            Vtx v{};
            const uint32_t* p = m.q[(h + 0x56 + i) & 0x3FF];
            const uint32_t* t = m.q[(h + 0x2C + i) & 0x3FF];
            const uint32_t* c = m.q[(h + 0x80 + i) & 0x3FF];
            const uint32_t* q = m.q[(h + 2 + i) & 0x3FF];
            for (int k = 0; k < 3; k++) v.pos[k] = float(int32_t(p[k])) * scale;
            if (uv16x4) {
                v.uv[0] = float(int16_t(t[0])) / 4096.f, v.uv[1] = float(int16_t(t[1])) / 4096.f;
                v.uv1[0] = float(int16_t(t[2])) / 4096.f, v.uv1[1] = float(int16_t(t[3])) / 4096.f;
            } else {
                v.uv[0] = float(int32_t(t[0])) / 4096.f, v.uv[1] = float(int32_t(t[1])) / 4096.f;
            }
            v.color = ((c[3] & 0xFF) << 24) | ((c[0] & 0xFF) << 16) | ((c[1] & 0xFF) << 8) | (c[2] & 0xFF);
            for (int k = 0; k < 3; k++) v.nrm[k] = float(int32_t(q[k])) / 127.f;
            v.adc = q[0] & 1;
            st.push_back(v);
        }
        out.push_back(std::move(st));
    }

    In in_;
    Out out_;
    const Effects& fx_;
    Report& rep_;
    bool page_model_ = false;
    std::unordered_set<uint32_t> null_textures_;

public:
    const SkeletonTemplate* skeleton_ = nullptr;  // for a page-file model whose own skeleton is invalid
    std::string error_;
    std::set<uint32_t> seen_, graph_seen_;
    std::vector<std::pair<uint32_t, bool>> graph_roots_;
    std::map<uint32_t, uint32_t> meshes_;
};

}  // namespace

double Report::ConsumedPercent() const {
    return file_bytes ? 100.0 * double(header + typed + graph + padding) / double(file_bytes) : 0.0;
}

std::string Report::Summary() const {
    char b[512];
    std::snprintf(b, sizeof b,
                  "%u bytes: %.2f%% consumed (header %u, typed %u, by pointer %u, padding %u), %u dead, %u unclaimed; "
                  "%u meshes, %u VU batches, %u triangles, %u vertices, %u shaders, %u textures (%u texels); "
                  "Xbox body %u bytes",
                  file_bytes, ConsumedPercent(), header, typed, graph, padding, dead, unknown, meshes, batches, triangles,
                  vertices, shaders, textures, texture_pixels, out_bytes);
    return b;
}

bool Convert(const std::vector<uint8_t>& file, const Effects& effects, std::vector<uint8_t>& body, uint32_t& base_va,
             uint32_t& type, Report& report, std::string& error) {
    report = Report{};
    Converter c(file, effects, report);
    return c.Run(body, base_va, type, error);
}

namespace {
// a page-file model image as Convert() takes it: the 0x80-byte header, then the model
std::vector<uint8_t> ShaderModelFile(const std::vector<uint8_t>& source, uint32_t source_base) {
    std::vector<uint8_t> file(0x80 + source.size());
    std::memcpy(file.data() + 0x80, source.data(), source.size());
    std::memcpy(file.data(), &source_base, sizeof source_base);
    const uint32_t version = 1, size = uint32_t(source.size());
    std::memcpy(file.data() + 4, &kTypeShaderModelImage, sizeof kTypeShaderModelImage);
    std::memcpy(file.data() + 8, &version, sizeof version);
    std::memcpy(file.data() + 12, &size, sizeof size);
    return file;
}
}  // namespace

bool FindSkeleton(const std::vector<uint8_t>& source, uint32_t source_base, SkeletonTemplate& skeleton) {
    const std::vector<uint8_t> file = ShaderModelFile(source, source_base);
    const In in(file);
    if (!in.Valid()) return false;
    const uint32_t s = in.Ptr(0x80 + 0x68);
    if (!ValidSkeleton(in, s)) return false;
    const uint32_t n = in.U16(s), nodes = in.Ptr(s + 8);
    uint32_t lo = nodes, hi = nodes + n * 0x44;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t node = nodes + i * 0x44;
        if (const uint32_t name = in.Ptr(node + 0xC)) {
            uint32_t end = name;
            while (end < in.End() && in.U8(end)) end++;
            lo = std::min(lo, name), hi = std::max(hi, end + 1);
        }
        // links to other nodes and joint limits: only a skeleton whose links stay among its nodes is copied
        for (uint32_t k : {0x14u, 0x18u, 0x1Cu, 0x40u})
            if (const uint32_t p = in.U32(node + k); p && (in.Off(p) < nodes || in.Off(p) >= nodes + n * 0x44))
                return false;
    }
    lo &= ~3u;
    skeleton.header.assign(in.At(s), in.At(s) + 0xC);
    skeleton.bytes.assign(in.At(lo), in.At(lo) + (hi - lo));
    skeleton.va = in.Va(lo);
    return true;
}

bool ConvertShaderModel(const std::vector<uint8_t>& source, uint32_t source_base, const Effects& effects,
                        std::vector<uint8_t>& body, Report& report, std::string& error,
                        const SkeletonTemplate* skeleton) {
    const std::vector<uint8_t> file = ShaderModelFile(source, source_base);
    uint32_t converted_base = 0, type = 0;
    Converter c(file, effects, report);
    c.skeleton_ = skeleton;
    if (!c.Run(body, converted_base, type, error)) return false;
    if (converted_base != source_base || type != kTypeShaderModelImage) {
        error = "shader-model conversion returned an inconsistent image";
        return false;
    }
    return true;
}

}  // namespace ps2veh

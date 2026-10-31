// Opt-in: --mc3-car-packs=<folder> adds cars to the game from folders of loose files (MC3-0022).
//
// A pack is a folder laid out like the game's ASSETS tree (an ASSETS folder inside it is entered too), for example a
// PS2 mod pack: tune\vehicle\vehicle.lst, tune\vehicle\customdata\<car>*.carcfg, tune\garage\<car>.mcgarage,
// tune\camera\<car>_*.camtrackcs, resources\vehicle\<car>\*.pck. <folder> is one pack, or holds one per subfolder.
//
// Every file the game opens goes through Stream::Open (0x1A2280, hooked by Mc2LaPaths, which calls in here first):
// - a pack file the game does not have (a new car's own files) is served from the pack;
// - a list the game has (vehicle.lst, unlockingrules.csv) is merged: the game's own copy, read through its asset
//   manager, plus the lines each pack inserts into it, so several packs and the retail entries all stay;
// - any other pack file that the game has replaces it;
// - resources\vehicle goes to the PS2 car loader (ps2_car_loader.cpp), which converts the PS2 files at load time.
// The game reads vehicle.lst into lists that grow as needed (AllocateCarData, 0x26ABC0), so a new Vehicle block is a
// new car in the menus. Served and merged files are written under the cache partition (hdd\Z\carpacks) and opened
// through the console's file system, the way mc2_city.cpp serves its loose files.
//
// A pack can extend the shared rim/exhaust PPFs too: the retail Xbox pages stay byte-for-byte first, while PS2-only
// tail pages are converted when the game bootstraps the page file. Shop movies can be supplied as Xbox .xbck or loose
// PS2 .pck images; the Flash constructors relocate either one from its relative base.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "game/ps2_vehicle.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dispatch {
X86Func* Find(uint32_t guest);
}

X86_EXTERN_FUNC(sub_001A23A0);  // Stream::Read(buffer, bytes)
X86_EXTERN_FUNC(sub_001A2630);  // Stream::Close
X86_EXTERN_FUNC(sub_001A2670);  // Stream::Size
X86_EXTERN_FUNC(sub_000EA710);  // txtStringTable hash of a key
X86_EXTERN_FUNC(sub_000EC580);  // hash table lookup (ecx = table, hash) -> entry or 0
X86_EXTERN_FUNC(sub_001B3B90);  // operator new
X86_EXTERN_FUNC(__imp__sub_000EA750);
X86_EXTERN_FUNC(sub_00210650);  // shared vehicle page-file bootstrap (jumps to 0x212100)
X86_EXTERN_FUNC(__imp__sub_00210650);
X86_EXTERN_FUNC(__imp__sub_002112C0);  // page block release
X86_EXTERN_FUNC(__imp__sub_00210690);  // page manager: add fixed blocks (bytes, count, id)

bool Mc3GarageScriptActive();  // garage_script.cpp, --mc3-garage-script
bool Mc3GarageScriptWants(const std::string& rel);
void Mc3GarageScriptSee(const std::string& rel, const std::string& file);
const ps2veh::Effects& Mc3Ps2Effects(uint8_t* base);  // ps2_car_loader.cpp
const std::u16string* Mc3GarageString(const std::string& key);

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kAssetManager = 0x5348B0;  // its vtbl+0x24 opens (name, extension, 0, 1) -> Stream*
constexpr char kCache[] = "\\Device\\Harddisk0\\Partition5\\carpacks\\";
constexpr char kFallbackCar[] = "vp_jetta_03";  // front-end images for a new car

uint32_t U32(const std::vector<uint8_t>& data, size_t at) {
    uint32_t value = 0;
    if (at + sizeof value <= data.size()) std::memcpy(&value, data.data() + at, sizeof value);
    return value;
}

void Put32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    if (at + sizeof value > data.size()) throw std::runtime_error("PPF write outside page");
    std::memcpy(data.data() + at, &value, sizeof value);
}

uint32_t Align(uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

bool PpfHeader(const std::vector<uint8_t>& file, uint32_t& count) {
    count = U32(file, 4);
    return file.size() >= 0x10000 && !std::memcmp(file.data(), "pf05", 4) && count && 12 + size_t(count) * 4 <= 0x10000;
}

std::vector<uint8_t> PpfPage(const std::vector<uint8_t>& file, uint32_t page) {
    const uint32_t descriptor = U32(file, 12 + size_t(page) * 4);
    const size_t begin = size_t(descriptor & 0x7ffff) * 0x800;
    const size_t bytes = size_t(descriptor >> 19) * 0x400;
    if (!bytes || begin + bytes > file.size()) throw std::runtime_error("invalid PPF page descriptor");
    return {file.begin() + begin, file.begin() + begin + bytes};
}

// A PS2 shared vehicle PPF has the same page-directory format as the Xbox file, but model pages contain PS2 VIF
// streams.  Retain the Xbox pages that the retail lists address and replace only the PS2-only tail with converted
// pages.  Virtual addresses in a page remain unchanged, so the game's page manager can keep its normal relocator.
struct PpfTextureTemplates {
    std::map<std::string, std::vector<uint8_t>> exact;
    std::map<uint32_t, std::vector<uint8_t>> by_ps2_bytes;
};

PpfTextureTemplates TextureTemplates(const std::vector<uint8_t>& xbox, const std::vector<uint8_t>& ps2,
                                     uint32_t retail_pages) {
    PpfTextureTemplates result;
    for (uint32_t page = 0; page < retail_pages; page++) {
        const std::vector<uint8_t> ps_page = PpfPage(ps2, page), xb_page = PpfPage(xbox, page);
        size_t ps_at = 0, xb_at = 0;
        while (ps_at + 32 <= ps_page.size() && xb_at + 32 <= xb_page.size()) {
            const uint32_t ps_bytes = U32(ps_page, ps_at + 12), xb_bytes = U32(xb_page, xb_at + 12);
            if (!ps_bytes || !xb_bytes) break;
            if (ps_bytes < 32 || xb_bytes < 32 || ps_at + ps_bytes > ps_page.size() ||
                xb_at + xb_bytes > xb_page.size())
                break;
            const uint32_t ps_type = U32(ps_page, ps_at + 20), xb_type = U32(xb_page, xb_at + 20);
            if (ps_type == 0x0Eu && (xb_type == 0x1Cu || xb_type == 0x11u) && xb_bytes <= ps_bytes) {
                std::string key(reinterpret_cast<const char*>(ps_page.data() + ps_at), ps_bytes);
                std::vector<uint8_t> replacement(xb_page.begin() + xb_at, xb_page.begin() + xb_at + xb_bytes);
                result.exact.emplace(std::move(key), replacement);
                result.by_ps2_bytes.emplace(ps_bytes, std::move(replacement));
            }
            ps_at += ps_bytes;
            xb_at += xb_bytes;
        }
    }
    return result;
}

// The first valid model skeleton of a PS2 page file, for its models whose own skeleton is invalid.
bool PpfSkeleton(const std::vector<uint8_t>& file, uint32_t count, ps2veh::SkeletonTemplate& skeleton) {
    for (uint32_t page = 0; page < count; page++) {
        const std::vector<uint8_t> source = PpfPage(file, page);
        for (size_t at = 0; at + 32 <= source.size();) {
            const uint32_t bytes = U32(source, at + 12);
            if (bytes < 32 || at + bytes > source.size()) break;
            if (U32(source, at + 20) == 0xffff0000u &&
                ps2veh::FindSkeleton({source.begin() + at + 32, source.begin() + at + bytes}, U32(source, at), skeleton))
                return true;
            at += bytes;
        }
    }
    return false;
}

std::vector<uint8_t> ConvertPpfPage(const std::vector<uint8_t>& source, const ps2veh::Effects& effects,
                                    const PpfTextureTemplates& textures, const ps2veh::SkeletonTemplate* skeleton,
                                    uint32_t& converted, uint32_t& skeletons, uint32_t& exact_textures,
                                    uint32_t& fallback_textures) {
    std::vector<uint8_t> output;
    bool parsed = false;
    for (size_t at = 0; at + 32 <= source.size();) {
        const uint32_t bytes = U32(source, at + 12), type = U32(source, at + 20);
        if (!bytes) break;  // directory pages are padded to 0x400
        if (bytes < 32 || at + bytes > source.size()) throw std::runtime_error("invalid PPF chunk");
        parsed = true;
        if (type == 0x0Eu) {
            // Page references encode both the page and the byte offset, so a
            // replacement must retain the PS2 chunk's extent. Prefer a byte-
            // identical texture seen in the retail PS2 prefix and use its
            // paired Xbox chunk. A new mod texture has no Xbox-compressed
            // counterpart; give it a valid retail texture of the same PS2
            // extent rather than feeding GS data to the D3D constructor.
            const std::string key(reinterpret_cast<const char*>(source.data() + at), bytes);
            auto exact = textures.exact.find(key);
            auto generic = textures.by_ps2_bytes.find(bytes);
            if (exact == textures.exact.end() && generic == textures.by_ps2_bytes.end())
                throw std::runtime_error("no compatible Xbox texture template for PS2 PPF chunk");
            std::vector<uint8_t> replacement =
                exact != textures.exact.end() ? exact->second : generic->second;
            if (replacement.size() > bytes) throw std::runtime_error("Xbox texture template exceeds PS2 chunk");
            replacement.resize(bytes, 0);
            Put32(replacement, 12, bytes);
            output.insert(output.end(), replacement.begin(), replacement.end());
            exact != textures.exact.end() ? exact_textures++ : fallback_textures++;
        } else if (type != 0xffff0000u) {
            output.insert(output.end(), source.begin() + at, source.begin() + at + bytes);
        } else {
            std::vector<uint8_t> model(source.begin() + at + 32, source.begin() + at + bytes), xbox;
            ps2veh::Report report;
            std::string error;
            if (!ps2veh::ConvertShaderModel(model, U32(source, at), effects, xbox, report, error, skeleton))
                throw std::runtime_error("PPF model conversion failed: " + error);
            for (const std::string& note : report.notes)
                if (note.find("invalid skeleton") != std::string::npos) skeletons++;
            xbox.resize(Align(uint32_t(xbox.size()), 16), 0xCD);
            const size_t here = output.size();
            output.insert(output.end(), source.begin() + at, source.begin() + at + 32);
            output.insert(output.end(), xbox.begin(), xbox.end());
            Put32(output, here + 12, uint32_t(32 + xbox.size()));
            converted++;
        }
        at += bytes;
    }
    if (!parsed) return source;
    // The page manager uses the zero-filled tail after the final chunk while
    // releasing a page.  Retail Xbox and PS2 PPFs both leave this padding at
    // zero; 0xCD here is observed as a cleanup callback on page-out.  The page manager reads a page in 0x800-byte
    // units, rounding the directory's size down (0x211BD0: size >> 19 & 0x1FFE), so a page padded only to 0x400
    // lost its last 0x400 bytes.
    output.resize(std::max(0x800u, Align(uint32_t(output.size()), 0x800)), 0);
    return output;
}

// decal.xbck is the shared vehicle catalog (resource type 0x25). Its rim and exhaust lists contain 16-byte page
// handles, not model data. Keep the Xbox resource graph intact and append only handles that the PS2 catalog adds.
// The last word of each handle encodes the PPF page number; the page-file merger supplies those pages separately.
std::vector<uint8_t> ExtendSharedCatalog(const std::vector<uint8_t>& retail, const std::vector<uint8_t>& ps2) {
    if (retail.size() < 0x100 || ps2.size() < 0x100 || U32(retail, 4) != 0x25 || U32(ps2, 4) != 0x25 ||
        U32(retail, 8) != 1 || U32(ps2, 8) != 1 || U32(retail, 12) + 0x80 != retail.size() ||
        U32(ps2, 12) + 0x80 != ps2.size())
        throw std::runtime_error("incompatible shared catalog headers");
    const uint32_t retail_base = U32(retail, 0) - 0x80, ps2_base = U32(ps2, 0) - 0x80;
    auto offset = [](const std::vector<uint8_t>& file, uint32_t base, uint32_t va, size_t bytes) -> size_t {
        if (va < base || size_t(va - base) + bytes > file.size())
            throw std::runtime_error("shared catalog pointer outside file");
        return va - base;
    };
    std::vector<uint8_t> out = retail;
    for (const uint32_t field : {0x28u, 0x38u}) {  // rims, exhausts
        const size_t xb_table = offset(retail, retail_base, U32(retail, 0x80 + field), 8);
        const size_t ps_table = offset(ps2, ps2_base, U32(ps2, 0x80 + field), 8);
        const uint32_t old_count = U32(retail, xb_table + 4), new_count = U32(ps2, ps_table + 4);
        if (!old_count || old_count > new_count || new_count > 256)
            throw std::runtime_error("incompatible rim/exhaust catalog lengths");
        const size_t xb_entries = offset(retail, retail_base, U32(retail, xb_table), size_t(old_count) * 4);
        const size_t ps_entries = offset(ps2, ps2_base, U32(ps2, ps_table), size_t(new_count) * 4);
        std::vector<uint32_t> handles;
        handles.reserve(new_count);
        for (uint32_t i = 0; i < old_count; i++) handles.push_back(U32(retail, xb_entries + size_t(i) * 4));
        for (uint32_t i = old_count; i < new_count; i++) {
            const size_t source = offset(ps2, ps2_base, U32(ps2, ps_entries + size_t(i) * 4), 16);
            if (U32(ps2, source) != 0x007AA718u || U32(ps2, source + 4) != 0)
                throw std::runtime_error("unsupported PS2 page handle");
            out.resize(Align(uint32_t(out.size()), 16), 0xCD);
            const size_t target = out.size();
            out.insert(out.end(), ps2.begin() + source, ps2.begin() + source + 16);
            Put32(out, target, 0x00608F80u);  // Xbox shared page-handle vtable
            handles.push_back(retail_base + uint32_t(target));
        }
        out.resize(Align(uint32_t(out.size()), 16), 0xCD);
        const size_t new_entries = out.size();
        out.resize(new_entries + size_t(new_count) * 4, 0);
        for (uint32_t i = 0; i < new_count; i++) Put32(out, new_entries + size_t(i) * 4, handles[i]);
        Put32(out, xb_table, retail_base + uint32_t(new_entries));
        Put32(out, xb_table + 4, new_count);
    }
    Put32(out, 12, uint32_t(out.size() - 0x80));
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = c == '/' ? '\\' : char(std::tolower(uint8_t(c)));
    return s;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

// how a line is compared: whitespace (the lists mix tabs and spaces) and case do not count
std::string Key(const std::string& line) {
    std::string k;
    for (char c : line)
        if (!std::isspace(uint8_t(c))) k.push_back(char(std::tolower(uint8_t(c))));
    return k;
}

// The lines a pack inserts into the game's file, placed after the base line they follow: a longest common
// subsequence of the two. Lines the pack changed or dropped are left as the game has them.
std::vector<std::string> MergeInsertions(const std::vector<std::string>& base, const std::vector<std::string>& pack,
                                         uint32_t& inserted) {
    const size_t n = base.size(), m = pack.size();
    std::vector<std::string> kb(n), kp(m);
    for (size_t i = 0; i < n; i++) kb[i] = Key(base[i]);
    for (size_t j = 0; j < m; j++) kp[j] = Key(pack[j]);
    std::vector<std::vector<uint16_t>> dp(n + 1, std::vector<uint16_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            dp[i][j] = kb[i] == kp[j] ? uint16_t(dp[i + 1][j + 1] + 1) : std::max(dp[i + 1][j], dp[i][j + 1]);
    std::vector<std::string> out;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && kb[i] == kp[j]) {
            out.push_back(base[i++]);
            j++;
        } else if (j < m && (i == n || dp[i][j + 1] >= dp[i + 1][j])) {
            if (!kp[j].empty()) inserted++;
            out.push_back(pack[j++]);
        } else {
            out.push_back(base[i++]);
        }
    }
    return out;
}

// MC3's string key hash (hash v1 of Edness' strtbl.py: signed characters)
uint32_t KeyHash(const std::string& key) {
    uint32_t h = 0;
    for (char c : key) {
        h += uint32_t(int32_t(int8_t(c)));
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    return h + (h << 15);
}

// The first language of a .strtbl (table v2): hash -> text.
void ReadStringTable(const std::string& data, std::map<uint32_t, std::u16string>& out) {
    auto u32 = [&](size_t o) -> uint32_t {
        uint32_t v = 0;
        if (o + 4 <= data.size()) std::memcpy(&v, &data[o], 4);
        return v;
    };
    const uint32_t langs = u32(0);
    if (!langs || langs > 64 || 4 + langs * 4 > data.size()) return;
    size_t o = u32(4);
    const uint32_t entries = u32(o);
    o += 4;
    for (uint32_t i = 0; i < entries && o < data.size(); i++) {
        const uint32_t hash = u32(o);
        o += 4 + 2;               // hash, size
        o += 4 + u32(o);          // font name, no terminator
        const uint32_t len = u32(o);  // characters, the terminator included
        o += 4;
        if (!len || o + size_t(len) * 2 > data.size()) return;
        std::u16string text(reinterpret_cast<const char16_t*>(&data[o]), len - 1);
        o += size_t(len) * 2 + 8 + 2;  // text, two scales, two bytes
        out.emplace(hash, std::move(text));
    }
}

struct Pack {
    fs::path root;      // the folder holding tune, resources...
    std::map<std::string, fs::path> files;  // lowercase relative path -> host file
};

// the game's lists that packs extend rather than replace
const std::set<std::string> kMerged = {
    "tune\\vehicle\\vehicle.lst",
    "tune\\progress\\unlockingrules.csv",
    // Catalog indices select names; the PPF page is located separately.
    "tune\\vehicle\\customdata\\customwheels.txt",
    "tune\\vehicle\\customdata\\customexhausts.txt",
};

class Packs {
public:
    void Init() {
        const wchar_t* arg = Mc3DebugArg(L"--mc3-car-packs");
        if (!arg || !*arg) return;
        std::error_code ec;
        const fs::path top(arg);
        auto add = [&](fs::path root) {
            if (fs::is_directory(root / "ASSETS", ec)) root /= "ASSETS";
            if (!fs::is_directory(root / "tune", ec) && !fs::is_directory(root / "resources", ec)) return;
            Pack p;
            p.root = root;
            for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (ec || !it->is_regular_file(ec)) continue;
                std::string rel = Lower(fs::relative(it->path(), root, ec).string());
                // rim/exhaust are shared page files rather than ordinary car resources.  Keep their source PS2 PPF
                // around; PageFile() combines it with the corresponding retail Xbox page file when the game opens it.
                if (rel == "resources\\vehicle\\rim.ppf" || rel == "resources\\vehicle\\exhaust.ppf") {
                    shared_ppf_[rel] = it->path();
                    continue;
                }
                if (rel == "resources\\vehicle\\decal.pck" || rel == "resources\\vehicle\\decal_g.pck") {
                    shared_catalog_[rel] = it->path();
                    continue;
                }
                if (rel.rfind("resources\\vehicle\\", 0) == 0) continue;  // the PS2 car loader's
                p.files.emplace(rel, it->path());
            }
            LoadStrings(p);
            LOGE("MC3PACK pack %ls: %zu files", root.c_str(), p.files.size());
            packs_.push_back(std::move(p));
        };
        add(top);
        if (packs_.empty())
            for (auto& e : fs::directory_iterator(top, ec))
                if (e.is_directory(ec)) add(e.path());
        if (packs_.empty()) LOGE("MC3PACK no pack in %ls", top.c_str());
    }

    bool Empty() const { return packs_.empty(); }

    // a pack's text for a key the game's string table lacks
    const std::u16string* Text(uint32_t hash) const {
        auto it = strings_.find(hash);
        return it == strings_.end() ? nullptr : &it->second;
    }

    // strings.txt ("key = text" per line, UTF-8) and the first language of each fonts\*.strtbl; the first pack and
    // file to name a key keeps it
    void LoadStrings(const Pack& p) {
        std::error_code ec;
        const size_t before = strings_.size();
        const fs::path txt = p.root / "strings.txt";
        if (fs::is_regular_file(txt, ec)) {
            for (const std::string& line : Lines(Read(txt))) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos || line[0] == ';' || line[0] == '#') continue;
                auto trim = [](std::string v) {
                    const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
                    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
                };
                const std::string key = trim(line.substr(0, eq)), utf8 = trim(line.substr(eq + 1));
                std::u16string text;
                for (size_t i = 0; i < utf8.size();) {  // UTF-8 to UTF-16, BMP only
                    uint32_t c = uint8_t(utf8[i]);
                    const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                    c &= n == 1 ? 0x7F : n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07;
                    for (int k = 1; k < n && i + k < utf8.size(); k++) c = (c << 6) | (uint8_t(utf8[i + k]) & 0x3F);
                    i += n;
                    text.push_back(char16_t(c <= 0xFFFF ? c : u'?'));
                }
                strings_.emplace(KeyHash(key), std::move(text));
            }
        }
        std::vector<fs::path> tables;
        for (const auto& [rel, path] : p.files)
            if (rel.rfind("fonts\\", 0) == 0 && rel.size() > 7 && rel.compare(rel.size() - 7, 7, ".strtbl") == 0)
                tables.push_back(path);
        std::sort(tables.begin(), tables.end());  // mcstrings01 before the _pal ones
        for (const fs::path& t : tables) ReadStringTable(Read(t), strings_);
        LOGE("MC3PACK pack %ls: %zu strings", p.root.c_str(), strings_.size() - before);
    }

    std::vector<fs::path> Roots() const {
        std::vector<fs::path> out;
        for (const Pack& p : packs_) out.push_back(p.root);
        return out;
    }

    std::vector<fs::path> VehicleFolders() const {
        std::vector<fs::path> out;
        std::error_code ec;
        for (const Pack& p : packs_)
            if (fs::is_directory(p.root / "resources" / "vehicle", ec)) out.push_back(p.root / "resources" / "vehicle");
        return out;
    }

    // Return the redirected page-file *stem* for a shared PS2 resource.  The asset manager appends xbpf itself and
    // stores this stem for later page-ins, hence both the bootstrap read and every later page read use the cache copy.
    std::string PageFile(uint8_t* base, const std::string& rel) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string source_rel = rel + ".ppf";
        auto source = shared_ppf_.find(source_rel);
        if (source == shared_ppf_.end()) return {};
        if (auto done = pagefiles_.find(rel); done != pagefiles_.end()) return done->second;
        try {
            const std::string retail_host = kernel::ResolvePath("D:\\" + rel + ".xbpf", 0);
            if (retail_host.empty()) throw std::runtime_error("retail xbpf is not mounted");
            const std::vector<uint8_t> retail = Bytes(retail_host), ps2 = Bytes(source->second);
            uint32_t retail_count = 0, ps2_count = 0;
            if (!PpfHeader(retail, retail_count) || !PpfHeader(ps2, ps2_count) || retail_count > ps2_count)
                throw std::runtime_error("incompatible pf05 directories");
            std::vector<std::vector<uint8_t>> pages;
            pages.reserve(ps2_count);
            uint32_t converted = 0, skeletons = 0, exact_textures = 0, fallback_textures = 0;
            const ps2veh::Effects& effects = Mc3Ps2Effects(base);
            const PpfTextureTemplates textures = TextureTemplates(retail, ps2, retail_count);
            ps2veh::SkeletonTemplate skeleton;
            const bool has_skeleton = PpfSkeleton(ps2, ps2_count, skeleton);
            for (uint32_t i = 0; i < ps2_count; i++)
                pages.push_back(i < retail_count
                                    ? PpfPage(retail, i)
                                    : ConvertPpfPage(PpfPage(ps2, i), effects, textures,
                                                     has_skeleton ? &skeleton : nullptr, converted, skeletons,
                                                     exact_textures, fallback_textures));

            std::vector<uint8_t> combined(0x10000, 0);
            std::memcpy(combined.data(), "pf05", 4);
            Put32(combined, 4, ps2_count);
            // +8 is the largest page; converted PS2 models outgrow both source files' values.
            uint32_t largest = std::max(U32(retail, 8), U32(ps2, 8));
            for (const std::vector<uint8_t>& page : pages) largest = std::max(largest, Align(uint32_t(page.size()), 0x800));
            Put32(combined, 8, largest);
            uint32_t cursor = 0x10000;
            for (uint32_t i = 0; i < ps2_count; i++) {
                cursor = Align(cursor, 0x800);
                const uint32_t bytes = uint32_t(pages[i].size());
                if ((bytes & 0x3ff) || cursor / 0x800 > 0x7ffff || bytes / 0x400 > 0x1fff)
                    throw std::runtime_error("combined page exceeds pf05 descriptor range");
                Put32(combined, 12 + size_t(i) * 4, (bytes / 0x400 << 19) | cursor / 0x800);
                combined.resize(size_t(cursor) + bytes, 0xCD);
                std::memcpy(combined.data() + cursor, pages[i].data(), bytes);
                cursor += bytes;
            }
            const std::string xbox = WriteBytes(rel + ".xbpf", combined);
            if (xbox.empty()) throw std::runtime_error("cache path is unavailable");
            pagefiles_[rel] = xbox.substr(0, xbox.size() - 5);  // PageFile consumers name the resource without .xbpf
            largest_[rel] = largest;
            LOGE("MC3PACK shared %s: %u retail + %u PS2 pages, %u models converted (%u invalid skeletons replaced), "
                 "%u exact + %u fallback Xbox texture chunks, largest page %X -> %s",
                 source_rel.c_str(), retail_count, ps2_count - retail_count, converted, skeletons, exact_textures,
                 fallback_textures, largest, pagefiles_[rel].c_str());
            return pagefiles_[rel];
        } catch (const std::exception& error) {
            LOGE("MC3PACK shared %s: not converted (%s); retail page file is used", source_rel.c_str(), error.what());
            pagefiles_[rel] = {};
            return {};
        }
    }

    // The largest page of a shared page file a pack converts (generating it now), 0 when the retail file is used.
    uint32_t LargestPage(uint8_t* base, const std::string& rel) {
        if (PageFile(base, rel).empty()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = largest_.find(rel);
        return it == largest_.end() ? 0 : it->second;
    }

    std::string SharedCatalog(const std::string& rel) {
        std::string source_rel = rel;
        source_rel.replace(source_rel.size() - 5, 5, ".pck");
        auto source = shared_catalog_.find(source_rel);
        if (source == shared_catalog_.end()) return {};
        try {
            const std::string host = kernel::ResolvePath("D:\\" + rel, 0);
            if (host.empty()) throw std::runtime_error("retail shared catalog is not mounted");
            const std::vector<uint8_t> extended = ExtendSharedCatalog(Bytes(host), Bytes(source->second));
            const std::string xbox = WriteBytes(rel, extended);
            LOGE("MC3PACK shared catalog %s: extended Xbox rim/exhaust page handles -> %s", rel.c_str(),
                 xbox.c_str());
            return xbox;
        } catch (const std::exception& error) {
            LOGE("MC3PACK shared catalog %s: not extended (%s); retail catalog is used", rel.c_str(), error.what());
            return {};
        }
    }

    // the host file standing in for `rel`, written on first use; empty to let the game open its own
    std::string Serve(X86Context& ctx, uint8_t* base, const std::string& rel) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = served_.find(rel); it != served_.end()) return it->second;
        std::string xbox;
        if (kMerged.count(rel)) xbox = Merge(ctx, base, rel);
        else if (rel == "resources\\vehicle\\decal.xbck" || rel == "resources\\vehicle\\decal_g.xbck")
            xbox = SharedCatalog(rel);
        else if (rel.rfind("flash\\", 0) == 0) xbox = Flash(ctx, base, rel);
        else if (rel.rfind("fonts\\", 0) == 0) xbox.clear();  // string sources, read above, never served
        else if (const fs::path* f = Find(rel)) xbox = Copy(rel, *f);
        served_[rel] = xbox;
        return xbox;
    }


private:
    const fs::path* Find(const std::string& rel) const {
        for (auto it = packs_.rbegin(); it != packs_.rend(); ++it)  // the last pack wins
            if (auto f = it->files.find(rel); f != it->files.end()) return &f->second;
        return nullptr;
    }

    std::string Write(const std::string& rel, const std::string& text) {
        const std::string xbox = kCache + rel;
        const std::string host = kernel::ResolvePath(xbox, 0);
        if (host.empty()) return {};
        std::error_code ec;
        fs::create_directories(fs::path(host).parent_path(), ec);
        std::ofstream(host, std::ios::binary).write(text.data(), std::streamsize(text.size()));
        return xbox;
    }

    std::string WriteBytes(const std::string& rel, const std::vector<uint8_t>& data) {
        const std::string xbox = kCache + rel;
        const std::string host = kernel::ResolvePath(xbox, 0);
        if (host.empty()) return {};
        std::error_code ec;
        fs::create_directories(fs::path(host).parent_path(), ec);
        std::ofstream stream(host, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return stream ? xbox : std::string();
    }

    static std::string Read(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    static std::vector<uint8_t> Bytes(const fs::path& p) {
        std::ifstream stream(p, std::ios::binary);
        if (!stream) throw std::runtime_error("could not open source file");
        return {(std::istreambuf_iterator<char>(stream)), {}};
    }

    // A pack's own file.  Config indices are accepted when the corresponding merged list exposes them.
    std::string Copy(const std::string& rel, const fs::path& host) {
        std::string text = Read(host);
        if (rel.size() > 7 && rel.compare(rel.size() - 7, 7, ".carcfg") == 0) text = FixCarCfg(rel, text);
        return Write(rel, text);
    }

    // The game's own copy of a file, through its asset manager (the archives it mounted), read once.
    std::string GameFile(X86Context& ctx, uint8_t* base, const std::string& rel) {
        const size_t dot = rel.rfind('.');
        const std::string name = rel.substr(0, dot), ext = dot == std::string::npos ? "" : rel.substr(dot + 1);
        const uint32_t strings = mem::AllocSystem(uint32_t(name.size() + ext.size() + 2), 16);
        std::memcpy(base + strings, name.c_str(), name.size() + 1);
        std::memcpy(base + strings + name.size() + 1, ext.c_str(), ext.size() + 1);
        const uint32_t mgr = X86_MEM32(kAssetManager);
        X86Func* open = mgr ? dispatch::Find(X86_MEM32(X86_MEM32(mgr) + 0x24)) : nullptr;
        std::string text;
        if (open) {
            reading_ = true;
            const uint32_t stream = Call(ctx, base, open, mgr, {strings, uint32_t(strings + name.size() + 1), 0, 1});
            if (stream) {
                const uint32_t size = Call(ctx, base, sub_001A2670, stream, {});
                const uint32_t buf = size ? mem::AllocSystem(size, 16) : 0;
                if (buf) {
                    Call(ctx, base, sub_001A23A0, stream, {buf, size});
                    text.assign(reinterpret_cast<const char*>(base + buf), size);
                    mem::FreeSystem(buf);
                }
                Call(ctx, base, sub_001A2630, stream, {});
            }
            reading_ = false;
        }
        mem::FreeSystem(strings);
        return text;
    }

    std::string Merge(X86Context& ctx, uint8_t* base, const std::string& rel) {
        const std::string game = GameFile(ctx, base, rel);
        return MergeGameFile(rel, game);
    }

    // Make a merged cache file from a retail copy already read through GameFile.  ReadLists uses this directly:
    // the vehicle loader asks for its shared lists before it asks for vehicle.lst, so waiting for a second open is
    // too late to make new config indices valid.
    std::string MergeGameFile(const std::string& rel, const std::string& game) {
        bool any = false;
        for (const Pack& p : packs_) any |= p.files.count(rel) != 0;
        if (!any) return {};
        if (game.empty()) {
            LOGE("MC3PACK %s: the game's copy could not be read; the pack's is used whole", rel.c_str());
            return Write(rel, Read(*Find(rel)));
        }
        std::vector<std::string> lines = Lines(game);
        uint32_t inserted = 0;
        for (const Pack& p : packs_)
            if (auto f = p.files.find(rel); f != p.files.end()) lines = MergeInsertions(lines, Lines(Read(f->second)), inserted);
        std::string out;
        for (const std::string& l : lines) out += l + "\r\n";
        if (rel == "tune\\vehicle\\customdata\\customwheels.txt") wheels_ = Lists(out);
        else if (rel == "tune\\vehicle\\customdata\\customexhausts.txt") exhausts_ = Lists(out);
        LOGE("MC3PACK %s: %u lines added by packs", rel.c_str(), inserted);
        return Write(rel, out);
    }

    // A car's shop movies (flash\ds_, ms_, ps_, vs_): the pack's own when it has Xbox ones, else the game's, else a
    // stock car's for a new car. The garage script (garage_script.cpp) is shown the movie the game will run.
    std::string Flash(X86Context& ctx, uint8_t* base, const std::string& rel) {
        std::string source = rel;
        const fs::path* f = Find(rel);
        // The game asks for .xbck even when a pack supplies its PS2 movie as .pck. Both containers carry a
        // relative Flash base, so serve that image under the requested path and let the native constructors fix it.
        // A supplied Xbox image remains the preferred choice when both files exist.
        if (!f && rel.size() >= 5 && rel.compare(rel.size() - 5, 5, ".xbck") == 0) {
            std::string pck = rel;
            pck.replace(pck.size() - 5, 5, ".pck");
            f = Find(pck);
            if (f) LOGE("MC3PACK %s: using PS2 %s", rel.c_str(), pck.c_str());
        }
        static const char* kPrefixes[] = {"flash\\ds_", "flash\\ms_", "flash\\ps_", "flash\\vs_"};
        for (const char* pre : kPrefixes) {
            const size_t n = std::strlen(pre);
            if (f || rel.compare(0, n, pre) != 0) continue;
            const std::string car = rel.substr(n, rel.rfind('.') - n);
            if (!IsPackCar(car)) break;
            std::error_code ec;
            const std::string own = kernel::ResolvePath(std::string("D:\\") + rel, 0);
            if (!own.empty() && fs::exists(own, ec)) break;
            LOGE("MC3PACK %s: no image for this car, %s's is shown", rel.c_str(), kFallbackCar);
            source = pre + std::string(kFallbackCar) + rel.substr(rel.rfind('.'));
        }
        if (Mc3GarageScriptWants(rel)) {  // the script's statements will run after this movie's own
            std::string file;  // flash\ is loose on the game's drive; the archives are the fallback
            std::error_code ec;
            const std::string loose = kernel::ResolvePath("D:\\" + source, 0);
            if (f) file = Read(*f);
            else if (!loose.empty() && fs::is_regular_file(loose, ec)) file = Read(loose);
            else file = GameFile(ctx, base, source);
            Mc3GarageScriptSee(rel, file);
        }
        if (f) return Write(rel, Read(*f));
        return source == rel ? std::string() : "D:\\" + source;
    }

    bool IsPackCar(const std::string& car) const {
        for (const Pack& p : packs_)
            for (const auto& [rel, path] : p.files)
                if (rel.find(car) != std::string::npos) return true;
        return false;
    }

    // block name -> entry count, for "name { entries }" lists (customwheels.txt, customexhausts.txt)
    static std::map<std::string, uint32_t> Lists(const std::string& text) {
        std::map<std::string, uint32_t> out;
        std::string block;
        bool inside = false;
        for (const std::string& line : Lines(text)) {
            const std::string k = Key(line);
            if (k.empty() || k[0] == ';') continue;
            if (k == "{") inside = true;
            else if (k == "}") inside = false, block.clear();
            else if (inside) out[block]++;
            else block = k;
        }
        return out;
    }

    std::string FixCarCfg(const std::string& rel, const std::string& text) {
        static const struct {
            const char* field;
            const char* list;
            const char* file;
        } kRefs[] = {
            {"rimmdlidx", "rims", "customwheels"},   {"tiremdlidx", "tires", "customwheels"},
            {"biketiremdlidx", "biketires", "customwheels"}, {"brakemdlidx", "brakes", "customwheels"},
            {"exhaustrear", "exhausts", "customexhausts"},   {"exhaustside", "exhausts", "customexhausts"},
        };
        std::string out;
        for (std::string line : Lines(text)) {
            std::istringstream in(line);
            std::string field;
            long value = 0;
            if (in >> field >> value) {
                const std::string f = Key(field);
                for (const auto& r : kRefs)
                    if (f == r.field) {
                        const uint32_t count = Count(r.file, r.list);
                        if (count && (value < 0 || uint32_t(value) >= count)) {
                            LOGE("MC3PACK %s: %s %ld is past the game's %u %s, stock one used", rel.c_str(), field.c_str(),
                                 value, count, r.list);
                            line = "  " + field + " 0 ";
                        }
                    }
                // a vinyl indexes the shared catalog's decal list; one a PS2 package added has no Xbox page
                if (f == "vinyl" && VinylCount() && (value < 0 || uint32_t(value) >= VinylCount())) {
                    LOGE("MC3PACK %s: %s %ld is past the game's %u vinyls, none used", rel.c_str(), field.c_str(), value,
                         VinylCount());
                    line = "  " + field + " 0 ";
                }
            }
            out += line + "\r\n";
        }
        return out;
    }

    // The retail catalog's vinyl count (decal.xbck, the list at +0x0C); 0 when it cannot be read.
    uint32_t VinylCount() {
        if (vinyls_ != ~0u) return vinyls_;
        vinyls_ = 0;
        try {
            const std::string host = kernel::ResolvePath(std::string("D:\\") + "resources\\vehicle\\decal.xbck", 0);
            if (host.empty()) return vinyls_;
            const std::vector<uint8_t> c = Bytes(host);
            if (c.size() < 0x100 || U32(c, 4) != 0x25) return vinyls_;
            const uint32_t table = U32(c, 0x80 + 0xC) - (U32(c, 0) - 0x80);
            if (size_t(table) + 8 <= c.size()) vinyls_ = U32(c, table + 4);
        } catch (const std::exception&) {
        }
        return vinyls_;
    }

    uint32_t Count(const char* file, const char* list) {
        auto& lists = std::strcmp(file, "customwheels") == 0 ? wheels_ : exhausts_;
        if (lists.empty()) {
            const std::string rel = std::string("tune\\vehicle\\customdata\\") + file + ".txt";
            lists = Lists(game_lists_.count(rel) ? game_lists_[rel] : std::string());
        }
        auto it = lists.find(list);
        return it == lists.end() ? 0 : it->second;
    }

public:
    // the game's own wheel and exhaust lists, read once its asset manager is up
    void ReadLists(X86Context& ctx, uint8_t* base) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lists_read_) return;
        lists_read_ = true;
        for (const char* rel : {"tune\\vehicle\\customdata\\customwheels.txt", "tune\\vehicle\\customdata\\customexhausts.txt"}) {
            const std::string retail = GameFile(ctx, base, rel);
            game_lists_[rel] = retail;
            if (const std::string merged = MergeGameFile(rel, retail); !merged.empty()) served_[rel] = merged;
        }
    }

    static bool Reading() { return reading_; }

private:
    static uint32_t Call(X86Context& ctx, uint8_t* base, X86Func* fn, uint32_t ecx, std::initializer_list<uint32_t> args) {
        const uint32_t eax = ctx.eax, ecx0 = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
        for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
        X86_PUSH32(0);  // return address: ret only pops it
        ctx.ecx = ecx;
        fn(ctx, base);
        const uint32_t r = ctx.eax;
        ctx.eax = eax, ctx.ecx = ecx0, ctx.edx = edx, ctx.esp = esp;
        return r;
    }

    std::vector<Pack> packs_;
    std::map<uint32_t, std::u16string> strings_;
    std::map<std::string, std::string> served_;  // relative path -> Xbox path served ("" = the game's own)
    std::map<std::string, std::string> game_lists_;
    std::map<std::string, uint32_t> wheels_, exhausts_;
    std::map<std::string, fs::path> shared_ppf_;  // resources\\vehicle\\{rim,exhaust}.ppf, last pack wins
    std::map<std::string, fs::path> shared_catalog_;  // resources\\vehicle\\{decal,decal_g}.pck
    std::map<std::string, std::string> pagefiles_;  // redirected page-file stems, including a remembered failure
    std::map<std::string, uint32_t> largest_;       // their largest page, the pf05 header's +8
    uint32_t vinyls_ = ~0u;                         // VinylCount(), read once
    bool lists_read_ = false;
    thread_local static bool reading_;
    std::mutex mutex_;
};

thread_local bool Packs::reading_ = false;

Packs& Get() {
    static Packs* p = [] {
        auto* packs = new Packs;
        packs->Init();
        return packs;
    }();
    return *p;
}

uint32_t PathSlot(uint8_t* base, const std::string& path) {
    static uint32_t ring = 0;
    static std::atomic<uint32_t> next{0};
    if (!ring) ring = mem::AllocSystem(32 * 256, 16);
    const uint32_t slot = ring + (next.fetch_add(1) & 31u) * 256u;
    const size_t n = std::min<size_t>(path.size(), 255);
    std::memcpy(base + slot, path.c_str(), n);
    base[slot + n] = 0;
    return slot;
}

}  // namespace

// Folders of PS2 car files the packs bring, for ps2_car_loader.cpp.
std::vector<std::filesystem::path> Mc3CarPackVehicleFolders() { return Get().VehicleFolders(); }

// The packs' folders, for garage_script.cpp (a pack's garage.txt).
std::vector<std::filesystem::path> Mc3CarPackRoots() { return Get().Roots(); }

// Page files do not use Stream::Open: 0x212100 calls the asset manager directly, then retains its first argument for
// asynchronous page-ins.  Rewrite that one bootstrap argument so the original page manager opens the generated cache
// file for both its directory and all later pages.
void Mc3CarPackPageFileBootstrap(X86Context& ctx, uint8_t* base) {
    Packs& packs = Get();
    if (packs.Empty()) return;
    const uint32_t name_va = X86_MEM32(ctx.esp + 4);
    if (name_va < 0x10000) return;
    std::string rel = Lower(std::string(reinterpret_cast<const char*>(base + name_va), 260));
    rel.resize(strnlen(rel.c_str(), rel.size()));
    if (rel.rfind("d:\\", 0) == 0) rel.erase(0, 3);
    if (rel.rfind("$\\", 0) == 0) rel.erase(0, 2);
    if (const std::string pagefile = packs.PageFile(base, rel); !pagefile.empty()) {
        X86_MEM32(ctx.esp + 4) = PathSlot(base, pagefile);
        LOGE("MC3PACK shared bootstrap %s -> %s", rel.c_str(), pagefile.c_str());
    }
}

X86_FUNC(sub_00210650) {
    Mc3CarPackPageFileBootstrap(ctx, base);
    __imp__sub_00210650(ctx, base);
}

// 0xF15B0 gives the page manager its fixed blocks, 0x210690(bytes, count, id), and a page goes to the smallest free
// block that holds its file's largest page (pf05 +8). The rim and exhaust blocks (0x1D800, 0x2800) fit the retail
// pages; converted PS2 models are larger and would overrun them, so with a converted file they grow to its largest page.
X86_FUNC(sub_00210690) {
    Packs& packs = Get();
    const uint32_t bytes = X86_MEM32(ctx.esp + 4);
    const char* rel = bytes == 0x1D800 ? "resources\\vehicle\\rim" : bytes == 0x2800 ? "resources\\vehicle\\exhaust" : nullptr;
    if (rel && !packs.Empty()) {
        if (Mc3Ps2Effects(base).empty()) {
            LOGE("MC3PACK page blocks %X: the effect table is not loaded yet, left as they are", bytes);
        } else if (const uint32_t largest = packs.LargestPage(base, rel); largest > bytes) {
            X86_MEM32(ctx.esp + 4) = largest;
            LOGE("MC3PACK page blocks for %s: %X -> %X bytes x %u", rel, bytes, largest, X86_MEM32(ctx.esp + 8));
        }
    }
    __imp__sub_00210690(ctx, base);
}

// datPageManager releases every block in a loaded page through the size at
// chunk+0x0C and optional callback at chunk+0x18. Runtime-built PS2 tail pages
// can expose a final allocator block that is still 0xCD debug fill. Normalize
// only that sentinel to the zero terminator/absent callback used by retail
// pages; real sizes and addresses stay untouched and the original release
// routine still owns the lifecycle.
X86_FUNC(sub_002112C0) {
    if (!Get().Empty()) {
        uint32_t chunk = X86_MEM32(ctx.ecx);
        for (uint32_t guard = 0; guard < 4096 && chunk && mem::IsCommitted(chunk) &&
                                 mem::IsCommitted(chunk + 0x1C);
             guard++) {
            const uint32_t bytes = X86_MEM32(chunk + 0xC);
            if (!bytes) break;
            if (bytes == 0xCDCDCDCDu) {
                LOGE("MC3PACK page release: terminated debug-fill block at %08X", chunk);
                X86_MEM32(chunk + 0xC) = 0;
                break;
            }
            if (X86_MEM32(chunk + 0x18) == 0xCDCDCDCDu) {
                LOGE("MC3PACK page release: cleared absent callback at %08X (block %X bytes)", chunk, bytes);
                X86_MEM32(chunk + 0x18) = 0;
            }
            if (bytes < 0x20 || bytes > 0x1000000u || chunk + bytes < chunk) break;
            chunk += bytes;
        }
    }
    __imp__sub_002112C0(ctx, base);
}

// Called first by Mc2LaPaths (Stream::Open, 0x1A2280). true when the path now names a file the console's file system
// opens (the caller switches Stream::Open to that device for this call).
bool Mc3CarPackOpen(X86Context& ctx, uint8_t* base) {
    Packs& packs = Get();
    if ((packs.Empty() && !Mc3GarageScriptActive()) || Packs::Reading()) return false;
    const uint32_t va = X86_MEM32(ctx.esp + 4);
    if (!va) return false;
    const char* p = reinterpret_cast<const char*>(base + va);
    std::string rel = Lower(std::string(p, strnlen(p, 260)));
    if (rel.rfind("d:\\", 0) == 0) rel.erase(0, 3);
    if (rel.rfind("$\\", 0) == 0) rel.erase(0, 2);
    if (rel.rfind("\\device\\", 0) == 0) return false;
    if (const std::string pagefile = packs.PageFile(base, rel); !pagefile.empty()) {
        X86_MEM32(ctx.esp + 4) = PathSlot(base, pagefile);
        LOGE("MC3PACK %s -> %s", rel.c_str(), pagefile.c_str());
        return true;
    }
    if (rel == "tune\\vehicle\\vehicle.lst") packs.ReadLists(ctx, base);
    const std::string xbox = packs.Serve(ctx, base, rel);
    if (xbox.empty()) return false;
    X86_MEM32(ctx.esp + 4) = PathSlot(base, xbox);
    static uint32_t shown = 0;
    if (shown++ < 400) LOGE("MC3PACK %s -> %s", rel.c_str(), xbox.c_str());
    return xbox.rfind("\\Device\\", 0) == 0;  // a stock image on D: opens the usual way
}

// txtStringTable::Get (0xEA750, ecx = table, key). Script labels override retail text; pack strings fill missing keys.
X86_FUNC(sub_000EA750) {
    const Packs& packs = Get();
    const uint32_t key = X86_MEM32(ctx.esp + 4);
    static const bool script = Mc3DebugHasArg(L"--mc3-garage-script");
    const std::u16string* label = script && key >= 0x10000
                                     ? Mc3GarageString(reinterpret_cast<const char*>(base + key)) : nullptr;
    if (packs.Empty() && !label) {
        __imp__sub_000EA750(ctx, base);
        return;
    }
    const uint32_t table = ctx.ecx;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, esp = ctx.esp;
    X86_PUSH32(key);
    X86_PUSH32(0);
    sub_000EA710(ctx, base);
    const uint32_t hash = ctx.eax;
    ctx.esp = esp;
    const uint32_t lang = X86_MEM32(table + 4 * X86_MEM32(table + 0x28));
    uint32_t found = 0;
    if (lang) {
        X86_PUSH32(hash);
        X86_PUSH32(0);
        ctx.ecx = lang;
        sub_000EC580(ctx, base);
        found = ctx.eax;
        ctx.esp = esp;
    }
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
    const std::u16string* text = label ? label : (found ? nullptr : packs.Text(hash));
    __imp__sub_000EA750(ctx, base);
    const uint32_t entry = ctx.eax;
    if (!text || !entry) return;
    // The hook is called on every lookup. Reuse the installed value on subsequent calls.
    const uint32_t current = X86_MEM32(entry + 8);
    if (current) {
        size_t i = 0;
        while (i < text->size() && X86_MEM16(current + uint32_t(i * 2)) == (*text)[i]) ++i;
        if (i == text->size() && X86_MEM16(current + uint32_t(i * 2)) == 0) return;
    }
    const uint32_t keep = ctx.eax, ecx2 = ctx.ecx, edx2 = ctx.edx, esp2 = ctx.esp;
    X86_PUSH32(uint32_t((text->size() + 1) * 2));
    X86_PUSH32(0);
    sub_001B3B90(ctx, base);  // the game's heap: the table frees its strings with the game's delete
    const uint32_t wide = ctx.eax;
    ctx.eax = keep, ctx.ecx = ecx2, ctx.edx = edx2, ctx.esp = esp2;
    if (!wide) return;
    std::memcpy(base + wide, text->c_str(), (text->size() + 1) * 2);
    X86_MEM32(entry + 8) = wide;
}

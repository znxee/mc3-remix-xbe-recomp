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
// What a pack cannot add yet: wheels and exhausts of its own (they live in the shared rim/exhaust packs), so a car
// config naming one past the game's lists gets the stock one; front-end images of a new car (flash\ds_/ms_/ps_/vs_)
// come from a stock car's.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
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

bool Mc3GarageScriptActive();  // garage_script.cpp, --mc3-garage-script
bool Mc3GarageScriptWants(const std::string& rel);
void Mc3GarageScriptSee(const std::string& rel, const std::string& file);

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kAssetManager = 0x5348B0;  // its vtbl+0x24 opens (name, extension, 0, 1) -> Stream*
constexpr char kCache[] = "\\Device\\Harddisk0\\Partition5\\carpacks\\";
constexpr char kFallbackCar[] = "vp_jetta_03";  // front-end images for a new car

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

    // the host file standing in for `rel`, written on first use; empty to let the game open its own
    std::string Serve(X86Context& ctx, uint8_t* base, const std::string& rel) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = served_.find(rel); it != served_.end()) {
            // A cached shop can become active again after another car or shop was opened.
            if (Mc3GarageScriptActive() && rel.rfind("flash\\", 0) == 0) Mc3GarageScriptWants(rel);
            return it->second;
        }
        std::string xbox;
        if (kMerged.count(rel)) xbox = Merge(ctx, base, rel);
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

    static std::string Read(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    // a pack's own file; a car config gets wheels and exhausts the game has
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
        bool any = false;
        for (const Pack& p : packs_) any |= p.files.count(rel) != 0;
        if (!any) return {};
        const std::string game = GameFile(ctx, base, rel);
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
        LOGE("MC3PACK %s: %u lines added by packs", rel.c_str(), inserted);
        return Write(rel, out);
    }

    // A car's shop movies (flash\ds_, ms_, ps_, vs_): the pack's own when it has Xbox ones, else the game's, else a
    // stock car's for a new car. The garage script (garage_script.cpp) is shown the movie the game will run.
    std::string Flash(X86Context& ctx, uint8_t* base, const std::string& rel) {
        std::string source = rel;
        const fs::path* f = Find(rel);
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
            }
            out += line + "\r\n";
        }
        return out;
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
        for (const char* rel : {"tune\\vehicle\\customdata\\customwheels.txt", "tune\\vehicle\\customdata\\customexhausts.txt"})
            game_lists_[rel] = GameFile(ctx, base, rel);
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
    if (rel == "tune\\vehicle\\vehicle.lst") packs.ReadLists(ctx, base);
    const std::string xbox = packs.Serve(ctx, base, rel);
    if (xbox.empty()) return false;
    X86_MEM32(ctx.esp + 4) = PathSlot(base, xbox);
    static uint32_t shown = 0;
    if (shown++ < 400) LOGE("MC3PACK %s -> %s", rel.c_str(), xbox.c_str());
    return xbox.rfind("\\Device\\", 0) == 0;  // a stock image on D: opens the usual way
}

// txtStringTable::Get (0xEA750, ecx = table, key). For a key the game lacks it makes and keeps an entry reading
// "!!key!!"; when a car pack names the key, that entry gets the pack's text instead (car names, descriptions).
X86_FUNC(sub_000EA750) {
    const Packs& packs = Get();
    if (packs.Empty()) {
        __imp__sub_000EA750(ctx, base);
        return;
    }
    const uint32_t table = ctx.ecx, key = X86_MEM32(ctx.esp + 4);
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
    const std::u16string* text = found ? nullptr : packs.Text(hash);
    __imp__sub_000EA750(ctx, base);
    const uint32_t entry = ctx.eax;
    if (!text || !entry) return;
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

// Opt-in: the garage's shop menus from an editable text script (MC3-0023).
//
// Each car has four front-end movies, flash\<shop>_<car>.xbck: ds (detail: vinyls, decals, badges, plates), ms
// (mechanic: engine, transmission, chassis...), ps (paint) and vs (visual: body kits, rims). The first DoInitAction of
// each one is plain AVM1 that builds the shop's menus as _global arrays, "<List>Names", "<List>Status" and
// "<List>Price" (and "<List>Order"): _global.UpgradeTypesNames = new Array("Power_Engine", "Transmission", ...).
// The garage movie draws its lists from them and the game reads them back (0x139690 for "selectedN", 0x139650 for an
// element) to know what was chosen, so these arrays are the menus' items and options.
//
// --mc3-garage-script=<file> (and garage.txt at the root of each car pack) sets arrays and values in them:
//
//   [vp_350z_04 ms]                     ; a car and a shop (ds, ms, ps, vs); * for every car
//   TurboUpgradePrice = [$0.00, $500.00, $1000.00]
//   Names = Default name                ; a plain value
//
// The script's statements for a movie are compiled to AVM1 and run right after the movie's own init action, in the
// same place: when the game opens the movie (Stream::Open, car_packs.cpp) its init action is kept, and when the action
// interpreter (0x68BC0) runs that same code it runs the script's after it. The movie is not served changed: its data
// does not come from the stream that open returns. Unmentioned arrays stay as the movie has them, except
// Order inferred from edited Names to keep the original icons with their items.
//
// --mc3-garage-dump writes every such movie's arrays, as script, to hdd\Z\carpacks\garage_dump\<shop>_<car>.txt, the
// starting point for editing one.
//
// --mc3-showroom-packs does the same for the garage movie (flash\toplevel_x.xbck): its Showroom arrays get the car
// packs' cars, with entries from each pack's own garage movie and [showroom] rows (see ShowroomStatements).

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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

std::vector<std::filesystem::path> Mc3CarPackRoots();  // car_packs.cpp
bool Mc3CarPackMissingShopMovie(X86Context& ctx, uint8_t* base, const std::string& rel);

namespace {

namespace fs = std::filesystem;

constexpr char kDump[] = "\\Device\\Harddisk0\\Partition5\\carpacks\\garage_dump\\";

struct Statement {
    std::string name;
    bool array = false;
    bool ints = false;  // pushed as integers, as the movie has them (ShowroomClass)
    std::vector<std::string> values;  // one for a plain value
};

// One car of the Showroom list: its entry in each of the movie's parallel arrays, Names first.
constexpr const char* kShowroom[] = {"ShowRoomNames", "ShowRoomPrice", "ShowRoomStatus", "ShowroomClass", "ShowroomPerf",
                                     "AccelMin",      "TopSpeedMin",   "HandlingMin",    "PrizeStatus"};
constexpr size_t kShowroomFields = std::size(kShowroom) - 1;
using ShowroomRow = std::vector<std::string>;  // kShowroomFields values, "" for one not given

struct Section {
    std::string car;   // "*" for every car
    std::string shop;  // ds, ms, ps, vs
    std::vector<Statement> statements;
};

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// "a, b, "c, d"" -> {a, b, c, d}
std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    if (Trim(s).empty()) return out;
    std::string cur;
    bool quoted = false;
    auto finish = [&] {
        std::string t = Trim(cur);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
        out.push_back(std::move(t));
        cur.clear();
    };
    for (char c : s) {
        if (c == '"') quoted = !quoted;
        if (c == ',' && !quoted) finish();
        else cur += c;
    }
    finish();
    return out;
}

std::string Quote(const std::string& v) {
    const bool needs = v.empty() || v.find(',') != std::string::npos || v.front() == ' ' || v.back() == ' ' ||
                       v.front() == '[' || v.find('"') != std::string::npos;
    return needs ? "\"" + v + "\"" : v;
}

// Strict UTF-8, including supplementary characters; reject overlong and surrogate encodings.
bool Utf16(const std::string& utf8, std::u16string& out) {
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t first = uint8_t(utf8[i++]);
        uint32_t c = 0;
        int n = 0;
        if (first < 0x80) c = first;
        else if (first >= 0xC2 && first <= 0xDF) c = first & 0x1F, n = 1;
        else if (first >= 0xE0 && first <= 0xEF) c = first & 0x0F, n = 2;
        else if (first >= 0xF0 && first <= 0xF4) c = first & 0x07, n = 3;
        else return false;
        if (i + n > utf8.size()) return false;
        for (int k = 0; k < n; ++k) {
            const uint8_t b = uint8_t(utf8[i++]);
            if ((b & 0xC0) != 0x80) return false;
            c = (c << 6) | (b & 0x3F);
        }
        if ((n == 2 && c < 0x800) || (n == 3 && c < 0x10000) || c > 0x10FFFF ||
            (c >= 0xD800 && c <= 0xDFFF) || c == 0) return false;
        if (c < 0x10000) out.push_back(char16_t(c));
        else {
            c -= 0x10000;
            out.push_back(char16_t(0xD800 + (c >> 10)));
            out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
        }
    }
    return true;
}

std::vector<Section> Parse(const fs::path& file, std::map<std::string, std::u16string>& strings,
                           std::map<std::string, std::string>& aliases,
                           std::vector<std::pair<std::string, ShowroomRow>>& showroom, bool enabled) {
    std::ifstream in(file, std::ios::binary);
    std::vector<Section> out;
    std::string line;
    bool in_strings = false, in_section = false, in_showroom = false;
    uint32_t number = 0;
    while (std::getline(in, line)) {
        number++;
        if (number == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.rfind("alias ", 0) == 0) {
            const size_t eq = line.find('=');
            const std::string name = eq == std::string::npos ? "" : Trim(line.substr(6, eq - 6));
            const std::string target = eq == std::string::npos ? "" : Trim(line.substr(eq + 1));
            if (name.empty() || target.empty()) LOGE("MC3GARAGE %ls:%u: expected alias <name> = <target>", file.c_str(), number);
            else if (enabled) aliases[name] = target;
            continue;
        }
        if (line[0] == '[') {
            in_strings = line == "[strings]";
            in_showroom = Lower(line) == "[showroom]";
            in_section = false;
            if (in_strings || in_showroom) continue;
            std::istringstream words(line.substr(1, line.find(']') - 1));
            Section s;
            words >> s.car >> s.shop;
            s.car = Lower(s.car), s.shop = Lower(s.shop);
            if (s.shop != "ds" && s.shop != "ms" && s.shop != "ps" && s.shop != "vs") {
                LOGE("MC3GARAGE %ls:%u: a section is [<car> <shop>], shop one of ds ms ps vs", file.c_str(), number);
                continue;
            }
            out.push_back(std::move(s));
            in_section = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || (!in_section && !in_strings && !in_showroom)) {
            LOGE("MC3GARAGE %ls:%u: expected <name> = <value> inside a [<car> <shop>] section", file.c_str(), number);
            continue;
        }
        if (in_showroom) {  // <car> = price, status, class, perf, accel, top speed, handling, prize
            const std::string car = Lower(Trim(line.substr(0, eq)));
            ShowroomRow row = SplitList(line.substr(eq + 1));
            if (car.empty() || row.empty() || row.size() > kShowroomFields)
                LOGE("MC3GARAGE %ls:%u: expected <car> = price, status, class, perf[, accel, top speed, handling, prize]",
                     file.c_str(), number);
            else {  // used only with --mc3-showroom-packs
                row.resize(kShowroomFields);
                showroom.emplace_back(car, std::move(row));
            }
            continue;
        }
        if (in_strings) {
            const std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            std::u16string text;
            if (key.empty() || !Utf16(value, text)) LOGE("MC3GARAGE %ls:%u: invalid string key or UTF-8", file.c_str(), number);
            else if (enabled) strings[key] = std::move(text);
            continue;
        }
        Statement st;
        st.name = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        if (!value.empty() && value[0] == '[') {
            st.array = true;
            const size_t close = value.rfind(']');
            st.values = SplitList(value.substr(1, close == std::string::npos ? std::string::npos : close - 1));
        } else {
            st.values.push_back(value.size() >= 2 && value.front() == '"' && value.back() == '"'
                                    ? value.substr(1, value.size() - 2)
                                    : value);
        }
        out.back().statements.push_back(std::move(st));
    }
    return out;
}

class Script {
public:
    Script() {
        dump_ = Mc3DebugHasArg(L"--mc3-garage-dump");
        showroom_packs_ = Mc3DebugHasArg(L"--mc3-showroom-packs");
        pack_shops_ = Mc3DebugHasArg(L"--mc3-pack-shops");
        const bool enabled = Mc3DebugHasArg(L"--mc3-garage-script");
        std::vector<fs::path> files;
        if (const wchar_t* v = Mc3DebugArg(L"--mc3-garage-script"); v && *v) files.emplace_back(v);
        std::error_code ec;
        for (const fs::path& root : Mc3CarPackRoots())
            if (fs::is_regular_file(root / "garage.txt", ec)) files.push_back(root / "garage.txt");
        for (const fs::path& f : files) {
            if (!fs::is_regular_file(f, ec)) {
                LOGE("MC3GARAGE script %ls not found", f.c_str());
                continue;
            }
            std::vector<std::pair<std::string, ShowroomRow>> showroom;
            std::vector<Section> s = Parse(f, strings_, aliases_, showroom, enabled);
            size_t n = 0;
            for (const Section& x : s) n += x.statements.size();
            LOGE("MC3GARAGE script %ls: %zu sections, %zu statements, %zu showroom cars", f.c_str(), s.size(), n,
                 showroom.size());
            for (Section& x : s) sections_.push_back(std::move(x));
            if (showroom_packs_)
                for (auto& row : showroom) showroom_.push_back(std::move(row));
        }
    }

    bool Active() const {
        return dump_ || showroom_packs_ || pack_shops_ || !sections_.empty() || !strings_.empty() || !aliases_.empty();
    }

    bool ShowroomPacks() const { return showroom_packs_; }
    bool PackShops() const { return pack_shops_; }

    // [showroom] rows, in script order (a later one wins field by field)
    const std::vector<std::pair<std::string, ShowroomRow>>& Showroom() const { return showroom_; }

    const std::u16string* Text(const std::string& key) const {
        const auto it = strings_.find(key);
        return it == strings_.end() ? nullptr : &it->second;
    }

    bool Alias(const std::string& name, std::string& target) const {
        const auto it = aliases_.find(name);
        if (it == aliases_.end()) return false;
        target = it->second;
        return true;
    }

    bool HasAliases() const { return !aliases_.empty(); }

    bool Wants(const std::string& shop, const std::string& car) const {
        if (dump_) return true;
        if (shop == "toplevel") return showroom_packs_;
        if (pack_shops_) return true;
        for (const Section& s : sections_)
            if (s.shop == shop && (s.car == "*" || s.car == car)) return true;
        return false;
    }

    // statements for this movie: every car's first, then the car's own, so the car's win
    std::vector<const Statement*> For(const std::string& shop, const std::string& car) const {
        std::vector<const Statement*> out;
        for (int pass = 0; pass < 2; pass++)
            for (const Section& s : sections_)
                if (s.shop == shop && (pass == 0 ? s.car == "*" : s.car == car))
                    for (const Statement& st : s.statements) out.push_back(&st);
        return out;
    }

    bool Dump() const { return dump_; }

private:
    bool dump_ = false;
    bool showroom_packs_ = false;
    bool pack_shops_ = false;
    std::vector<std::pair<std::string, ShowroomRow>> showroom_;
    std::vector<Section> sections_;
    std::map<std::string, std::u16string> strings_;
    std::map<std::string, std::string> aliases_;
};

const Script& Get() {
    static const Script script;
    return script;
}

// The init action of a shop movie: [sprite id u16][AVM1 ... 0x00], found as the first constant pool whose first
// constant is "_global".
struct Action {
    size_t block = 0;    // file offset of the sprite id
    size_t code = 0;     // first action
    size_t end = 0;      // the ActionEnd byte
};

uint32_t U32(const std::string& f, size_t o) {
    uint32_t v;
    std::memcpy(&v, f.data() + o, 4);
    return v;
}

bool FindAction(const std::string& f, Action& a, std::string& error) {
    if (f.size() < 0x100 || U32(f, 8) != 1 || U32(f, 0xC) != f.size() - 0x80) return error = "not a resource", false;
    const size_t g = f.find(std::string("_global\0", 8), 0x80);
    if (g == std::string::npos || g < 0x80 + 7 || uint8_t(f[g - 5]) != 0x88) return error = "no init action", false;
    a.code = g - 5;
    a.block = a.code - 2;
    for (size_t p = a.code; p < f.size();) {  // walk the actions to ActionEnd
        const uint8_t op = uint8_t(f[p]);
        if (op == 0) {
            a.end = p;
            break;
        }
        p += 1;
        if (op >= 0x80) {
            if (p + 2 > f.size()) break;
            p += 2 + (uint8_t(f[p]) | (uint8_t(f[p + 1]) << 8));
        }
    }
    if (!a.end) return error = "the init action has no end", false;
    return true;
}

// The statements of an init action (ConstantPool, Push, GetVariable, NewObject, SetMember is all the shop movies use;
// toplevel's ends in a Stop).
bool Decode(const std::string& f, const Action& a, std::vector<Statement>& out, std::string& error) {
    struct Value {
        std::string text;
        bool array = false;
        bool number = false;  // an integer push; for an array, every item one
        std::vector<std::string> items;
    };
    std::vector<std::string> pool;
    std::vector<Value> stack;
    auto pop = [&](Value& v) {
        if (stack.empty()) return false;
        v = std::move(stack.back());
        stack.pop_back();
        return true;
    };
    for (size_t p = a.code; p < a.end;) {
        const uint8_t op = uint8_t(f[p++]);
        size_t len = 0;
        if (op >= 0x80) len = uint8_t(f[p]) | (uint8_t(f[p + 1]) << 8), p += 2;
        const size_t body = p, stop = p + len;
        p = stop;
        auto cstr = [&](size_t& q) {
            const size_t e = f.find('\0', q);
            std::string s = f.substr(q, e - q);
            q = e + 1;
            return s;
        };
        if (op == 0x88) {
            size_t q = body + 2;
            const uint32_t n = uint8_t(f[body]) | (uint8_t(f[body + 1]) << 8);
            for (uint32_t i = 0; i < n && q < stop; i++) pool.push_back(cstr(q));
        } else if (op == 0x96) {
            for (size_t q = body; q < stop;) {
                const uint8_t t = uint8_t(f[q++]);
                Value v;
                if (t == 0) v.text = cstr(q);
                else if (t == 7) v.text = std::to_string(int32_t(U32(f, q))), v.number = true, q += 4;
                else if (t == 8) {
                    const size_t i = uint8_t(f[q]);
                    v.text = i < pool.size() ? pool[i] : "";
                    q += 1;
                } else if (t == 9) {
                    const size_t i = uint8_t(f[q]) | (uint8_t(f[q + 1]) << 8);
                    v.text = i < pool.size() ? pool[i] : "";
                    q += 2;
                } else if (t == 5) v.text = f[q] ? "true" : "false", q += 1;
                else if (t == 1) {
                    float x;
                    std::memcpy(&x, f.data() + q, 4);
                    v.text = std::to_string(x), q += 4;
                } else if (t == 6) {  // a double, its two words swapped
                    uint32_t w[2];
                    std::memcpy(w, f.data() + q, 8);
                    const uint64_t bits = (uint64_t(w[0]) << 32) | w[1];
                    double x;
                    std::memcpy(&x, &bits, 8);
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%.9g", x);
                    v.text = buf, q += 8;
                } else if (t == 2 || t == 3) {  // null, undefined
                } else return error = "push of type " + std::to_string(t), false;
                stack.push_back(std::move(v));
            }
        } else if (op == 0x1C) {  // GetVariable: only _global is read
            Value v;
            if (!pop(v)) return error = "stack", false;
            stack.push_back(std::move(v));
        } else if (op == 0x40) {  // NewObject(name, count, args...): only Array
            Value name, count;
            if (!pop(name) || !pop(count)) return error = "stack", false;
            Value arr;
            arr.array = true;
            const int n = std::atoi(count.text.c_str());
            arr.number = n > 0;
            for (int i = 0; i < n; i++) {
                Value e;
                if (!pop(e)) return error = "stack", false;
                arr.number &= e.number;
                arr.items.push_back(std::move(e.text));
            }
            stack.push_back(std::move(arr));
        } else if (op == 0x4F) {  // SetMember(object, name, value)
            Value value, name, object;
            if (!pop(value) || !pop(name) || !pop(object)) return error = "stack", false;
            Statement st;
            st.name = name.text;
            st.array = value.array;
            st.ints = value.array && value.number;
            st.values = value.array ? std::move(value.items) : std::vector<std::string>{value.text};
            out.push_back(std::move(st));
        } else if (op == 0x07) {  // Stop
        } else {
            char buf[48];
            std::snprintf(buf, sizeof buf, "action 0x%02X", op);
            return error = buf, false;
        }
    }
    return true;
}

void Push(std::string& code, const std::string& s) {
    const size_t len = s.size() + 2;
    code += char(0x96), code += char(len & 0xFF), code += char(len >> 8), code += char(0), code += s, code += char(0);
}

void PushInt(std::string& code, int32_t v) {
    code += char(0x96), code += char(5), code += char(0), code += char(7);
    code.append(reinterpret_cast<const char*>(&v), 4);
}

bool Int(const std::string& text, int32_t& v) {
    char* end = nullptr;
    const long long n = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || *end || n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max())
        return false;
    v = int32_t(n);
    return true;
}

// _global.<name> = <value> or new Array(<values>), as the movies write it: the first element is pushed last
std::string Compile(const Statement& st) {
    std::string code;
    Push(code, "_global");
    code += char(0x1C);
    Push(code, st.name);
    if (st.array) {
        for (auto it = st.values.rbegin(); it != st.values.rend(); ++it) {
            int32_t v;
            if (st.ints && Int(*it, v)) PushInt(code, v);
            else Push(code, *it);
        }
        PushInt(code, int32_t(st.values.size()));
        Push(code, "Array");
        code += char(0x40);
    } else {
        Push(code, st.values.empty() ? std::string() : st.values[0]);
    }
    code += char(0x4F);
    return code;
}

// The ring animates slots, but each slot's clip event reads Order to select the item's artwork.
// Resolve the final Names against the unmodified movie, not against an earlier wildcard edit.
// Explicit Order statements keep their existing meaning and always win over inference.
std::vector<Statement> DeriveOrders(const std::vector<Statement>& original,
                                  const std::vector<const Statement*>& edits, const std::string& rel) {
    auto findOriginal = [&](const std::string& name) -> const Statement* {
        for (auto it = original.rbegin(); it != original.rend(); ++it)
            if (it->name == name) return &*it;
        return nullptr;
    };
    auto finalEdit = [&](const std::string& name) -> const Statement* {
        for (auto it = edits.rbegin(); it != edits.rend(); ++it)
            if ((*it)->name == name) return *it;
        return nullptr;
    };
    std::vector<Statement> out;
    for (const Statement* st : edits) {
        if (!st->array || st->name.size() <= 5 || st->name.compare(st->name.size() - 5, 5, "Names") != 0 ||
            finalEdit(st->name) != st) continue;
        std::string orderName = st->name.substr(0, st->name.size() - 5) + "Order";
        // This is the movie's actual spelling, including the extra 's'.
        if (st->name == "ChassisUpgradeNames") orderName = "ChassisUpgradesOrder";
        if (finalEdit(orderName)) continue;
        const Statement* names = findOriginal(st->name);
        const Statement* order = findOriginal(orderName);
        // Lists without an Order array have their own movie-specific presentation.
        if (!names || !order || !names->array || !order->array || names->values == st->values) continue;
        Statement derived;
        derived.name = orderName;
        derived.array = true;
        bool valid = names->values.size() == order->values.size();
        for (const std::string& item : st->values) {
            auto it = std::find(names->values.begin(), names->values.end(), item);
            if (it == names->values.end()) { valid = false; break; }
            const size_t index = size_t(it - names->values.begin());
            if (index >= order->values.size()) { valid = false; break; }
            // A repeated original name is inferable only if every occurrence has the same icon.
            for (size_t i = index + 1; i < names->values.size() && i < order->values.size(); i++)
                if (names->values[i] == item && order->values[i] != order->values[index]) valid = false;
            derived.values.push_back(order->values[index]);
        }
        if (!valid) {
            LOGE("MC3GARAGE %s: cannot derive %s from %s; set %s explicitly to match the edited list",
                 rel.c_str(), orderName.c_str(), st->name.c_str(), orderName.c_str());
            continue;
        }
        LOGE("MC3GARAGE %s: derived %s from %s (%zu items)", rel.c_str(), orderName.c_str(),
             st->name.c_str(), derived.values.size());
        out.push_back(std::move(derived));
    }
    return out;
}

void WriteDump(const std::string& shop, const std::string& car, const std::vector<Statement>& sts) {
    const std::string host = kernel::ResolvePath(std::string(kDump) + shop + "_" + car + ".txt", 0);
    if (host.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(host).parent_path(), ec);
    std::ofstream out(host, std::ios::binary);
    out << "; flash\\" << shop << "_" << car << ".xbck, as the game has it\r\n[" << car << " " << shop << "]\r\n";
    for (const Statement& st : sts) {
        out << st.name << " = ";
        if (!st.array) {
            out << Quote(st.values.empty() ? std::string() : st.values[0]) << "\r\n";
            continue;
        }
        out << "[";
        for (size_t i = 0; i < st.values.size(); i++) out << (i ? ", " : "") << Quote(st.values[i]);
        out << "]\r\n";
    }
    LOGE("MC3GARAGE dump %s", host.c_str());
}

// The Showroom as the game will list it, as a [showroom] section, to garage_dump\showroom_<movie>.txt.
void WriteShowroom(const std::string& movie, const std::vector<std::string>& rows) {
    const std::string host = kernel::ResolvePath(std::string(kDump) + "showroom_" + movie + ".txt", 0);
    if (host.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(host).parent_path(), ec);
    std::ofstream out(host, std::ios::binary);
    out << "; flash\\" << movie << ".xbck's Showroom, in list order\r\n"
        << "; <car> = price, status, class, perf, accel, top speed, handling, prize\r\n[showroom]\r\n";
    for (const std::string& r : rows) out << r << "\r\n";
    LOGE("MC3GARAGE dump %s", host.c_str());
}

// "flash\ms_vp_350z_04.xbck" -> ms, vp_350z_04; "flash\toplevel_x.xbck" (the garage, with the Showroom) -> toplevel
bool ShopAndCar(const std::string& rel, std::string& shop, std::string& car) {
    static const char kPrefix[] = "flash\\";
    const size_t n = sizeof kPrefix - 1;
    if (rel == "flash\\toplevel_x.xbck" || rel == "flash\\toplevel.xbck") {
        shop = "toplevel";
        car = rel.substr(n, rel.rfind('.') - n);
        return true;
    }
    if (rel.compare(0, n, kPrefix) != 0 || rel.size() < n + 4 || rel[n + 2] != '_') return false;
    shop = rel.substr(n, 2);
    if (shop != "ds" && shop != "ms" && shop != "ps" && shop != "vs") return false;
    const size_t dot = rel.rfind('.');
    if (dot == std::string::npos || rel.compare(dot, std::string::npos, ".xbck") != 0) return false;
    car = rel.substr(n + 3, dot - n - 3);
    return true;
}

// --mc3-showroom-packs: the Showroom (Buy Vehicles) lists the packs' cars.
//
// The list is the nine parallel arrays (kShowroom) of the garage movie's init action, flash\toplevel_x.xbck: 94 cars
// and 23 vp_dlslot_NN placeholders (Xbox Live slots, no status: not shown). Buying goes by name (0x1440D0 resolves the
// selected name in the vehicle list), but 0x152420 writes the Remix SpecialEdition cars' class, perf and price into
// these arrays at their vehicle-list index, so the list's order is the vehicle list's: a pack car takes the place of
// the placeholder at its own vehicle index, or is appended when it is the next one. Name -> list index (0x144BE0)
// counts in a signed byte, so the list stays under 128 entries.
//
// A car's entries come from its pack's own garage movie (flash\toplevel(_x).pck or .xbck, a PS2 one fills the same
// arrays, often element by element in later action blocks), then from [showroom] rows of the scripts:
//   [showroom]
//   vp_d_golf_83 = $16000.00, 2, 1, d, 12.358152, 33.451324, 41.176467, 0
// (price, status, class, perf, accel, top speed, handling, prize; trailing or empty fields keep the movie's). A row for
// a car already on the list changes it in place.

constexpr size_t kShowroomMax = 127;
constexpr uint32_t kVehicleList = 0x557EB0;  // 0x28-byte entries, name at +0; count word at +4

bool Placeholder(const std::string& name) { return name.rfind("vp_dlslot_", 0) == 0; }

// The garage movie shows ShowRoomNames.length - 23 + XBliveCars entries (its ring set-up, "numberofitems": the 23
// placeholders are not shown). Placeholders taken by pack cars are counted back in through XBliveCars.
std::atomic<int> g_showroom_extra{0};

// The vehicle list's names, in its order.
std::vector<std::string> VehicleNames() {
    std::vector<std::string> out;
    uint8_t* base = mem::Base();
    const uint32_t list = *reinterpret_cast<uint32_t*>(base + kVehicleList);
    const uint32_t count = *reinterpret_cast<uint16_t*>(base + kVehicleList + 4);
    if (!list || !mem::IsCommitted(list) || !mem::IsCommitted(list + count * 0x28 - 1)) return out;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t name = *reinterpret_cast<uint32_t*>(base + list + i * 0x28);
        std::string s;
        for (uint32_t k = 0; k < 64 && name >= 0x10000 && mem::IsCommitted(name + k) && base[name + k]; k++)
            s += char(std::tolower(base[name + k]));
        out.push_back(std::move(s));
    }
    return out;
}

// The _global arrays a movie sets (those `wanted` accepts): every action block whose constant pool names `word`, run
// straight through in file order, the last assignment winning. A PS2 mod's movie keeps the original init action and
// rebuilds arrays element by element in blocks appended later (VIP). Enough for these blocks (pushes, members, arrays
// and nothing that branches); a block that branches or defines a function is left at that point.
std::map<std::string, std::vector<std::string>> MovieArrays(const std::string& f, const char* word,
                                                            bool (*wanted)(const std::string&)) {
    struct Value {
        int kind = 0;  // 0 a value, 1 _global, 2 one of _global's wanted arrays, 3 a new array, 4 anything else
        std::string text;
        std::vector<std::string> items;
    };
    std::map<std::string, std::vector<std::string>> arrays;
    auto number = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", v);
        return std::string(buf);
    };
    for (size_t at = f.find('\x88'); at != std::string::npos && at + 5 <= f.size(); at = f.find('\x88', at + 1)) {
        const size_t len = uint8_t(f[at + 1]) | (uint8_t(f[at + 2]) << 8), n = uint8_t(f[at + 3]) | (uint8_t(f[at + 4]) << 8);
        const size_t end = at + 3 + len;
        if (!n || end > f.size()) continue;
        std::vector<std::string> pool;
        size_t q = at + 5;
        for (size_t i = 0; i < n && q < end; i++) {
            const size_t z = f.find('\0', q);
            if (z == std::string::npos || z >= end) break;
            pool.push_back(f.substr(q, z - q));
            q = z + 1;
        }
        if (pool.size() != n || q != end || std::find(pool.begin(), pool.end(), word) == pool.end()) continue;
        std::vector<Value> stack;
        auto pop = [&] {
            Value v;
            if (!stack.empty()) v = std::move(stack.back()), stack.pop_back();
            return v;
        };
        for (size_t p = end; p < f.size();) {
            const uint8_t op = uint8_t(f[p++]);
            if (!op) break;
            size_t body = p, stop = p;
            if (op >= 0x80) {
                if (p + 2 > f.size()) break;
                stop = p + 2 + (uint8_t(f[p]) | (uint8_t(f[p + 1]) << 8));
                body = p + 2;
                if (stop > f.size()) break;
            }
            p = stop;
            if (op == 0x96) {
                for (size_t b = body; b < stop;) {
                    const uint8_t t = uint8_t(f[b++]);
                    Value v;
                    if (t == 0) {
                        const size_t z = f.find('\0', b);
                        if (z == std::string::npos || z >= stop) break;
                        v.text = f.substr(b, z - b), b = z + 1;
                    } else if (t == 1 && b + 4 <= stop) {
                        float x;
                        std::memcpy(&x, f.data() + b, 4), b += 4;
                        v.text = number(x);
                    } else if (t == 6 && b + 8 <= stop) {
                        uint32_t w[2];
                        std::memcpy(w, f.data() + b, 8), b += 8;
                        const uint64_t bits = (uint64_t(w[0]) << 32) | w[1];  // AVM1 doubles are word-swapped
                        double x;
                        std::memcpy(&x, &bits, 8);
                        v.text = number(x);
                    } else if (t == 7 && b + 4 <= stop) v.text = std::to_string(int32_t(U32(f, b))), b += 4;
                    else if (t == 8 && b < stop) v.text = uint8_t(f[b]) < pool.size() ? pool[uint8_t(f[b])] : "", b += 1;
                    else if (t == 9 && b + 2 <= stop) {
                        const size_t i = uint8_t(f[b]) | (uint8_t(f[b + 1]) << 8);
                        v.text = i < pool.size() ? pool[i] : "", b += 2;
                    } else if (t == 5 && b < stop) v.text = f[b] ? "true" : "false", b += 1;
                    else if (t == 4 && b < stop) v.kind = 4, b += 1;
                    else if (t == 2 || t == 3) v.kind = 4;
                    else break;
                    stack.push_back(std::move(v));
                }
            } else if (op == 0x1C) {  // GetVariable
                Value v = pop();
                v.kind = v.kind == 0 && v.text == "_global" ? 1 : 4;
                stack.push_back(std::move(v));
            } else if (op == 0x4E) {  // GetMember
                Value name = pop(), object = pop();
                Value v;
                v.kind = object.kind == 1 && wanted(name.text) ? 2 : 4;
                v.text = name.text;
                stack.push_back(std::move(v));
            } else if (op == 0x4F) {  // SetMember
                Value value = pop(), name = pop(), object = pop();
                if (object.kind == 1 && wanted(name.text)) {
                    arrays[name.text] = value.kind == 3 ? std::move(value.items) : std::vector<std::string>();
                } else if (object.kind == 2) {
                    int32_t i;
                    if (Int(name.text, i) && i >= 0 && i < 4096 && value.kind == 0) {
                        std::vector<std::string>& a = arrays[object.text];
                        if (a.size() <= size_t(i)) a.resize(size_t(i) + 1);
                        a[size_t(i)] = value.text;
                    }
                }
            } else if (op == 0x40 || op == 0x42) {  // NewObject(name, count, args) / InitArray(count, items)
                const Value name = op == 0x40 ? pop() : Value{0, "Array", {}};
                const Value count = pop();
                int32_t c = 0;
                Int(count.text, c);
                Value v;
                v.kind = name.text == "Array" ? 3 : 4;
                for (int32_t i = 0; i < c && !stack.empty(); i++) v.items.push_back(pop().text);
                int32_t length;  // new Array(n) is n empty entries
                if (op == 0x40 && c == 1 && v.items.size() == 1 && Int(v.items[0], length) && length >= 0 &&
                    length < 4096)
                    v.items.assign(size_t(length), std::string());
                stack.push_back(std::move(v));
            } else if (op == 0x17) {  // Pop
                pop();
            } else if (op == 0x1D) {  // SetVariable
                pop(), pop();
            } else if (op == 0x99 || op == 0x9D || op == 0x9E || op == 0x9B || op == 0x8E || op == 0x94 || op == 0x8F) {
                break;  // Jump, If, Call, DefineFunction(2), With, Try
            }
        }
    }
    return arrays;
}

bool ShowroomArray(const std::string& name) {
    return std::find_if(std::begin(kShowroom), std::end(kShowroom), [&](const char* s) { return name == s; }) !=
           std::end(kShowroom);
}

// The Showroom rows a movie sets, by car.
std::map<std::string, ShowroomRow> ShowroomOf(const std::string& f) {
    std::map<std::string, std::vector<std::string>> arrays = MovieArrays(f, "ShowRoomNames", ShowroomArray);
    std::map<std::string, ShowroomRow> out;
    const std::vector<std::string>& names = arrays[kShowroom[0]];
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i].empty()) continue;
        ShowroomRow row(kShowroomFields);
        for (size_t k = 0; k < kShowroomFields; k++) {
            const std::vector<std::string>& a = arrays[kShowroom[k + 1]];
            if (i < a.size()) row[k] = a[i];
        }
        out[Lower(names[i])] = std::move(row);
    }
    return out;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The Showroom arrays with the packs' cars, as statements to run after the movie's own; empty when nothing changes.
std::vector<Statement> ShowroomStatements(const std::string& rel, const std::vector<Statement>& original,
                                          std::vector<std::string>& dump) {
    std::vector<Statement> cols;
    for (const char* name : kShowroom) {
        auto it = std::find_if(original.rbegin(), original.rend(),
                               [&](const Statement& s) { return s.name == name && s.array; });
        if (it == original.rend() || (!cols.empty() && it->values.size() != cols[0].values.size())) {
            LOGE("MC3SHOWROOM %s: no %s array of the list's length; the Showroom is left as it is", rel.c_str(), name);
            return {};
        }
        cols.push_back(*it);
    }
    // each pack's own garage movies, the Xbox one last so it wins
    std::map<std::string, ShowroomRow> rows;
    static const char* kMovies[] = {"flash\\toplevel.pck", "flash\\toplevel.xbck", "flash\\toplevel_x.pck",
                                    "flash\\toplevel_x.xbck"};
    std::error_code ec;
    const bool add = Get().ShowroomPacks();  // else only the dump
    for (const fs::path& root : add ? Mc3CarPackRoots() : std::vector<fs::path>())
        for (const char* movie : kMovies) {
            const fs::path p = root / movie;
            if (!fs::is_regular_file(p, ec)) continue;
            const std::map<std::string, ShowroomRow> got = ShowroomOf(ReadFile(p));
            LOGE("MC3SHOWROOM %ls: %zu cars", p.c_str(), got.size());
            for (const auto& [car, row] : got) rows[car] = row;
        }
    auto fill = [](ShowroomRow& into, const ShowroomRow& from) {
        for (size_t k = 0; k < kShowroomFields; k++)
            if (!from[k].empty()) into[k] = from[k];
    };
    std::map<std::string, ShowroomRow> scripted;
    for (const auto& [car, row] : Get().Showroom()) {
        auto [it, added] = scripted.emplace(car, ShowroomRow(kShowroomFields));
        fill(it->second, row);
    }
    std::vector<std::string>& names = cols[0].values;
    auto row_at = [&](size_t i) {
        ShowroomRow r(kShowroomFields);
        for (size_t k = 0; k < kShowroomFields; k++) r[k] = cols[k + 1].values[i];
        return r;
    };
    auto set_at = [&](size_t i, const std::string& car, const ShowroomRow& r) {
        if (i == names.size())
            for (Statement& c : cols) c.values.emplace_back();
        names[i] = car;
        for (size_t k = 0; k < kShowroomFields; k++) cols[k + 1].values[i] = r[k];
    };
    bool changed = false;
    for (size_t i = 0; i < names.size(); i++) {  // scripted changes to cars on the list
        auto s = scripted.find(Lower(names[i]));
        if (s == scripted.end()) continue;
        ShowroomRow r = row_at(i);
        fill(r, s->second);
        set_at(i, names[i], r);
        changed = true;
    }
    const std::vector<std::string> vehicles = add ? VehicleNames() : std::vector<std::string>();
    std::set<std::string> listed;
    for (const std::string& n : names) listed.insert(Lower(n));
    size_t replaced = 0, appended = 0, moved = 0;
    const size_t placeholders = size_t(std::count_if(names.begin(), names.end(), Placeholder));
    for (size_t vi = 0; vi < vehicles.size(); vi++) {
        const std::string& car = vehicles[vi];
        if (car.empty() || listed.count(car) || Placeholder(car)) continue;
        auto pack = rows.find(car);
        auto s = scripted.find(car);
        if (pack == rows.end() && s == scripted.end()) {
            LOGE("MC3SHOWROOM %s: %s has no Showroom entry in its pack; give it a [showroom] row", rel.c_str(), car.c_str());
            continue;
        }
        ShowroomRow r = pack != rows.end() ? pack->second : ShowroomRow(kShowroomFields);
        if (s != scripted.end()) fill(r, s->second);
        size_t at = names.size();
        if (vi < names.size() && Placeholder(names[vi])) at = vi;
        else if (vi != names.size()) {
            auto free = std::find_if(names.begin(), names.end(), Placeholder);
            if (free != names.end()) at = size_t(free - names.begin());
            moved++;
            LOGE("MC3SHOWROOM %s: %s is vehicle %zu but goes to Showroom entry %zu", rel.c_str(), car.c_str(), vi, at);
        }
        if (at == names.size() && names.size() >= kShowroomMax) {
            LOGE("MC3SHOWROOM %s: %s left out, the Showroom holds %zu entries", rel.c_str(), car.c_str(), kShowroomMax);
            continue;
        }
        (at == names.size() ? appended : replaced)++;
        set_at(at, car, r);
        listed.insert(car);
        changed = true;
    }
    if (add && vehicles.empty()) LOGE("MC3SHOWROOM %s: the vehicle list is not loaded yet; no pack car added", rel.c_str());
    if (add) LOGE("MC3SHOWROOM %s: %zu vehicles; %zu pack cars on %zu placeholders, %zu appended (%zu off their vehicle index), "
         "%zu entries", rel.c_str(), vehicles.size(), replaced + appended, replaced, appended, moved, names.size());
    for (size_t i = 0; i < names.size(); i++) {
        if (Placeholder(names[i])) continue;
        std::string line = names[i] + " =";
        for (size_t k = 0; k < kShowroomFields; k++) line += (k ? ", " : " ") + Quote(cols[k + 1].values[i]);
        dump.push_back(std::move(line));
    }
    const size_t left = size_t(std::count_if(names.begin(), names.end(), Placeholder));
    if (rel == "flash\\toplevel_x.xbck") g_showroom_extra = int(placeholders - left);
    if (!changed) return {};
    return cols;
}

// --mc3-pack-shops: a car pack's shop movie for a car the game has its own movie for (a PS2 mod's ds_, ms_, ps_ or
// vs_ of a retail car) is not what the game runs, so its menus are read from it and run after the game's init action:
// every array of that init action the pack's movie ends up with different. The pack's movie (flash\<shop>_<car>.xbck
// or .pck, the last pack that has one) is read straight through like the Showroom's; plain values are left out (later
// blocks set runtime values, not menus). A new car's own pack movie is the one the game runs, and needs nothing.
bool AnyArray(const std::string&) { return true; }

std::vector<Statement> PackShopStatements(const std::string& rel, const std::string& running,
                                          const std::vector<Statement>& original) {
    const std::string stem = rel.substr(6, rel.rfind('.') - 6);  // "flash\ds_vp_x.xbck" -> ds_vp_x
    std::string movie;
    fs::path from;
    std::error_code ec;
    for (const fs::path& root : Mc3CarPackRoots())
        for (const char* ext : {".xbck", ".pck"}) {
            const fs::path p = root / "flash" / (stem + ext);
            if (fs::is_regular_file(p, ec)) from = p;
        }
    if (from.empty()) return {};
    movie = ReadFile(from);
    if (movie == running) return {};  // the game runs the pack's own movie
    const std::map<std::string, std::vector<std::string>> arrays = MovieArrays(movie, "_global", AnyArray);
    std::vector<Statement> out;
    std::set<std::string> seen;
    for (auto it = original.rbegin(); it != original.rend(); ++it) {
        if (!it->array || !seen.insert(it->name).second) continue;
        auto pack = arrays.find(it->name);
        if (pack == arrays.end() || pack->second == it->values) continue;
        Statement st = *it;
        st.values = pack->second;
        out.push_back(std::move(st));
    }
    std::reverse(out.begin(), out.end());
    LOGE("MC3GARAGE %s: %zu menu arrays from %ls", rel.c_str(), out.size(), from.c_str());
    return out;
}

// A shop movie's init action as the game opened it, and what the script runs after it.
struct Pending {
    std::string rel;     // the movie the game asked for (a new car's, when a stand-in runs for it)
    bool stand_in = false;
    std::string code;   // the movie's own actions, through ActionEnd
    std::string extra;  // sprite id, the script's statements, ActionEnd
    uint32_t guest = 0; // `extra` in guest memory, kept: pushed strings are referenced, not copied
    std::vector<Statement> status; // explicit status arrays, in script precedence order
};

std::mutex g_mutex;
std::vector<Pending> g_pending;
std::string g_active_rel;
// A new car's shop movie the game does not have is loaded under a stock car's name (sub_00139500): that name, and the
// movie it stands in for.
std::string g_stand_in, g_stand_in_for;

// The movie `rel` is opened for (g_mutex held).
const std::string& Requested(const std::string& rel) { return rel == g_stand_in ? g_stand_in_for : rel; }
std::atomic<bool> g_any{false};  // the interpreter runs every frame: nothing to look for until a movie is seen

}  // namespace

// For car_packs.cpp, which sees the files the game opens.
bool Mc3GarageScriptActive() { return Get().Active(); }

const std::u16string* Mc3GarageString(const std::string& key) { return Get().Text(key); }

bool Mc3GarageAlias(const std::string& name, std::string& target) { return Get().Alias(name, target); }

bool Mc3GarageHasAliases() { return Get().HasAliases(); }

// A movie whose pack copy is read for its menus (--mc3-showroom-packs: the garage movie; --mc3-pack-shops: a shop
// movie) rather than served, when the game has its own: car_packs.cpp then lets the game open its own.
bool Mc3GaragePackMovie(const std::string& rel) {
    std::string shop, car;
    if (!ShopAndCar(rel, shop, car)) return false;
    return shop == "toplevel" ? Get().ShowroomPacks() : Get().PackShops();
}

bool Mc3GarageScriptWants(const std::string& rel) {
    std::string shop, car;
    if (!Get().Active() || !ShopAndCar(rel, shop, car)) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active_rel = Requested(rel);
        ShopAndCar(g_active_rel, shop, car);
    }
    return Get().Wants(shop, car);
}

// `file` is the movie the game is opening for `rel` (lowercase, relative to the assets).
void Mc3GarageScriptSee(const std::string& opened, const std::string& file) {
    std::string shop, car;
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string rel = Requested(opened);  // a stand-in's statements are the new car's
    if (!ShopAndCar(rel, shop, car)) return;
    for (const Pending& p : g_pending)
        if (p.rel == rel) return;
    Action a;
    std::string error;
    if (!FindAction(file, a, error)) {
        LOGE("MC3GARAGE %s: %s, left as it is", rel.c_str(), error.c_str());
        return;
    }
    const std::vector<const Statement*> sts = Get().For(shop, car);
    std::vector<Statement> original;
    const bool decoded = Decode(file, a, original, error);
    std::vector<Statement> showroom, pack;
    if (decoded && shop != "toplevel" && Get().PackShops()) pack = PackShopStatements(rel, file, original);
    if (decoded && shop == "toplevel") {
        std::vector<std::string> rows;
        showroom = ShowroomStatements(rel, original, rows);
        if (Get().Dump()) WriteShowroom(car, rows);
    } else if (decoded) {
        if (Get().Dump()) WriteDump(shop, car, original);
    } else {
        LOGE("MC3GARAGE %s: not decoded (%s); no dump or automatic icon order", rel.c_str(), error.c_str());
    }
    if (sts.empty() && showroom.empty() && pack.empty()) return;
    Pending p;
    p.rel = rel;
    p.stand_in = rel != opened;
    p.code = file.substr(a.code, a.end + 1 - a.code);
    p.extra = file.substr(a.block, 2);
    for (const Statement& st : pack) p.extra += Compile(st);  // before the script's, so the script wins
    for (const Statement* st : sts) {
        Statement compiled = *st;
        if (shop == "vs" && compiled.array && compiled.name == "RimsStatus" && decoded) {
            for (const Statement& source : original) {
                if (source.name != compiled.name || !source.array || source.values.size() <= compiled.values.size()) continue;
                compiled.values.insert(compiled.values.end(), source.values.begin() + compiled.values.size(), source.values.end());
                break;
            }
        }
        p.extra += Compile(compiled);
        if (st->array && st->name.size() >= 6 && st->name.compare(st->name.size() - 6, 6, "Status") == 0)
            p.status.push_back(*st);
    }
    if (decoded)
        for (const Statement& st : DeriveOrders(original, sts, rel)) p.extra += Compile(st);
    for (const Statement& st : showroom) p.extra += Compile(st);
    p.extra += char(0);
    LOGE("MC3GARAGE %s: %zu statements from the script, %zu Showroom arrays, %zu pack menu arrays", rel.c_str(),
         sts.size(), showroom.size(), pack.size());
    g_pending.push_back(std::move(p));
    g_any = true;
}

// The movies' action interpreter (ecx = [sprite id][actions], instance, kind; ret 8). After a shop movie's init
// action, the script's statements run in the same place.
X86_EXTERN_FUNC(__imp__sub_00068BC0);

X86_FUNC(sub_00068BC0) {
    const uint32_t data = ctx.ecx, instance = X86_MEM32(ctx.esp + 4), kind = X86_MEM32(ctx.esp + 8);
    uint32_t extra = 0;
    if (g_any.load(std::memory_order_relaxed) && data >= 0x10000 && base[data + 2] == 0x88 && std::memcmp(base + data + 7, "_global", 8) == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        // A stand-in shares its init action with the stock car's own movie: it matches only while it is the movie
        // the game opened last; any other movie matches by its code alone.
        Pending* match = nullptr;
        for (int pass = 0; pass < 2 && !match; pass++)
            for (Pending& p : g_pending)
                if ((pass == 0 ? p.rel == g_active_rel : !p.stand_in) &&
                    std::memcmp(base + data + 2, p.code.data(), p.code.size()) == 0) {
                    match = &p;
                    break;
                }
        if (Pending* p = match) {
            g_active_rel = p->rel;
            if (!p->guest && (p->guest = mem::AllocSystem(uint32_t(p->extra.size()), 16)))
                std::memcpy(base + p->guest, p->extra.data(), p->extra.size());
            extra = p->guest;
        }
    }
    __imp__sub_00068BC0(ctx, base);
    if (!extra) return;
    const uint32_t eax = ctx.eax, esp = ctx.esp;
    X86_PUSH32(kind);
    X86_PUSH32(instance);
    X86_PUSH32(0);
    ctx.ecx = extra;
    __imp__sub_00068BC0(ctx, base);
    ctx.eax = eax, ctx.esp = esp;
}

// The colour menu recalculates every Status entry on each update (0x14C180), including entries supplied by the
// script. Preserve only explicitly scripted entries in that recalculation. Other writes, including the game's
// installed-state update, still run normally. No matching script means the original value is untouched.
X86_EXTERN_FUNC(__imp__sub_00139590);

X86_FUNC(sub_00139590) {
    const uint32_t sp = ctx.esp;
    if (g_any.load(std::memory_order_relaxed) && X86_MEM32(sp) == 0x0014C2A3) {
        const uint32_t name = X86_MEM32(sp + 4), index = X86_MEM32(sp + 8);
        std::string key;
        for (uint32_t i = 0; i < 128 && name >= 0x10000 && mem::IsCommitted(name + i); i++) {
            const char c = char(base[name + i]);
            if (!c) break;
            key += c;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Pending& p : g_pending) {
            if (p.rel != g_active_rel || p.rel.compare(0, 9, "flash\\ps_") != 0) continue;
            for (auto it = p.status.rbegin(); it != p.status.rend(); ++it) {
                if (it->name != key || index >= it->values.size()) continue;
                const std::string& text = it->values[index];
                char* end = nullptr;
                const long long n = std::strtoll(text.c_str(), &end, 10);
                if (end != text.c_str() && *end == '\0' && n >= 0 && n <= std::numeric_limits<int32_t>::max())
                    X86_MEM32(sp + 12) = uint32_t(n);
                break;
            }
            break;
        }
    }
    __imp__sub_00139590(ctx, base);
}

// 0x152420 sets XBliveCars (the Xbox Live car count, [0x557EAC]) on both garage movies through 0x139520 (movie
// wrapper, name, value). With --mc3-showroom-packs it is raised by the placeholders pack cars took (g_showroom_extra).
X86_EXTERN_FUNC(__imp__sub_00139520);

X86_FUNC(sub_00139520) {
    const uint32_t ret = X86_MEM32(ctx.esp);
    if ((ret == 0x00153128 || ret == 0x00153136) && g_showroom_extra.load(std::memory_order_relaxed))
        X86_MEM32(ctx.esp + 8) += uint32_t(g_showroom_extra.load(std::memory_order_relaxed));
    __imp__sub_00139520(ctx, base);
}

// The garage names the shop movie it loads with "<shop>_%s.swf" and the car (0x14E8E0 -> 0x139500, which copies the name
// into the movie wrapper). The Flash loader finds a movie only among the game's own: a new car of a car pack gets an empty
// shop with no menu, whatever is served for it. Its shops run the stock car's movie instead (with --mc3-pack-shops,
// carrying the menus of the car's own pack movie); log once per movie.
constexpr char kStandInCar[] = "vp_jetta_03";

X86_EXTERN_FUNC(__imp__sub_00139500);

X86_FUNC(sub_00139500) {
    const uint32_t name = X86_MEM32(ctx.esp + 4);
    std::string movie;
    for (uint32_t i = 0; i < 80 && name >= 0x10000 && mem::IsCommitted(name + i) && base[name + i]; i++)
        movie += char(std::tolower(base[name + i]));
    std::string shop, car;
    const size_t n = movie.size();
    const std::string rel = n > 4 && movie.compare(n - 4, 4, ".swf") == 0 ? "flash\\" + movie.substr(0, n - 4) + ".xbck" : "";
    if (!rel.empty() && ShopAndCar(rel, shop, car)) {
        const bool stand_in = shop != "toplevel" && car != kStandInCar && Mc3CarPackMissingShopMovie(ctx, base, rel);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (stand_in) {
            static uint32_t buffer = mem::AllocSystem(64, 16);
            const std::string sub = shop + "_" + kStandInCar + ".swf";
            std::memcpy(base + buffer, sub.c_str(), sub.size() + 1);
            X86_MEM32(ctx.esp + 4) = buffer;
            g_stand_in = "flash\\" + shop + "_" + kStandInCar + ".xbck";
            g_stand_in_for = g_active_rel = rel;
            static std::set<std::string> told;
            if (told.insert(rel).second) LOGE("MC3GARAGE %s: the game has no such movie; %s's runs for it", rel.c_str(), kStandInCar);
        } else {
            g_stand_in.clear(), g_stand_in_for.clear();
        }
    }
    __imp__sub_00139500(ctx, base);
}

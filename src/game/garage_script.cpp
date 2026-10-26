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
// does not come from the stream that open returns. Arrays the script does not name stay as the movie has them.
//
// --mc3-garage-dump writes every such movie's arrays, as script, to hdd\Z\carpacks\garage_dump\<shop>_<car>.txt, the
// starting point for editing one.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

std::vector<std::filesystem::path> Mc3CarPackRoots();  // car_packs.cpp

namespace {

namespace fs = std::filesystem;

constexpr char kDump[] = "\\Device\\Harddisk0\\Partition5\\carpacks\\garage_dump\\";

struct Statement {
    std::string name;
    bool array = false;
    std::vector<std::string> values;  // one for a plain value
};

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

std::vector<Section> Parse(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<Section> out;
    std::string line;
    uint32_t number = 0;
    while (std::getline(in, line)) {
        number++;
        if (number == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            std::istringstream words(line.substr(1, line.find(']') - 1));
            Section s;
            words >> s.car >> s.shop;
            s.car = Lower(s.car), s.shop = Lower(s.shop);
            if (s.shop != "ds" && s.shop != "ms" && s.shop != "ps" && s.shop != "vs") {
                LOGE("MC3GARAGE %ls:%u: a section is [<car> <shop>], shop one of ds ms ps vs", file.c_str(), number);
                continue;
            }
            out.push_back(std::move(s));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || out.empty()) {
            LOGE("MC3GARAGE %ls:%u: expected <name> = <value> inside a [<car> <shop>] section", file.c_str(), number);
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
            std::vector<Section> s = Parse(f);
            size_t n = 0;
            for (const Section& x : s) n += x.statements.size();
            LOGE("MC3GARAGE script %ls: %zu sections, %zu statements", f.c_str(), s.size(), n);
            for (Section& x : s) sections_.push_back(std::move(x));
        }
    }

    bool Active() const { return dump_ || !sections_.empty(); }

    bool Wants(const std::string& shop, const std::string& car) const {
        if (dump_) return true;
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
    std::vector<Section> sections_;
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

// The statements of an init action (ConstantPool, Push, GetVariable, NewObject, SetMember is all the shop movies use).
bool Decode(const std::string& f, const Action& a, std::vector<Statement>& out, std::string& error) {
    struct Value {
        std::string text;
        bool array = false;
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
                else if (t == 7) v.text = std::to_string(int32_t(U32(f, q))), q += 4;
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
            for (int i = 0, n = std::atoi(count.text.c_str()); i < n; i++) {
                Value e;
                if (!pop(e)) return error = "stack", false;
                arr.items.push_back(std::move(e.text));
            }
            stack.push_back(std::move(arr));
        } else if (op == 0x4F) {  // SetMember(object, name, value)
            Value value, name, object;
            if (!pop(value) || !pop(name) || !pop(object)) return error = "stack", false;
            Statement st;
            st.name = name.text;
            st.array = value.array;
            st.values = value.array ? std::move(value.items) : std::vector<std::string>{value.text};
            out.push_back(std::move(st));
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

// _global.<name> = <value> or new Array(<values>), as the movies write it: the first element is pushed last
std::string Compile(const Statement& st) {
    std::string code;
    Push(code, "_global");
    code += char(0x1C);
    Push(code, st.name);
    if (st.array) {
        for (auto it = st.values.rbegin(); it != st.values.rend(); ++it) Push(code, *it);
        const uint32_t n = uint32_t(st.values.size());
        code += char(0x96), code += char(5), code += char(0), code += char(7);
        code.append(reinterpret_cast<const char*>(&n), 4);
        Push(code, "Array");
        code += char(0x40);
    } else {
        Push(code, st.values.empty() ? std::string() : st.values[0]);
    }
    code += char(0x4F);
    return code;
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

// "flash\ms_vp_350z_04.xbck" -> ms, vp_350z_04
bool ShopAndCar(const std::string& rel, std::string& shop, std::string& car) {
    static const char kPrefix[] = "flash\\";
    const size_t n = sizeof kPrefix - 1;
    if (rel.compare(0, n, kPrefix) != 0 || rel.size() < n + 4 || rel[n + 2] != '_') return false;
    shop = rel.substr(n, 2);
    if (shop != "ds" && shop != "ms" && shop != "ps" && shop != "vs") return false;
    const size_t dot = rel.rfind('.');
    if (dot == std::string::npos || rel.compare(dot, std::string::npos, ".xbck") != 0) return false;
    car = rel.substr(n + 3, dot - n - 3);
    return true;
}

// A shop movie's init action as the game opened it, and what the script runs after it.
struct Pending {
    std::string rel;
    std::string code;   // the movie's own actions, through ActionEnd
    std::string extra;  // sprite id, the script's statements, ActionEnd
    uint32_t guest = 0; // `extra` in guest memory, kept: pushed strings are referenced, not copied
};

std::mutex g_mutex;
std::vector<Pending> g_pending;
std::atomic<bool> g_any{false};  // the interpreter runs every frame: nothing to look for until a movie is seen

}  // namespace

// For car_packs.cpp, which sees the files the game opens.
bool Mc3GarageScriptActive() { return Get().Active(); }

bool Mc3GarageScriptWants(const std::string& rel) {
    std::string shop, car;
    return Get().Active() && ShopAndCar(rel, shop, car) && Get().Wants(shop, car);
}

// `file` is the movie the game is opening for `rel` (lowercase, relative to the assets).
void Mc3GarageScriptSee(const std::string& rel, const std::string& file) {
    std::string shop, car;
    if (!ShopAndCar(rel, shop, car)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Pending& p : g_pending)
        if (p.rel == rel) return;
    Action a;
    std::string error;
    if (!FindAction(file, a, error)) {
        LOGE("MC3GARAGE %s: %s, left as it is", rel.c_str(), error.c_str());
        return;
    }
    if (Get().Dump()) {
        std::vector<Statement> sts;
        if (Decode(file, a, sts, error)) WriteDump(shop, car, sts);
        else LOGE("MC3GARAGE %s: not dumped (%s)", rel.c_str(), error.c_str());
    }
    const std::vector<const Statement*> sts = Get().For(shop, car);
    if (sts.empty()) return;
    Pending p;
    p.rel = rel;
    p.code = file.substr(a.code, a.end + 1 - a.code);
    p.extra = file.substr(a.block, 2);
    for (const Statement* st : sts) p.extra += Compile(*st);
    p.extra += char(0);
    LOGE("MC3GARAGE %s: %zu statements from the script", rel.c_str(), sts.size());
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
        for (Pending& p : g_pending)
            if (std::memcmp(base + data + 2, p.code.data(), p.code.size()) == 0) {
                if (!p.guest && (p.guest = mem::AllocSystem(uint32_t(p.extra.size()), 16)))
                    std::memcpy(base + p.guest, p.extra.data(), p.extra.size());
                extra = p.guest;
                break;
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

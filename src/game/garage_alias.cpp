// Opt-in garage aliases: resolve a script name to the retail action and part.
// The menu continues to see the alias so its label and price remain independent.

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/debug_tools.h"
#include "kernel/memory.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

bool Mc3GarageAlias(const std::string& name, std::string& target);  // garage_script.cpp
bool Mc3GarageHasAliases();

X86_EXTERN_FUNC(__imp__sub_00146B40);  // garage name -> action id, thiscall, ret 4
X86_EXTERN_FUNC(__imp__sub_002730B0);  // vehicle SetPart(type, name, ...), thiscall, ret 16

namespace {

// Targets live in guest memory because the original functions expect guest pointers. Keep them alive for the whole
// process: the game's own implementations may keep a string pointer beyond the immediate call.
uint32_t Target(uint8_t* base, uint32_t name) {
    // Avoid even initializing the script or constructing a host string on the default path.
    static const bool enabled = Mc3DebugHasArg(L"--mc3-garage-script");
    if (!enabled || !Mc3GarageHasAliases() || name < 0x10000) return 0;
    std::string target;
    if (!Mc3GarageAlias(reinterpret_cast<const char*>(base + name), target)) return 0;
    static std::mutex mutex;
    static std::map<std::string, uint32_t> guests;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = guests.find(target);
    if (found != guests.end()) return found->second;
    const uint32_t guest = mem::AllocSystem(uint32_t(target.size() + 1), 16);
    if (!guest) {
        LOGE("MC3GARAGE: could not allocate alias target");
        return 0;
    }
    std::memcpy(base + guest, target.c_str(), target.size() + 1);
    guests.emplace(target, guest);
    return guest;
}

}  // namespace

// Only substitute the argument while the original runs; leave both the saved stack slot and its callee cleanup intact.
X86_FUNC(sub_00146B40) {
    const uint32_t slot = ctx.esp + 4;
    const uint32_t name = X86_MEM32(slot);
    const uint32_t target = Target(base, name);
    if (target) X86_MEM32(slot) = target;
    __imp__sub_00146B40(ctx, base);
    if (target) X86_MEM32(slot) = name;
}

// The selected Names entry is fetched again at installation, independently of the action id. Resolve it here as well:
// the stock function looks up the real part index and saves that index, rather than writing the alias into the save.
X86_FUNC(sub_002730B0) {
    const uint32_t slot = ctx.esp + 8;
    const uint32_t name = X86_MEM32(slot);
    const uint32_t target = Target(base, name);
    if (target) X86_MEM32(slot) = target;
    __imp__sub_002730B0(ctx, base);
    if (target) X86_MEM32(slot) = name;
}

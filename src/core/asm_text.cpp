#include "asm_text.hpp"

#include <windows.h>

#include <asmjit/x86.h>
#include <asmtk/asmtk.h>

#include <cstring>
#include <mutex>
#include <set>

namespace vanacore::asm_text {
namespace {

// Caves are AsmJit's own executable memory, as Ashita's are: made and freed by its runtime.
asmjit::JitRuntime& runtime()
{
    static asmjit::JitRuntime instance;
    return instance;
}

std::mutex g_lock; // the caves made, and the runtime
std::set<std::uintptr_t> g_caves;

// `text` parsed into `code` (initialised for `base`): false with AsmJit's or AsmTK's message in `error`.
bool parse(const std::string& text, asmjit::CodeHolder& code, std::uint64_t base, std::string& error)
{
    asmjit::Error result = code.init(runtime().environment(), runtime().cpu_features(), base);
    asmjit::x86::Assembler assembler;
    if (result == asmjit::kErrorOk)
        result = code.attach(&assembler);
    if (result == asmjit::kErrorOk) {
        asmtk::AsmParser parser(&assembler);
        result = parser.parse(text.c_str(), text.size());
    }
    if (result != asmjit::kErrorOk) {
        error = asmjit::DebugUtils::error_as_string(result);
        return false;
    }
    return true;
}

// Writes `size` bytes at `address`, the page made writable for it, and has the processor see them as code.
void write_code(std::uintptr_t address, const void* data, std::size_t size)
{
    auto* at = reinterpret_cast<void*>(address);
    DWORD protect = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &protect))
        return;
    std::memcpy(at, data, size);
    VirtualProtect(at, size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, size);
}

} // namespace

bool assemble(const std::string& text, std::uint32_t base, std::vector<std::uint8_t>& out, std::string& error)
{
    const std::lock_guard lock(g_lock);
    asmjit::CodeHolder code;
    if (!parse(text, code, base, error))
        return false;
    asmjit::Error result = code.flatten();
    if (result == asmjit::kErrorOk)
        result = code.resolve_cross_section_fixups();
    if (result == asmjit::kErrorOk)
        result = code.relocate_to_base(base);
    if (result == asmjit::kErrorOk) {
        out.assign(code.code_size(), 0);
        result = code.copy_flattened_data(out.data(), out.size(), asmjit::CopySectionFlags::kPadTargetBuffer);
    }
    if (result != asmjit::kErrorOk) {
        error = asmjit::DebugUtils::error_as_string(result);
        return false;
    }
    return true;
}

std::uintptr_t cave(const std::string& text, std::string& error)
{
    const std::lock_guard lock(g_lock);
    asmjit::CodeHolder code;
    if (!parse(text, code, asmjit::Globals::kNoBaseAddress, error))
        return 0;
    void* function = nullptr;
    const asmjit::Error result = runtime()._add(&function, &code); // relocated to where it's put
    if (result != asmjit::kErrorOk || function == nullptr) {
        error = asmjit::DebugUtils::error_as_string(result);
        return 0;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(function);
    g_caves.insert(address);
    return address;
}

bool release(std::uintptr_t address)
{
    const std::lock_guard lock(g_lock);
    if (g_caves.erase(address) == 0)
        return false;
    return runtime()._release(reinterpret_cast<void*>(address)) == asmjit::kErrorOk;
}

void branch(std::uintptr_t address, std::uint8_t opcode, std::uintptr_t target, std::uint32_t nops)
{
    if (address == 0 || target == 0)
        return;
    std::uint8_t bytes[5] = {opcode};
    const auto distance = static_cast<std::int32_t>(target - (address + 5));
    std::memcpy(bytes + 1, &distance, 4);
    write_code(address, bytes, sizeof(bytes));
    nop(address + 5, nops);
}

void nop(std::uintptr_t address, std::uint32_t count)
{
    if (address == 0 || count == 0)
        return;
    const std::vector<std::uint8_t> nops(count, 0x90);
    write_code(address, nops.data(), nops.size());
}

} // namespace vanacore::asm_text

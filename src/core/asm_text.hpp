#pragma once

#include <cstdint>
#include <string>
#include <vector>

// x86 machine code for Ashita addons' ashita.memory.assembler. Ashita 4.30 assembles text with AsmJit, its syntax read
// by AsmTK (Intel's: "push eax; mov eax, [0x10012345]; call 0x10023456"), so the same here (third_party\asmjit,
// third_party\asmtk): what an addon's code cave says assembles as it does in Ashita. call, jmp and nop write their bytes
// as Ashita's do.
namespace vanacore::asm_text {

// The machine code `text` assembles to, encoded to run at `base` (relative calls and jumps to absolute addresses are
// counted from there). False with what's wrong in `error`.
bool assemble(const std::string& text, std::uint32_t base, std::vector<std::uint8_t>& code, std::string& error);

// `text` assembled into executable memory of its own (a code cave, encoded for its address): the address, or 0 with
// what's wrong in `error`.
std::uintptr_t cave(const std::string& text, std::string& error);

// Frees a cave cave() made. False if `address` isn't one.
bool release(std::uintptr_t address);

// Writes `opcode` (0xE8 call, 0xE9 jmp) and a rel32 to `target` at `address`, then `nops` NOPs after it; nothing when
// either address is 0. The page is made writable for it.
void branch(std::uintptr_t address, std::uint8_t opcode, std::uintptr_t target, std::uint32_t nops);

// Writes `count` NOPs (0x90) at `address`; nothing when either is 0.
void nop(std::uintptr_t address, std::uint32_t count);

} // namespace vanacore::asm_text

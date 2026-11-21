#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// A 32-bit x86 disassembler, Intel syntax, for the MCP server's disassemble and find_references
// (mcp_tools.cpp): what FFXiMain.dll and its neighbours are made of - the integer instructions, x87,
// MMX and SSE/SSE2. Anything else (16-bit addressing, system instructions) is reported as not decoded
// and shown as a byte.
namespace vanacore::disasm {

struct Instruction {
    std::uint32_t address = 0;
    unsigned length = 0;
    std::string text; // "mov eax, dword ptr [ecx+0x10]"

    bool call = false;        // call
    bool jump = false;        // jmp, jcc, loop, jecxz
    bool conditional = false; // jcc, loop, jecxz
    bool ret = false;         // ret, retf, iretd
    bool has_target = false;  // a direct call or jump: where it goes
    std::uint32_t target = 0;
    bool has_absolute = false; // a memory operand at a fixed address ([0x10ABCDEF], or a table: [ecx*4+0x10ABCDEF])
    std::uint32_t absolute = 0;
    bool has_immediate = false; // a 32-bit immediate (push offset ..., mov reg, offset ...)
    std::uint32_t immediate = 0;
};

// Decodes the instruction at `code` (at most `size` bytes readable there), as if it were at
// `address`. False if it isn't one this knows, or runs past `size`.
bool decode(const std::uint8_t* code, std::size_t size, std::uint32_t address, Instruction& out);

} // namespace vanacore::disasm

#include "disasm.hpp"

#include <cstdio>
#include <initializer_list>

namespace vanacore::disasm {
namespace {

const char* const kReg32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
const char* const kReg16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
const char* const kReg8[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
const char* const kSegment[] = {"es", "cs", "ss", "ds", "fs", "gs"};
const char* const kCondition[] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};
const char* const kAlu[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
const char* const kShift[] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};

std::string hex(std::uint32_t value)
{
    char text[16];
    if (value < 10)
        std::snprintf(text, sizeof text, "%u", value);
    else
        std::snprintf(text, sizeof text, "0x%X", value);
    return text;
}

std::string signed_hex(std::int32_t value)
{
    if (value < 0)
        return "-" + hex(static_cast<std::uint32_t>(-static_cast<std::int64_t>(value)));
    return "+" + hex(static_cast<std::uint32_t>(value));
}

const char* size_ptr(unsigned bytes)
{
    switch (bytes) {
    case 1: return "byte ptr ";
    case 2: return "word ptr ";
    case 4: return "dword ptr ";
    case 6: return "fword ptr ";
    case 8: return "qword ptr ";
    case 10: return "tbyte ptr ";
    case 16: return "xmmword ptr ";
    default: return "";
    }
}

struct Decoder {
    const std::uint8_t* code;
    std::size_t size;
    std::uint32_t address;
    Instruction& out;

    std::size_t at = 0;
    bool ok = true;
    bool opsize = false, adsize = false, lock = false, rep = false, repne = false;
    int segment = -1;

    bool have_modrm = false;
    unsigned mod = 0, reg = 0, rm = 0;
    std::string mem; // the memory operand, "[...]" with any segment, without its size

    std::uint8_t u8()
    {
        if (at >= size) {
            ok = false;
            return 0;
        }
        return code[at++];
    }
    std::uint16_t u16()
    {
        const std::uint16_t low = u8();
        const std::uint16_t high = u8();
        return static_cast<std::uint16_t>(low | (high << 8));
    }
    std::uint32_t u32()
    {
        const std::uint32_t low = u16();
        const std::uint32_t high = u16();
        return low | (high << 16);
    }

    void modrm()
    {
        if (have_modrm)
            return;
        have_modrm = true;
        const std::uint8_t m = u8();
        mod = m >> 6;
        reg = (m >> 3) & 7;
        rm = m & 7;
        if (mod != 3)
            memory();
    }

    void memory()
    {
        if (adsize)
            ok = false; // 16-bit addressing: not in the game's code (data, decoded by mistake)
        std::string base, index;
        unsigned scale = 1;
        std::int32_t disp = 0;
        bool has_disp = false, disp32 = false;
        if (rm == 4) {
            const std::uint8_t sib = u8();
            scale = 1u << (sib >> 6);
            const unsigned i = (sib >> 3) & 7, b = sib & 7;
            if (i != 4)
                index = kReg32[i];
            if (b == 5 && mod == 0) {
                disp = static_cast<std::int32_t>(u32());
                has_disp = disp32 = true;
            } else {
                base = kReg32[b];
            }
        } else if (rm == 5 && mod == 0) {
            disp = static_cast<std::int32_t>(u32());
            has_disp = disp32 = true;
        } else {
            base = kReg32[rm];
        }
        if (mod == 1) {
            disp = static_cast<std::int8_t>(u8());
            has_disp = true;
        } else if (mod == 2) {
            disp = static_cast<std::int32_t>(u32());
            has_disp = true;
        }
        std::string text;
        if (segment >= 0)
            text = std::string(kSegment[segment]) + ":";
        text += "[";
        text += base;
        if (!index.empty()) {
            if (!base.empty())
                text += "+";
            text += index;
            if (scale > 1)
                text += "*" + std::to_string(scale);
        }
        const auto fixed = static_cast<std::uint32_t>(disp);
        if (base.empty() && disp32 && (index.empty() || (fixed >= 0x10000 && fixed < 0xFFFF0000u))) {
            // a fixed address: a global, or a table indexed by a register
            if (!index.empty())
                text += "+";
            text += hex(fixed);
            out.has_absolute = true;
            out.absolute = fixed;
        } else if (has_disp && disp != 0) {
            text += signed_hex(disp);
        }
        text += "]";
        mem = text;
    }

    // Operands, as Intel's opcode tables name them
    std::string E(unsigned bytes)
    {
        modrm();
        if (mod == 3)
            return bytes == 1 ? kReg8[rm] : bytes == 2 ? kReg16[rm] : kReg32[rm];
        return size_ptr(bytes) + mem;
    }
    unsigned v() const { return opsize ? 2u : 4u; }
    std::string Eb() { return E(1); }
    std::string Ev() { return E(v()); }
    std::string Ew() { return E(2); }
    std::string Ed() { return E(4); }
    std::string G(unsigned bytes)
    {
        modrm();
        return bytes == 1 ? kReg8[reg] : bytes == 2 ? kReg16[reg] : kReg32[reg];
    }
    std::string Gb() { return G(1); }
    std::string Gv() { return G(v()); }
    std::string Gd() { return G(4); }
    // memory only
    std::string M(unsigned bytes)
    {
        modrm();
        if (mod == 3)
            ok = false;
        return size_ptr(bytes) + mem;
    }
    std::string Sw()
    {
        modrm();
        if (reg > 5)
            ok = false;
        return reg <= 5 ? kSegment[reg] : "?";
    }
    std::string Ib() { return hex(u8()); }
    std::string Iw() { return hex(u16()); }
    // an imm8 sign-extended to the operand size
    std::string Ibs()
    {
        const std::int32_t value = static_cast<std::int8_t>(u8());
        return hex(opsize ? static_cast<std::uint16_t>(value) : static_cast<std::uint32_t>(value));
    }
    std::string Iz()
    {
        if (opsize)
            return hex(u16());
        const std::uint32_t value = u32();
        out.has_immediate = true;
        out.immediate = value;
        return hex(value);
    }
    std::string Jb()
    {
        const std::int32_t rel = static_cast<std::int8_t>(u8());
        return branch(rel);
    }
    std::string Jz()
    {
        const std::int32_t rel = static_cast<std::int32_t>(u32());
        return branch(rel);
    }
    std::string branch(std::int32_t rel)
    {
        out.has_target = true;
        out.target = address + static_cast<std::uint32_t>(at) + static_cast<std::uint32_t>(rel);
        return hex(out.target);
    }
    std::string reg_v(unsigned r) const { return opsize ? kReg16[r] : kReg32[r]; }
    std::string accumulator() const { return opsize ? "ax" : "eax"; }
    // a moffs: mov al/eax to or from a fixed address
    std::string O(unsigned bytes)
    {
        const std::uint32_t where = u32();
        out.has_absolute = true;
        out.absolute = where;
        std::string text = size_ptr(bytes);
        if (segment >= 0)
            text += std::string(kSegment[segment]) + ":";
        return text + "[" + hex(where) + "]";
    }

    // SSE and MMX registers
    std::string V() { modrm(); return "xmm" + std::to_string(reg); }
    std::string W(unsigned bytes) { modrm(); return mod == 3 ? "xmm" + std::to_string(rm) : size_ptr(bytes) + mem; }
    std::string P() { modrm(); return "mm" + std::to_string(reg); }
    std::string Q() { modrm(); return mod == 3 ? "mm" + std::to_string(rm) : size_ptr(8) + mem; }

    bool op(const char* name, std::initializer_list<std::string> operands = {})
    {
        std::string text;
        if (lock)
            text = "lock ";
        text += name;
        bool first = true;
        for (const std::string& operand : operands) {
            text += first ? " " : ", ";
            text += operand;
            first = false;
        }
        out.text = text;
        return ok;
    }

    // A string instruction, with its rep prefix
    bool string_op(const char* name, bool compares)
    {
        std::string text;
        if (rep)
            text = compares ? "repe " : "rep ";
        else if (repne)
            text = "repne ";
        text += name;
        out.text = text;
        return ok;
    }

    bool run();
    bool two_byte();
    bool sse(std::uint8_t op);
    bool x87(std::uint8_t op);
};

bool Decoder::run()
{
    for (;;) {
        if (at >= size)
            return false;
        const std::uint8_t p = code[at];
        if (p == 0xF0) lock = true;
        else if (p == 0xF2) repne = true;
        else if (p == 0xF3) rep = true;
        else if (p == 0x26) segment = 0;
        else if (p == 0x2E) segment = 1;
        else if (p == 0x36) segment = 2;
        else if (p == 0x3E) segment = 3;
        else if (p == 0x64) segment = 4;
        else if (p == 0x65) segment = 5;
        else if (p == 0x66) opsize = true;
        else if (p == 0x67) adsize = true;
        else break;
        if (++at > 14)
            return false;
    }
    const std::uint8_t op = u8();

    if (op < 0x40 && (op & 7) < 6) {
        const char* name = kAlu[op >> 3];
        switch (op & 7) {
        case 0: return this->op(name, {Eb(), Gb()});
        case 1: return this->op(name, {Ev(), Gv()});
        case 2: return this->op(name, {Gb(), Eb()});
        case 3: return this->op(name, {Gv(), Ev()});
        case 4: return this->op(name, {"al", Ib()});
        default: return this->op(name, {accumulator(), Iz()});
        }
    }
    if (op >= 0x40 && op <= 0x5F) {
        static const char* const kNames[] = {"inc", "dec", "push", "pop"};
        return this->op(kNames[(op - 0x40) >> 3], {reg_v(op & 7)});
    }
    if (op >= 0x70 && op <= 0x7F) {
        out.jump = out.conditional = true;
        const std::string name = std::string("j") + kCondition[op & 15];
        return this->op(name.c_str(), {Jb()});
    }
    if (op >= 0x91 && op <= 0x97)
        return this->op("xchg", {reg_v(op & 7), accumulator()});
    if (op >= 0xB0 && op <= 0xB7)
        return this->op("mov", {kReg8[op & 7], Ib()});
    if (op >= 0xB8 && op <= 0xBF)
        return this->op("mov", {reg_v(op & 7), Iz()});
    if (op >= 0xD8 && op <= 0xDF)
        return x87(op);

    switch (op) {
    case 0x06: return this->op("push", {"es"});
    case 0x07: return this->op("pop", {"es"});
    case 0x0E: return this->op("push", {"cs"});
    case 0x0F: return two_byte();
    case 0x16: return this->op("push", {"ss"});
    case 0x17: return this->op("pop", {"ss"});
    case 0x1E: return this->op("push", {"ds"});
    case 0x1F: return this->op("pop", {"ds"});
    case 0x27: return this->op("daa");
    case 0x2F: return this->op("das");
    case 0x37: return this->op("aaa");
    case 0x3F: return this->op("aas");
    case 0x60: return this->op(opsize ? "pusha" : "pushad");
    case 0x61: return this->op(opsize ? "popa" : "popad");
    case 0x62: return this->op("bound", {Gv(), M(8)});
    case 0x63: return this->op("arpl", {Ew(), G(2)});
    case 0x68: return this->op("push", {Iz()});
    case 0x69: return this->op("imul", {Gv(), Ev(), Iz()});
    case 0x6A: return this->op("push", {Ibs()});
    case 0x6B: return this->op("imul", {Gv(), Ev(), Ibs()});
    case 0x6C: return string_op("insb", false);
    case 0x6D: return string_op(opsize ? "insw" : "insd", false);
    case 0x6E: return string_op("outsb", false);
    case 0x6F: return string_op(opsize ? "outsw" : "outsd", false);
    case 0x80:
    case 0x82: { modrm(); const char* name = kAlu[reg]; return this->op(name, {Eb(), Ib()}); }
    case 0x81: { modrm(); const char* name = kAlu[reg]; return this->op(name, {Ev(), Iz()}); }
    case 0x83: { modrm(); const char* name = kAlu[reg]; return this->op(name, {Ev(), Ibs()}); }
    case 0x84: return this->op("test", {Eb(), Gb()});
    case 0x85: return this->op("test", {Ev(), Gv()});
    case 0x86: return this->op("xchg", {Eb(), Gb()});
    case 0x87: return this->op("xchg", {Ev(), Gv()});
    case 0x88: return this->op("mov", {Eb(), Gb()});
    case 0x89: return this->op("mov", {Ev(), Gv()});
    case 0x8A: return this->op("mov", {Gb(), Eb()});
    case 0x8B: return this->op("mov", {Gv(), Ev()});
    case 0x8C: return this->op("mov", {Ew(), Sw()});
    case 0x8D: return this->op("lea", {Gv(), M(0)});
    case 0x8E: return this->op("mov", {Sw(), Ew()});
    case 0x8F: modrm(); if (reg != 0) return false; return this->op("pop", {Ev()});
    case 0x90: return this->op(rep ? "pause" : "nop");
    case 0x98: return this->op(opsize ? "cbw" : "cwde");
    case 0x99: return this->op(opsize ? "cwd" : "cdq");
    case 0x9A: { const std::uint32_t offset = u32(); const std::uint16_t selector = u16();
                 out.call = true; return this->op("call", {hex(selector) + ":" + hex(offset)}); }
    case 0x9B: return this->op("fwait");
    case 0x9C: return this->op(opsize ? "pushf" : "pushfd");
    case 0x9D: return this->op(opsize ? "popf" : "popfd");
    case 0x9E: return this->op("sahf");
    case 0x9F: return this->op("lahf");
    case 0xA0: return this->op("mov", {"al", O(1)});
    case 0xA1: return this->op("mov", {accumulator(), O(v())});
    case 0xA2: return this->op("mov", {O(1), "al"});
    case 0xA3: return this->op("mov", {O(v()), accumulator()});
    case 0xA4: return string_op("movsb", false);
    case 0xA5: return string_op(opsize ? "movsw" : "movsd", false);
    case 0xA6: return string_op("cmpsb", true);
    case 0xA7: return string_op(opsize ? "cmpsw" : "cmpsd", true);
    case 0xA8: return this->op("test", {"al", Ib()});
    case 0xA9: return this->op("test", {accumulator(), Iz()});
    case 0xAA: return string_op("stosb", false);
    case 0xAB: return string_op(opsize ? "stosw" : "stosd", false);
    case 0xAC: return string_op("lodsb", false);
    case 0xAD: return string_op(opsize ? "lodsw" : "lodsd", false);
    case 0xAE: return string_op("scasb", true);
    case 0xAF: return string_op(opsize ? "scasw" : "scasd", true);
    case 0xC0: { modrm(); const char* name = kShift[reg]; return this->op(name, {Eb(), Ib()}); }
    case 0xC1: { modrm(); const char* name = kShift[reg]; return this->op(name, {Ev(), Ib()}); }
    case 0xC2: out.ret = true; return this->op("ret", {Iw()});
    case 0xC3: out.ret = true; return this->op(rep ? "rep ret" : "ret");
    case 0xC4: return this->op("les", {Gv(), M(6)});
    case 0xC5: return this->op("lds", {Gv(), M(6)});
    case 0xC6: modrm(); if (reg != 0) return false; return this->op("mov", {Eb(), Ib()});
    case 0xC7: modrm(); if (reg != 0) return false; return this->op("mov", {Ev(), Iz()});
    case 0xC8: return this->op("enter", {Iw(), Ib()});
    case 0xC9: return this->op("leave");
    case 0xCA: out.ret = true; return this->op("retf", {Iw()});
    case 0xCB: out.ret = true; return this->op("retf");
    case 0xCC: return this->op("int3");
    case 0xCD: return this->op("int", {Ib()});
    case 0xCE: return this->op("into");
    case 0xCF: out.ret = true; return this->op("iretd");
    case 0xD0: { modrm(); const char* name = kShift[reg]; return this->op(name, {Eb(), "1"}); }
    case 0xD1: { modrm(); const char* name = kShift[reg]; return this->op(name, {Ev(), "1"}); }
    case 0xD2: { modrm(); const char* name = kShift[reg]; return this->op(name, {Eb(), "cl"}); }
    case 0xD3: { modrm(); const char* name = kShift[reg]; return this->op(name, {Ev(), "cl"}); }
    case 0xD4: return this->op("aam", {Ib()});
    case 0xD5: return this->op("aad", {Ib()});
    case 0xD6: return this->op("salc");
    case 0xD7: return this->op("xlatb");
    case 0xE0: out.jump = out.conditional = true; return this->op("loopne", {Jb()});
    case 0xE1: out.jump = out.conditional = true; return this->op("loope", {Jb()});
    case 0xE2: out.jump = out.conditional = true; return this->op("loop", {Jb()});
    case 0xE3: out.jump = out.conditional = true; return this->op("jecxz", {Jb()});
    case 0xE4: return this->op("in", {"al", Ib()});
    case 0xE5: return this->op("in", {accumulator(), Ib()});
    case 0xE6: return this->op("out", {Ib(), "al"});
    case 0xE7: return this->op("out", {Ib(), accumulator()});
    case 0xE8: out.call = true; return this->op("call", {Jz()});
    case 0xE9: out.jump = true; return this->op("jmp", {Jz()});
    case 0xEA: { const std::uint32_t offset = u32(); const std::uint16_t selector = u16();
                 out.jump = true; return this->op("jmp", {hex(selector) + ":" + hex(offset)}); }
    case 0xEB: out.jump = true; return this->op("jmp", {Jb()});
    case 0xEC: return this->op("in", {"al", "dx"});
    case 0xED: return this->op("in", {accumulator(), "dx"});
    case 0xEE: return this->op("out", {"dx", "al"});
    case 0xEF: return this->op("out", {"dx", accumulator()});
    case 0xF1: return this->op("int1");
    case 0xF4: return this->op("hlt");
    case 0xF5: return this->op("cmc");
    case 0xF6:
    case 0xF7: {
        modrm();
        static const char* const kNames[] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};
        const bool byte = op == 0xF6;
        const std::string operand = byte ? Eb() : Ev();
        if (reg < 2)
            return this->op("test", {operand, byte ? Ib() : Iz()});
        return this->op(kNames[reg], {operand});
    }
    case 0xF8: return this->op("clc");
    case 0xF9: return this->op("stc");
    case 0xFA: return this->op("cli");
    case 0xFB: return this->op("sti");
    case 0xFC: return this->op("cld");
    case 0xFD: return this->op("std");
    case 0xFE:
        modrm();
        if (reg > 1)
            return false;
        return this->op(reg == 0 ? "inc" : "dec", {Eb()});
    case 0xFF:
        modrm();
        switch (reg) {
        case 0: return this->op("inc", {Ev()});
        case 1: return this->op("dec", {Ev()});
        case 2: out.call = true; return this->op("call", {Ed()});
        case 3: out.call = true; return this->op("call", {M(6)});
        case 4: out.jump = true; return this->op("jmp", {Ed()});
        case 5: out.jump = true; return this->op("jmp", {M(6)});
        case 6: return this->op("push", {Ev()});
        default: return false;
        }
    default:
        return false;
    }
}

bool Decoder::two_byte()
{
    const std::uint8_t op = u8();
    if (op >= 0x40 && op <= 0x4F) {
        const std::string name = std::string("cmov") + kCondition[op & 15];
        return this->op(name.c_str(), {Gv(), Ev()});
    }
    if (op >= 0x80 && op <= 0x8F) {
        out.jump = out.conditional = true;
        const std::string name = std::string("j") + kCondition[op & 15];
        return this->op(name.c_str(), {Jz()});
    }
    if (op >= 0x90 && op <= 0x9F) {
        const std::string name = std::string("set") + kCondition[op & 15];
        return this->op(name.c_str(), {Eb()});
    }
    if (op >= 0xC8 && op <= 0xCF)
        return this->op("bswap", {kReg32[op & 7]});
    switch (op) {
    case 0x00: {
        modrm();
        static const char* const kNames[] = {"sldt", "str", "lldt", "ltr", "verr", "verw"};
        if (reg > 5)
            return false;
        return this->op(kNames[reg], {Ew()});
    }
    case 0x01: {
        modrm();
        if (mod == 3) {
            const unsigned byte = 0xC0 | (reg << 3) | rm;
            if (byte == 0xD0) return this->op("xgetbv");
            if (byte == 0xF9) return this->op("rdtscp");
            return false;
        }
        static const char* const kNames[] = {"sgdt", "sidt", "lgdt", "lidt", "smsw", nullptr, "lmsw", "invlpg"};
        if (kNames[reg] == nullptr)
            return false;
        return this->op(kNames[reg], {M(reg == 4 || reg == 6 ? 2 : reg == 7 ? 1 : 6)});
    }
    case 0x05: return this->op("syscall");
    case 0x06: return this->op("clts");
    case 0x08: return this->op("invd");
    case 0x09: return this->op("wbinvd");
    case 0x0B: return this->op("ud2");
    case 0x0D: modrm(); return this->op(reg == 1 ? "prefetchw" : "prefetch", {M(1)});
    case 0x0E: return this->op("femms");
    case 0x0F: {
        // AMD's 3DNow!: the operation is the byte after the operands (FFXiMain has an AMD path)
        const std::string a = P(), b = Q();
        const std::uint8_t which = u8();
        static const struct { std::uint8_t code; const char* name; } k3DNow[] = {
            {0x0C, "pi2fw"}, {0x0D, "pi2fd"}, {0x1C, "pf2iw"}, {0x1D, "pf2id"}, {0x8A, "pfnacc"}, {0x8E, "pfpnacc"},
            {0x90, "pfcmpge"}, {0x94, "pfmin"}, {0x96, "pfrcp"}, {0x97, "pfrsqrt"}, {0x9A, "pfsub"}, {0x9E, "pfadd"},
            {0xA0, "pfcmpgt"}, {0xA4, "pfmax"}, {0xA6, "pfrcpit1"}, {0xA7, "pfrsqit1"}, {0xAA, "pfsubr"}, {0xAE, "pfacc"},
            {0xB0, "pfcmpeq"}, {0xB4, "pfmul"}, {0xB6, "pfrcpit2"}, {0xB7, "pmulhrw"}, {0xBB, "pswapd"}, {0xBF, "pavgusb"}};
        for (const auto& entry : k3DNow)
            if (entry.code == which)
                return this->op(entry.name, {a, b});
        return false;
    }
    case 0x20: modrm(); if (mod != 3) return false; return this->op("mov", {kReg32[rm], "cr" + std::to_string(reg)});
    case 0x21: modrm(); if (mod != 3) return false; return this->op("mov", {kReg32[rm], "dr" + std::to_string(reg)});
    case 0x22: modrm(); if (mod != 3) return false; return this->op("mov", {"cr" + std::to_string(reg), kReg32[rm]});
    case 0x23: modrm(); if (mod != 3) return false; return this->op("mov", {"dr" + std::to_string(reg), kReg32[rm]});
    case 0x18: {
        modrm();
        static const char* const kNames[] = {"prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2"};
        if (reg > 3)
            return this->op("nop", {Ev()});
        return this->op(kNames[reg], {M(1)});
    }
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        return this->op("nop", {Ev()});
    case 0x30: return this->op("wrmsr");
    case 0x31: return this->op("rdtsc");
    case 0x32: return this->op("rdmsr");
    case 0x33: return this->op("rdpmc");
    case 0x34: return this->op("sysenter");
    case 0x35: return this->op("sysexit");
    case 0xA0: return this->op("push", {"fs"});
    case 0xA1: return this->op("pop", {"fs"});
    case 0xA2: return this->op("cpuid");
    case 0xA3: return this->op("bt", {Ev(), Gv()});
    case 0xA4: return this->op("shld", {Ev(), Gv(), Ib()});
    case 0xA5: return this->op("shld", {Ev(), Gv(), "cl"});
    case 0xA8: return this->op("push", {"gs"});
    case 0xA9: return this->op("pop", {"gs"});
    case 0xAB: return this->op("bts", {Ev(), Gv()});
    case 0xAC: return this->op("shrd", {Ev(), Gv(), Ib()});
    case 0xAD: return this->op("shrd", {Ev(), Gv(), "cl"});
    case 0xAE: {
        modrm();
        if (mod == 3) {
            if (reg == 5) return this->op("lfence");
            if (reg == 6) return this->op("mfence");
            if (reg == 7) return this->op("sfence");
            return false;
        }
        switch (reg) {
        case 0: return this->op("fxsave", {M(0)});
        case 1: return this->op("fxrstor", {M(0)});
        case 2: return this->op("ldmxcsr", {M(4)});
        case 3: return this->op("stmxcsr", {M(4)});
        case 7: return this->op("clflush", {M(1)});
        default: return false;
        }
    }
    case 0xAF: return this->op("imul", {Gv(), Ev()});
    case 0xB0: return this->op("cmpxchg", {Eb(), Gb()});
    case 0xB1: return this->op("cmpxchg", {Ev(), Gv()});
    case 0xB3: return this->op("btr", {Ev(), Gv()});
    case 0xB6: return this->op("movzx", {Gv(), Eb()});
    case 0xB7: return this->op("movzx", {Gv(), Ew()});
    case 0xB8: if (!rep) return false; rep = false; return this->op("popcnt", {Gv(), Ev()});
    case 0xBA: {
        modrm();
        static const char* const kNames[] = {"bt", "bts", "btr", "btc"};
        if (reg < 4)
            return false;
        return this->op(kNames[reg - 4], {Ev(), Ib()});
    }
    case 0xBB: return this->op("btc", {Ev(), Gv()});
    case 0xBC: if (rep) { rep = false; return this->op("tzcnt", {Gv(), Ev()}); } return this->op("bsf", {Gv(), Ev()});
    case 0xBD: if (rep) { rep = false; return this->op("lzcnt", {Gv(), Ev()}); } return this->op("bsr", {Gv(), Ev()});
    case 0xBE: return this->op("movsx", {Gv(), Eb()});
    case 0xBF: return this->op("movsx", {Gv(), Ew()});
    case 0xC0: return this->op("xadd", {Eb(), Gb()});
    case 0xC1: return this->op("xadd", {Ev(), Gv()});
    case 0xC7: modrm(); if (reg != 1) return false; return this->op("cmpxchg8b", {M(8)});
    default:
        return sse(op);
    }
}

// 0F xx with SSE's mandatory prefixes: none (ps / MMX), 66 (pd / SSE2 integer), F3 (ss), F2 (sd).
bool Decoder::sse(std::uint8_t op)
{
    enum { None, Op66, F3, F2 } kind = rep ? F3 : repne ? F2 : opsize ? Op66 : None;
    // the prefix is the instruction's, not an operand size or a repeat
    const auto suffix = [&](const char* stem, bool scalar_too = true) {
        static thread_local std::string name;
        name = stem;
        switch (kind) {
        case None: name += "ps"; break;
        case Op66: name += "pd"; break;
        case F3: if (!scalar_too) ok = false; name += "ss"; break;
        case F2: if (!scalar_too) ok = false; name += "sd"; break;
        }
        return name.c_str();
    };
    const unsigned packed = 16;
    const unsigned scalar = kind == F3 ? 4u : kind == F2 ? 8u : packed;
    switch (op) {
    case 0x10:
    case 0x11: {
        const char* name = kind == F3 ? "movss" : kind == F2 ? "movsd" : kind == Op66 ? "movupd" : "movups";
        if (op == 0x10)
            return this->op(name, {V(), W(scalar)});
        return this->op(name, {W(scalar), V()});
    }
    case 0x12:
        modrm();
        if (kind == None) return mod == 3 ? this->op("movhlps", {V(), W(16)}) : this->op("movlps", {V(), M(8)});
        if (kind == Op66) return this->op("movlpd", {V(), M(8)});
        return false;
    case 0x13: return this->op(kind == Op66 ? "movlpd" : "movlps", {M(8), V()});
    case 0x14: return this->op(kind == Op66 ? "unpcklpd" : "unpcklps", {V(), W(16)});
    case 0x15: return this->op(kind == Op66 ? "unpckhpd" : "unpckhps", {V(), W(16)});
    case 0x16:
        modrm();
        if (kind == None) return mod == 3 ? this->op("movlhps", {V(), W(16)}) : this->op("movhps", {V(), M(8)});
        if (kind == Op66) return this->op("movhpd", {V(), M(8)});
        return false;
    case 0x17: return this->op(kind == Op66 ? "movhpd" : "movhps", {M(8), V()});
    case 0x28: return this->op(kind == Op66 ? "movapd" : "movaps", {V(), W(16)});
    case 0x29: return this->op(kind == Op66 ? "movapd" : "movaps", {W(16), V()});
    case 0x2A:
        if (kind == F3) return this->op("cvtsi2ss", {V(), Ed()});
        if (kind == F2) return this->op("cvtsi2sd", {V(), Ed()});
        return this->op(kind == Op66 ? "cvtpi2pd" : "cvtpi2ps", {V(), Q()});
    case 0x2B: return this->op(kind == Op66 ? "movntpd" : "movntps", {M(16), V()});
    case 0x2C:
    case 0x2D: {
        const bool truncate = op == 0x2C;
        if (kind == F3) return this->op(truncate ? "cvttss2si" : "cvtss2si", {Gd(), W(4)});
        if (kind == F2) return this->op(truncate ? "cvttsd2si" : "cvtsd2si", {Gd(), W(8)});
        if (kind == Op66) return this->op(truncate ? "cvttpd2pi" : "cvtpd2pi", {P(), W(16)});
        return this->op(truncate ? "cvttps2pi" : "cvtps2pi", {P(), W(8)});
    }
    case 0x2E: return this->op(kind == Op66 ? "ucomisd" : "ucomiss", {V(), W(kind == Op66 ? 8 : 4)});
    case 0x2F: return this->op(kind == Op66 ? "comisd" : "comiss", {V(), W(kind == Op66 ? 8 : 4)});
    case 0x50: modrm(); if (mod != 3) return false; return this->op(kind == Op66 ? "movmskpd" : "movmskps", {Gd(), W(16)});
    case 0x51: return this->op(suffix("sqrt"), {V(), W(scalar)});
    case 0x52: return this->op(suffix("rsqrt"), {V(), W(scalar)});
    case 0x53: return this->op(suffix("rcp"), {V(), W(scalar)});
    case 0x54: return this->op(suffix("and", false), {V(), W(16)});
    case 0x55: return this->op(suffix("andn", false), {V(), W(16)});
    case 0x56: return this->op(suffix("or", false), {V(), W(16)});
    case 0x57: return this->op(suffix("xor", false), {V(), W(16)});
    case 0x58: return this->op(suffix("add"), {V(), W(scalar)});
    case 0x59: return this->op(suffix("mul"), {V(), W(scalar)});
    case 0x5A:
        if (kind == F3) return this->op("cvtss2sd", {V(), W(4)});
        if (kind == F2) return this->op("cvtsd2ss", {V(), W(8)});
        if (kind == Op66) return this->op("cvtpd2ps", {V(), W(16)});
        return this->op("cvtps2pd", {V(), W(8)});
    case 0x5B:
        if (kind == F2) return false;
        return this->op(kind == F3 ? "cvttps2dq" : kind == Op66 ? "cvtps2dq" : "cvtdq2ps", {V(), W(16)});
    case 0x5C: return this->op(suffix("sub"), {V(), W(scalar)});
    case 0x5D: return this->op(suffix("min"), {V(), W(scalar)});
    case 0x5E: return this->op(suffix("div"), {V(), W(scalar)});
    case 0x5F: return this->op(suffix("max"), {V(), W(scalar)});
    case 0xC2: {
        // the predicate's own name when it's one of the eight: cmpltps, cmpneqss, ...
        static const char* const kPredicates[] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"};
        const std::string a = V(), b = W(scalar);
        const std::uint8_t predicate = u8();
        if (predicate < 8) {
            const std::string name = std::string("cmp") + kPredicates[predicate] + suffix("");
            return this->op(name.c_str(), {a, b});
        }
        return this->op(suffix("cmp"), {a, b, hex(predicate)});
    }
    case 0xC6: return this->op(kind == Op66 ? "shufpd" : "shufps", {V(), W(16), Ib()});
    default: break;
    }

    // MMX, and SSE2's integer forms of it with 66
    const bool xmm = kind == Op66;
    if (kind == F2 && op != 0x70)
        return false;
    const auto mmx = [&](const char* name) {
        if (kind == F3)
            return false;
        if (xmm)
            return this->op(name, {V(), W(16)});
        return this->op(name, {P(), Q()});
    };
    switch (op) {
    case 0x6E: return xmm ? this->op("movd", {V(), Ed()}) : this->op("movd", {P(), Ed()});
    case 0x7E:
        if (kind == F3) return this->op("movq", {V(), W(8)});
        return xmm ? this->op("movd", {Ed(), V()}) : this->op("movd", {Ed(), P()});
    case 0x6F:
        if (kind == F3) return this->op("movdqu", {V(), W(16)});
        return xmm ? this->op("movdqa", {V(), W(16)}) : this->op("movq", {P(), Q()});
    case 0x7F:
        if (kind == F3) return this->op("movdqu", {W(16), V()});
        return xmm ? this->op("movdqa", {W(16), V()}) : this->op("movq", {Q(), P()});
    case 0x70:
        if (kind == F3) return this->op("pshufhw", {V(), W(16), Ib()});
        if (kind == F2) return this->op("pshuflw", {V(), W(16), Ib()});
        return xmm ? this->op("pshufd", {V(), W(16), Ib()}) : this->op("pshufw", {P(), Q(), Ib()});
    case 0x71:
    case 0x72:
    case 0x73: {
        modrm();
        if (mod != 3 || kind == F3)
            return false;
        const char width = op == 0x71 ? 'w' : op == 0x72 ? 'd' : 'q';
        std::string name;
        if (reg == 2) name = std::string("psrl") + width;
        else if (reg == 4 && op != 0x73) name = std::string("psra") + width;
        else if (reg == 6) name = std::string("psll") + width;
        else if (reg == 3 && op == 0x73 && xmm) name = "psrldq";
        else if (reg == 7 && op == 0x73 && xmm) name = "pslldq";
        else return false;
        const std::string target = xmm ? "xmm" + std::to_string(rm) : "mm" + std::to_string(rm);
        return this->op(name.c_str(), {target, Ib()});
    }
    case 0x77: return this->op("emms");
    case 0xD6: if (!xmm) return false; return this->op("movq", {W(8), V()});
    case 0xD7: modrm(); if (mod != 3) return false;
        return this->op("pmovmskb", {Gd(), xmm ? "xmm" + std::to_string(rm) : "mm" + std::to_string(rm)});
    case 0xE7: return xmm ? this->op("movntdq", {M(16), V()}) : this->op("movntq", {M(8), P()});
    case 0xF7: return xmm ? this->op("maskmovdqu", {V(), W(16)}) : this->op("maskmovq", {P(), Q()});
    default: break;
    }
    static const char* const k60[] = {"punpcklbw", "punpcklwd", "punpckldq", "packsswb", "pcmpgtb", "pcmpgtw", "pcmpgtd", "packuswb",
                                      "punpckhbw", "punpckhwd", "punpckhdq", "packssdw", "punpcklqdq", "punpckhqdq"};
    if (op >= 0x60 && op <= 0x6D) {
        if (op >= 0x6C && !xmm)
            return false;
        if (op <= 0x62 && !xmm && kind != F3) { // MMX's low unpacks read 4 bytes
            modrm();
            return this->op(k60[op - 0x60], {P(), mod == 3 ? "mm" + std::to_string(rm) : size_ptr(4) + mem});
        }
        return mmx(k60[op - 0x60]);
    }
    if (op >= 0x74 && op <= 0x76) {
        static const char* const kNames[] = {"pcmpeqb", "pcmpeqw", "pcmpeqd"};
        return mmx(kNames[op - 0x74]);
    }
    static const char* const kD0[] = {nullptr,   "psrlw",  "psrld",   "psrlq",   "paddq",   "pmullw", nullptr,  nullptr,
                                      "psubusb", "psubusw", "pminub", "pand",    "paddusb", "paddusw", "pmaxub", "pandn",
                                      "pavgb",   "psraw",  "psrad",   "pavgw",   "pmulhuw", "pmulhw", nullptr,  nullptr,
                                      "psubsb",  "psubsw", "pminsw",  "por",     "paddsb",  "paddsw", "pmaxsw", "pxor",
                                      nullptr,   "psllw",  "pslld",   "psllq",   "pmuludq", "pmaddwd", "psadbw", nullptr,
                                      "psubb",   "psubw",  "psubd",   "psubq",   "paddb",   "paddw",  "paddd",  nullptr};
    if (op >= 0xD0 && kD0[op - 0xD0] != nullptr)
        return mmx(kD0[op - 0xD0]);
    return false;
}

// x87: D8-DF
bool Decoder::x87(std::uint8_t op)
{
    modrm();
    const unsigned group = op - 0xD8u;
    const std::string st_i = "st(" + std::to_string(rm) + ")";
    if (mod != 3) {
        struct Form { const char* name; unsigned bytes; };
        static const Form kMemory[8][8] = {
            {{"fadd", 4}, {"fmul", 4}, {"fcom", 4}, {"fcomp", 4}, {"fsub", 4}, {"fsubr", 4}, {"fdiv", 4}, {"fdivr", 4}},
            {{"fld", 4}, {nullptr, 0}, {"fst", 4}, {"fstp", 4}, {"fldenv", 0}, {"fldcw", 2}, {"fnstenv", 0}, {"fnstcw", 2}},
            {{"fiadd", 4}, {"fimul", 4}, {"ficom", 4}, {"ficomp", 4}, {"fisub", 4}, {"fisubr", 4}, {"fidiv", 4}, {"fidivr", 4}},
            {{"fild", 4}, {"fisttp", 4}, {"fist", 4}, {"fistp", 4}, {nullptr, 0}, {"fld", 10}, {nullptr, 0}, {"fstp", 10}},
            {{"fadd", 8}, {"fmul", 8}, {"fcom", 8}, {"fcomp", 8}, {"fsub", 8}, {"fsubr", 8}, {"fdiv", 8}, {"fdivr", 8}},
            {{"fld", 8}, {"fisttp", 8}, {"fst", 8}, {"fstp", 8}, {"frstor", 0}, {nullptr, 0}, {"fnsave", 0}, {"fnstsw", 2}},
            {{"fiadd", 2}, {"fimul", 2}, {"ficom", 2}, {"ficomp", 2}, {"fisub", 2}, {"fisubr", 2}, {"fidiv", 2}, {"fidivr", 2}},
            {{"fild", 2}, {"fisttp", 2}, {"fist", 2}, {"fistp", 2}, {"fbld", 10}, {"fild", 8}, {"fbstp", 10}, {"fistp", 8}},
        };
        const Form& form = kMemory[group][reg];
        if (form.name == nullptr)
            return false;
        return this->op(form.name, {size_ptr(form.bytes) + mem});
    }
    switch (group) {
    case 0: {
        static const char* const kNames[] = {"fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"};
        if (reg == 2 || reg == 3)
            return this->op(kNames[reg], {st_i});
        return this->op(kNames[reg], {"st", st_i});
    }
    case 1: {
        if (reg == 0) return this->op("fld", {st_i});
        if (reg == 1) return this->op("fxch", {st_i});
        static const char* const kD9[] = {
            "fnop", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,   // D0
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // D8
            "fchs", "fabs", nullptr, nullptr, "ftst", "fxam", nullptr, nullptr,       // E0
            "fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz", nullptr, // E8
            "f2xm1", "fyl2x", "fptan", "fpatan", "fxtract", "fprem1", "fdecstp", "fincstp", // F0
            "fprem", "fyl2xp1", "fsqrt", "fsincos", "frndint", "fscale", "fsin", "fcos",    // F8
        };
        const unsigned index = (reg - 2) * 8 + rm;
        if (kD9[index] == nullptr)
            return false;
        return this->op(kD9[index]);
    }
    case 2: {
        static const char* const kNames[] = {"fcmovb", "fcmove", "fcmovbe", "fcmovu"};
        if (reg < 4)
            return this->op(kNames[reg], {"st", st_i});
        if (reg == 5 && rm == 1)
            return this->op("fucompp");
        return false;
    }
    case 3: {
        static const char* const kNames[] = {"fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"};
        if (reg < 4)
            return this->op(kNames[reg], {"st", st_i});
        if (reg == 4 && rm == 2) return this->op("fnclex");
        if (reg == 4 && rm == 3) return this->op("fninit");
        if (reg == 5) return this->op("fucomi", {"st", st_i});
        if (reg == 6) return this->op("fcomi", {"st", st_i});
        return false;
    }
    case 4: {
        static const char* const kNames[] = {"fadd", "fmul", "fcom", "fcomp", "fsubr", "fsub", "fdivr", "fdiv"};
        if (reg == 2 || reg == 3)
            return this->op(kNames[reg], {st_i});
        return this->op(kNames[reg], {st_i, "st"});
    }
    case 5: {
        static const char* const kNames[] = {"ffree", "fxch", "fst", "fstp", "fucom", "fucomp", nullptr, nullptr};
        if (kNames[reg] == nullptr)
            return false;
        return this->op(kNames[reg], {st_i});
    }
    case 6: {
        if (reg == 3) {
            if (rm != 1)
                return false;
            return this->op("fcompp");
        }
        static const char* const kNames[] = {"faddp", "fmulp", "fcomp", nullptr, "fsubrp", "fsubp", "fdivrp", "fdivp"};
        return this->op(kNames[reg], {st_i, "st"});
    }
    default: { // DF
        if (reg == 4 && rm == 0) return this->op("fnstsw", {"ax"});
        if (reg == 5) return this->op("fucomip", {"st", st_i});
        if (reg == 6) return this->op("fcomip", {"st", st_i});
        if (reg == 0) return this->op("ffreep", {st_i});
        return false;
    }
    }
}

} // namespace

bool decode(const std::uint8_t* code, std::size_t size, std::uint32_t address, Instruction& out)
{
    out = Instruction{};
    out.address = address;
    if (code == nullptr || size == 0)
        return false;
    Decoder decoder{code, size < 15 ? size : 15, address, out};
    if (!decoder.run() || !decoder.ok) {
        out = Instruction{};
        out.address = address;
        return false;
    }
    out.length = static_cast<unsigned>(decoder.at);
    return true;
}

} // namespace vanacore::disasm

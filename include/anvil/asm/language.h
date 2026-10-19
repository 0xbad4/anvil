#pragma once

#include <cstdint>

namespace anvil::s {
    enum class mnemonics : uint8_t {
        LD, LDI, LDH, LDB, LDX, LDXI, LDXB, ST, STX,
        ADD, SUB, MUL, DIV, MOD, NEG, AND, OR, XOR, LSH, RSH,
        JA, JMP,                                 // `ja` and `jmp`
        JEQ, JGT, JGE, JSET,
        JNE, JLT, JLE,                       // pseudo-ops
        RET, TAX, TXA
    };

    enum class extension : uint8_t {
        Len, Proto, Type, Poff, Ifidx, Nla, Nlan, Mark, Queue,
        Hatype, Rxhash, Cpu, VlanTci, VlanAvail, VlanTpid, Rand
    };

    enum class addr_mode : uint8_t {
        RegX = 0,
        PktAbs = 1,
        PktInd = 2,
        Scratch = 3,
        Imm = 4,
        Msh = 5,
        Label = 6,
        CondImmTF = 7,
        CondXTF = 8,
        CondImmT = 9,
        CondXT = 10,
        RegA = 11,
        Ext = 12,
        None = 13
    };
}
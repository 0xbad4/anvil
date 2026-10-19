#pragma once

#include <variant>
#include <optional>
#include <unordered_map>
#include "types.h"
#include "language.h"


namespace anvil::s {
    struct symbol {
        token  tok;
        size_t address;
    };

    using symtab_t = std::unordered_map<std::string, symbol>;

    namespace ops {
        struct no_operand {};                   // neg, tax, txa
        struct reg_x      {};                   //  0   x / %x
        struct pkt_abs    { int64_t k; };       //  1   [k]
        struct pkt_ind    { int64_t k; };       //  2   [x + k]
        struct scratch    { int64_t k; };       //  3   M[k]
        struct imm        { int64_t k; };       //  4   #k
        struct msh        { int64_t k; };       //  5   4*([k]&0xf)   (4 and 0xf are fixed)
        struct jmp_label  { token   label; };   //  6   L
        struct reg_a      {};                   // 11   a / %a
        struct ext        { extension kind; };  // 12   extension
        struct jmp_cond {
            std::variant<imm, reg_x> rhs;       //  #k  |  x
            token                    lt;
            std::optional<token>     lf;
        };
    } // namespace operand

    using operand = std::variant<ops::no_operand, 
                                ops::reg_x,
                                ops::reg_a,
                                ops::pkt_abs,
                                ops::pkt_ind,
                                ops::scratch,
                                ops::imm,
                                ops::msh,
                                ops::jmp_label,
                                ops::jmp_cond,
                                ops::ext>;


    struct instruction {
        mnemonics mnemonic;
        operand   ops;
        token     tok;
    };

    using instructions_t = std::vector<instruction>;

}
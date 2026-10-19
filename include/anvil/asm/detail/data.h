#pragma once

#include <string>
#include <unordered_map>
#include "../language.h"

namespace anvil::s::data
{
    inline const std::unordered_map<std::string, mnemonics> MNEMONICS = {
        {"ld", mnemonics::LD},
        {"ldi", mnemonics::LDI},
        {"ldh", mnemonics::LDH},
        {"ldb", mnemonics::LDB},
        {"ldx", mnemonics::LDX},
        {"ldxi", mnemonics::LDXI},
        {"ldxb", mnemonics::LDXB},
        {"st", mnemonics::ST},
        {"stx", mnemonics::STX},
        {"ja", mnemonics::JA},
        {"jmp", mnemonics::JMP},
        {"jeq", mnemonics::JEQ},
        {"jne", mnemonics::JNE},
        {"jneq", mnemonics::JNE},
        {"jlt", mnemonics::JLT},
        {"jle", mnemonics::JLE},
        {"jgt", mnemonics::JGT},
        {"jge", mnemonics::JGE},
        {"jset", mnemonics::JSET},
        {"add", mnemonics::ADD},
        {"sub", mnemonics::SUB},
        {"mul", mnemonics::MUL},
        {"div", mnemonics::DIV},
        {"mod", mnemonics::MOD},
        {"neg", mnemonics::NEG},
        {"and", mnemonics::AND},
        {"or", mnemonics::OR},
        {"xor", mnemonics::XOR},
        {"lsh", mnemonics::LSH},
        {"rsh", mnemonics::RSH},
        {"tax", mnemonics::TAX},
        {"txa", mnemonics::TXA},
        {"ret", mnemonics::RET},
    };

    inline const std::unordered_map<std::string, extension> EXTENSIONS = {
        {"len", extension::Len},
        {"proto", extension::Proto},
        {"type", extension::Type},
        {"poff", extension::Poff},
        {"ifidx", extension::Ifidx},
        {"nla", extension::Nla},
        {"nlan", extension::Nlan},
        {"mark", extension::Mark},
        {"queue", extension::Queue},
        {"hatype", extension::Hatype},
        {"rxhash", extension::Rxhash},
        {"cpu", extension::Cpu},
        {"vlan_tci", extension::VlanTci},
        {"vlan_avail", extension::VlanAvail},
        {"vlan_tpid", extension::VlanTpid},
        {"rand", extension::Rand}};

    inline constexpr const char *MODES_NAMES[] = {
        "x", "[k]", "[x+k]", "M[k]", "#k", "4*([k]&0xf)", "label",
        "#k,Lt,Lf", "x,Lt,Lf", "#k,Lt", "x,Lt", "a", "extension", "no operand"};

} // namespace anvil

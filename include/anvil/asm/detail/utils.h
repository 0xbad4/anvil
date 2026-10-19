#pragma once

#include <cstdint>
#include <string>
#include <charconv>
#include <cctype>
#include "data.h"
#include "modes.h"

namespace anvil::s::utils
{
    // "#42", "#-42", "-2", "0x10", "255"  ->  value
    inline bool to_int(const std::string& text, int64_t& out) noexcept {
        const char* p   = text.data();
        const char* end = p + text.size();

        if (p != end && *p == '#') {
            p++;
        }

        bool neg = false;

        if (p != end && *p == '-') {
            neg = true;
            p++;
        }

        int base = 10;

        if (end - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            base = 16;
        }

        uint64_t mag = 0;
        auto [ptr, ec] = std::from_chars(p, end, mag, base);
        if (ec != std::errc{} || ptr != end || mag > static_cast<uint64_t>(INT64_MAX)) {
            return false;
        }

        out = neg ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
        return true;
    }

    inline const char *describe_mode(addr_mode m) {
        return data::MODES_NAMES[static_cast<size_t>(m)];
    }

    inline std::string list_modes(uint16_t mask) {
        std::string out;
        for (unsigned i = 0; i <= static_cast<unsigned>(addr_mode::None); ++i) {
            if (!(mask & (1u << i))) {
                continue;
            }

            if (!out.empty()) {
                out += "  |  ";
            }

            out += describe_mode(static_cast<addr_mode>(i));
        }
        return out;
    }


    inline bool is_digit(char ch) noexcept {
        return std::isdigit(ch);
    }
    
    inline bool is_alpha(char ch) noexcept {
        return std::isalpha(ch) || ch == '_';
    }
    
    inline bool is_almun(char ch) noexcept {
        return is_alpha(ch) || is_digit(ch);
    }

    inline constexpr uint16_t allowed_modes(mnemonics mn) {
        using namespace detail;

        switch (mn) {
            case mnemonics::LD:
                return M_ABS | M_IND | M_MEM | M_IMM | M_EXT;
            case mnemonics::LDI:
                return M_IMM;
            case mnemonics::LDH:
            case mnemonics::LDB:
                return M_ABS | M_IND;
            case mnemonics::LDX:
                return M_MEM | M_IMM | M_MSH | M_EXT;
            case mnemonics::LDXI:
                return M_IMM;
            case mnemonics::LDXB:
                return M_MSH | M_ABS;
            case mnemonics::ST:
            case mnemonics::STX:
                return M_MEM;
            case mnemonics::ADD:
            case mnemonics::SUB:
            case mnemonics::MUL:
            case mnemonics::DIV:
            case mnemonics::MOD:
            case mnemonics::AND:
            case mnemonics::OR:
            case mnemonics::XOR:
            case mnemonics::LSH:
            case mnemonics::RSH:
                return M_X | M_IMM;
            case mnemonics::NEG:
            case mnemonics::TAX:
            case mnemonics::TXA:
                return M_NONE;
            case mnemonics::JA:
            case mnemonics::JMP:
                return M_LBL;
            case mnemonics::JEQ:
            case mnemonics::JGT:
            case mnemonics::JGE:
            case mnemonics::JSET:
                return M_TF_I | M_TF_X | M_T_I | M_T_X;
            case mnemonics::JNE:
            case mnemonics::JLT:
            case mnemonics::JLE:
                return M_T_I | M_T_X;
            case mnemonics::RET:
                return M_IMM | M_A;
        }

        return 0;
    }

} // namespace anvil::s::utils

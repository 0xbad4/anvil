#pragma once

#include <cstdint>
#include "../language.h"

namespace anvil::s::detail
{
    constexpr uint16_t bit(addr_mode m) { return static_cast<uint16_t>(1u << static_cast<unsigned>(m)); }

    constexpr uint16_t M_X    = bit(addr_mode::RegX);
    constexpr uint16_t M_ABS  = bit(addr_mode::PktAbs);
    constexpr uint16_t M_IND  = bit(addr_mode::PktInd);
    constexpr uint16_t M_MEM  = bit(addr_mode::Scratch);
    constexpr uint16_t M_IMM  = bit(addr_mode::Imm);
    constexpr uint16_t M_MSH  = bit(addr_mode::Msh);
    constexpr uint16_t M_LBL  = bit(addr_mode::Label);
    constexpr uint16_t M_TF_I = bit(addr_mode::CondImmTF);
    constexpr uint16_t M_TF_X = bit(addr_mode::CondXTF);
    constexpr uint16_t M_T_I  = bit(addr_mode::CondImmT);
    constexpr uint16_t M_T_X  = bit(addr_mode::CondXT);
    constexpr uint16_t M_A    = bit(addr_mode::RegA);
    constexpr uint16_t M_EXT  = bit(addr_mode::Ext);
    constexpr uint16_t M_NONE = bit(addr_mode::None);
}
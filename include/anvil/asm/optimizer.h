#pragma once

#include "validator.h"

namespace anvil::s {
    class Optimizer : public BaseObj {
        public:
            Optimizer(const instructions_t& ins, const symtab_t& st, profile p = profile::SOCKET_FILTER) noexcept 
                    : instructions(ins), symtab(st), m_profile(p) {}

            bool optimize() noexcept {
            
            }

        protected:
            // TODO: see if better to alter original instructions instead of creating new container
            instructions_t optimized_ins; // optimized instructions
            const instructions_t& instructions;  // original instructions
            const symtab_t& symtab;
            profile m_profile;
    };
    
} // namespace anvil:s

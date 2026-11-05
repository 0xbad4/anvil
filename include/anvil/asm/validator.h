#pragma once

#include <algorithm>
#include <unordered_set>
#include <vector>
#include "parser.h"

namespace anvil::s
{
    #define vholds(X, Y) std::holds_alternative<X>(Y)
    #define vget(X, Y) std::get<X>(Y)

    class Validator : public BaseObj {
        public:
            Validator(const instructions_t& ins, const symtab_t& st, profile p = profile::SOCKET_FILTER) noexcept 
                    : instructions(ins), symtab(st), m_profile(p), m_address(0), m_flow_ok(true) {}

            bool validate() noexcept {
                if (!validate_program()) {
                    return false;
                }

                for (m_address = 0; m_address < instructions.size(); ++m_address) {
                    validate_instruction();
                }

                validate_unused_labels();

                // the flow analysis needs every jump to land on an existing, later instruction
                if (m_flow_ok) {
                    validate_flow();
                }

                return ok();
            }

        protected:
            // sizeof(struct seccomp_data)
            static constexpr uint32_t SECCOMP_DATA_SIZE = 64;

            // known SECCOMP_RET_* actions, compared against the high 16 bits of the return value
            static constexpr uint32_t SECCOMP_ACTION_MASK        = 0xffff0000;
            static constexpr uint32_t SECCOMP_RET_KILL_PROCESS   = 0x80000000;
            static constexpr uint32_t SECCOMP_RET_KILL_THREAD    = 0x00000000;
            static constexpr uint32_t SECCOMP_RET_TRAP           = 0x00030000;
            static constexpr uint32_t SECCOMP_RET_ERRNO          = 0x00050000;
            static constexpr uint32_t SECCOMP_RET_USER_NOTIF     = 0x7fc00000;
            static constexpr uint32_t SECCOMP_RET_TRACE          = 0x7ff00000;
            static constexpr uint32_t SECCOMP_RET_LOG            = 0x7ffc0000;
            static constexpr uint32_t SECCOMP_RET_ALLOW          = 0x7fff0000;

            // what is known about the machine just BEFORE an instruction runs
            struct flow_state {
                bool     reached = false;  // some path reaches this instruction
                uint32_t slots   = 0;      // scratch slots written on EVERY path that reaches it
                bool     a_set   = false;  // A assigned on every path
                bool     x_set   = false;  // X assigned on every path
                bool     x_nz    = false;  // X known to be non-zero on every path
            };

            const instructions_t& instructions;
            const symtab_t&       symtab;
            profile               m_profile;
            size_t                m_address;  // NOTE: address serves as index into instructions vector too.
            bool                  m_flow_ok;  // false once a jump is invalid, the flow analysis needs a sound graph
            std::vector<flow_state> m_flow;

            bool validate_program() noexcept {
                // 1. Program has at least one instruction.
                if (instructions.empty()) {
                    report("program has no instructions");
                    return false;
                }

                // 2. Instruction count is at most MAX_INSTRUCTION_COUNT.
                if (instructions.size() > data::MAX_INSTRUCTION_COUNT) {
                    report("instruction count exceeds maximum of " + std::to_string(data::MAX_INSTRUCTION_COUNT));
                    return false;
                }
                
                // 3. The last instruction is a RET instruction.
                // FIXME: conceptually not necessary, do further tests
                // if (instructions.back().mnemonic != mnemonics::RET) {
                //     report("last instruction is not a RET instruction");
                //     return false;
                // }
                return true;  // Program is valid if all checks pass
            }

            bool validate_instruction() noexcept {
                bool valid = true;

                // 1. validate modes and operands
                if (!validate_mode()) {
                    valid = false;
                }

                // 2. validate operand ranges
                if (!validate_ranges()) {
                    valid = false;
                }

                // 3. validate the seccomp restrictions
                if (m_profile == profile::SECCOMP) {
                    if (!validate_seccomp()) {
                        valid = false;
                    }
                }

                // 4. validate jump targets
                if (is_jump(peek())) {
                    if (!validate_control_flow()) {
                        valid = false;
                    }
                }

                return valid;
            }

            bool validate_control_flow() noexcept {
                const auto& ins = peek();

                const symbol* true_sym = nullptr;
                const symbol* false_sym = nullptr;
                bool valid = true;

                auto resolve = [&](const token& label, const std::string& msg_prefix) -> const symbol* {
                    const auto it = symtab.find(label.value);

                    if (it == symtab.end()) {
                        report_error(msg_prefix + label.value, label.pos);
                        return nullptr;
                    }
                    
                    return &it->second;
                };

                if (vholds(ops::jmp_label, ins.ops)) {
                    const token& label = vget(ops::jmp_label, ins.ops).label;

                    true_sym = resolve(label, "undefined jump label: ");

                    if (!validate_target(true_sym, label)) {
                        valid = false;
                    }
                }

                else if (vholds(ops::jmp_cond, ins.ops)) {
                    const auto& op = vget(ops::jmp_cond, ins.ops);

                    // report as many errors as possible, both branches are checked even if the first one fails
                    true_sym = resolve(op.lt, "undefined true-branch label: ");

                    if (!validate_target(true_sym, op.lt)) {
                        valid = false;
                    }

                    if (op.lf.has_value()) {
                        false_sym = resolve(*op.lf, "undefined false-branch label: ");

                        if (!validate_target(false_sym, *op.lf)) {
                            valid = false;
                        }
                    }
                }

                else {
                    report_error("jump instruction has no label operand");
                    valid = false;
                }

                if (!valid) {
                    m_flow_ok = false;
                    return false;
                }

                // lint: both paths lead to the same instruction (a missing false branch falls through to the next one)
                if (vholds(ops::jmp_cond, ins.ops)) {
                    const size_t true_addr = static_cast<size_t>(true_sym->address);
                    size_t false_addr = m_address + 1;

                    if (false_sym != nullptr) {
                        false_addr = static_cast<size_t>(false_sym->address);
                    }

                    if (true_addr == false_addr) {
                        report_warning("both branches of '" + ins.tok.value + "' lead to the same instruction, the jump has no effect");
                    }
                }

                return true;
            }

            // a jump target must exist, come after the jump, and be followed by an instruction
            bool validate_target(const symbol* sym, const token& label) noexcept {
                // undefined label, already reported
                if (sym == nullptr) {
                    return false;
                }

                const size_t target = static_cast<size_t>(sym->address);

                if (target <= m_address) {
                    report_error("backward jump to label '" + label.value + "', jumps must go forward", label.pos);
                    return false;
                }

                if (target >= instructions.size()) {
                    report_error("label '" + label.value + "' is not followed by an instruction", label.pos);
                    return false;
                }

                return true;
            }

            // lint: a label that is defined but never jumped to
            void validate_unused_labels() noexcept {
                std::unordered_set<std::string> used;

                for (const auto& ins : instructions) {
                    if (vholds(ops::jmp_label, ins.ops)) {
                        used.insert(vget(ops::jmp_label, ins.ops).label.value);
                    }

                    else if (vholds(ops::jmp_cond, ins.ops)) {
                        const auto& op = vget(ops::jmp_cond, ins.ops);

                        used.insert(op.lt.value);

                        if (op.lf.has_value()) {
                            used.insert(op.lf->value);
                        }
                    }
                }

                std::vector<const std::pair<const std::string, symbol>*> unused;

                for (const auto& entry : symtab) {
                    if (used.find(entry.first) == used.end()) {
                        unused.push_back(&entry);
                    }
                }

                // the symtab is unordered, report in source order
                std::sort(unused.begin(), unused.end(), [](const auto* a, const auto* b) {
                    if (a->second.tok.pos.line != b->second.tok.pos.line) {
                        return a->second.tok.pos.line < b->second.tok.pos.line;
                    }

                    return a->second.tok.pos.col < b->second.tok.pos.col;
                });

                for (const auto* entry : unused) {
                    report_warning("label '" + entry->first + "' is defined but never used", entry->second.tok.pos);
                }
            }

            // forward data-flow over the instructions: unreachable code, scratch slots, registers, div by x
            void validate_flow() noexcept {
                build_flow();

                bool in_dead_code = false;

                for (m_address = 0; m_address < instructions.size(); ++m_address) {
                    if (!m_flow[m_address].reached) {
                        // one warning per run of unreachable instructions
                        if (!in_dead_code) {
                            report_warning("unreachable code, this instruction can never execute");
                        }

                        in_dead_code = true;
                        continue;
                    }

                    in_dead_code = false;

                    validate_scratch_mem_isset();
                    validate_registers_set();
                    validate_div_by_x();
                }
            }

            // validates that a scratch memory slot is set before use 'ld/ldx'.
            bool validate_scratch_mem_isset() noexcept  {
                const auto& ins = peek();

                // only 'ld M[k]' and 'ldx M[k]' read a slot
                if ((ins.mnemonic != mnemonics::LD && ins.mnemonic != mnemonics::LDX) || !vholds(ops::scratch, ins.ops)) {
                    return true;
                }

                const int64_t k = vget(ops::scratch, ins.ops).k;
                const uint32_t bit = slot_bit(k);

                // an out of range slot is already reported by validate_scratch
                if (bit != 0 && !(m_flow[m_address].slots & bit)) {
                    report_error("scratch memory slot " + std::to_string(k) + " is read before it is written on at least one path");
                    return false;
                }

                return true;
            }

            // lint: A or X read before anything was assigned to it (both start as 0)
            bool validate_registers_set() noexcept {
                const auto& ins = peek();
                const flow_state& state = m_flow[m_address];
                bool valid = true;

                if (reads_a(ins) && !state.a_set) {
                    report_warning("register a is read before it is assigned on at least one path, it starts as 0");
                    valid = false;
                }

                if (reads_x(ins) && !state.x_set) {
                    report_warning("register x is read before it is assigned on at least one path, it starts as 0");
                    valid = false;
                }

                return valid;
            }

            // lint: 'div x' / 'mod x' where x is not known to be non-zero (only 'ldx #k' with k != 0 proves it)
            bool validate_div_by_x() noexcept {
                const auto& ins = peek();

                if ((ins.mnemonic != mnemonics::DIV && ins.mnemonic != mnemonics::MOD) || !vholds(ops::reg_x, ins.ops)) {
                    return true;
                }

                if (!m_flow[m_address].x_nz) {
                    report_warning("'" + ins.tok.value + " x' may divide by zero, at runtime a zero divisor ends the program with return value 0");
                    return false;
                }

                return true;
            }

            void build_flow() noexcept {
                m_flow.assign(instructions.size(), flow_state{});

                // entry: nothing assigned, no slot written, x is 0
                m_flow[0].reached = true;

                for (size_t i = 0; i < instructions.size(); ++i) {
                    if (!m_flow[i].reached) {
                        continue;
                    }

                    const flow_state out = transfer(i);

                    for (size_t next : successors(i)) {
                        merge(m_flow[next], out);
                    }
                }
            }

            // the state after instruction 'i' ran
            flow_state transfer(size_t i) const noexcept {
                const auto& ins = instructions[i];
                flow_state out = m_flow[i];

                if (writes_a(ins.mnemonic)) {
                    out.a_set = true;
                }

                if (writes_x(ins.mnemonic)) {
                    out.x_set = true;
                    out.x_nz = false;

                    // 'ldx #k' with k != 0 is the only thing that proves x is non-zero
                    if (vholds(ops::imm, ins.ops)) {
                        out.x_nz = static_cast<uint32_t>(vget(ops::imm, ins.ops).k) != 0;
                    }
                }

                if ((ins.mnemonic == mnemonics::ST || ins.mnemonic == mnemonics::STX) && vholds(ops::scratch, ins.ops)) {
                    out.slots |= slot_bit(vget(ops::scratch, ins.ops).k);
                }

                return out;
            }

            // where execution can go after instruction 'i', only valid once every jump is validated
            std::vector<size_t> successors(size_t i) const noexcept {
                const auto& ins = instructions[i];
                std::vector<size_t> next;

                if (ins.mnemonic == mnemonics::RET) {
                    return next;
                }

                if (vholds(ops::jmp_label, ins.ops)) {
                    next.push_back(address_of(vget(ops::jmp_label, ins.ops).label));
                    return next;
                }

                if (vholds(ops::jmp_cond, ins.ops)) {
                    const auto& op = vget(ops::jmp_cond, ins.ops);

                    next.push_back(address_of(op.lt));

                    if (op.lf.has_value()) {
                        next.push_back(address_of(*op.lf));
                    }
                    else if (i + 1 < instructions.size()) {
                        next.push_back(i + 1);
                    }

                    return next;
                }

                if (i + 1 < instructions.size()) {
                    next.push_back(i + 1);
                }

                return next;
            }

            // joins two paths: only what is true on BOTH survives
            static void merge(flow_state& into, const flow_state& from) noexcept {
                if (!into.reached) {
                    into = from;
                    into.reached = true;
                    return;
                }

                into.slots &= from.slots;
                into.a_set = into.a_set && from.a_set;
                into.x_set = into.x_set && from.x_set;
                into.x_nz = into.x_nz && from.x_nz;
            }

            bool validate_ranges() noexcept {
                const auto& ins = peek();

                // Immediate 
                if (vholds(ops::imm, ins.ops)) {
                    return validate_imm();
                }

                else if (vholds(ops::scratch, ins.ops)) {
                    return validate_scratch();
                }

                else if (vholds(ops::pkt_abs, ins.ops) || vholds(ops::pkt_ind, ins.ops) || vholds(ops::msh, ins.ops)) {
                    return validate_packet();
                }

                // immediate on the right side of a compare
                else if (vholds(ops::jmp_cond, ins.ops)) {
                    const auto& op = vget(ops::jmp_cond, ins.ops);

                    if (vholds(ops::imm, op.rhs)) {
                        return validate_imm_range(vget(ops::imm, op.rhs).k);
                    }
                }
                
                return true;  // no range to check for the other operands
            }

            bool validate_packet() noexcept {
                int64_t k = 0;

                if (vholds(ops::pkt_abs, peek().ops)) {
                    k = vget(ops::pkt_abs, peek().ops).k;
                }
                else if (vholds(ops::pkt_ind, peek().ops)) {
                    k = vget(ops::pkt_ind, peek().ops).k;
                }
                else if (vholds(ops::msh, peek().ops)) {
                    k = vget(ops::msh, peek().ops).k;
                }
                else {
                    // safe fallback, should not reach here if validate_ranges is implemented correctly
                    report_error("packet offset check got an operand that is not [k], [x+k] or msh");
                    return false;
                }

                // 1. Packet offsets in [k], [x+k] and msh must fit in 32 bits.
                if (k < INT32_MIN || k > UINT32_MAX) {
                    report_error("packet offset " + std::to_string(k) + " is out of range [" + std::to_string(INT32_MIN) + ", " + std::to_string(UINT32_MAX) + "]");
                    return false;
                }
 
                // 2. A negative k is legal only as an extension offset
                if (k < 0 && k < data::PKT_DATA_OFF) {
                    report_error("negative packet offset " + std::to_string(k) + " is out of range, it must be >= " + std::to_string(data::PKT_DATA_OFF));
                    return false;
                }
 
                return true;
            }

            // the AST stores int64 and the encoder narrows to uint32, so a value must fit in 32 bits
            bool validate_imm_range(int64_t K) noexcept {
                if (K < INT32_MIN || K > UINT32_MAX) {
                    report_error("immediate " + std::to_string(K) + " is out of range [" + std::to_string(INT32_MIN) + ", " + std::to_string(UINT32_MAX) + "]");
                    return false;
                }

                return true;
            }

            bool validate_imm() noexcept {
                const auto& ins = peek();
                const int64_t K = vget(ops::imm, ins.ops).k;
 
                // 1. Immediate must fit in 32 bits.
                if (!validate_imm_range(K)) {
                    return false;
                }
 
                // 2. Division by zero is not allowed.
                if (ins.mnemonic == mnemonics::DIV || ins.mnemonic == mnemonics::MOD) {
                    if (K == 0) {
                        report_error("'" + ins.tok.value + " #0' divides by zero, the divisor must not be 0");
                        return false;
                    }
                }
 
                // 3. Left and right shifts must be in the range.
                else if (ins.mnemonic == mnemonics::LSH || ins.mnemonic == mnemonics::RSH) {
                    if (K < 0 || K > data::SH_MAX_SHIFT) {
                        report_error("shift amount " + std::to_string(K) + " is out of range [0, " + std::to_string(data::SH_MAX_SHIFT) + "]");
                        return false;
                    }
                }
 
                return true;
            }

            bool validate_scratch() noexcept {
                const auto& scr = vget(ops::scratch, peek().ops);
 
                if (scr.k < 0 || scr.k >= data::SC_MEM_SIZE) {
                    report_error("scratch memory index " + std::to_string(scr.k) + " is out of range [0, " + std::to_string(data::SC_MEM_SIZE - 1) + "]");
                    return false;
                }
 
                return true;
            }

            bool validate_mode() noexcept {
                // NOTE: Mode is validated during parsing. This remains valid by default
                // unless future changes introduce additional validation requirements.
                return true;
            }

            // seccomp: the filter only sees struct seccomp_data, and returns SECCOMP_RET_* actions
            bool validate_seccomp() noexcept {
                const auto& ins = peek();
                bool valid = true;

                // loads: only the word form 'ld [k]' reads the data, k aligned to 4 and inside seccomp_data
                if (vholds(ops::pkt_ind, ins.ops)) {
                    report_error("'" + ins.tok.value + " [x+k]' is not allowed in seccomp filters, only 'ld [k]' reads the data");
                    valid = false;
                }

                else if (vholds(ops::msh, ins.ops)) {
                    report_error("'" + ins.tok.value + "' with 4*([k]&0xf) is not allowed in seccomp filters, only 'ld [k]' reads the data");
                    valid = false;
                }

                else if (vholds(ops::pkt_abs, ins.ops)) {
                    const int64_t k = vget(ops::pkt_abs, ins.ops).k;

                    if (ins.mnemonic != mnemonics::LD) {
                        report_error("'" + ins.tok.value + "' is not allowed in seccomp filters, only the word load 'ld [k]' reads the data");
                        valid = false;
                    }

                    else if (k < 0 || k % 4 != 0 || k >= SECCOMP_DATA_SIZE) {
                        report_error("seccomp data offset " + std::to_string(k) + " must be a multiple of 4 and less than " + std::to_string(SECCOMP_DATA_SIZE));
                        valid = false;
                    }
                }

                // extensions: only 'len' is accepted, and it is always the size of seccomp_data
                else if (vholds(ops::ext, ins.ops)) {
                    if (vget(ops::ext, ins.ops).kind == extension::Len) {
                        report_warning("'len' is always " + std::to_string(SECCOMP_DATA_SIZE) + " in a seccomp filter");
                    }
                    else {
                        report_error("extensions other than 'len' are not allowed in seccomp filters");
                        valid = false;
                    }
                }

                // ret: the high 16 bits must be a known action
                else if (ins.mnemonic == mnemonics::RET && vholds(ops::imm, ins.ops)) {
                    const uint32_t k = static_cast<uint32_t>(vget(ops::imm, ins.ops).k);

                    if (!is_seccomp_action(k)) {
                        report_warning("ret value " + std::to_string(k) + " is not a known seccomp action, the high 16 bits must be one of kill, trap, errno, user_notif, trace, log or allow");
                    }
                }

                return valid;
            }

            // Utils
            const instruction& peek(uint32_t off = 0) const noexcept {
                return instructions[m_address + off];
            }

            // reports at the current instruction
            void report_error(const std::string& msg) noexcept {
                report(msg, peek().tok.pos, log_type::ERROR);
            }

            void report_error(const std::string& msg, const token_pos& pos) noexcept {
                report(msg, pos, log_type::ERROR);
            }

            void report_warning(const std::string& msg) noexcept {
                report(msg, peek().tok.pos, log_type::WARNING);
            }

            void report_warning(const std::string& msg, const token_pos& pos) noexcept {
                report(msg, pos, log_type::WARNING);
            }

            static bool is_jump(const instruction& ins) noexcept {
                return vholds(ops::jmp_label, ins.ops) || vholds(ops::jmp_cond, ins.ops);
            }

            size_t address_of(const token& label) const noexcept {
                return static_cast<size_t>(symtab.find(label.value)->second.address);
            }

            // the bit of scratch slot 'k' in flow_state::slots, 0 if 'k' is out of range
            static uint32_t slot_bit(int64_t k) noexcept {
                if (k < 0 || k >= data::SC_MEM_SIZE) {
                    return 0;
                }

                return 1u << k;
            }

            static bool is_seccomp_action(uint32_t k) noexcept {
                switch (k & SECCOMP_ACTION_MASK) {
                    case SECCOMP_RET_KILL_PROCESS:
                    case SECCOMP_RET_KILL_THREAD:
                    case SECCOMP_RET_TRAP:
                    case SECCOMP_RET_ERRNO:
                    case SECCOMP_RET_USER_NOTIF:
                    case SECCOMP_RET_TRACE:
                    case SECCOMP_RET_LOG:
                    case SECCOMP_RET_ALLOW:
                        return true;

                    default:
                        return false;
                }
            }

            static bool is_alu(mnemonics mn) noexcept {
                switch (mn) {
                    case mnemonics::ADD:
                    case mnemonics::SUB:
                    case mnemonics::MUL:
                    case mnemonics::DIV:
                    case mnemonics::MOD:
                    case mnemonics::NEG:
                    case mnemonics::AND:
                    case mnemonics::OR:
                    case mnemonics::XOR:
                    case mnemonics::LSH:
                    case mnemonics::RSH:
                        return true;

                    default:
                        return false;
                }
            }

            static bool is_cond_jump(mnemonics mn) noexcept {
                switch (mn) {
                    case mnemonics::JEQ:
                    case mnemonics::JGT:
                    case mnemonics::JGE:
                    case mnemonics::JSET:
                    case mnemonics::JNE:
                    case mnemonics::JLT:
                    case mnemonics::JLE:
                        return true;

                    default:
                        return false;
                }
            }

            static bool writes_a(mnemonics mn) noexcept {
                switch (mn) {
                    case mnemonics::LD:
                    case mnemonics::LDI:
                    case mnemonics::LDH:
                    case mnemonics::LDB:
                    case mnemonics::TXA:
                        return true;

                    default:
                        return is_alu(mn);
                }
            }

            static bool writes_x(mnemonics mn) noexcept {
                switch (mn) {
                    case mnemonics::LDX:
                    case mnemonics::LDXI:
                    case mnemonics::LDXB:
                    case mnemonics::TAX:
                        return true;

                    default:
                        return false;
                }
            }

            static bool reads_a(const instruction& ins) noexcept {
                if (is_alu(ins.mnemonic) || is_cond_jump(ins.mnemonic)) {
                    return true;
                }

                switch (ins.mnemonic) {
                    case mnemonics::ST:
                    case mnemonics::TAX:
                        return true;

                    case mnemonics::RET:
                        return vholds(ops::reg_a, ins.ops);

                    default:
                        return false;
                }
            }

            static bool reads_x(const instruction& ins) noexcept {
                if (ins.mnemonic == mnemonics::STX || ins.mnemonic == mnemonics::TXA) {
                    return true;
                }

                // 'add x', 'sub x' ...
                if (vholds(ops::reg_x, ins.ops)) {
                    return true;
                }

                // '[x+k]'
                if (vholds(ops::pkt_ind, ins.ops)) {
                    return true;
                }

                // 'jeq x, ...'
                if (vholds(ops::jmp_cond, ins.ops)) {
                    return vholds(ops::reg_x, vget(ops::jmp_cond, ins.ops).rhs);
                }

                return false;
            }
    };
    

    #undef vholds
    #undef vget
} // namespace anvil::s

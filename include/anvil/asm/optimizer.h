#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>
#include "validator.h"

namespace anvil::s
{

    #define vholds(X, Y) std::holds_alternative<X>(Y)
    #define vget(X, Y) std::get<X>(Y)

    class Optimizer : public BaseObj {

        public:
            // O0 leaves the program untouched, O1 applies every rewrite until nothing changes.
            enum class level : uint8_t {
                O0,
                O1
            };

            Optimizer(instructions_t& ins, symtab_t& st, level lvl = level::O1) noexcept : instructions(ins), symtab(st), m_level(lvl) {}

            // returns true if the program was changed
            bool optimize() noexcept {
                if (m_level == level::O0) {
                    return false;
                }

                // the passes assume every jump goes forward to an existing instruction
                if (!check_program("cannot optimize")) {
                    return false;
                }

                #ifndef NDEBUG
                const bool was_valid = Validator(instructions, symtab).validate();
                #endif

                const pass passes[] = {
                    &Optimizer::fold_pass,
                    &Optimizer::merge_pass,
                    &Optimizer::liveness_pass,
                    &Optimizer::branch_pass,
                    &Optimizer::dead_code_pass
                };

                bool changed = false;
                bool converged = false;

                // O-FIXPOINT: deciding a branch creates dead code, removing it exposes more folds
                for (size_t round = 0; round < MAX_ROUNDS && !converged; ++round) {
                    bool progress = false;

                    for (const pass run : passes) {
                        if ((this->*run)()) {
                            progress = true;
                        }

                        if (!pass_is_sound()) {
                            return true;
                        }
                    }

                    if (progress) {
                        changed = true;
                    }
                    else {
                        converged = true;
                    }
                }

                if (!converged) {
                    report("internal error: the optimizer did not converge");
                }

                // O-REVALIDATE
                revalidate(was_valid_or_true(
                    #ifndef NDEBUG
                    was_valid
                    #else
                    true
                    #endif
                ));

                return changed;
            }

        protected:
            using pass = bool (Optimizer::*)() noexcept;

            static constexpr size_t MAX_ROUNDS = 64;

            // a packet load (or len) whose result a register still holds
            struct load_key {
                int       kind = 0;  // 0: nothing, 1: packet [k], 2: len, 3: msh
                mnemonics mn   = mnemonics::LD;
                int64_t   k    = 0;

                bool operator==(const load_key& o) const noexcept {
                    return kind == o.kind && mn == o.mn && k == o.k;
                }
            };

            // what is known about the machine just BEFORE an instruction, inside one basic block
            struct value_state {
                std::optional<uint32_t> a;                    // A holds this constant
                std::optional<uint32_t> x;                    // X holds this constant
                uint32_t                a_bits = 0xffffffff;  // the only bits A can have
                bool                    eq     = false;       // A == X
                int                     a_slot = -1;          // M[a_slot] == A
                int                     x_slot = -1;          // M[x_slot] == X
                load_key                a_load;               // the packet load A still holds
                load_key                x_load;               // the packet load X still holds
            };

            // what is needed by the instructions that run AFTER a point
            struct live_state {
                bool     a     = false;
                bool     x     = false;
                uint32_t slots = 0;
            };

            struct effects {
                bool use_a    = false;
                bool use_x    = false;
                bool def_a    = false;
                bool def_x    = false;
                int  use_slot = -1;
                int  def_slot = -1;
            };

            enum class merged : uint8_t {
                none,
                replaced,   // the first instruction now does both
                cancelled   // both instructions are gone
            };

            instructions_t& instructions;
            symtab_t&       symtab;
            level           m_level;

            std::vector<bool>       m_del;       // instructions to erase at the end of the pass
            std::vector<bool>       m_mod;       // instructions already rewritten by the pass
            std::vector<bool>       m_leader;    // instructions that start a basic block
            std::vector<live_state> m_live_out;  // what is live after each instruction

            // ------------------------------------------------------------------
            // 1. constants, algebra and work that was already done (forward, one block at a time)
            // ------------------------------------------------------------------
            bool fold_pass() noexcept {
                compute_leaders();
                m_del.assign(instructions.size(), false);

                bool changed = false;
                value_state st;

                for (size_t i = 0; i < instructions.size(); ++i) {
                    // O-BLOCKS: nothing is known at the start of a block
                    if (m_leader[i]) {
                        st = value_state{};
                    }

                    instruction& ins = instructions[i];

                    if (fold_instruction(i, ins, st)) {
                        changed = true;
                    }

                    if (!m_del[i]) {
                        advance(ins, st);
                    }
                }

                apply_deletions();
                return changed;
            }

            bool fold_instruction(size_t i, instruction& ins, const value_state& st) noexcept {
                bool changed = false;

                // 1. work that was already done
                if (is_redundant(ins, st)) {
                    m_del[i] = true;
                    return true;
                }

                // 2. a slot that already sits in a register
                if (forward_slot(ins, st)) {
                    changed = true;
                }

                // 3. a known x replaces itself
                if (propagate_x(ins, st)) {
                    changed = true;
                }

                // 4. a known a folds the instruction into a load
                if (fold_a(ins, st)) {
                    changed = true;
                }

                // 5. algebra on immediates
                if (simplify_alu(i, ins, st)) {
                    changed = true;

                    if (m_del[i]) {
                        return true;
                    }
                }

                // 6. compares that are already decided
                if (fold_jump(i, ins, st)) {
                    changed = true;

                    if (m_del[i]) {
                        return true;
                    }
                }

                // 7. the rewrites above can leave work that was already done
                if (changed && is_redundant(ins, st)) {
                    m_del[i] = true;
                }

                return changed;
            }

            // O-MOV-*, O-MEM-FORWARD-*, O-MEM-NOOP-STORE, O-LD-REPEAT
            bool is_redundant(const instruction& ins, const value_state& st) const noexcept {
                const mnemonics mn = ins.mnemonic;

                // tax / txa when a and x are already equal
                if (mn == mnemonics::TAX || mn == mnemonics::TXA) {
                    if (st.eq) {
                        return true;
                    }

                    return st.a.has_value() && st.x.has_value() && *st.a == *st.x;
                }

                // a load of the constant the register already holds
                if (is_ld(mn) && vholds(ops::imm, ins.ops)) {
                    return st.a.has_value() && *st.a == u32(vget(ops::imm, ins.ops).k);
                }

                if (is_ldx(mn) && vholds(ops::imm, ins.ops)) {
                    return st.x.has_value() && *st.x == u32(vget(ops::imm, ins.ops).k);
                }

                // a slot that already equals the register
                if (vholds(ops::scratch, ins.ops)) {
                    const int k = slot_of(vget(ops::scratch, ins.ops).k);

                    if (is_ld(mn) || mn == mnemonics::ST) {
                        return slot_equals_a(st, k);
                    }

                    if (is_ldx(mn) || mn == mnemonics::STX) {
                        return slot_equals_x(st, k);
                    }

                    return false;
                }

                // the same packet load again: the first one already proved the access is in bounds
                if (is_ld(mn)) {
                    const load_key key = a_key(ins);
                    return key.kind != 0 && key == st.a_load;
                }

                if (is_ldx(mn)) {
                    const load_key key = x_key(ins);
                    return key.kind != 0 && key == st.x_load;
                }

                return false;
            }

            // O-MEM-CROSS-1, O-MEM-CROSS-2
            bool forward_slot(instruction& ins, const value_state& st) noexcept {
                if (!vholds(ops::scratch, ins.ops)) {
                    return false;
                }

                const int k = slot_of(vget(ops::scratch, ins.ops).k);

                // st M[k] ; ldx M[k]  ->  tax
                if (is_ldx(ins.mnemonic) && slot_equals_a(st, k)) {
                    ins = instruction{mnemonics::TAX, ops::no_operand{}, ins.tok};
                    return true;
                }

                // stx M[k] ; ld M[k]  ->  txa
                if (is_ld(ins.mnemonic) && slot_equals_x(st, k)) {
                    ins = instruction{mnemonics::TXA, ops::no_operand{}, ins.tok};
                    return true;
                }

                return false;
            }

            // O-CF-X-OPERAND, O-CF-X-INDEX
            bool propagate_x(instruction& ins, const value_state& st) noexcept {
                if (!st.x.has_value()) {
                    return false;
                }

                const uint32_t kx = *st.x;

                if (is_alu(ins.mnemonic) && vholds(ops::reg_x, ins.ops)) {
                    const mnemonics mn = ins.mnemonic;

                    // a zero divisor aborts the program and a shift of 32 or more is undefined, leave both alone
                    if ((mn == mnemonics::DIV || mn == mnemonics::MOD) && kx == 0) {
                        return false;
                    }

                    if ((mn == mnemonics::LSH || mn == mnemonics::RSH) && kx >= 32) {
                        return false;
                    }

                    ins.ops = make_imm(kx);
                    return true;
                }

                if (vholds(ops::jmp_cond, ins.ops)) {
                    auto& op = vget(ops::jmp_cond, ins.ops);

                    if (vholds(ops::reg_x, op.rhs)) {
                        op.rhs = make_imm(kx);
                        return true;
                    }

                    return false;
                }

                if (is_ld(ins.mnemonic) && vholds(ops::pkt_ind, ins.ops)) {
                    const int64_t n = vget(ops::pkt_ind, ins.ops).k;
                    const int64_t offset = static_cast<int64_t>(kx) + n;

                    // x + n wraps around 32 bits in the machine, only fold the sums that do not
                    if (n >= 0 && offset <= INT32_MAX) {
                        ins.ops = ops::pkt_abs{offset};
                        return true;
                    }
                }

                return false;
            }

            // O-CF-ALU, O-CF-NEG
            bool fold_a(instruction& ins, const value_state& st) noexcept {
                if (!st.a.has_value() || !is_alu(ins.mnemonic)) {
                    return false;
                }

                const uint32_t a = *st.a;

                if (ins.mnemonic == mnemonics::NEG) {
                    ins = load_imm(0u - a, ins.tok);
                    return true;
                }

                if (!vholds(ops::imm, ins.ops)) {
                    return false;
                }

                const std::optional<uint32_t> result = alu_eval(ins.mnemonic, a, u32(vget(ops::imm, ins.ops).k));

                if (!result.has_value()) {
                    return false;
                }

                ins = load_imm(*result, ins.tok);
                return true;
            }

            // O-ALG-IDENTITY, O-ALG-ABSORB, O-ALG-ABSORB-ONES, O-ALG-MASK, O-ALG-STRENGTH
            bool simplify_alu(size_t i, instruction& ins, const value_state& st) noexcept {
                if (!is_alu(ins.mnemonic) || !vholds(ops::imm, ins.ops)) {
                    return false;
                }

                const mnemonics mn = ins.mnemonic;
                const uint32_t b = u32(vget(ops::imm, ins.ops).k);

                // nothing changes
                if (is_identity(mn, b)) {
                    m_del[i] = true;
                    return true;
                }

                // the result does not depend on a
                if ((mn == mnemonics::AND && b == 0) || (mn == mnemonics::MUL && b == 0) || (mn == mnemonics::MOD && b == 1)) {
                    ins = load_imm(0, ins.tok);
                    return true;
                }

                if (mn == mnemonics::OR && b == 0xffffffff) {
                    ins = load_imm(0xffffffff, ins.tok);
                    return true;
                }

                // the bits that are cleared were never set, e.g. ldb [k] ; and #0xff
                if (mn == mnemonics::AND && (st.a_bits & ~b) == 0) {
                    m_del[i] = true;
                    return true;
                }

                // mul / div / mod by a power of two are shifts and masks, which merge and fold further
                if ((mn == mnemonics::MUL || mn == mnemonics::DIV || mn == mnemonics::MOD) && b > 1 && is_pow2(b)) {
                    if (mn == mnemonics::MUL) {
                        ins.mnemonic = mnemonics::LSH;
                        ins.ops = make_imm(log2_of(b));
                    }
                    else if (mn == mnemonics::DIV) {
                        ins.mnemonic = mnemonics::RSH;
                        ins.ops = make_imm(log2_of(b));
                    }
                    else {
                        ins.mnemonic = mnemonics::AND;
                        ins.ops = make_imm(b - 1);
                    }

                    return true;
                }

                return false;
            }

            // O-CF-BRANCH, O-CF-ALWAYS
            bool fold_jump(size_t i, instruction& ins, const value_state& st) noexcept {
                if (!vholds(ops::jmp_cond, ins.ops)) {
                    return false;
                }

                const auto& op = vget(ops::jmp_cond, ins.ops);
                const std::optional<bool> outcome = decide(ins.mnemonic, op, st);

                if (!outcome.has_value()) {
                    return false;
                }

                // the label the jump takes, none when it falls through to the next instruction
                std::optional<token> taken;

                if (*outcome) {
                    taken = op.lt;
                }
                else if (op.lf.has_value()) {
                    taken = *op.lf;
                }

                if (!taken.has_value() || address_of(*taken) == i + 1) {
                    m_del[i] = true;
                    return true;
                }

                ins = instruction{mnemonics::JA, ops::jmp_label{*taken}, ins.tok};
                return true;
            }

            // whether the condition of a jump holds, if that is already known
            std::optional<bool> decide(mnemonics mn, const ops::jmp_cond& op, const value_state& st) const noexcept {
                std::optional<uint32_t> rhs;

                if (vholds(ops::imm, op.rhs)) {
                    rhs = u32(vget(ops::imm, op.rhs).k);
                }
                else if (st.x.has_value()) {
                    rhs = st.x;
                }

                if (!rhs.has_value()) {
                    return std::nullopt;
                }

                const uint32_t v = *rhs;

                if (st.a.has_value()) {
                    return compare(mn, *st.a, v);
                }

                // a is not known, but the bits it can have are. All compares are UNSIGNED.
                const uint32_t bits = st.a_bits;

                switch (mn) {
                    case mnemonics::JEQ:
                        if ((v & ~bits) != 0) {
                            return false;
                        }
                        break;

                    case mnemonics::JNE:
                        if ((v & ~bits) != 0) {
                            return true;
                        }
                        break;

                    case mnemonics::JGT:
                        if (bits <= v) {
                            return false;
                        }
                        break;

                    case mnemonics::JGE:
                        if (v == 0) {
                            return true;
                        }

                        if (bits < v) {
                            return false;
                        }
                        break;

                    case mnemonics::JLT:
                        if (v == 0) {
                            return false;
                        }

                        if (bits < v) {
                            return true;
                        }
                        break;

                    case mnemonics::JLE:
                        if (bits <= v) {
                            return true;
                        }
                        break;

                    case mnemonics::JSET:
                        if ((v & bits) == 0) {
                            return false;
                        }
                        break;

                    default:
                        break;
                }

                return std::nullopt;
            }

            static std::optional<bool> compare(mnemonics mn, uint32_t a, uint32_t v) noexcept {
                switch (mn) {
                    case mnemonics::JEQ:
                        return a == v;

                    case mnemonics::JNE:
                        return a != v;

                    case mnemonics::JGT:
                        return a > v;

                    case mnemonics::JGE:
                        return a >= v;

                    case mnemonics::JLT:
                        return a < v;

                    case mnemonics::JLE:
                        return a <= v;

                    case mnemonics::JSET:
                        return (a & v) != 0;

                    default:
                        return std::nullopt;
                }
            }

            // the state after the instruction ran
            void advance(const instruction& ins, value_state& st) const noexcept {
                const mnemonics mn = ins.mnemonic;

                if (is_ld(mn)) {
                    st.a.reset();
                    st.a_bits = 0xffffffff;
                    st.a_slot = -1;
                    st.eq = false;
                    st.a_load = a_key(ins);

                    if (vholds(ops::imm, ins.ops)) {
                        st.a = u32(vget(ops::imm, ins.ops).k);
                        st.a_bits = *st.a;
                    }
                    else if (vholds(ops::scratch, ins.ops)) {
                        st.a_slot = slot_of(vget(ops::scratch, ins.ops).k);
                    }
                    else if (mn == mnemonics::LDB) {
                        st.a_bits = 0xff;
                    }
                    else if (mn == mnemonics::LDH) {
                        st.a_bits = 0xffff;
                    }

                    return;
                }

                if (is_ldx(mn)) {
                    st.x.reset();
                    st.x_slot = -1;
                    st.eq = false;
                    st.x_load = x_key(ins);

                    if (vholds(ops::imm, ins.ops)) {
                        st.x = u32(vget(ops::imm, ins.ops).k);
                    }
                    else if (vholds(ops::scratch, ins.ops)) {
                        st.x_slot = slot_of(vget(ops::scratch, ins.ops).k);
                    }

                    return;
                }

                if (is_alu(mn)) {
                    const uint32_t before = st.a_bits;

                    st.a.reset();
                    st.a_bits = 0xffffffff;
                    st.a_slot = -1;
                    st.eq = false;
                    st.a_load = load_key{};

                    if (vholds(ops::imm, ins.ops)) {
                        const uint32_t b = u32(vget(ops::imm, ins.ops).k);

                        if (mn == mnemonics::AND) {
                            st.a_bits = before & b;
                        }
                        else if (mn == mnemonics::OR || mn == mnemonics::XOR) {
                            st.a_bits = before | b;
                        }
                        else if (mn == mnemonics::RSH && b < 32) {
                            st.a_bits = before >> b;
                        }
                        else if (mn == mnemonics::LSH && b < 32) {
                            st.a_bits = before << b;
                        }
                    }

                    return;
                }

                if (mn == mnemonics::TAX) {
                    st.x = st.a;
                    st.x_slot = st.a_slot;
                    st.x_load = load_key{};
                    st.eq = true;
                    return;
                }

                if (mn == mnemonics::TXA) {
                    st.a = st.x;
                    st.a_bits = st.x.has_value() ? *st.x : 0xffffffff;
                    st.a_slot = st.x_slot;
                    st.a_load = load_key{};
                    st.eq = true;
                    return;
                }

                if ((mn == mnemonics::ST || mn == mnemonics::STX) && vholds(ops::scratch, ins.ops)) {
                    const int k = slot_of(vget(ops::scratch, ins.ops).k);

                    // a slot that is not tracked: forget every slot
                    if (k < 0) {
                        st.a_slot = -1;
                        st.x_slot = -1;
                        return;
                    }

                    if (mn == mnemonics::ST) {
                        st.a_slot = k;

                        if (st.x_slot == k && !st.eq) {
                            st.x_slot = -1;
                        }
                    }
                    else {
                        st.x_slot = k;

                        if (st.a_slot == k && !st.eq) {
                            st.a_slot = -1;
                        }
                    }
                }
            }

            // ------------------------------------------------------------------
            // 2. adjacent instructions that merge or cancel
            // ------------------------------------------------------------------
            bool merge_pass() noexcept {
                const size_t n = instructions.size();

                compute_leaders();
                m_del.assign(n, false);

                bool changed = false;

                for (size_t i = 0; i < n; ++i) {
                    if (m_del[i]) {
                        continue;
                    }

                    // keep merging the next instruction into this one for as long as it works
                    while (true) {
                        // the next surviving instruction, and whether a block starts on the way
                        size_t j = i + 1;
                        bool boundary = false;

                        for (; j < n; ++j) {
                            if (m_leader[j]) {
                                boundary = true;
                            }

                            if (!m_del[j]) {
                                break;
                            }
                        }

                        // O-BLOCKS: a window may contain a block start only as its first member
                        if (j >= n || boundary) {
                            break;
                        }

                        const merged result = merge_pair(instructions[i], instructions[j]);

                        if (result == merged::none) {
                            break;
                        }

                        m_del[j] = true;
                        changed = true;

                        if (result == merged::cancelled) {
                            m_del[i] = true;
                            break;
                        }
                    }
                }

                apply_deletions();
                return changed;
            }

            // O-ALG-DOUBLE-NEG, O-ALG-MERGE
            merged merge_pair(instruction& first, const instruction& second) const noexcept {
                if (first.mnemonic == mnemonics::NEG && second.mnemonic == mnemonics::NEG) {
                    return merged::cancelled;
                }

                if (!is_alu(first.mnemonic) || !is_alu(second.mnemonic) || !vholds(ops::imm, first.ops) || !vholds(ops::imm, second.ops)) {
                    return merged::none;
                }

                const mnemonics m1 = first.mnemonic;
                const mnemonics m2 = second.mnemonic;
                const uint32_t a = u32(vget(ops::imm, first.ops).k);
                const uint32_t b = u32(vget(ops::imm, second.ops).k);

                // add and sub chain: both are an add of a constant
                if ((m1 == mnemonics::ADD || m1 == mnemonics::SUB) && (m2 == mnemonics::ADD || m2 == mnemonics::SUB)) {
                    uint32_t da = a;
                    uint32_t db = b;

                    if (m1 == mnemonics::SUB) {
                        da = 0u - a;
                    }

                    if (m2 == mnemonics::SUB) {
                        db = 0u - b;
                    }

                    first.mnemonic = mnemonics::ADD;
                    first.ops = make_imm(da + db);
                    return merged::replaced;
                }

                if (m1 != m2) {
                    return merged::none;
                }

                switch (m1) {
                    case mnemonics::AND:
                        first.ops = make_imm(a & b);
                        return merged::replaced;

                    case mnemonics::OR:
                        first.ops = make_imm(a | b);
                        return merged::replaced;

                    case mnemonics::XOR:
                        first.ops = make_imm(a ^ b);
                        return merged::replaced;

                    case mnemonics::MUL:
                        first.ops = make_imm(a * b);
                        return merged::replaced;

                    // the shift amounts add up, 32 or more shifts everything out
                    case mnemonics::LSH:
                    case mnemonics::RSH:
                        if (a >= 32 || b >= 32) {
                            return merged::none;
                        }

                        if (a + b < 32) {
                            first.ops = make_imm(a + b);
                        }
                        else {
                            first = load_imm(0, first.tok);
                        }

                        return merged::replaced;

                    default:
                        return merged::none;
                }
            }

            // ------------------------------------------------------------------
            // 3. dead results (backward): a pure instruction nothing reads is deleted
            // ------------------------------------------------------------------
            bool liveness_pass() noexcept {
                const size_t n = instructions.size();

                compute_leaders();
                compute_liveness();
                m_del.assign(n, false);
                m_mod.assign(n, false);

                bool changed = false;

                for (size_t i = 0; i < n; ++i) {
                    if (m_del[i] || m_mod[i]) {
                        continue;
                    }

                    if (rewrite_idiom(i)) {
                        changed = true;
                        continue;
                    }

                    // O-MOV-DEAD-TAX, O-MEM-DEAD-STORE, O-LD-OVERWRITTEN
                    if (is_pure(instructions[i]) && is_dead(i)) {
                        m_del[i] = true;
                        changed = true;
                    }
                }

                apply_deletions();
                return changed;
            }

            // O-ID-LDX-IMM, O-ID-MSH: both need a to be dead afterwards, the replacement leaves a alone
            bool rewrite_idiom(size_t i) noexcept {
                const size_t n = instructions.size();
                instruction& first = instructions[i];

                // ld #k ; tax  ->  ldx #k
                if (is_ld(first.mnemonic) && vholds(ops::imm, first.ops) && i + 1 < n) {
                    const instruction& second = instructions[i + 1];

                    if (second.mnemonic == mnemonics::TAX && window_is_free(i, 2) && !m_live_out[i + 1].a) {
                        first = instruction{mnemonics::LDX, first.ops, first.tok};
                        m_mod[i] = true;
                        m_del[i + 1] = true;
                        return true;
                    }
                }

                // ldb [k] ; and #0xf ; lsh #2 ; tax  ->  ldxb 4*([k]&0xf)
                if (first.mnemonic == mnemonics::LDB && vholds(ops::pkt_abs, first.ops) && i + 3 < n) {
                    const instruction& and_ins = instructions[i + 1];
                    const instruction& lsh_ins = instructions[i + 2];
                    const instruction& tax_ins = instructions[i + 3];

                    if (and_ins.mnemonic == mnemonics::AND && vholds(ops::imm, and_ins.ops) && u32(vget(ops::imm, and_ins.ops).k) == 0xf
                        && lsh_ins.mnemonic == mnemonics::LSH && vholds(ops::imm, lsh_ins.ops) && u32(vget(ops::imm, lsh_ins.ops).k) == 2
                        && tax_ins.mnemonic == mnemonics::TAX && window_is_free(i, 4) && !m_live_out[i + 3].a) {
                        first = instruction{mnemonics::LDXB, ops::msh{vget(ops::pkt_abs, first.ops).k}, first.tok};
                        m_mod[i] = true;
                        m_del[i + 1] = true;
                        m_del[i + 2] = true;
                        m_del[i + 3] = true;
                        return true;
                    }
                }

                return false;
            }

            // O-BLOCKS: 'size' instructions from 'start' are untouched so far and no block starts inside them
            bool window_is_free(size_t start, size_t size) const noexcept {
                for (size_t k = 0; k < size; ++k) {
                    if (m_del[start + k] || m_mod[start + k]) {
                        return false;
                    }

                    if (k > 0 && m_leader[start + k]) {
                        return false;
                    }
                }

                return true;
            }

            void compute_liveness() noexcept {
                const size_t n = instructions.size();
                std::vector<live_state> live_in(n);

                m_live_out.assign(n, live_state{});

                for (size_t k = n; k-- > 0;) {
                    live_state out;

                    for (size_t next : successors(k)) {
                        out.a = out.a || live_in[next].a;
                        out.x = out.x || live_in[next].x;
                        out.slots |= live_in[next].slots;
                    }

                    m_live_out[k] = out;

                    const effects e = effects_of(instructions[k]);
                    live_state in = out;

                    if (e.def_a) {
                        in.a = false;
                    }

                    if (e.def_x) {
                        in.x = false;
                    }

                    if (e.def_slot >= 0) {
                        in.slots &= ~slot_bit(e.def_slot);
                    }

                    if (e.use_a) {
                        in.a = true;
                    }

                    if (e.use_x) {
                        in.x = true;
                    }

                    if (e.use_slot >= 0) {
                        in.slots |= slot_bit(e.use_slot);
                    }

                    live_in[k] = in;
                }
            }

            // whatever the instruction writes is not read before it is written again
            bool is_dead(size_t i) const noexcept {
                const effects e = effects_of(instructions[i]);
                const live_state& out = m_live_out[i];

                if (e.def_slot >= 0) {
                    return !(out.slots & slot_bit(e.def_slot));
                }

                if (e.def_a) {
                    return !out.a;
                }

                if (e.def_x) {
                    return !out.x;
                }

                return false;
            }

            // O-SIDE-EFFECTS: packet loads, msh and 'div x' / 'mod x' can abort the program, they are never dead code
            bool is_pure(const instruction& ins) const noexcept {
                const mnemonics mn = ins.mnemonic;

                if (is_ld(mn) || is_ldx(mn)) {
                    if (vholds(ops::imm, ins.ops) || vholds(ops::scratch, ins.ops)) {
                        return true;
                    }

                    if (vholds(ops::ext, ins.ops)) {
                        return vget(ops::ext, ins.ops).kind == extension::Len;
                    }

                    return false;
                }

                if (mn == mnemonics::TAX || mn == mnemonics::TXA || mn == mnemonics::ST || mn == mnemonics::STX) {
                    return true;
                }

                if (is_alu(mn)) {
                    if (!vholds(ops::imm, ins.ops)) {
                        return mn != mnemonics::DIV && mn != mnemonics::MOD;
                    }

                    const uint32_t b = u32(vget(ops::imm, ins.ops).k);

                    if (mn == mnemonics::DIV || mn == mnemonics::MOD) {
                        return b != 0;
                    }

                    if (mn == mnemonics::LSH || mn == mnemonics::RSH) {
                        return b < 32;
                    }

                    return true;
                }

                return false;
            }

            // ------------------------------------------------------------------
            // 4. jumps
            // ------------------------------------------------------------------
            bool branch_pass() noexcept {
                const size_t n = instructions.size();

                m_del.assign(n, false);

                bool changed = false;

                for (size_t i = 0; i < n; ++i) {
                    instruction& ins = instructions[i];

                    // O-BR-THREAD
                    if (thread_jump(ins)) {
                        changed = true;
                    }

                    if (vholds(ops::jmp_label, ins.ops)) {
                        const token label = vget(ops::jmp_label, ins.ops).label;
                        const size_t target = address_of(label);

                        // O-BR-TO-NEXT
                        if (target == i + 1) {
                            m_del[i] = true;
                            changed = true;
                        }

                        // O-BR-TO-RET: same size, and one hop less
                        else if (instructions[target].mnemonic == mnemonics::RET) {
                            ins = instruction{mnemonics::RET, instructions[target].ops, ins.tok};
                            changed = true;
                        }
                    }

                    // O-BR-SAME-TARGET
                    else if (vholds(ops::jmp_cond, ins.ops)) {
                        const auto& op = vget(ops::jmp_cond, ins.ops);
                        const token true_label = op.lt;
                        const size_t true_addr = address_of(true_label);
                        size_t false_addr = i + 1;

                        if (op.lf.has_value()) {
                            false_addr = address_of(*op.lf);
                        }

                        if (true_addr == false_addr) {
                            if (true_addr == i + 1) {
                                m_del[i] = true;
                            }
                            else {
                                ins = instruction{mnemonics::JA, ops::jmp_label{true_label}, ins.tok};
                            }

                            changed = true;
                        }
                    }
                }

                if (merge_returns()) {
                    changed = true;
                }

                apply_deletions();
                return changed;
            }

            // a jump to a 'ja L2' jumps to L2 directly
            bool thread_jump(instruction& ins) noexcept {
                bool changed = false;

                for (token* label : jump_labels(ins)) {
                    for (size_t hops = 0; hops < instructions.size(); ++hops) {
                        const instruction& target = instructions[address_of(*label)];

                        if (!vholds(ops::jmp_label, target.ops)) {
                            break;
                        }

                        *label = vget(ops::jmp_label, target.ops).label;
                        changed = true;
                    }
                }

                return changed;
            }

            // O-BR-MERGE-RET: the LAST ret of each kind survives. Jumps only go forward, so a label may only move forward.
            bool merge_returns() noexcept {
                std::map<std::pair<int, uint32_t>, size_t> last;

                for (size_t i = 0; i < instructions.size(); ++i) {
                    if (instructions[i].mnemonic == mnemonics::RET) {
                        last[ret_key(instructions[i])] = i;
                    }
                }

                bool changed = false;

                for (auto& entry : symtab) {
                    const size_t at = static_cast<size_t>(entry.second.address);

                    if (instructions[at].mnemonic != mnemonics::RET) {
                        continue;
                    }

                    const size_t survivor = last[ret_key(instructions[at])];

                    if (survivor != at) {
                        set_address(entry.second, survivor);
                        changed = true;
                    }
                }

                return changed;
            }

            static std::pair<int, uint32_t> ret_key(const instruction& ins) noexcept {
                if (vholds(ops::imm, ins.ops)) {
                    return {0, u32(vget(ops::imm, ins.ops).k)};
                }

                return {1, 0};
            }

            // ------------------------------------------------------------------
            // 5. dead code
            // ------------------------------------------------------------------
            bool dead_code_pass() noexcept {
                const size_t n = instructions.size();
                std::vector<bool> reached(n, false);

                reached[0] = true;

                for (size_t i = 0; i < n; ++i) {
                    if (!reached[i]) {
                        continue;
                    }

                    for (size_t next : successors(i)) {
                        reached[next] = true;
                    }
                }

                m_del.assign(n, false);

                bool changed = false;

                // O-DCE-UNREACHABLE
                for (size_t i = 0; i < n; ++i) {
                    if (!reached[i]) {
                        m_del[i] = true;
                        changed = true;
                    }
                }

                apply_deletions();

                // O-DCE-LABELS
                if (drop_unused_labels()) {
                    changed = true;
                }

                return changed;
            }

            bool drop_unused_labels() noexcept {
                std::unordered_set<std::string> used;

                for (instruction& ins : instructions) {
                    for (const token* label : jump_labels(ins)) {
                        used.insert(label->value);
                    }
                }

                bool changed = false;

                for (auto it = symtab.begin(); it != symtab.end();) {
                    if (used.find(it->first) == used.end()) {
                        it = symtab.erase(it);
                        changed = true;
                    }
                    else {
                        ++it;
                    }
                }

                return changed;
            }

            // ------------------------------------------------------------------
            // Utils
            // ------------------------------------------------------------------

            // O-SYMTAB: erases the marked instructions, a label that pointed at one now points at the next survivor
            void apply_deletions() noexcept {
                const size_t n = instructions.size();
                std::vector<size_t> before(n + 1, 0);  // before[a]: marked instructions below address a
                size_t removed = 0;

                for (size_t i = 0; i < n; ++i) {
                    before[i] = removed;

                    if (m_del[i]) {
                        ++removed;
                    }
                }

                before[n] = removed;

                if (removed == 0) {
                    return;
                }

                for (auto& entry : symtab) {
                    const size_t at = static_cast<size_t>(entry.second.address);
                    set_address(entry.second, at - before[at]);
                }

                instructions_t kept;
                kept.reserve(n - removed);

                for (size_t i = 0; i < n; ++i) {
                    if (!m_del[i]) {
                        kept.push_back(std::move(instructions[i]));
                    }
                }

                instructions = std::move(kept);
            }

            // O-BLOCKS: an instruction that is a jump target starts a block, so does the one after a jump or a ret
            void compute_leaders() noexcept {
                const size_t n = instructions.size();

                m_leader.assign(n, false);
                m_leader[0] = true;

                for (size_t i = 0; i < n; ++i) {
                    for (const token* label : jump_labels(instructions[i])) {
                        m_leader[address_of(*label)] = true;
                    }

                    if ((is_jump(instructions[i]) || instructions[i].mnemonic == mnemonics::RET) && i + 1 < n) {
                        m_leader[i + 1] = true;
                    }
                }
            }

            // where execution can go after instruction 'i'
            std::vector<size_t> successors(size_t i) noexcept {
                instruction& ins = instructions[i];
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

            // the program is sound: it ends with a ret and every jump goes forward to an existing instruction
            bool check_program(const std::string& prefix) noexcept {
                if (instructions.empty()) {
                    report(prefix + ": program has no instructions");
                    return false;
                }

                if (instructions.back().mnemonic != mnemonics::RET) {
                    report(prefix + ": last instruction is not a ret instruction");
                    return false;
                }

                for (size_t i = 0; i < instructions.size(); ++i) {
                    for (const token* label : jump_labels(instructions[i])) {
                        const auto it = symtab.find(label->value);

                        if (it == symtab.end()) {
                            report(prefix + ": undefined label '" + label->value + "'");
                            return false;
                        }

                        const size_t target = static_cast<size_t>(it->second.address);

                        if (target <= i || target >= instructions.size()) {
                            report(prefix + ": the jump to label '" + label->value + "' does not go forward to an existing instruction");
                            return false;
                        }
                    }
                }

                return true;
            }

            // debug builds check after every pass that the jumps are still sound
            bool pass_is_sound() noexcept {
                #ifndef NDEBUG
                return check_program("internal error");
                #else
                return true;
                #endif
            }

            // O-REVALIDATE: a program that was valid must still be valid
            void revalidate(bool was_valid) noexcept {
                #ifndef NDEBUG
                if (!was_valid) {
                    return;
                }

                Validator after(instructions, symtab);

                if (after.validate()) {
                    return;
                }

                for (const auto& entry : after.logs()) {
                    if (entry.type == log_type::ERROR) {
                        report("internal error: the optimized program is invalid: " + entry.msg);
                        return;
                    }
                }
                #else
                (void)was_valid;
                #endif
            }

            static bool was_valid_or_true(bool v) noexcept {
                return v;
            }

            size_t address_of(const token& label) const noexcept {
                return static_cast<size_t>(symtab.find(label.value)->second.address);
            }

            static void set_address(symbol& sym, size_t address) noexcept {
                sym.address = static_cast<decltype(sym.address)>(address);
            }

            static std::vector<token*> jump_labels(instruction& ins) noexcept {
                std::vector<token*> labels;

                if (vholds(ops::jmp_label, ins.ops)) {
                    labels.push_back(&vget(ops::jmp_label, ins.ops).label);
                }

                else if (vholds(ops::jmp_cond, ins.ops)) {
                    auto& op = vget(ops::jmp_cond, ins.ops);

                    labels.push_back(&op.lt);

                    if (op.lf.has_value()) {
                        labels.push_back(&*op.lf);
                    }
                }

                return labels;
            }

            static bool is_jump(const instruction& ins) noexcept {
                return vholds(ops::jmp_label, ins.ops) || vholds(ops::jmp_cond, ins.ops);
            }

            static uint32_t u32(int64_t v) noexcept {
                return static_cast<uint32_t>(v);
            }

            static ops::imm make_imm(uint32_t v) noexcept {
                return ops::imm{static_cast<int64_t>(v)};
            }

            static instruction load_imm(uint32_t v, const token& tok) noexcept {
                return instruction{mnemonics::LD, make_imm(v), tok};
            }

            static bool is_pow2(uint32_t v) noexcept {
                return v != 0 && (v & (v - 1)) == 0;
            }

            static uint32_t log2_of(uint32_t v) noexcept {
                uint32_t n = 0;

                while (v > 1) {
                    v >>= 1;
                    ++n;
                }

                return n;
            }

            // the scratch slot, -1 when it is out of range (the validator reports it, the optimizer leaves it alone)
            static int slot_of(int64_t k) noexcept {
                if (k < 0 || k >= data::SC_MEM_SIZE) {
                    return -1;
                }

                return static_cast<int>(k);
            }

            static uint32_t slot_bit(int slot) noexcept {
                return 1u << slot;
            }

            static bool slot_equals_a(const value_state& st, int k) noexcept {
                return k >= 0 && (st.a_slot == k || (st.eq && st.x_slot == k));
            }

            static bool slot_equals_x(const value_state& st, int k) noexcept {
                return k >= 0 && (st.x_slot == k || (st.eq && st.a_slot == k));
            }

            static load_key a_key(const instruction& ins) noexcept {
                if (vholds(ops::pkt_abs, ins.ops)) {
                    return load_key{1, ins.mnemonic, vget(ops::pkt_abs, ins.ops).k};
                }

                if (vholds(ops::ext, ins.ops) && vget(ops::ext, ins.ops).kind == extension::Len) {
                    return load_key{2, mnemonics::LD, 0};
                }

                return load_key{};
            }

            // 'ldxb [k]' and 'ldxb 4*([k]&0xf)' are the same load
            static load_key x_key(const instruction& ins) noexcept {
                if (vholds(ops::msh, ins.ops)) {
                    return load_key{3, mnemonics::LDXB, vget(ops::msh, ins.ops).k};
                }

                if (ins.mnemonic == mnemonics::LDXB && vholds(ops::pkt_abs, ins.ops)) {
                    return load_key{3, mnemonics::LDXB, vget(ops::pkt_abs, ins.ops).k};
                }

                if (vholds(ops::ext, ins.ops) && vget(ops::ext, ins.ops).kind == extension::Len) {
                    return load_key{2, mnemonics::LDX, 0};
                }

                return load_key{};
            }

            static bool is_identity(mnemonics mn, uint32_t b) noexcept {
                switch (mn) {
                    case mnemonics::ADD:
                    case mnemonics::SUB:
                    case mnemonics::OR:
                    case mnemonics::XOR:
                    case mnemonics::LSH:
                    case mnemonics::RSH:
                        return b == 0;

                    case mnemonics::MUL:
                    case mnemonics::DIV:
                        return b == 1;

                    case mnemonics::AND:
                        return b == 0xffffffff;

                    default:
                        return false;
                }
            }

            // 32-bit wraparound arithmetic. A zero divisor and a shift of 32 or more are never folded.
            static std::optional<uint32_t> alu_eval(mnemonics mn, uint32_t a, uint32_t b) noexcept {
                switch (mn) {
                    case mnemonics::ADD:
                        return a + b;

                    case mnemonics::SUB:
                        return a - b;

                    case mnemonics::MUL:
                        return a * b;

                    case mnemonics::DIV:
                        if (b == 0) {
                            return std::nullopt;
                        }

                        return a / b;

                    case mnemonics::MOD:
                        if (b == 0) {
                            return std::nullopt;
                        }

                        return a % b;

                    case mnemonics::AND:
                        return a & b;

                    case mnemonics::OR:
                        return a | b;

                    case mnemonics::XOR:
                        return a ^ b;

                    case mnemonics::LSH:
                        if (b >= 32) {
                            return std::nullopt;
                        }

                        return a << b;

                    case mnemonics::RSH:
                        if (b >= 32) {
                            return std::nullopt;
                        }

                        return a >> b;

                    default:
                        return std::nullopt;
                }
            }

            static bool is_ld(mnemonics mn) noexcept {
                return mn == mnemonics::LD || mn == mnemonics::LDI || mn == mnemonics::LDH || mn == mnemonics::LDB;
            }

            static bool is_ldx(mnemonics mn) noexcept {
                return mn == mnemonics::LDX || mn == mnemonics::LDXI || mn == mnemonics::LDXB;
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

            // what an instruction reads and writes
            static effects effects_of(const instruction& ins) noexcept {
                effects e;
                const mnemonics mn = ins.mnemonic;

                if (vholds(ops::jmp_cond, ins.ops)) {
                    e.use_a = true;
                    e.use_x = vholds(ops::reg_x, vget(ops::jmp_cond, ins.ops).rhs);
                    return e;
                }

                if (is_alu(mn)) {
                    e.use_a = true;
                    e.def_a = true;
                    e.use_x = vholds(ops::reg_x, ins.ops);
                    return e;
                }

                if (is_ld(mn)) {
                    e.def_a = true;
                    e.use_x = vholds(ops::pkt_ind, ins.ops);

                    if (vholds(ops::scratch, ins.ops)) {
                        e.use_slot = slot_of(vget(ops::scratch, ins.ops).k);
                    }

                    return e;
                }

                if (is_ldx(mn)) {
                    e.def_x = true;

                    if (vholds(ops::scratch, ins.ops)) {
                        e.use_slot = slot_of(vget(ops::scratch, ins.ops).k);
                    }

                    return e;
                }

                switch (mn) {
                    case mnemonics::ST:
                    case mnemonics::STX:
                        e.use_a = mn == mnemonics::ST;
                        e.use_x = mn == mnemonics::STX;

                        if (vholds(ops::scratch, ins.ops)) {
                            e.def_slot = slot_of(vget(ops::scratch, ins.ops).k);
                        }

                        return e;

                    case mnemonics::TAX:
                        e.use_a = true;
                        e.def_x = true;
                        return e;

                    case mnemonics::TXA:
                        e.use_x = true;
                        e.def_a = true;
                        return e;

                    case mnemonics::RET:
                        e.use_a = vholds(ops::reg_a, ins.ops);
                        return e;

                    default:
                        return e;
                }
            }
    };
    

    #undef vholds
    #undef vget
} // namespace anvil::s

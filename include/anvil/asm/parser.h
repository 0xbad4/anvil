#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include "lexer.h"
#include "ast.h"
#include "detail/modes.h"

namespace anvil::s
{
    class Parser : public BaseObj {
        public:
            Parser(const tokens_t& toks) : m_address(0), m_tok_idx(0), m_tokens(toks) { }

            bool parse() noexcept {
                while (valid()) {
                    parse_line();
                }

                return ok();
            }

            /**
             * @brief Returns the vector @c instructions_t of parsed instructions (the AST)
             */
            const instructions_t& instructions() const noexcept {
                return m_ins;
            }

            /**
             * @brief Returns the @c symtab_t of labels, label name -> instruction address
             */
            const symtab_t& symtab() const noexcept {
                return m_symtab;
            }

        protected:
            struct parsed_operand {
                operand   op;
                addr_mode mode;
            };

            symtab_t  m_symtab;
            size_t    m_address;
            size_t    m_tok_idx;
            const tokens_t& m_tokens;
            instructions_t m_ins;

            void insert(const instruction& ins) noexcept {
                // insert current and increment address
                m_ins.push_back(ins);

                m_address++;  // now points to next instruction
            }

            void isym(std::string label) noexcept {
                auto [it, inserted] = m_symtab.insert({ label, { peek(), m_address } });

                if (!inserted) {
                    report("label '" + label + "' is already defined at line " + std::to_string(it->second.tok.pos.line + 1));
                }
            }

            // ------------------- Parsers -------------------
            void parse_line() noexcept {
                // handle multiple labels on a line
                // label1: label2: instruction
                while (peek().type == token_type::IDENTIFIER && peek(1).type == token_type::COLON) {
                    // record symbol/label
                    isym(peek().value);

                    // skip label and colon
                    advance();
                    advance();
                }

                if (peek().type != token_type::EOL && peek().type != token_type::EOF_) {
                    // the first error of an instruction stops it, skip to the next line
                    if (!parse_instruction()) {  // NOTE: does not consume new line
                        skip_line();
                        return;
                    }
                }

                // NOTE: after parsing an instruction, new line or end of file expected, otherwise ERROR
                if (peek().type != token_type::EOL && peek().type != token_type::EOF_) {
                    report("unexpected '" + peek().value + "' after instruction");
                }

                skip_line();   // on a clean line this just eats the EOL
            }

            bool parse_instruction() noexcept {
                // expect mnemonic otherwise ERROR
                if (peek().type != token_type::MNEMONIC) {
                    report("expected an instruction, found '" + peek().value + "'");
                    return false;
                }

                const token mnt = peek();  // copy, the parser moves on after advance()
                
                advance();

                // the lexer keeps the original case of mnemonics, the table is lowercase
                std::string lower = mnt.value;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                auto it = data::MNEMONICS.find(lower);

                if (it == data::MNEMONICS.end()) {
                    report("unknown mnemonic '" + mnt.value + "'", mnt);
                    return false;
                }

                const mnemonics mn = it->second;

                const token operand_token = peek();  // first operand token, for error positions
                const uint32_t errors_before = errc();

                parsed_operand operand = parse_operand();

                // operand parser reported errors
                if (errc() > errors_before) {
                    return false;
                }

                // is this operand form legal for this mnemonic?
                const uint16_t allowed = utils::allowed_modes(mn);

                if (!(allowed & detail::bit(operand.mode))) {
                    if (operand.mode == addr_mode::None) {
                        report("'" + mnt.value + "' is missing its operand; it takes: " + utils::list_modes(allowed), mnt);
                    }

                    else if (allowed == detail::M_NONE) {
                        report("'" + mnt.value + "' takes no operand", mnt);
                    }

                    else if (mn == mnemonics::LDX && (operand.mode == addr_mode::PktAbs || operand.mode == addr_mode::PktInd)) {
                        report("'ldx' has no packet-offset form; did you mean 'ldxb' or 'ldb' ?");
                    }
                    else {
                        report("'" + mnt.value + "' does not accept " + utils::describe_mode(operand.mode) + "; it takes: " + utils::list_modes(allowed));
                    }
                    return false;
                }

                // ldx only supports the 'len' extension
                if (mn == mnemonics::LDX && operand.mode == addr_mode::Ext && std::get<ops::ext>(operand.op).kind != extension::Len) {
                    report("'ldx' only supports the 'len' extension");
                    return false;
                }

                insert(instruction{mn, std::move(operand.op), mnt});
                return true;
            }

            parsed_operand parse_operand() noexcept {
                const token& tok = peek();

                switch (tok.type) {
                    case token_type::EOL:
                    case token_type::EOF_:
                        return {ops::no_operand{}, addr_mode::None};

                    case token_type::REG_X:
                        return parse_register_x();

                    case token_type::REG_A:
                        advance();
                        return {ops::reg_a{}, addr_mode::RegA};

                    case token_type::NUMBER:
                        return parse_number();

                    case token_type::LBRACKET:
                        return parse_packet();

                    case token_type::KW_M:
                        return parse_scratch();

                    case token_type::IDENTIFIER:  // unconditional jump
                        advance();
                        return {ops::jmp_label{tok}, addr_mode::Label};

                    case token_type::EXT:
                        return parse_extension();
                        
                    default:  // handle token_type::UNKNOWN
                        report("unexpected '" + tok.value + "' where an operand was expected");
                        return {};
                }
            }

            parsed_operand parse_number() noexcept {
                // SUPPORTED: K, -K, 0xK, #K, #-K

                if (peek(1).type == token_type::STAR) {
                    // MSH `4*([k]&0xf)`
                    return parse_msh();
                }

                // convert BEFORE advancing, so an error points at the number itself
                int64_t k = 0;

                if (!utils::to_int(peek().value, k)) {
                    report("number '" + peek().value + "' is out of range");
                    return {};
                }

                advance();  // consume number

                ops::imm imm{k};

                // 1. not followed by a comma -> immediate (mode 4)
                if (peek().type != token_type::COMMA) {
                    return {imm, addr_mode::Imm};
                }

                // 2. followed by a comma -> conditional jump (mode 9 or 7)
                return parse_condition_tail(imm, addr_mode::CondImmT, addr_mode::CondImmTF);
            }

            parsed_operand parse_msh() noexcept {
                // 4*([k]&0xf)  (mode 5)
                // The 4 and the 0xf are fixed by the instruction, only k is a real value.

                int64_t multiplier = 0;
                int64_t mask = 0;
                int64_t k = 0;

                // check each constant BEFORE consuming it, so the error points at that token
                if (!utils::to_int(peek().value, multiplier) || multiplier != 4) {
                    report("only 4*([k]&0xf) is supported; the multiplier must be 4");
                    return {};
                }

                advance();  // consume '4'

                if (!expect(token_type::STAR, "'*' in MSH operand")) {
                    return {};
                }

                if (!expect(token_type::LPARENTH, "'(' in MSH operand")) {
                    return {};
                }

                if (!expect(token_type::LBRACKET, "'[' in MSH operand")) {
                    return {};
                }

                const token& offset = peek();

                if (!expect(token_type::NUMBER, "a packet offset in MSH operand")) {
                    return {};
                }

                if (!expect(token_type::RBRACKET, "']' in MSH operand")) {
                    return {};
                }

                if (!expect(token_type::AMP, "'&' in MSH operand")) {
                    return {};
                }

                if (peek().type == token_type::NUMBER && (!utils::to_int(peek().value, mask) || mask != 0xF)) {
                    report("only 4*([k]&0xf) is supported; the mask must be 0xf");
                    return {};
                }

                if (!expect(token_type::NUMBER, "the mask 0xf in MSH operand")) {
                    return {};
                }

                if (!expect(token_type::RPARENTH, "')' in MSH operand")) {
                    return {};
                }

                if (!utils::to_int(offset.value, k)) {
                    report("number '" + offset.value + "' is out of range");
                    return {};
                }

                return {ops::msh{k}, addr_mode::Msh};
            }
           
            parsed_operand parse_register_x() noexcept {
                advance(); // consume REG_X

                // Not followed by a comma, so it's just the register X operand (arithmetic)
                if (peek().type != token_type::COMMA) {
                    return {ops::reg_x{}, addr_mode::RegX};
                }

                // Followed by a comma, so it's a conditional jump
                return parse_condition_tail(ops::reg_x{}, addr_mode::CondXT, addr_mode::CondXTF);
            }

            parsed_operand parse_condition_tail(std::variant<ops::imm, ops::reg_x> rhs, addr_mode t1, addr_mode t2) noexcept {
                /*
                    if (A <op> X)
                        goto true_label
                    else
                        goto false_label    # if set, otherwise continue to the next instruction
                */

                // prefixed with 'X' or '#k' (immediate)
                if (!expect(token_type::COMMA, "','")) {
                    return {};
                }

                ops::jmp_cond cj;
                addr_mode mode = t1;

                cj.rhs = rhs;

                std::optional<token> lt = get_symbol();

                if (!lt) {
                    return {};
                }

                cj.lt = *lt;

                if (accept(token_type::COMMA)) {
                    std::optional<token> lf = get_symbol();

                    if (!lf) {
                        return {};
                    }

                    cj.lf = lf;
                    mode = t2;
                }

                return {std::move(cj), mode};
            }

            parsed_operand parse_packet() noexcept {
                // [ k ]   (mode 1)        [ x + k ]   (mode 2)
                advance(); // consume '['

                int64_t k = 0;
                bool indexed = false;

                // mode 1
                if (peek().type == token_type::NUMBER) {
                    if (!utils::to_int(peek().value, k)) {
                        report("number '" + peek().value + "' is out of range");
                        return {};
                    }
                    advance(); // consume number
                }

                // mode 2
                else if (peek().type == token_type::REG_X) {
                    advance(); // consume 'x'
                    indexed = true;

                    if (!expect(token_type::PLUS, "'+'")) {
                        return {};
                    }

                    if (peek().type != token_type::NUMBER) {
                        report("expected a number after '+'");
                        return {};
                    }

                    if (!utils::to_int(peek().value, k)) {
                        report("number '" + peek().value + "' is out of range");
                        return {};
                    }
                    advance(); // consume number
                }

                else {
                    report("expected a number or 'x' after '['");
                    return {};
                }

                if (!expect(token_type::RBRACKET, "']'")) {
                    return {};
                }

                if (indexed) {
                    return {ops::pkt_ind{k}, addr_mode::PktInd};
                }

                return {ops::pkt_abs{k}, addr_mode::PktAbs};
            }

            parsed_operand parse_scratch() noexcept {
                // M [ k ]   (mode 3)
                advance(); // consume 'M'

                if (!expect(token_type::LBRACKET, "'[' after M")) {
                    return {};
                }

                if (peek().type != token_type::NUMBER) {
                    report("expected a number after '['");
                    return {};
                }

                int64_t k = 0;

                if (!utils::to_int(peek().value, k)) {
                    report("number '" + peek().value + "' is out of range");
                    return {};
                }

                advance(); // consume number

                if (!expect(token_type::RBRACKET, "']'")) {
                    return {};
                }

                return {ops::scratch{k}, addr_mode::Scratch};
            }

            parsed_operand parse_extension() noexcept {
                // #name  (mode 12)
                auto it = data::EXTENSIONS.find(peek().value);

                if (it == data::EXTENSIONS.end()) {
                    report("unknown BPF extension '" + peek().value + "'");  // before advance(), so it points at the name
                    return {};
                }

                advance();
                return {ops::ext{it->second}, addr_mode::Ext};
            }

            // ------------------- Cursor -------------------
            void advance() noexcept {
                if (m_tok_idx + 1 < m_tokens.size()) {
                    ++m_tok_idx;
                }
            }

            bool accept(token_type t) {
                if (peek().type == t)
                {
                    advance();
                    return true;
                }
                return false;
            }

            const token &peek(size_t n = 0) const noexcept {
                return m_tokens[std::min(m_tok_idx + n, m_tokens.size() - 1)];
            }

            bool expect(token_type t, const char *what) noexcept{
                const token &cur = peek();

                if (cur.type != t) {
                    report(std::string("expected ") + what + ", found '" + cur.value + "'");
                    return false;
                }

                advance(); // consume expected token
                return true;
            }

            void skip_line() noexcept {
                while (peek().type != token_type::EOL && peek().type != token_type::EOF_) {
                    advance();
                }

                if (peek().type == token_type::EOL) {
                    advance();
                }
            }

            // ------------------- Utils -------------------
            bool valid() noexcept {
                return (m_tok_idx < m_tokens.size()) && (peek().type != token_type::EOF_);
            }
            
            std::optional<token> get_symbol() noexcept {
                const token &t = peek();

                if (!expect(token_type::IDENTIFIER, "a label")) {
                    return std::nullopt;
                }

                return t;
            }

            void report(const std::string& msg, log_type type = log_type::ERROR) noexcept {
                BaseObj::report(msg, peek().pos, type);
            }

            void report(const std::string& msg, token t, log_type type = log_type::ERROR) noexcept {
                BaseObj::report(msg, t.pos, type);
            }
    };
} // namespace anvil

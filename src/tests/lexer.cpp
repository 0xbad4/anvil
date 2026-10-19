#include <fmt/core.h>
#include <fmt/color.h>
#include "anvil/asm/lexer.h"
#include "test_util.h"

static const std::string target_asm = R"(
; comment-only line, then blank lines: no tokens at all

    ld [0x10]             ; hex
    ld [0XfF]             ; uppercase X, mixed-case hex digits
    ld [0]                ; plain zero, not the start of 0x
    ld [7]                ; single digit
    ld [255]              ; bare decimal
    ldx len               ; len keyword
    ldx LEN               ; keywords are case-insensitive, value keeps its case
    ld #len               ; '#' makes it an extension, so EXT and not KW_LEN
    ld m[0x30]            ; memory keyword
    ld M[3]               ; uppercase M
    ldx #42               ; decimal with hash
    ldx #0                ; hash + zero
    ldx #-42              ; signed decimal with hash
    add -2                ; signed decimal
    add x                 ; register
    add %x                ; register with percent sign
    add X                 ; uppercase register
    ret a
    ret %a
    ret A
    st [x+0xA]            ; indexed
    ldh [x + 4]           ; spaces around '+'
    ld #proto             ; extension
    ld proto              ; no '#': just a label name
    ld #PROTO             ; extension, case of the name is kept
    ld #vlan_tci          ; extension with an underscore
    ja proto              ; a label that happens to share an extension's name
    jeq #80, tcp, other   ; two targets
    jgt x, tcp            ; register + one target
    jmp end
    JA end                ; mnemonics are case-insensitive
tcp: other: L_1:          ; three labels, no instruction
    len2: ret #-1         ; label + instruction on one line, keyword-like prefix
    end:                  ; label alone
        ret a             ; trailing comment on the last line
)";

// NOTE: only type and value matter
static const anvil::s::tokens_t expected_tokens = {
    // ld [0x10]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "0x10"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ld [0XfF]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "0XfF"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ld [0]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "0"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ld [7]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "7"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ld [255]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "255"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ldx len
    {anvil::s::token_type::MNEMONIC, {}, "ldx"},
    {anvil::s::token_type::EXT, {}, "len"},
    {anvil::s::token_type::EOL, {}, ""},

    // ldx LEN
    {anvil::s::token_type::MNEMONIC, {}, "ldx"},
    {anvil::s::token_type::EXT, {}, "LEN"},
    {anvil::s::token_type::EOL, {}, ""},

    // ld #len
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::EXT, {}, "len"},
    {anvil::s::token_type::EOL, {}, ""},

    // ld m[0x30]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::KW_M, {}, "m"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "0x30"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ld M[3]
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::KW_M, {}, "M"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "3"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ldx #42
    {anvil::s::token_type::MNEMONIC, {}, "ldx"},
    {anvil::s::token_type::NUMBER, {}, "#42"},
    {anvil::s::token_type::EOL, {}, ""},

    // ldx #0
    {anvil::s::token_type::MNEMONIC, {}, "ldx"},
    {anvil::s::token_type::NUMBER, {}, "#0"},
    {anvil::s::token_type::EOL, {}, ""},

    // ldx #-42
    {anvil::s::token_type::MNEMONIC, {}, "ldx"},
    {anvil::s::token_type::NUMBER, {}, "#-42"},
    {anvil::s::token_type::EOL, {}, ""},

    // add -2
    {anvil::s::token_type::MNEMONIC, {}, "add"},
    {anvil::s::token_type::NUMBER, {}, "-2"},
    {anvil::s::token_type::EOL, {}, ""},

    // add x
    {anvil::s::token_type::MNEMONIC, {}, "add"},
    {anvil::s::token_type::REG_X, {}, "x"},
    {anvil::s::token_type::EOL, {}, ""},

    // add %x
    {anvil::s::token_type::MNEMONIC, {}, "add"},
    {anvil::s::token_type::REG_X, {}, "%x"},
    {anvil::s::token_type::EOL, {}, ""},

    // add X
    {anvil::s::token_type::MNEMONIC, {}, "add"},
    {anvil::s::token_type::REG_X, {}, "X"},
    {anvil::s::token_type::EOL, {}, ""},

    // ret a
    {anvil::s::token_type::MNEMONIC, {}, "ret"},
    {anvil::s::token_type::REG_A, {}, "a"},
    {anvil::s::token_type::EOL, {}, ""},

    // ret %a
    {anvil::s::token_type::MNEMONIC, {}, "ret"},
    {anvil::s::token_type::REG_A, {}, "%a"},
    {anvil::s::token_type::EOL, {}, ""},

    // ret A
    {anvil::s::token_type::MNEMONIC, {}, "ret"},
    {anvil::s::token_type::REG_A, {}, "A"},
    {anvil::s::token_type::EOL, {}, ""},

    // st [x+0xA]
    {anvil::s::token_type::MNEMONIC, {}, "st"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::REG_X, {}, "x"},
    {anvil::s::token_type::PLUS, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "0xA"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ldh [x + 4]
    {anvil::s::token_type::MNEMONIC, {}, "ldh"},
    {anvil::s::token_type::LBRACKET, {}, ""},
    {anvil::s::token_type::REG_X, {}, "x"},
    {anvil::s::token_type::PLUS, {}, ""},
    {anvil::s::token_type::NUMBER, {}, "4"},
    {anvil::s::token_type::RBRACKET, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ld #proto
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::EXT, {}, "proto"},
    {anvil::s::token_type::EOL, {}, ""},

    // ld proto
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::IDENTIFIER, {}, "proto"},
    {anvil::s::token_type::EOL, {}, ""},

    // ld #PROTO
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::EXT, {}, "PROTO"},
    {anvil::s::token_type::EOL, {}, ""},

    // ld #vlan_tci
    {anvil::s::token_type::MNEMONIC, {}, "ld"},
    {anvil::s::token_type::EXT, {}, "vlan_tci"},
    {anvil::s::token_type::EOL, {}, ""},

    // ja proto
    {anvil::s::token_type::MNEMONIC, {}, "ja"},
    {anvil::s::token_type::IDENTIFIER, {}, "proto"},
    {anvil::s::token_type::EOL, {}, ""},

    // jeq #80, tcp, other
    {anvil::s::token_type::MNEMONIC, {}, "jeq"},
    {anvil::s::token_type::NUMBER, {}, "#80"},
    {anvil::s::token_type::COMMA, {}, ""},
    {anvil::s::token_type::IDENTIFIER, {}, "tcp"},
    {anvil::s::token_type::COMMA, {}, ""},
    {anvil::s::token_type::IDENTIFIER, {}, "other"},
    {anvil::s::token_type::EOL, {}, ""},

    // jgt x, tcp
    {anvil::s::token_type::MNEMONIC, {}, "jgt"},
    {anvil::s::token_type::REG_X, {}, "x"},
    {anvil::s::token_type::COMMA, {}, ""},
    {anvil::s::token_type::IDENTIFIER, {}, "tcp"},
    {anvil::s::token_type::EOL, {}, ""},

    // jmp end
    {anvil::s::token_type::MNEMONIC, {}, "jmp"},
    {anvil::s::token_type::IDENTIFIER, {}, "end"},
    {anvil::s::token_type::EOL, {}, ""},

    // JA end
    {anvil::s::token_type::MNEMONIC, {}, "JA"},
    {anvil::s::token_type::IDENTIFIER, {}, "end"},
    {anvil::s::token_type::EOL, {}, ""},

    // tcp: other: L_1:
    {anvil::s::token_type::IDENTIFIER, {}, "tcp"},
    {anvil::s::token_type::COLON, {}, ""},
    {anvil::s::token_type::IDENTIFIER, {}, "other"},
    {anvil::s::token_type::COLON, {}, ""},
    {anvil::s::token_type::IDENTIFIER, {}, "L_1"},
    {anvil::s::token_type::COLON, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // len2: ret #-1
    {anvil::s::token_type::IDENTIFIER, {}, "len2"},
    {anvil::s::token_type::COLON, {}, ""},
    {anvil::s::token_type::MNEMONIC, {}, "ret"},
    {anvil::s::token_type::NUMBER, {}, "#-1"},
    {anvil::s::token_type::EOL, {}, ""},

    // end:
    {anvil::s::token_type::IDENTIFIER, {}, "end"},
    {anvil::s::token_type::COLON, {}, ""},
    {anvil::s::token_type::EOL, {}, ""},

    // ret a
    {anvil::s::token_type::MNEMONIC, {}, "ret"},
    {anvil::s::token_type::REG_A, {}, "a"},
    {anvil::s::token_type::EOL, {}, ""},

    {anvil::s::token_type::EOF_, {}, ""}
};

void assert_tokens(const anvil::s::tokens_t &actual)
{
    if (actual.size() != expected_tokens.size()) {
        fmt::print(
            "{} {}: expected {}, got {}\n",
            fmt::styled("[-]", fmt::fg(fmt::color::red)),
            fmt::styled("Token count mismatch", fmt::fg(fmt::color::red)),
            fmt::styled(expected_tokens.size(), fmt::fg(fmt::color::green)),
            fmt::styled(actual.size(), fmt::fg(fmt::color::red)));
        return;
    }

    bool success = true;

    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto &act = actual[i];
        const auto &exp = expected_tokens[i];

        if (act.type != exp.type || act.value != exp.value) {
            success = false;

            const bool value_changed = act.value != exp.value;
            const bool type_changed = act.type != exp.type;

            fmt::print(
                "{} | expected [{}, type@{}] got [{}, type@{}]\n",

                // LINE
                fmt::styled(
                    fmt::format("{:>3}", act.pos.line),
                    fmt::fg(fmt::color::yellow)),

                // expected
                fmt::styled(
                    exp.value,
                    fmt::fg(value_changed
                                ? fmt::color::red
                                : fmt::color::light_green)),

                fmt::styled(
                    static_cast<int>(exp.type),
                    fmt::fg(type_changed
                                ? fmt::color::red
                                : fmt::color::light_green)),

                // got
                fmt::styled(
                    act.value,
                    fmt::fg(value_changed
                                ? fmt::color::red
                                : fmt::color::light_green)),

                fmt::styled(
                    static_cast<int>(act.type),
                    fmt::fg(type_changed
                                ? fmt::color::red
                                : fmt::color::light_green)));
        }
    }

    if (success) {
        fmt::print(
            "{} {}\n",
            fmt::styled("[+]", fmt::fg(fmt::color::green)),
            fmt::styled(
                fmt::format("All {} tokens match", actual.size()),
                fmt::fg(fmt::color::green)));
    }
}


int main() {
    anvil::s::source source{target_asm.data(), target_asm.size()};
    anvil::s::Lexer lexer(source);
    bool okay = lexer.tokenize();

    if (!okay) {
        anvil_test::print_logs(lexer.logs(), source);
        return 1;
    }

    anvil_test::assert_tokens(lexer.tokens(), expected_tokens);

    return 0;
}

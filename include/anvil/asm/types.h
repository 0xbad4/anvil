#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace anvil::s {
    enum class token_type : uint8_t {
        UNKNOWN,    ///< Unknown or invalid token
        NUMBER,     ///< Numeric literal (k, #-k, xk)
        IDENTIFIER, ///< Variable or label identifier
        REG_A,      ///< Register A operand (appears in 'ret a')
        REG_X,      ///< Register X operand (used in 'add x', 'jeq x', 'ld [x+k]')
        KW_M,       ///< Keyword M for scratch-memory access (M[k])
        LBRACKET,   ///< Left bracket '['
        RBRACKET,   ///< Right bracket ']'
        LPARENTH,   ///< Left parenthesis '('
        RPARENTH,   ///< Right parenthesis ')'
        COMMA,      ///< Comma ',' separating jump targets
        COLON,      ///< Colon ':' for label definition
        PLUS,       ///< Plus '+' for address offsets ([x+k])
        STAR,       ///< Star '*' for multiplication ([x*k])
        AMP,        ///< Ampersand '&' for bitwise AND ([x&k])
        MNEMONIC,   ///< Mnemonic instruction (e.g., 'ld', 'st', 'add')
        EXT,        ///< Extension keyword (e.g., 'proto', 'type', 'poff')
        EOL,        ///< End of line marker
        EOF_,       ///< End of file marker
    };

    struct __attribute__((packed)) token_pos {
        uint32_t line;        ///< Line number (0-indexed)
        uint32_t col;         ///< Column number (0-indexed)
    };

    struct __attribute__((packed)) source {
        const char* data;        ///< Pointer to source code buffer
        size_t      size;        ///< Size of source code in bytes
    };

    struct token {
        token_type  type = token_type::UNKNOWN;     ///< Type classification of the token
        token_pos   pos {0, 0};      ///< Source code position of the token
        std::string value;    ///< String representation of the token
    };

    enum class log_type : uint8_t {
        OK,
        WARNING,
        ERROR
    };
    
    struct log {
        log_type          type; ///< Log type associated with the token
        token_pos         pos;  ///< Source code position of the error
        std::string       msg;  ///< Error message describing the issue
    };

    using tokens_t = std::vector<token>;  ///< Vector of tokens
    using logs_t = std::vector<log>;  ///< Vector of logs
}
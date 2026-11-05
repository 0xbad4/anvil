#include <fmt/color.h>
#include <fmt/core.h>

#include "anvil/asm/lexer.h"
#include "anvil/asm/parser.h"
#include "anvil/asm/validator.h"
#include "test_util.h"

namespace s = anvil::s;
namespace t = anvil_test;

struct outcome {
    bool lexed = false;
    s::logs_t lex_errors;
    bool parsed = false;
    s::logs_t parse_errors;
    s::logs_t errors;
};

static s::logs_t only_errors(const s::logs_t &logs) {
    s::logs_t out;

    for (const auto &log : logs)
    {
        if (log.type == s::log_type::ERROR)
        {
            out.push_back(log);
        }
    }

    return out;
}

static outcome run(const std::string &text, s::profile profile = s::profile::SOCKET_FILTER) {
    outcome out;

    s::source src{text.data(), text.size()};
    s::Lexer lexer(src);

    if (!lexer.tokenize())
    {
        out.lex_errors = lexer.logs();
        return out;
    }

    out.lexed = true;

    s::Parser parser(lexer.tokens());
    parser.parse();

    out.parse_errors = parser.logs();
    if (!out.parse_errors.empty())
    {
        return out;
    }

    out.parsed = true;

    s::Validator validator(parser.instructions(), parser.symtab(), profile);
    validator.validate();
    out.errors = validator.logs();
    return out;
}

struct ok_case {
    std::string name;
    std::string source;
};

struct err_case {
    std::string name;
    std::string source;
    s::profile profile = s::profile::SOCKET_FILTER;
    std::vector<std::pair<std::size_t, std::string>> errors;
};

static std::vector<std::string> check_ok(const ok_case &tc) {
    std::vector<std::string> problems;

    const auto out = run(tc.source);
    const s::source src{tc.source.data(), tc.source.size()};

    if (!out.lexed)
    {
        problems.push_back("the lexer failed:\n" + t::format_logs(out.lex_errors, src));
        return problems;
    }

    if (!out.parsed)
    {
        problems.push_back("the parser failed:\n" + t::format_logs(out.parse_errors, src));
        return problems;
    }

    const auto validation_errors = only_errors(out.errors);
    if (!validation_errors.empty()) {
        problems.push_back("unexpected validator errors:\n" + t::format_logs(validation_errors, src));
    }

    return problems;
}

static std::vector<std::string> check_err(const err_case &tc) {
    std::vector<std::string> problems;

    const auto out = run(tc.source, tc.profile);
    const s::source src{tc.source.data(), tc.source.size()};

    if (!out.lexed) {
        problems.push_back("the lexer failed (validator case meant to fail later):\n" +
                           t::format_logs(out.lex_errors, src));
        return problems;
    }

    if (!out.parsed) {
        problems.push_back("the parser failed unexpectedly:\n" + t::format_logs(out.parse_errors, src));
        return problems;
    }

    const auto validation_errors = only_errors(out.errors);

    if (validation_errors.size() != tc.errors.size()) {
        problems.push_back(fmt::format("expected {} error(s), got {}:\n{}",
                                       tc.errors.size(), validation_errors.size(),
                                       t::format_logs(validation_errors, src)));
        return problems;
    }

    for (std::size_t i = 0; i < tc.errors.size(); ++i) {
        const auto &[line, part] = tc.errors[i];
        const auto &got = validation_errors[i];

        if (got.pos.line != line)
        {
            problems.push_back(fmt::format("error {}: expected on line {}, got {}", i, line, got.pos.line));
        }

        if (!t::contains(got.msg, part))
        {
            problems.push_back(fmt::format("error {}: message should contain [{}], got [{}]", i, part, got.msg));
        }
    }

    return problems;
}

static std::vector<ok_case> ok_cases() {
    return {
        {"minimal valid program", "ret #0"},
        {"simple forward jump", R"(start: jmp end
end: ret #0)"},
        {"conditional jump and fallthrough", R"(ld #1
jeq #1, yes, no
yes: ret #0
no: ret #-1)"},
        {"valid scratch flow", R"(ldx #1
st m[0]
ld m[0]
ret #0)"},
        {"valid register use from zero", R"(add #1
ret #0)"},
    };
}

static std::vector<err_case> err_cases() {
    return {
        // {"program must end in ret", "ld #1", s::profile::SOCKET_FILTER,
        //  {{0, "last instruction is not a RET instruction"}}},

        {"undefined jump target", R"(jmp missing
ret #0)", s::profile::SOCKET_FILTER,
         {{0, "undefined jump label"}}},

        {"backward jump rejected", R"(start: ret #0
jmp start)", s::profile::SOCKET_FILTER,
         {{1, "backward jump to label 'start'"}}},

        {"scratch index must be in range", R"(ld m[16]
ret #0)", s::profile::SOCKET_FILTER,
         {{0, "scratch memory index 16 is out of range"}}},

        {"divide by zero rejected", R"(div #0
ret #0)", s::profile::SOCKET_FILTER,
         {{0, "divides by zero"}}},

        {"empty programs rejected", "  ", s::profile::SOCKET_FILTER,
         {{0, "program has no instructions"}}},

        {"seccomp rejects non-word load", R"(ld [x+4]
ret #0)", s::profile::SECCOMP,
         {{0, "not allowed in seccomp filters"}}},

        {"seccomp rejects invalid offset", R"(ld [13]
ret #0)", s::profile::SECCOMP,
         {{0, "must be a multiple of 4 and less than 64"}}},
    };
}

int main() {
    int passed = 0;
    int failed = 0;

    const auto report = [&](const std::string &name, const std::vector<std::string> &problems) {
        t::report(name, problems.empty());

        if (problems.empty()) {
            ++passed;
            return;
        }

        ++failed;

        for (const auto &problem : problems) {
            fmt::print("      {}\n", problem);
        }
    };

    fmt::print("{}\n", fmt::styled("-- must validate --", fmt::emphasis::bold));

    for (const auto &tc : ok_cases()) {
        report(tc.name, check_ok(tc));
    }

    fmt::print("\n{}\n", fmt::styled("-- must be rejected --", fmt::emphasis::bold));

    for (const auto &tc : err_cases()) {
        report(tc.name, check_err(tc));
    }

    return t::finish(passed, failed);
}

#include <map>
#include <iostream>
#include <fmt/color.h>
#include <fmt/core.h>

#include "anvil/asm/lexer.h"
#include "anvil/asm/parser.h"
#include "test_util.h"

namespace s = anvil::s;
namespace t = anvil_test;

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};

template <class... F>
overloaded(F...) -> overloaded<F...>;

// "jneq" and "jne" are one enum value; the shortest spelling wins
static std::string mn_name(s::mnemonics mn)
{
    std::string best;

    for (const auto &[name, value] : s::data::MNEMONICS)
    {
        if (value != mn)
        {
            continue;
        }

        if (best.empty() || name.size() < best.size() || (name.size() == best.size() && name < best))
        {
            best = name;
        }
    }

    return best;
}

static std::string ext_name(s::extension e)
{
    for (const auto &[name, value] : s::data::EXTENSIONS)
    {
        if (value == e)
        {
            return name;
        }
    }

    return "?";
}

static std::string to_text(const s::instruction &ins)
{
    const std::string mn = mn_name(ins.mnemonic);

    return std::visit(
        overloaded{
            [&](const s::ops::no_operand &)
            { return mn; },
            [&](const s::ops::reg_x &)
            { return mn + " x"; },
            [&](const s::ops::reg_a &)
            { return mn + " a"; },
            [&](const s::ops::imm &o)
            { return mn + " #" + std::to_string(o.k); },
            [&](const s::ops::pkt_abs &o)
            { return mn + " [" + std::to_string(o.k) + "]"; },
            [&](const s::ops::pkt_ind &o)
            { return mn + " [x+" + std::to_string(o.k) + "]"; },
            [&](const s::ops::scratch &o)
            { return mn + " M[" + std::to_string(o.k) + "]"; },
            [&](const s::ops::msh &o)
            { return mn + " msh[" + std::to_string(o.k) + "]"; },
            [&](const s::ops::ext &o)
            { return mn + " #" + ext_name(o.kind); },
            [&](const s::ops::jmp_label &o)
            { return mn + " " + o.label.value; },
            [&](const s::ops::jmp_cond &o)
            {
                const std::string rhs = std::visit(
                    overloaded{
                        [](const s::ops::imm &v)
                        { return "#" + std::to_string(v.k); },
                        [](const s::ops::reg_x &)
                        { return std::string("x"); },
                    },
                    o.rhs);

                std::string text = mn + " " + rhs + ", " + o.lt.value;

                if (o.lf)
                {
                    text += ", " + o.lf->value;
                }

                return text;
            },
        },
        ins.ops);
}

struct outcome
{
    bool lexed = false;
    s::logs_t lex_errors;
    std::vector<std::string> instructions;     // canonical text, in order
    std::map<std::string, std::size_t> labels; // name -> instruction index
    s::logs_t errors;                          // parser errors
};

static outcome run(const std::string &text)
{
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

    for (const auto &ins : parser.instructions())
    {
        out.instructions.push_back(to_text(ins));
    }

    for (const auto &[name, sym] : parser.symtab())
    {
        out.labels[name] = sym.address;
    }

    out.errors = parser.logs();
    return out;
}
// ============================ end of AST access ==============================

// -----------------------------------------------------------------------------
// Test case shapes
// -----------------------------------------------------------------------------
struct ok_case
{
    std::string name;
    std::string source;
    std::vector<std::string> instructions;     // canonical text
    std::map<std::string, std::size_t> labels; // label -> instruction index
};

struct err_case
{
    std::string name;
    std::string source;
    // one entry per expected error, in order: {0-based line, text the message must contain}
    std::vector<std::pair<std::size_t, std::string>> errors;
};

// -----------------------------------------------------------------------------
// Checks. Both return the list of problems; empty means the case passed.
// -----------------------------------------------------------------------------
static std::vector<std::string> check_ok(const ok_case &tc)
{
    std::vector<std::string> problems;

    const outcome out = run(tc.source);
    const s::source src{tc.source.data(), tc.source.size()};

    if (!out.lexed)
    {
        problems.push_back("the lexer failed:\n" + t::format_logs(out.lex_errors, src));
        return problems;
    }

    if (!out.errors.empty())
    {
        problems.push_back("unexpected parser errors:\n" + t::format_logs(out.errors, src));
    }

    const std::size_t n = std::max(out.instructions.size(), tc.instructions.size());

    for (std::size_t i = 0; i < n; ++i)
    {
        const std::string expected = i < tc.instructions.size() ? tc.instructions[i] : "<nothing>";
        const std::string got = i < out.instructions.size() ? out.instructions[i] : "<nothing>";

        if (expected != got)
        {
            problems.push_back(fmt::format("instruction {}: expected [{}] got [{}]", i, expected, got));
        }
    }

    for (const auto &[name, index] : tc.labels)
    {
        const auto it = out.labels.find(name);

        if (it == out.labels.end())
        {
            problems.push_back(fmt::format("label '{}': expected at {}, but it is missing", name, index));
        }
        else if (it->second != index)
        {
            problems.push_back(fmt::format("label '{}': expected at {}, got {}", name, index, it->second));
        }
    }

    for (const auto &[name, index] : out.labels)
    {
        if (!tc.labels.contains(name))
        {
            problems.push_back(fmt::format("label '{}' (at {}) was not expected", name, index));
        }
    }

    return problems;
}

static std::vector<std::string> check_err(const err_case &tc)
{
    std::vector<std::string> problems;

    const outcome out = run(tc.source);
    const s::source src{tc.source.data(), tc.source.size()};

    if (!out.lexed)
    {
        problems.push_back("the lexer failed (this case is meant to fail in the parser):\n" +
                           t::format_logs(out.lex_errors, src));
        return problems;
    }

    if (out.errors.size() != tc.errors.size())
    {
        problems.push_back(fmt::format("expected {} error(s), got {}:\n{}",
                                       tc.errors.size(), out.errors.size(), t::format_logs(out.errors, src)));
        return problems;
    }

    for (std::size_t i = 0; i < tc.errors.size(); ++i)
    {
        const auto &[line, part] = tc.errors[i];
        const auto &got = out.errors[i];

        if (got.pos.line != line)
        {
            problems.push_back(fmt::format("error {}: expected on line {}, got line {}", i, line, got.pos.line));
        }

        if (!t::contains(got.msg, part))
        {
            problems.push_back(fmt::format("error {}: message should contain [{}], got [{}]", i, part, got.msg));
        }
    }

    return problems;
}

// -----------------------------------------------------------------------------
// Generated cases: one table drives many lines
// -----------------------------------------------------------------------------
static ok_case alu_case()
{
    ok_case tc{"every ALU mnemonic, register and immediate forms", "", {}, {}};

    for (const char *name : {"add", "sub", "mul", "div", "mod", "and", "or", "xor", "lsh", "rsh"})
    {
        const std::string mn = name;

        tc.source += mn + " x\n" + mn + " #1\n";
        tc.instructions.push_back(mn + " x");
        tc.instructions.push_back(mn + " #1");
    }

    return tc;
}

static ok_case jump_forms_case()
{
    ok_case tc{"every conditional jump in every legal form", "", {}, {}};

    // real jumps: modes 7, 8, 9, 10
    for (const char *name : {"jeq", "jgt", "jge", "jset"})
    {
        const std::string mn = name;

        tc.source += mn + " #1, l1\n" + mn + " x, l1\n" + mn + " #1, l1, l2\n" + mn + " x, l1, l2\n";
        tc.instructions.push_back(mn + " #1, l1");
        tc.instructions.push_back(mn + " x, l1");
        tc.instructions.push_back(mn + " #1, l1, l2");
        tc.instructions.push_back(mn + " x, l1, l2");
    }

    // pseudo jumps exist only in the single-target forms: modes 9, 10
    for (const char *name : {"jne", "jlt", "jle"})
    {
        const std::string mn = name;

        tc.source += mn + " #1, l1\n" + mn + " x, l1\n";
        tc.instructions.push_back(mn + " #1, l1");
        tc.instructions.push_back(mn + " x, l1");
    }

    return tc;
}

static ok_case extension_case()
{
    ok_case tc{"every extension name from the table", "", {}, {}};

    for (const auto &entry : s::data::EXTENSIONS)
    {
        tc.source += "ld #" + entry.first + "\n";
        tc.instructions.push_back("ld #" + entry.first);
    }

    return tc;
}

// -----------------------------------------------------------------------------
// Cases that must parse
// -----------------------------------------------------------------------------
static std::vector<ok_case> ok_cases()
{
    std::vector<ok_case> cases = {
        {
            "load forms",
            R"(ld #7
ldi #8
ld [4]
ldh [x+2]
ldb [9]
ld m[3]
ld #len
ld #proto
ld #vlan_tci
ldx #1
ldx m[2]
ldx len
)",
            {"ld #7", "ldi #8", "ld [4]", "ldh [x+2]", "ldb [9]", "ld M[3]", "ld #len", "ld #proto",
             "ld #vlan_tci", "ldx #1", "ldx M[2]", "ldx #len"},
            {},
        },
        {
            "numbers: hex, mixed case, zero, decimal",
            R"(ld [0x10]
ld [0XfF]
ld [0]
ld [255]
ld [x+0xA]
add -2
add #-5
)",
            {"ld [16]", "ld [255]", "ld [0]", "ld [255]", "ld [x+10]", "add #-2", "add #-5"},
            {},
        },
        {
            // `ldxb [14]` stays a plain [k] in the AST; the encoder gives it the same opcode
            "msh: long forms become msh, the short form stays [k]",
            R"(ldxb 4*([14]&0xf)
ldx 4 * ( [ 14 ] & 0xf )
ldxb [14]
)",
            {"ldxb msh[14]", "ldx msh[14]", "ldxb [14]"},
            {},
        },
        {
            "alu, misc, ret, stores",
            R"(add x
add %x
neg
tax
txa
ret a
ret %a
ret #0
ret #-1
st m[0]
stx m[15]
)",
            {"add x", "add x", "neg", "tax", "txa", "ret a", "ret a", "ret #0", "ret #-1", "st M[0]", "stx M[15]"},
            {},
        },
        {
            "jumps and labels",
            R"(start: jeq #80, tcp, other
jgt x, tcp
jne #6, other
jlt x, other
jle #9, other
jmp tcp
ja other
tcp:
other: ret #0
)",
            {"jeq #80, tcp, other", "jgt x, tcp", "jne #6, other", "jlt x, other", "jle #9, other",
             "jmp tcp", "ja other", "ret #0"},
            {{"start", 0}, {"tcp", 7}, {"other", 7}},
        },
        {
            "labels: several on one instruction, comments, blank lines",
            R"(; leading comment

first: second:
    ld #1        ; trailing comment
third: ret #0
)",
            {"ld #1", "ret #0"},
            {{"first", 0}, {"second", 0}, {"third", 1}},
        },
        {
            "label names that start like registers and keywords",
            R"(len2: ret #0
x2: a1: ret #1
)",
            {"ret #0", "ret #1"},
            {{"len2", 0}, {"x2", 1}, {"a1", 1}},
        },
        {
            "a label can share its name with an extension",
            R"(ja proto
proto: ret #0
)",
            {"ja proto", "ret #0"},
            {{"proto", 1}},
        },
        {
            // jneq is another spelling of jne
            "reference example: ipv4 + tcp filter",
            R"(ldh [12]
jne #2048, drop
ldb [23]
jneq #6, drop
pass: ret #-1
drop: ret #0
)",
            {"ldh [12]", "jne #2048, drop", "ldb [23]", "jne #6, drop", "ret #-1", "ret #0"},
            {{"pass", 4}, {"drop", 5}},
        },
        {
            "case-insensitive mnemonics and registers, label case preserved",
            R"(LD #1
JEQ #1, Done
Done: RET A
)",
            {"ld #1", "jeq #1, Done", "ret a"},
            {{"Done", 2}},
        },
        {
            "only a comment and blank lines",
            "; nothing here\n\n",
            {},
            {},
        },
        {
            "no newline at the end of the file",
            "ret #0",
            {"ret #0"},
            {},
        },
    };

    cases.push_back(alu_case());
    cases.push_back(jump_forms_case());
    cases.push_back(extension_case());

    return cases;
}

// -----------------------------------------------------------------------------
// Cases that must fail. Each source starts at line 0.
// -----------------------------------------------------------------------------
static std::vector<err_case> err_cases()
{
    return {
        // --- operand form not allowed for the mnemonic
        {"st takes only M[k]", "st #5", {{0, "does not accept"}}},
        {"ret does not take x", "ret x", {{0, "does not accept"}}},
        {"jeq needs a target", "jeq #1", {{0, "does not accept"}}},
        {"jne has no two-target form", "jne #1, l1, l2", {{0, "does not accept"}}},
        {"ld does not take a label (extensions need '#')", "ld proto", {{0, "does not accept"}}},
        {"add needs an operand", "add", {{0, "missing its operand"}}},
        {"neg takes no operand", "neg #1", {{0, "takes no operand"}}},

        // --- extensions
        {"unknown extension", "ld #bogus", {{0, "unknown BPF extension"}}},
        {"ldx only supports len", "ldx #proto", {{0, "only supports"}}},

        // --- packet and msh operands
        {"ldx has no packet-offset form", "ldx [14]", {{0, "no packet-offset"}}},
        {"[x] needs '+ k'", "ld [x]", {{0, "+"}}},
        {"msh multiplier must be 4", "ldxb 3*([2]&0xf)", {{0, "multiplier"}}},
        {"msh mask must be 0xf", "ldxb 4*([2]&0xe)", {{0, "mask"}}},
        {"number too large", "ld [99999999999999999999]", {{0, "out of range"}}},

        // --- line structure
        {"not an instruction", "foo ret #0", {{0, "expected an instruction"}}},
        {"junk after the instruction", "ret #0 junk", {{0, "after instruction"}}},
        {"only one error per line", "st #5 junk", {{0, "does not accept"}}},

        // --- labels
        {"duplicate label", "dup: ret #0\ndup: ret #1", {{1, "already defined"}}},

        // --- recovery: bad lines are skipped, good lines in between still parse
        {"recovery after errors",
         R"(st #5
ld #1
add
ld #2
neg #1
)",
         {{0, "does not accept"}, {2, "missing its operand"}, {4, "takes no operand"}}},
    };
}

int main()
{
    int passed = 0;
    int failed = 0;

    const auto report = [&](const std::string &name, const std::vector<std::string> &problems)
    {
        t::report(name, problems.empty());

        if (problems.empty())
        {
            ++passed;
            return;
        }

        ++failed;

        for (const auto &problem : problems)
        {
            fmt::print("      {}\n", problem);
        }
    };

    fmt::print("{}\n", fmt::styled("-- must parse --", fmt::emphasis::bold));
    for (const auto &tc : ok_cases())
    {
        report(tc.name, check_ok(tc));
    }

    fmt::print("\n{}\n", fmt::styled("-- must be rejected --", fmt::emphasis::bold));
    for (const auto &tc : err_cases())
    {
        report(tc.name, check_err(tc));
    }

    return t::finish(passed, failed);
}

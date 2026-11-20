#include <map>
#include <fmt/color.h>
#include <fmt/core.h>

#include "anvil/asm/lexer.h"
#include "anvil/asm/parser.h"
#include "anvil/asm/validator.h"
#include "anvil/asm/optimizer.h"
#include "test_util.h"

namespace s = anvil::s;
namespace t = anvil_test;

template <class... F>
struct overloaded : F... { using F::operator()...; };

template <class... F>
overloaded(F...) -> overloaded<F...>;

static std::string mn_name(s::mnemonics mn)
{
    std::string best;
    for (const auto &[name, value] : s::data::MNEMONICS)
    {
        if (value != mn)
            continue;
        if (best.empty() || name.size() < best.size() || (name.size() == best.size() && name < best))
            best = name;
    }
    return best;
}

static std::string ext_name(s::extension e)
{
    for (const auto &[name, value] : s::data::EXTENSIONS)
        if (value == e)
            return name;
    return "?";
}

static std::string to_text(const s::instruction &ins)
{
    const std::string mn = mn_name(ins.mnemonic);
    return std::visit(overloaded{
        [&](const s::ops::no_operand &) { return mn; },
        [&](const s::ops::reg_x &) { return mn + " x"; },
        [&](const s::ops::reg_a &) { return mn + " a"; },
        [&](const s::ops::imm &o) { return mn + " #" + std::to_string(o.k); },
        [&](const s::ops::pkt_abs &o) { return mn + " [" + std::to_string(o.k) + "]"; },
        [&](const s::ops::pkt_ind &o) { return mn + " [x+" + std::to_string(o.k) + "]"; },
        [&](const s::ops::scratch &o) { return mn + " M[" + std::to_string(o.k) + "]"; },
        [&](const s::ops::msh &o) { return mn + " msh[" + std::to_string(o.k) + "]"; },
        [&](const s::ops::ext &o) { return mn + " #" + ext_name(o.kind); },
        [&](const s::ops::jmp_label &o) { return mn + " " + o.label.value; },
        [&](const s::ops::jmp_cond &o) {
            std::string rhs = std::visit(overloaded{
                [](const s::ops::imm &v) { return "#" + std::to_string(v.k); },
                [](const s::ops::reg_x &) { return std::string("x"); },
            }, o.rhs);
            std::string text = mn + " " + rhs + ", " + o.lt.value;
            if (o.lf)
                text += ", " + o.lf->value;
            return text;
        },
    }, ins.ops);
}

struct case_data
{
    std::string name;
    std::string source;
    bool changed;
    std::vector<std::string> instructions;
    std::map<std::string, std::size_t> labels;
    s::Optimizer::level level = s::Optimizer::level::O1;
};

struct outcome
{
    bool lexed = false;
    bool parsed = false;
    bool valid = false;
    bool changed = false;
    s::logs_t logs;
    std::vector<std::string> instructions;
    std::map<std::string, std::size_t> labels;
};

static s::logs_t only_errors(const s::logs_t &logs)
{
    s::logs_t errors;
    for (const auto &log : logs)
        if (log.type == s::log_type::ERROR)
            errors.push_back(log);
    return errors;
}

static outcome run(const case_data &tc)
{
    outcome out;
    s::source src{tc.source.data(), tc.source.size()};
    s::Lexer lexer(src);

    if (!lexer.tokenize())
    {
        out.logs = lexer.logs();
        return out;
    }
    out.lexed = true;

    s::Parser parser(lexer.tokens());
    parser.parse();
    out.logs = parser.logs();
    if (!parser.logs().empty())
        return out;
    out.parsed = true;

    s::instructions_t ins = parser.instructions();
    s::symtab_t sym = parser.symtab();

    s::Validator validator(ins, sym);
    validator.validate();
    out.logs = validator.logs();
    out.valid = only_errors(out.logs).empty();
    if (!out.valid)
        return out;

    s::Optimizer optimizer(ins, sym, tc.level);
    out.changed = optimizer.optimize();

    for (const auto &instruction : ins)
        out.instructions.push_back(to_text(instruction));
    for (const auto &[name, symbol] : sym)
        out.labels[name] = symbol.address;

    return out;
}

static std::vector<std::string> check(const case_data &tc)
{
    std::vector<std::string> problems;
    outcome out = run(tc);

    if (!out.lexed)
    {
        problems.push_back("lexer failed");
        return problems;
    }
    if (!out.parsed)
    {
        problems.push_back("parser failed");
        return problems;
    }
    if (!out.valid)
    {
        problems.push_back("program did not validate");
        return problems;
    }
    if (out.changed != tc.changed)
        problems.push_back(fmt::format("changed: expected {}, got {}", tc.changed, out.changed));

    const std::size_t n = std::max(out.instructions.size(), tc.instructions.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::string expected = i < tc.instructions.size() ? tc.instructions[i] : "<nothing>";
        const std::string got = i < out.instructions.size() ? out.instructions[i] : "<nothing>";
        if (expected != got)
            problems.push_back(fmt::format("instruction {}: expected [{}], got [{}]", i, expected, got));
    }

    for (const auto &[name, address] : tc.labels)
    {
        auto it = out.labels.find(name);
        if (it == out.labels.end())
            problems.push_back(fmt::format("label '{}': expected at {}, but missing", name, address));
        else if (it->second != address)
            problems.push_back(fmt::format("label '{}': expected at {}, got {}", name, address, it->second));
    }

    for (const auto &[name, address] : out.labels)
        if (!tc.labels.contains(name))
            problems.push_back(fmt::format("label '{}' at {} was not expected", name, address));

    return problems;
}

static std::vector<case_data> cases()
{
    return {
        {"O0 leaves the program untouched", R"(ld #1
add #2
ret a)", false,
         {"ld #1", "add #2", "ret a"}, {}, s::Optimizer::level::O0},

        {"fold a known constant through ALU operations", R"(ld #2
add #3
ret a)", true,
         {"ld #5", "ret a"}, {}},

        {"merge add and sub chains", R"(add #5
sub #2
ret a)", true,
         {"add #3", "ret a"}, {}},

        {"cancel double negation", R"(neg
neg
ret a)", true,
         {"ret a"}, {}},

        {"remove dead loads and stores", R"(ld #7
st M[0]
ret #0)", true,
         {"ret #0"}, {}},

        {"fold an always-taken branch", R"(ld #1
jeq #1, yes, no
yes: ret #0
no: ret #-1)", true,
         {"ret #0"}, {}},

        {"jump to the next instruction is removed", R"(ja next
next: ret #0)", true,
         {"ret #0"}, {}},

        {"remove unreachable code", R"(jmp done
ld #123
done: ret #0)", true,
         {"ret #0"}, {}},
    };
}

int main()
{
    int passed = 0;
    int failed = 0;

    fmt::print("{}\n", fmt::styled("-- optimizer tests --", fmt::emphasis::bold));

    for (const auto &tc : cases())
    {
        const auto problems = check(tc);
        t::report(tc.name, problems.empty());
        if (problems.empty())
            ++passed;
        else
        {
            ++failed;
            for (const auto &problem : problems)
                fmt::print("      {}\n", problem);
        }
    }

    return t::finish(passed, failed);
}

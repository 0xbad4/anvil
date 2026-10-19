// test_util.h -- helpers shared by the lexer and parser tests.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <fmt/color.h>
#include <fmt/core.h>

#include "anvil/asm/lexer.h"

namespace anvil_test
{
    inline std::string format_logs(const anvil::s::logs_t &l, const anvil::s::source &src)
    {
        const std::string_view data(src.data, src.size);
        std::string out;

        for (const auto &err : l)
        {
            if (err.type == anvil::s::log_type::OK)
            {
                continue;
            }

            const std::size_t line = err.pos.line;
            const std::size_t col = err.pos.col;

            std::size_t line_start = 0;
            for (std::size_t i = 0; i < line; ++i)
            {
                const auto nl = data.find('\n', line_start);

                if (nl == std::string_view::npos)
                {
                    break;
                }

                line_start = nl + 1;
            }

            const std::size_t line_end = data.find('\n', line_start);

            std::string_view source_line = data.substr(
                line_start,
                line_end == std::string_view::npos ? data.size() - line_start : line_end - line_start);

            if (!source_line.empty() && source_line.back() == '\r')
            {
                source_line.remove_suffix(1);
            }

            // width of the printed line number, so the caret stays aligned past line 9
            const std::string gutter = std::to_string(line + 1);

            const auto colour = err.type == anvil::s::log_type::ERROR
                                    ? fmt::fg(fmt::color::red)
                                    : fmt::fg(fmt::color::orange);

            out += fmt::format("{} | {}\n", gutter, source_line);
            out += fmt::format("{}{} {}\n\n",
                               std::string(gutter.size() + 2 + col, ' '),
                               fmt::styled("^~~~", fmt::fg(fmt::color::red)),
                               fmt::styled(err.msg, colour));
        }

        return out;
    }

    inline void print_logs(const anvil::s::logs_t &logs, const anvil::s::source &src)
    {
        fmt::print("{}", format_logs(logs, src));
    }

    // ---------------------------------------------------------------------------
    // Token comparison (lexer test)
    // ---------------------------------------------------------------------------

    // Compares type and value only. Returns true when every token matches.
    inline bool assert_tokens(const anvil::s::tokens_t &actual, const anvil::s::tokens_t &expected)
    {
        if (actual.size() != expected.size())
        {
            fmt::print("{} {}: expected {}, got {}\n",
                       fmt::styled("[-]", fmt::fg(fmt::color::red)),
                       fmt::styled("Token count mismatch", fmt::fg(fmt::color::red)),
                       fmt::styled(expected.size(), fmt::fg(fmt::color::green)),
                       fmt::styled(actual.size(), fmt::fg(fmt::color::red)));
            return false;
        }

        bool success = true;

        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            const auto &act = actual[i];
            const auto &exp = expected[i];

            if (act.type == exp.type && act.value == exp.value)
            {
                continue;
            }

            success = false;

            const bool value_changed = act.value != exp.value;
            const bool type_changed = act.type != exp.type;

            fmt::print("{} | expected [{}, type@{}] got [{}, type@{}]\n",
                       fmt::styled(fmt::format("{:>3}", act.pos.line), fmt::fg(fmt::color::yellow)),
                       fmt::styled(exp.value, fmt::fg(value_changed ? fmt::color::red : fmt::color::light_green)),
                       fmt::styled(static_cast<int>(exp.type), fmt::fg(type_changed ? fmt::color::red : fmt::color::light_green)),
                       fmt::styled(act.value, fmt::fg(value_changed ? fmt::color::red : fmt::color::light_green)),
                       fmt::styled(static_cast<int>(act.type), fmt::fg(type_changed ? fmt::color::red : fmt::color::light_green)));
        }

        if (success)
        {
            fmt::print("{} {}\n",
                       fmt::styled("[+]", fmt::fg(fmt::color::green)),
                       fmt::styled(fmt::format("All {} tokens match", actual.size()), fmt::fg(fmt::color::green)));
        }

        return success;
    }

    // ---------------------------------------------------------------------------
    // Tiny test harness
    // ---------------------------------------------------------------------------

    inline bool contains(std::string_view haystack, std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    inline void report(std::string_view name, bool ok)
    {
        if (ok)
        {
            fmt::print("{} {}\n", fmt::styled("[+]", fmt::fg(fmt::color::green)), name);
        }
        else
        {
            fmt::print("{} {}\n", fmt::styled("[-]", fmt::fg(fmt::color::red)), fmt::styled(name, fmt::fg(fmt::color::red)));
        }
    }

    // Prints the summary and returns the process exit code.
    inline int finish(int passed, int failed)
    {
        const auto colour = failed == 0 ? fmt::color::green : fmt::color::red;

        fmt::print("\n{}\n", fmt::styled(fmt::format("{} passed, {} failed", passed, failed), fmt::fg(colour)));

        return failed == 0 ? 0 : 1;
    }

} // namespace anvil_test

#pragma once

#include "types.h"
#include "detail/utils.h"

namespace anvil::s
{
    class BaseObj {
        public:
            const logs_t& logs() const noexcept {
                return m_logs;
            }

            bool ok() const noexcept {
                return errc() == 0;
            }

            uint32_t errc() const noexcept {
                uint32_t c = 0;

                for (const log &l : logs()) {
                    if (l.type == log_type::ERROR) {
                        c++;
                    }
                }

                return c;
            }

            uint32_t warnc() const noexcept {
                uint32_t w = 0;

                for (const log &l : logs()) {
                    if (l.type == log_type::WARNING) {
                        w++;
                    }
                }

                return w;
            }

        protected:
            logs_t m_logs;

            void report(const std::string& msg, token_pos pos, log_type type = log_type::ERROR) noexcept {
                m_logs.push_back({type, pos, msg});
            }
    };
} // namespace anvil::s

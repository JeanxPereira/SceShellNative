// Process-wide logging: every line goes to stderr and to <cache>/launcher.log.
#pragma once
#include <format>
#include <string>
#include <string_view>

namespace ssn::log {

enum class Level { Debug, Info, Warn, Error };

void open_file(const std::string& path);
void write(Level level, std::string_view channel, std::string_view message);

template <class... A>
void info(std::string_view ch, std::format_string<A...> f, A&&... a) { write(Level::Info, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void warn(std::string_view ch, std::format_string<A...> f, A&&... a) { write(Level::Warn, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void error(std::string_view ch, std::format_string<A...> f, A&&... a) { write(Level::Error, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void debug(std::string_view ch, std::format_string<A...> f, A&&... a) { write(Level::Debug, ch, std::format(f, std::forward<A>(a)...)); }

}  // namespace ssn::log

#pragma once

#include "mkrhi/Rhi.h"

// Library-internal logging through the host's setLog callback (stderr without one)
namespace mkrhi::detail {

void log(LogLevel level, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);

} // namespace mkrhi::detail

#define MKRHI_INFO(...) ::mkrhi::detail::log(::mkrhi::LogLevel::Info, __VA_ARGS__)
#define MKRHI_WARN(...) ::mkrhi::detail::log(::mkrhi::LogLevel::Warn, __VA_ARGS__)
#define MKRHI_ERROR(...) ::mkrhi::detail::log(::mkrhi::LogLevel::Error, __VA_ARGS__)
#define MKRHI_FATAL(...) ::mkrhi::detail::fatal(__VA_ARGS__)

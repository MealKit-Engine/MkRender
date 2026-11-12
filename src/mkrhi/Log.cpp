#include "mkrhi/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace mkrhi {
namespace {

LogFn g_log = nullptr;
void* g_logUser = nullptr;

void emit(LogLevel level, const char* fmt, va_list args) {
    char text[2048];
    std::vsnprintf(text, sizeof(text), fmt, args);
    if (g_log) {
        g_log(level, text, g_logUser);
    } else {
        static const char* const names[] = {"info", "warn", "error", "fatal"};
        std::fprintf(stderr, "[mkrhi %s] %s\n", names[static_cast<int>(level)], text);
    }
}

} // namespace

void setLog(LogFn fn, void* user) {
    g_log = fn;
    g_logUser = user;
}

namespace detail {

void log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(level, fmt, args);
    va_end(args);
}

void fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(LogLevel::Fatal, fmt, args);
    va_end(args);
    std::abort();
}

} // namespace detail
} // namespace mkrhi

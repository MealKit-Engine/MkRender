// Whether a process id is running (editor link: info files of crashed editors stay behind, and connecting to their port can block)
#include "mkrender/Config.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#endif

namespace mkrender::tool {

bool processAlive(u32 pid) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

} // namespace mkrender::tool

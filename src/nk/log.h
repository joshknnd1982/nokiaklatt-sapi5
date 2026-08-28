// Shared debug logging for every Nokia Klatt component.
//
// Opened plain "a" with _SH_DENYNO: the SAPI DLL, the host process and the
// configuration utility all write the same file, and the CRT's ccs=UTF-8 mode
// fail-fasts inside fprintf, which leaves a three-byte BOM and a dead process
// instead of a log.
#pragma once

#include <stdarg.h>

namespace nk {

// Enabled when NOKIAKLATT_LOG is set in the environment, or when a file named
// nokiaklatt.log already exists beside the module. `tag` names the component
// in every line.
void log_init(const char* tag);
void log_write(const char* file, int line, const char* fmt, ...);
bool log_enabled();

// Where the log is being written, for the configuration utility to show.
const wchar_t* log_path();

// Turn logging on (or off) for this process regardless of the environment,
// and remember the choice for the next one.
void log_set_enabled(bool on);

}  // namespace nk

#define NK_LOG(...)                                            \
    do {                                                       \
        if (::nk::log_enabled())                               \
            ::nk::log_write(__FILE__, __LINE__, __VA_ARGS__);  \
    } while (0)

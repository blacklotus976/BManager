#pragma once
#include <ctime>

// MSVC's localtime_s(tm*, const time_t*) has no POSIX equivalent of the same
// name; map it onto localtime_r so the same call sites build on Linux.
#ifndef _WIN32
inline int localtime_s(std::tm* out, const std::time_t* t) { return localtime_r(t, out) ? 0 : 1; }
#endif

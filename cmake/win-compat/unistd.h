#pragma once
/*
 * Intentionally (almost) empty.
 *
 * MSVC ships no <unistd.h>. Bungee's src/Assert.cpp includes it unconditionally,
 * but only uses POSIX symbols (getpid/write/STDERR_FILENO) inside its
 * BUNGEE_PETRIFY debug block, which is not compiled in this project. This stub
 * satisfies the include on Windows/MSVC. It is placed on the include path for
 * the Bungee targets only (Windows/MSVC only) by the top-level CMakeLists.txt,
 * so it never shadows anything for the plugin's own sources or other platforms.
 */

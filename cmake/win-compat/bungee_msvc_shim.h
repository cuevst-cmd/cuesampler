#pragma once
/*
 * MSVC compatibility shim for the Bungee + vendored pffft sources.
 *
 * Force-included (cl /FI) into the bungee_library and pffft targets on Windows
 * by the top-level CMakeLists.txt. Upstream Bungee assumes a GCC/Clang
 * toolchain in a couple of spots that MSVC lacks; this neutralises them so we
 * can build against pristine pinned upstream without maintaining a fork.
 *
 * Everything here is a compiler hint — removing it is correctness-neutral and
 * leaves numerical results identical.
 */
#ifdef _MSC_VER

/* submodules/pffft/fftpack.c uses M_PI but never defines _USE_MATH_DEFINES,
 * which MSVC requires before <math.h> to expose M_PI/M_SQRT2/etc. (pffft.c
 * defines it itself; fftpack.c does not.) Define it here — this shim is
 * force-included before any system header, so <math.h> picks it up in both. */
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

/* src/Resample.h: `static __attribute__((noinline)) void run(...)`.
 * MSVC has no __attribute__; expanding it away is correct (the equivalent on
 * MSVC would be __declspec(noinline), an optimisation hint we can simply drop).
 * Under MSVC, __GNUC__ is undefined, so Bungee/Eigen GCC-only __attribute__
 * paths are inactive anyway — this only affects the always-on usage. */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* submodules/pffft/pffft.c: bare __builtin_prefetch() calls that sit outside
 * pffft's own COMPILER_MSVC macro path. The prefetch is purely a performance
 * hint; no-oping it changes nothing about the computed FFT. */
#ifndef __builtin_prefetch
#define __builtin_prefetch(...) ((void)0)
#endif

#endif /* _MSC_VER */

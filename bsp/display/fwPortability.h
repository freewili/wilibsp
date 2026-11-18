//
// fwPortability.h -- cross-compiler (GCC firmware / MSVC PC_SIMULATION)
// attribute shims. Lives directly under rmpLib/ (NOT under rmpLib/simpico/)
// specifically because it must be visible to BOTH builds:
//
//   - The real ARM/GCC firmware build's include path is rmpLib/, MenuX/,
//     the project root, etc. (cmake/variant.cmake) -- it never adds
//     rmpLib/simpico, which is PC_SIMULATION-only stub scaffolding by design.
//   - The PC_SIMULATION build (fwsimmain/fwsimruntime) puts rmpLib/simpico
//     BEFORE rmpLib on its include path, but rmpLib/ itself is still on
//     that path too, so this header resolves identically from both builds.
//
// A firmware .cpp that used to reach these macros only through
// rmpLib/simpico/pico/platform.h (the PC-only stub) would silently get an
// undefined identifier on the real device build, because that path is never
// searched there -- the #include "pico/platform.h" would instead resolve to
// the REAL Pico SDK's header, which naturally has none of this. That was a
// real, shipped-then-caught bug (Task 3 round 1); every one of the affected
// firmware sources now includes THIS header by its own explicit name
// instead, so there is exactly one place these three macros are defined.
//
#ifndef FW_PORTABILITY_H
#define FW_PORTABILITY_H

// GCC-only attribute shims. Hardware (GCC) keeps the exact same attribute
// text via these macros; MSVC (PC_SIMULATION) gets a no-op/inline equivalent
// instead of a hard compile error. Callers must use these macros at the call
// site rather than raw __attribute__(...), so the hardware build is
// byte-for-byte unaffected (verified: the #if defined(__GNUC__) branch below
// expands to literally the same attribute text GCC always saw).
#if defined(__GNUC__)
#define FW_ATTR_USED __attribute__((used))
#define FW_ALWAYS_INLINE __attribute__((always_inline)) inline
#define FW_ASM_NOP() asm volatile ("nop")
#else
#define FW_ATTR_USED
#define FW_ALWAYS_INLINE inline
// MSVC's x64 front end has no inline-asm support at all (not even a syntax
// error dodge -- "nop" isn't a keyword there). These call sites are bare
// GPIO bit-bang timing spins (rmpLib/rpUSB1248.cpp); on PC_SIMULATION
// there's no real hardware to pace, so this is an empty statement -- the
// surrounding loop's own volatile counter/variable already keeps the
// compiler from optimizing the spin away.
#define FW_ASM_NOP() ((void)0)

// MSVC has no __builtin_ctz (gcc/clang provide it as a compiler builtin, so
// the #if defined(__GNUC__) branch above deliberately declares NOTHING -- on
// the real ARM/GCC firmware build this header still contributes only the
// three object-like macros, no declaration, type or symbol).
// rmpLib/rpPIO.cpp defines the one PC_SIMULATION/_MSC_VER instance, under an
// identical guard and without including this header (returns 0 -- callers
// only use it for a power-of-two-size log2, so a real implementation belongs
// there if this ever needs to be correct, not just linkable). This is only
// the declaration OTHER TUs need in order to call it, since C++ has no
// implicit function declarations.
//
// It lives HERE, next to the attribute macros, for exactly the same reason
// they do: rmpLib/simpico is never on the firmware build's include path, so a
// firmware .cpp reaching this declaration through #include "pico/platform.h"
// would resolve to the REAL Pico SDK header on the device build and fail to
// compile. Callers (rmpLib/rpLogicAnalyzer.cpp) include "fwPortability.h" by
// its own explicit name instead.
#if defined(PC_SIMULATION) && defined(_MSC_VER)
int __builtin_ctz(int i);
#endif
#endif

#endif //FW_PORTABILITY_H

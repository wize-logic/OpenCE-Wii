/*
HALO_WII_PREFIX.H

Force-included ahead of every translation unit in the Wii build (clang
-include), in place of halo_linux_prefix.h, which it then includes: the same
MSVC/XDK environment, on 32-bit big-endian PowerPC with devkitPPC's newlib.
See port/wii/README.md.
*/

#ifndef __HALO_WII_PREFIX_H
#define __HALO_WII_PREFIX_H

#if !defined(__ppc__) || !defined(__BIG_ENDIAN__)
#error the Wii port targets 32-bit big-endian PowerPC
#endif

#ifndef HALO_WII
#define HALO_WII 1
#endif

/* ---------- wint_t

The port's <wchar.h> (port/linux/include/wchar.h) makes wint_t 16-bit, as
wchar_t is (-fshort-wchar). newlib's <sys/_types.h> otherwise pulls in
clang's own, a 32-bit int, first; define the 16-bit one before it can. */

#define _WINT_T 1
#define __wint_t_defined 1
typedef unsigned short wint_t;

#include "halo_linux_prefix.h"

#endif /* __HALO_WII_PREFIX_H */

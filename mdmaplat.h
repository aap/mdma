/*
 * mdmaplat.h — the only file in mdma that knows what machine it is on.
 *
 * mdma speaks the SCE runtime API (sceDmaChan, sceDmaSend, sceGs*, and the
 * SCE_VIF0/1_SET_* / SCE_GIF_* / SCE_GS_* constants from eestruct.h). That is
 * what /usr/local/sce provides and what freesce reimplements on top of the
 * ps2dev toolchain, so targeting it costs nothing and buys both.
 *
 * Only §9 (kicking) needs the DMAC; everything in §§1-6 is plain writes into
 * a buffer, which is why the same source builds on the host and can be unit
 * tested there. On the host, freesce's headers stand in for the SDK's —
 * they are written to be toolchain-independent, and the host tests then
 * verify chains built with the exact constants the EE will use.
 *
 *   MDMA_TARGET  building for the EE   (u_long128, pcpyld, sceDma*)
 *   MDMA_HOST    building for the host (freesce headers, no DMAC)
 *
 * Neither needs to be defined by hand; define one to override the guess.
 */

#ifndef MDMAPLAT_H
#define MDMAPLAT_H

#if !defined(MDMA_TARGET) && !defined(MDMA_HOST)
#  if defined(__mips__) || defined(_EE) || defined(R5900)
#    define MDMA_TARGET 1
#  else
#    define MDMA_HOST 1
#  endif
#endif
#if !defined(MDMA_TARGET)
#  define MDMA_TARGET 0
#endif
#if !defined(MDMA_HOST)
#  define MDMA_HOST 0
#endif

#include <stddef.h>
#include <stdio.h>
/* Before eetypes.h, which defers its BSD spellings (u_char … u_long) to a
 * real <sys/types.h> if one has already been seen. The other order works but
 * warns once per translation unit. */
#if MDMA_TARGET
#include <sys/types.h>
#endif
#include <eetypes.h>
#include <eestruct.h>

#if MDMA_TARGET
#include <eekernel.h>               /* FlushCache */
#include <libdma.h>
/*
 * The sized types are the headers' job, not ours: freesce's eetypes.h pulls
 * in a <stdint.h> that works on every EE toolchain, so there is nothing to
 * declare here. A duplicate typedef is not merely redundant — it is an error
 * under gcc 2.9, which is C89, where identical redefinition is not the
 * courtesy C11 made it.
 *
 * Sony's own eetypes.h is the exception: it has u_long128 and nothing else,
 * and that toolchain ships no <stdint.h> at all. Building against the stock
 * SDK therefore needs the names supplied here.
 */
#ifndef FREESCE_EETYPES
#ifndef __uint8_t_defined
#define __uint8_t_defined
typedef unsigned char      uint8_t;
#endif
#ifndef __uint16_t_defined
#define __uint16_t_defined
typedef unsigned short     uint16_t;
#endif
#ifndef __uint32_t_defined
#define __uint32_t_defined
typedef unsigned int       uint32_t;
#endif
#ifndef __int32_t_defined
#define __int32_t_defined
typedef int                int32_t;
#endif
#ifndef __uint64_t_defined
#define __uint64_t_defined
typedef unsigned long long uint64_t;
#endif
#ifndef __int64_t_defined
#define __int64_t_defined
typedef long long          int64_t;
#endif
#ifndef __uint128_t_defined
#define __uint128_t_defined
typedef u_long128          uint128_t;   /* the SDK's spelling of a qword */
#endif
#endif /* !FREESCE_EETYPES */

typedef sceDmaChan mdmaChan;
#define MDMA_HAVE_DMAC 1
#else
/* the host reaches the same types through freesce's eetypes.h, included above */
typedef struct mdmaChanStub mdmaChan;   /* nothing to kick on the host */
#define MDMA_HAVE_DMAC 0
#endif

/* Assembling a qword from halves matters: on the EE this is one pcpyld and
 * one sq, which is what lets the UCA write-gather buffer coalesce. */
#if MDMA_TARGET
#define MAKE128(RES,MSB,LSB) \
    __asm__("pcpyld %0, %1, %2" : "=r"(RES) \
            : "r"((uint64_t)(MSB)), "r"((uint64_t)(LSB)))
#else
#define MAKE128(RES,MSB,LSB) \
    ((RES) = ((uint128_t)(uint64_t)(MSB) << 64) | (uint128_t)(uint64_t)(LSB))
#endif

#define UINT64(HI,LO) (((uint64_t)(uint32_t)(HI))<<32 | (uint64_t)(uint32_t)(LO))
#define MAKEQ(RES,W3,W2,W1,W0) \
    MAKE128(RES, UINT64(W3,W2), UINT64(W1,W0))

/* memory segments */
#define MDMA_SEG_UCA    0x30000000u     /* uncached accelerated */
#define MDMA_SEG_SPR    0x70000000u     /* scratchpad */
#define MDMA_SEG_MASK   0xf0000000u
/* The address a DMA tag holds is a 32-bit physical one, which is also why a
 * chain built on the host has its pointers truncated — harmless, since the
 * host only ever inspects chains, never runs them. */
#define MDMA_PHYS(P)    ((uint32_t)(size_t)(P) & ~MDMA_SEG_MASK)

#if MDMA_TARGET
#define MDMA_FLUSHCACHE()  FlushCache(0)
#else
#define MDMA_FLUSHCACHE()  ((void)0)
#endif

#endif /* MDMAPLAT_H */

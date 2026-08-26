/*
 * mdma — PS2 DMA/VIF/GIF packet library
 *
 * Tags, VIF codes, GIF tags, counted scopes, arenas, kicking. No opinion
 * about rendering; nothing in here knows what a vertex is.
 *
 * Rationale and worked examples: MDMA-PROPOSAL.md, XTC-DESIGN.md §4.
 *
 * Naming: mdma wraps the SCE runtime API rather than reaching around it, so a
 * consumer needs no eestruct.h of its own for anything mdma understands. It
 * owns the DMA tag ids, the UNPACK formats and their modifier bits, the VIF
 * codes (as functions, whose argument order is the SCE macros' argument
 * order), and the GIF tag fields — because mdma constructs GIF tags and so
 * ought to name their parts. It stops there: GS register numbers and the
 * SCE_GS_SET_* value constructors are the renderer's business, not mdma's.
 * Types are the C99 uintN_t names, with uint128_t the qword.
 *
 * Implemented here: §§0-6, §9, §11. Slots (§7), sealed chains (§8) and MFIFO
 * (§10) are steps 4-6 of MDMA-PROPOSAL.md §7 and are not in yet; ref/call
 * already record their relocations so §8 lands cheaply when it does.
 */

#ifndef MDMA_H
#define MDMA_H

#include "mdmaplat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * 0. counts, and the debug build
 * ------------------------------------------------------------------ */

/* Counted things take a count, or MDMA_AUTO to have it patched on close.
 *
 * Explicit is the default idiom: it keeps the write stream strictly
 * sequential, which matters because chains are usually built in
 * uncached-accelerated memory where the write-gather buffer only coalesces
 * sequential stores. AUTO costs exactly one out-of-order store per scope.
 *
 * In a debug build an explicit count is checked against what was actually
 * written: a mismatch reports file:line and then patches it so the frame
 * still runs. In a release build it is written once and trusted. */
#define MDMA_AUTO (-1)

/* MDMA_DEBUG is a level, not a flag, because the checking and the *reporting*
 * cost wildly different amounts and you rarely want the second:
 *
 *   0  nothing is checked. A count is written once at open and trusted.
 *   1  counts are checked and repaired, silently. Costs a compare and, when
 *      something was wrong, one store. This is the default.
 *   2  ...and reported with file:line. This is the only level that costs
 *      string literals, sprintf, the stack frames they need, and two extra
 *      arguments on every scope-opening call. Turn it on when a chain is
 *      wrong; leave it off the rest of the time, including in debug builds.
 *
 * Level 2 is what MDMA-PROPOSAL.md §11 describes. Levels 0 and 1 still report
 * the handful of conditions that cannot be carried on — see mdmaPanic. */
#if !defined(MDMA_DEBUG)
#  ifdef NDEBUG
#    define MDMA_DEBUG 0
#  else
#    define MDMA_DEBUG 1
#  endif
#endif

/* Only level 2 threads a source location through the scope-opening calls.
 * Hence the trailing-macro dance. */
#if MDMA_DEBUG >= 2
#define MDMA_SRC        , const char *mdmaFile_, int mdmaLine_
#define MDMA_SRCARG     , __FILE__, __LINE__
#define MDMA_SRCPASS    , mdmaFile_, mdmaLine_
#else
#define MDMA_SRC
#define MDMA_SRCARG
#define MDMA_SRCPASS
#endif

/* Reported on a count mismatch or a broken scope, at level 2 only. The default
 * prints and, for anything fatal, calls abort(). Replace it to get the message
 * onto a screen or a DECI2 link instead. */
extern void (*mdmaFail)(const char *msg, const char *file, int line, int fatal);

/* The conditions no build can carry on from. The hot code passes a code and
 * nothing else, so the call site is a load-immediate in a branch never taken;
 * whatever wants to say more about it belongs in the hook. */
enum {
    MDMA_PANIC_OVERFLOW = 1,    /* the arena is full                        */
    MDMA_PANIC_SCOPE,           /* scopes opened or closed out of order     */
    MDMA_PANIC_STREAM,          /* a VIF code with nowhere to go            */
    MDMA_PANIC_RANGE            /* a count too big for the field it goes in */
};
extern void (*mdmaPanic)(int code);

/* ------------------------------------------------------------------ *
 * 1. DMA tag ids / UNPACK formats
 * ------------------------------------------------------------------ */

/* DMA tag ids. libdma.h hardcodes these in sceDmaAdd* and names none of
 * them, so mdma owns the spelling. The IRQ and PCE bits are what
 * mdmaTagFlags() can OR into a tag's id word. */
enum {
    IntFlg      = 0x80000000,     /* tag IRQ bit                            */
    DMApce_low  = 0x08000000,     /* precharge off/on pair, in the id field */
    DMApce_high = 0x0c000000,

    DMArefe = 0x00000000,
    DMAcnt  = 0x10000000,
    DMAnext = 0x20000000,
    DMAref  = 0x30000000,
    DMArefs = 0x40000000,
    DMAcall = 0x50000000,
    DMAret  = 0x60000000,
    DMAend  = 0x70000000
};

/* UNPACK format — the `cmd` argument of mdmaVifUnpack/mdmaBeginUnpack.
 *
 * The modifier bits go where the hardware keeps them, which is also where the
 * SCE macro's arguments would have put them: USN and DBLBUF are bits of the
 * ADDR immediate, so they OR into `vuaddr`; MSK is bit 4 of the CMD byte, so
 * it ORs into `cmd`. There is no INTR bit here because that is the `irq`
 * argument every VIF code takes. */
enum {
    UNPACK_V1_32 = 0x60, UNPACK_V1_16 = 0x61, UNPACK_V1_8 = 0x62,
    UNPACK_V2_32 = 0x64, UNPACK_V2_16 = 0x65, UNPACK_V2_8 = 0x66,
    UNPACK_V3_32 = 0x68, UNPACK_V3_16 = 0x69, UNPACK_V3_8 = 0x6a,
    UNPACK_V4_32 = 0x6c, UNPACK_V4_16 = 0x6d, UNPACK_V4_8 = 0x6e,
    UNPACK_V4_5 = 0x6f,

    UNPACK_MSK    = 0x0010,     /* into cmd:    use STMASK                  */
    UNPACK_USN    = 0x4000,     /* into vuaddr: unsigned                    */
    UNPACK_DBLBUF = 0x8000      /* into vuaddr: relative to TOPS            */
};

/* GIF tag fields, for mdmaGifTag and mdmaBeginGifTag.
 *
 * FLG says how the loop body is laid out; the PACKED descriptors are the
 * 4-bit codes of the `regs` word, packed four bits each from the bottom. The
 * set is closed and small, which is why mdma can name it without owning the
 * GS register space it partly overlaps. */
enum {
    GIF_PACKED = 0, GIF_REGLIST = 1, GIF_IMAGE = 2,

    GIF_PRIM = 0x0, GIF_RGBAQ = 0x1, GIF_ST = 0x2, GIF_UV = 0x3,
    GIF_XYZF2 = 0x4, GIF_XYZ2 = 0x5, GIF_TEX0_1 = 0x6, GIF_TEX0_2 = 0x7,
    GIF_CLAMP_1 = 0x8, GIF_CLAMP_2 = 0x9, GIF_FOG = 0xa,
    GIF_XYZF3 = 0xc, GIF_XYZ3 = 0xd,
    GIF_AD = 0xe,               /* the address is in the qword's high half  */
    GIF_NOP = 0xf
};

/* ------------------------------------------------------------------ *
 * 2. arenas — where chains get built
 *
 * A list writes into an arena. The arena knows whether back-patching is
 * cheap, which lets the debug build warn when MDMA_AUTO is used somewhere it
 * shouldn't be.
 * ------------------------------------------------------------------ */

enum {
    MDMA_MEM_CACHED = 0,        /* normal memory; AUTO is free              */
    MDMA_MEM_UCA    = 1,        /* uncached accelerated; AUTO breaks gather  */
    MDMA_MEM_SPR    = 2,        /* scratchpad                               */
    MDMA_MEM_MASK   = 0x0f,

    MDMA_ARENA_GROW = 0x100     /* step 6; rejected by mdmaArenaInit for now */
};

typedef struct mdmaArena mdmaArena;
struct mdmaArena {
    uint128_t *base;            /* buffer 0, segment bits applied           */
    uint32_t   size;            /* qwords per buffer                        */
    uint32_t   nbuf;            /* 1 = single, 2 = double, ...              */
    uint32_t   cur;             /* which buffer a new list writes into      */
    uint32_t   flags;
};

/* `mem` needs nbuf*qwSize qwords, 16-byte aligned, and for MDMA_MEM_UCA it
 * should be the plain cached pointer — mdma applies the segment itself. */
void   mdmaArenaInit(mdmaArena*, void *mem, uint32_t qwSize, uint32_t nbuf,
                     uint32_t flags);
void   mdmaArenaFlip(mdmaArena*);
uint128_t *mdmaArenaBuf(mdmaArena*, uint32_t i);
uint128_t *mdmaArenaCurBuf(mdmaArena*);

/* allocation hooks, so librw and xtc keep their own allocators */
extern void *(*mdmaMalloc)(size_t);
extern void  (*mdmaFree)(void*);

/* ------------------------------------------------------------------ *
 * 3. the list (builder)
 * ------------------------------------------------------------------ */

enum { MDMA_MAXDEPTH = 8 };

enum {                          /* mdmaScope.kind */
    MDMA_SC_TAG = 1, MDMA_SC_UNPACK, MDMA_SC_DIRECT, MDMA_SC_GIFTAG
};

typedef struct mdmaScope mdmaScope;
struct mdmaScope {
    uint32_t kind;
    uint32_t off;               /* qword holding the count being patched    */
    uint32_t word;              /* which u32 of it (0 for tags, 2/3 for VIF)*/
    uint32_t hdr;               /* that word with the count field zeroed,
                                 * so patching is one store and no read     */
    int32_t  declared;          /* the count passed in, or MDMA_AUTO        */
    uint32_t start;             /* qword where the counted body begins      */
    uint32_t startb;            /* byte within that qword                   */
    uint32_t fmt;               /* unpack fmt, or giftag flg | nreg<<8      */
#if MDMA_DEBUG >= 2
    const char *file;
    int line;
#endif
};

typedef struct mdmaList mdmaList;
struct mdmaList {
    mdmaArena *arena;
    uint128_t *p;               /* buffer base (segment bits applied)       */
    uint32_t   size;            /* whole qwords written                     */
    uint32_t   pendb;           /* bytes filled in the partial qword, 0..15 */
    uint32_t   limit;           /* qwords available                         */
    mdmaScope stack[MDMA_MAXDEPTH];
    int32_t   depth;
    int32_t   tagOff;           /* qword of the current tag, or -1          */
    uint32_t  nslot;            /* stream words placed in that tag (0..2)   */
    uint32_t  slots;            /* how many it may place there: 2, or 0 if
                                 * TTE is off or mdmaTagWords claimed them  */
    uint32_t  tte;              /* the DMAC hands tags to the VIF (default) */
    uint32_t   pend[4];         /* the partial qword, staged in cached
                                 * memory so the store to the arena is one
                                 * whole sequential qword either way. Words,
                                 * not bytes, because the VIF stream appends
                                 * to it a word at a time and that should be
                                 * one store, not four.                     */
    /* every absolute address written by ref/call is noted here, so a sealed
     * chain can be serialized later (§8). NULL until mdmaListRelocs(). */
    uint32_t  *relocs;
    int32_t   numRelocs, maxRelocs;
};

void    mdmaListInit  (mdmaList*, mdmaArena*);
void    mdmaListReset (mdmaList*);
uint32_t mdmaListSize (mdmaList*);      /* qwords so far, partial rounded up */
uint128_t *mdmaListData  (mdmaList*);   /* the base pointer                  */
void    mdmaListRelocs(mdmaList*, uint32_t *buf, int32_t max);

/* Whether the DMAC transfers each tag to the channel, which is what puts the
 * tag's spare 64 bits in front of the VIF as two VIFcodes (Dn_CHCR.TTE; see
 * mdmaInit, which sets it for VIF1 and clears it for the GIF). On by default.
 * Turning it off costs nothing at the call sites — the VIF stream simply
 * starts in the tag's body — but it does change every qwc by up to one, so a
 * list written with explicit counts is written for one setting. In a debug
 * build the count check says so, in file:line terms. */
void    mdmaListTTE(mdmaList*, int on);

/* ------------------------------------------------------------------ *
 *   the fast paths
 *
 * Writing a qword and appending a VIF code are what a frame does thousands of
 * times, and both are a handful of instructions when nothing unusual is going
 * on. They live here so they inline; the cold half of each is out of line
 * below, so the caller keeps no stack frame for a branch it does not take.
 *
 * The mdma* functions further down are these, mostly — read them as the API
 * and this as how it is spelled.
 * ------------------------------------------------------------------ */

/* always_inline, not merely inline: several of these are big enough that gcc
 * declines on its own judgement and emits a local copy, and then the count a
 * caller passed as a constant is a runtime argument again — which is exactly
 * the fold the whole scheme depends on. gcc 3.1 is where the attribute
 * arrived; before that (freesce's 2.9) we take what we are given. */
#ifndef MDMA_INLINE
#if defined(__GNUC__) && (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1))
#define MDMA_INLINE static __inline__ __attribute__((always_inline))
#else
#define MDMA_INLINE static __inline__
#endif
#endif



/* internal: the cold halves. Not API — call the functions below. */
void mdmaFlushPend(mdmaList*);          /* the staged qword is full         */
void mdmaPadPend(mdmaList*);            /* zero-fill and flush it           */
void mdmaOverflow(mdmaList*, uint32_t nq);
void mdmaAddSlow(mdmaList*, uint128_t); /* cursor is mid-qword              */
uint32_t mdmaStreamSlow(mdmaList*, uint32_t code);

/* Is there a body for the VIF stream to continue into? A ref has none, so a
 * third word is an error — but only a checking build pays to notice. */
#if MDMA_DEBUG >= 1
#define MDMA_TAGHASBODY(l) \
    ((*(const uint32_t*)&(l)->p[(l)->tagOff] & 0x30000000u) != 0x30000000u && \
     (*(const uint32_t*)&(l)->p[(l)->tagOff] & 0x70000000u) != 0x00000000u)
#else
#define MDMA_TAGHASBODY(l) 1
#endif

/* byte offset of the write cursor within the list */
MDMA_INLINE uint32_t
mdmaBytePos(mdmaList *l)
{
    return l->size*16 + l->pendb;
}

/* one whole qword at an aligned cursor: a compare and an sq */
MDMA_INLINE void
mdmaAdd(mdmaList *l, uint128_t q)
{
    if(l->pendb != 0 || l->size >= l->limit){
        mdmaAddSlow(l, q);
        return;
    }
    l->p[l->size++] = q;
}

MDMA_INLINE void
mdmaAddD(mdmaList *l, uint64_t lo, uint64_t hi)
{
    uint128_t q;
    MAKE128(q, hi, lo);
    mdmaAdd(l, q);
}

MDMA_INLINE void
mdmaAddW(mdmaList *l, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint128_t q;
    MAKEQ(q, w3, w2, w1, w0);
    mdmaAdd(l, q);
}

MDMA_INLINE uint32_t
mdmaFbits(float f)
{
    union { float f; uint32_t w; } u;
    u.f = f;
    return u.w;
}

MDMA_INLINE void
mdmaAddF(mdmaList *l, float f0, float f1, float f2, float f3)
{
    mdmaAddW(l, mdmaFbits(f0), mdmaFbits(f1), mdmaFbits(f2), mdmaFbits(f3));
}

/* the A+D pair: the register number rides in the qword's high half */
MDMA_INLINE void
mdmaAddAD(mdmaList *l, uint32_t reg, uint64_t val)
{
    mdmaAddD(l, val, reg);
}

/* reserve nq qwords and return where they start. Out of line: it happens once
 * per batch of vertices, not once per vertex. */
uint128_t *mdmaSkip(mdmaList*, uint32_t nq);

/* One word of the current tag's VIF stream: into the tag's spare 64 bits while
 * they are still in front of the cursor, otherwise into the body. Returns
 * where it landed, as qword*4 + word, for a scope to patch.
 *
 * Out of line, unlike its neighbours: it is forty-odd instructions and every
 * mdmaVif* goes through it, so one copy in the program reached by a jal beats
 * one copy per translation unit — never mind one per call site, which is what
 * forcing it inline produced. The call is the cheap part. */
uint32_t mdmaStreamWord(mdmaList*, uint32_t code);

/* the same, for a code whose data follows it in the stream */
uint32_t mdmaStreamData(mdmaList*, uint32_t code);

/* ------------------------------------------------------------------ *
 * 4. tags
 *
 * The scoped forms open a scope closed by mdmaCloseTag(). Anything written
 * between them is counted. `qwc` is the count you expect, or MDMA_AUTO. Each
 * returns where the tag sits, which is what you hold on to if its address has
 * to be filled in later — see mdmaJumpHere().
 * ------------------------------------------------------------------ */

/* A DMA tag, as it sits in the chain. Every opener returns a pointer to the
 * one it just wrote, which is how you get back to `addr` later. */
typedef struct mdmaTag mdmaTag;
struct mdmaTag {
    uint32_t id;                /* id | qwc, plus IntFlg / DMApce_*        */
    uint32_t addr;              /* what the DMAC jumps to; see mdmaAddr()  */
    uint32_t vif[2];            /* the spare 64 bits — §5's two VIFcodes   */
};

mdmaTag *mdmaCnt_ (mdmaList*, int32_t qwc MDMA_SRC);
mdmaTag *mdmaRet_ (mdmaList*, int32_t qwc MDMA_SRC);
mdmaTag *mdmaEnd_ (mdmaList*, int32_t qwc MDMA_SRC);
mdmaTag *mdmaCall_(mdmaList*, void *addr, int32_t qwc MDMA_SRC);
mdmaTag *mdmaNext_(mdmaList*, void *addr, int32_t qwc MDMA_SRC);
void mdmaCloseTag(mdmaList*);

#define mdmaCnt(l,qwc)        mdmaCnt_ ((l),(qwc) MDMA_SRCARG)
#define mdmaRet(l,qwc)        mdmaRet_ ((l),(qwc) MDMA_SRCARG)
#define mdmaEnd(l,qwc)        mdmaEnd_ ((l),(qwc) MDMA_SRCARG)
#define mdmaCall(l,addr,qwc)  mdmaCall_((l),(addr),(qwc) MDMA_SRCARG)
#define mdmaNext(l,addr,qwc)  mdmaNext_((l),(addr),(qwc) MDMA_SRCARG)

/* A pointer as a DMA tag holds it. Only the segment bits come off, so on a
 * cached arena this is the identity and the conversion is free; on a UCA one
 * it is the difference between a chain that runs and one that does not. */
uint32_t mdmaAddr(void*);

/* Patching a tag's target is a plain assignment, because that is all it is:
 *
 *      tag->addr = mdmaAddr(mdmaHere(l));      the chain continues here
 *
 * which is the jump-over-inline-data idiom, xtc's zero-copy path and the
 * nicest thing in it: a `next` transfers its own body, then jumps *past*
 * qwords that no tag counts — vertices written straight into the chain buffer
 * — and a later `ref` points back at them.
 *
 *      tag = mdmaNext(l, nil, 14); ... body ...; mdmaCloseTag(l);
 *      verts = mdmaHere(l);                    where the vertices will go
 *      ... fill them in, mdmaSkip over them ...
 *      tag->addr = mdmaAddr(mdmaHere(l));      the next resumes past them
 *      mdmaRef(l, verts, n);                   and now they get transferred
 *
 * Note this is not the same as closing a tag: the qwc covers the body only.
 * A `next` whose ADDR is the qword after its body is just a `cnt`.
 *
 * An address written this way is not in the relocation list (§8); a sealed
 * chain will have to find them by walking it, which it can, since the tag id
 * says whether there is an address there at all. */

/* "that tag continues here", for when the assignment above wants a name — it
 * also records the relocation that a sealed chain (§8) will want. */
void mdmaSetTarget(mdmaList*, mdmaTag*, void *dst);

/* the tag currently open, for code that did not keep what the opener returned */
mdmaTag *mdmaCurTag(mdmaList*);

/* unscoped: the payload is elsewhere, so there is nothing to count. A ref has
 * no body, so its VIF stream is only ever the tag's two spare words. */
mdmaTag *mdmaRef (mdmaList*, void *data, uint32_t qwc);
mdmaTag *mdmaRefs(mdmaList*, void *data, uint32_t qwc);
mdmaTag *mdmaRefe(mdmaList*, void *data, uint32_t qwc);

/* The tag's spare 64 bits, for a use of your own.
 *
 * With TTE on and a VIF downstream they are two VIFcodes, which is what §5
 * claims them for by default — but they are only 64 bits, and whoever built
 * the chain may have something else to do with them. Claim them here and the
 * tag behaves exactly as it would with TTE off: its VIF stream starts in the
 * body. Must come before any VIF code in that tag; what you put in them is
 * yours to get right. */
void mdmaTagWords(mdmaList*, uint32_t w0, uint32_t w1);

/* OR bits into the current tag's id word — IntFlg, DMApce_* */
void mdmaTagFlags(mdmaList*, uint32_t bits);

/* ------------------------------------------------------------------ *
 * 5. VIF codes
 *
 * A tag is one VIF word stream. Every call here appends one 32-bit word to
 * it: while the tag's spare 64 bits are free the word goes there, and after
 * that it goes into the body, which is exactly the order the VIF sees them
 * in. Where a word lands is mdma's business, not the caller's — a tag with
 * TTE off, or one whose spare words mdmaTagWords claimed, takes the same
 * calls and just starts in the body. A `ref` has no body, so its third word
 * is an error rather than a silently extra tag.
 *
 * One code never lands in the tag's first spare word: one whose data follows
 * it in the stream — UNPACK, DIRECT, MPG, the ST* codes — because the second
 * spare word is the very next word the VIF reads and would be taken as the
 * first word of that data. mdma slips a NOP in ahead of it, which is what
 * (VIFnop, VIFdirect) in a hand-written chain has always been for. It costs
 * nothing: the word was going to be a NOP either way.
 *
 * The VIF works in words; qword alignment is a DMA property, not a VIF one.
 * mdmaCloseTag() pads the tail of the transfer with zeros, which VIF reads as
 * NOP, and nothing else is padded behind your back.
 *
 * Names and argument order follow eestruct.h's SCE_VIF1_SET_* macros, so
 * these are a shorter spelling and not a second dialect. BASE, OFFSET,
 * MSKPATH3, FLUSHA, DIRECT and DIRECTHL exist on VIF1 only; the rest encode
 * identically on both.
 * ------------------------------------------------------------------ */

MDMA_INLINE void mdmaVifNop(mdmaList *l, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_NOP(irq)); }
MDMA_INLINE void mdmaVifStCycl(mdmaList *l, uint32_t wl, uint32_t cl, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_STCYCL(wl, cl, irq)); }
MDMA_INLINE void mdmaVifOffset(mdmaList *l, uint32_t offset, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_OFFSET(offset, irq)); }
MDMA_INLINE void mdmaVifBase(mdmaList *l, uint32_t base, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_BASE(base, irq)); }
MDMA_INLINE void mdmaVifItop(mdmaList *l, uint32_t itop, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_ITOP(itop, irq)); }
MDMA_INLINE void mdmaVifStMod(mdmaList *l, uint32_t stmod, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_STMOD(stmod, irq)); }
MDMA_INLINE void mdmaVifMskPath3(mdmaList *l, uint32_t msk, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_MSKPATH3(msk, irq)); }
MDMA_INLINE void mdmaVifMark(mdmaList *l, uint32_t mark, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_MARK(mark, irq)); }
MDMA_INLINE void mdmaVifFlushE(mdmaList *l, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_FLUSHE(irq)); }
MDMA_INLINE void mdmaVifFlush(mdmaList *l, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_FLUSH(irq)); }
MDMA_INLINE void mdmaVifFlushA(mdmaList *l, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_FLUSHA(irq)); }
MDMA_INLINE void mdmaVifMsCal(mdmaList *l, uint32_t vuaddr, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_MSCAL(vuaddr, irq)); }
MDMA_INLINE void mdmaVifMsCalF(mdmaList *l, uint32_t vuaddr, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_MSCALF(vuaddr, irq)); }
MDMA_INLINE void mdmaVifMsCnt(mdmaList *l, uint32_t irq)
    { mdmaStreamWord(l, SCE_VIF1_SET_MSCNT(irq)); }
MDMA_INLINE void mdmaVifMpg(mdmaList *l, uint32_t vuaddr, uint32_t num, uint32_t irq)
    { mdmaStreamData(l, SCE_VIF1_SET_MPG(vuaddr, num & 0xff, irq)); }

/* Each of these is followed by data — one word for STMASK, four for STROW and
 * STCOL — which you write yourself with mdmaAddW1() and friends. Whether they
 * ought instead to take that data as arguments is not decided; nothing has
 * used them yet, and inventing a shape for data whose origin we do not know
 * seemed the worse mistake. */
MDMA_INLINE void mdmaVifStMask(mdmaList *l, uint32_t irq)
    { mdmaStreamData(l, SCE_VIF1_SET_STMASK(irq)); }
MDMA_INLINE void mdmaVifStRow(mdmaList *l, uint32_t irq)
    { mdmaStreamData(l, SCE_VIF1_SET_STROW(irq)); }
MDMA_INLINE void mdmaVifStCol(mdmaList *l, uint32_t irq)
    { mdmaStreamData(l, SCE_VIF1_SET_STCOL(irq)); }

/* the escape hatch, for a code mdma has no name for */
MDMA_INLINE void mdmaVif(mdmaList *l, uint32_t code)
    { mdmaStreamWord(l, code); }

/* NOPs up to the next qword boundary.
 *
 * Never needed for correctness — the VIF does not care. It is worth doing
 * before a run of whole-qword adds, because mdmaAdd() at an aligned cursor is
 * one 128-bit store and at an unaligned one is a byte-wise shuffle through
 * the staging qword. (The store into the arena is one sequential qword
 * either way, so write-gather is not at stake; the cost is CPU cycles.) In a
 * debug build, qword adds at an unaligned cursor in a UCA or SPR arena report
 * once per site. */
void mdmaVifAlign(mdmaList*);

/* ------------------------------------------------------------------ *
 *   scopes, and why a trusted one costs nothing
 *
 * A scope exists to settle a count: to derive it for MDMA_AUTO, or to verify
 * an explicit one. With an explicit count and MDMA_DEBUG 0 there is neither
 * to do — the count went into the header word when the scope opened and is
 * never read again — so no scope is pushed and the close is four instructions
 * that find nothing to close. At level 1 and above the scope is pushed and
 * the close verifies it, which for a right answer is one multiply.
 * ------------------------------------------------------------------ */

mdmaScope *mdmaPush(mdmaList*, uint32_t kind, uint32_t off, uint32_t word,
                    uint32_t hdr, int32_t declared, uint32_t fmt MDMA_SRC);
void mdmaOpenUnpack_(mdmaList*, uint32_t vuaddr, int32_t num, uint32_t cmd,
                     uint32_t irq MDMA_SRC);
void mdmaOpenDirect_(mdmaList*, uint32_t id, int32_t count MDMA_SRC);
void mdmaOpenGifTag_(mdmaList*, int32_t nloop, uint32_t eop, uint32_t pre,
                     uint32_t prim, uint32_t flg, uint32_t nreg,
                     uint64_t regs MDMA_SRC);
void mdmaEndUnpackChecked(mdmaList*);
void mdmaEndDirectChecked(mdmaList*);
void mdmaEndGifTagChecked(mdmaList*);
extern const uint8_t mdmaElemBytes[16];

/* true when the close has something to settle */
#if MDMA_DEBUG >= 1
#define MDMA_WANTSCOPE(declared) 1
#define MDMA_HASSCOPE(l, k)      1
#else
#define MDMA_WANTSCOPE(declared) ((declared) == MDMA_AUTO)
#define MDMA_HASSCOPE(l, k) \
    ((l)->depth > 0 && (l)->stack[(l)->depth-1].kind == (k))
#endif

/* ------------------------------------------------------------------ *
 *   counted VIF/GIF scopes
 *
 * The bare form emits just the code, for when the data it describes is not in
 * this list — what a `ref` tag wants, in the zero-copy idiom where vertices
 * stay in the buffer that wrote them. The Begin/End form emits the code and
 * counts the body that follows it in the stream.
 * ------------------------------------------------------------------ */

/* UNPACK. `cmd` is UNPACK_V*_* (| UNPACK_MSK), `vuaddr` the VU address
 * (| UNPACK_USN | UNPACK_DBLBUF), `num` the element count or MDMA_AUTO. The
 * body may be written with sub-qword adds and need not start or end on a
 * qword boundary — nothing is padded, the next VIF code simply follows it in
 * the next word. */
MDMA_INLINE void
mdmaVifUnpack(mdmaList *l, uint32_t vuaddr, uint32_t num, uint32_t cmd,
              uint32_t irq)
{
    mdmaStreamData(l, SCE_VIF1_SET_UNPACK(vuaddr, num & 0xff, cmd, irq));
}
/* The test on `num` comes first and touches nothing else, because that is what
 * it takes for ee-gcc to fold it away when the count is a constant: put any
 * inlined control flow in front of it and the compiler stops believing the
 * argument is a constant and emits `li 7 / li -1 / bne`. */
MDMA_INLINE void
mdmaBeginUnpack_(mdmaList *l, uint32_t vuaddr, int32_t num, uint32_t cmd,
                 uint32_t irq MDMA_SRC)
{
    if(MDMA_WANTSCOPE(num))
        mdmaOpenUnpack_(l, vuaddr, num, cmd, irq MDMA_SRCPASS);
    else
        /* NUM is 8 bits at 16, with 0 meaning 256 */
        mdmaStreamData(l, SCE_VIF1_SET_UNPACK(vuaddr, (uint32_t)num & 0xff,
                                              cmd, irq));
}

MDMA_INLINE void
mdmaEndUnpack(mdmaList *l)
{
    if(MDMA_HASSCOPE(l, MDMA_SC_UNPACK))
        mdmaEndUnpackChecked(l);
}
#define mdmaBeginUnpack(l,a,n,c,irq) \
    mdmaBeginUnpack_((l),(a),(n),(c),(irq) MDMA_SRCARG)

/* DIRECT / DIRECTHL. `count` is in qwords, or MDMA_AUTO. The body is bound
 * for the GIF, which is qword-granular, so mdma requires the cursor to be
 * qword-aligned here — a limitation of mdma's own qword writing rather than
 * of the VIF. mdmaVifAlign() is the fix. */
MDMA_INLINE void mdmaVifDirect(mdmaList *l, uint32_t count, uint32_t irq)
    { mdmaStreamData(l, SCE_VIF1_SET_DIRECT(count, irq)); }
MDMA_INLINE void mdmaVifDirectHL(mdmaList *l, uint32_t count, uint32_t irq)
    { mdmaStreamData(l, SCE_VIF1_SET_DIRECTHL(count, irq)); }
MDMA_INLINE void
mdmaBeginDirect_(mdmaList *l, int32_t count, uint32_t irq MDMA_SRC)
{
    if(MDMA_WANTSCOPE(count))
        mdmaOpenDirect_(l, SCE_VIF1_SET_DIRECT(0, irq), count MDMA_SRCPASS);
    else
        mdmaStreamData(l, SCE_VIF1_SET_DIRECT((uint32_t)count, irq));
}

MDMA_INLINE void
mdmaBeginDirectHL_(mdmaList *l, int32_t count, uint32_t irq MDMA_SRC)
{
    if(MDMA_WANTSCOPE(count))
        mdmaOpenDirect_(l, SCE_VIF1_SET_DIRECTHL(0, irq), count MDMA_SRCPASS);
    else
        mdmaStreamData(l, SCE_VIF1_SET_DIRECTHL((uint32_t)count, irq));
}

MDMA_INLINE void
mdmaEndDirect(mdmaList *l)
{
    if(MDMA_HASSCOPE(l, MDMA_SC_DIRECT))
        mdmaEndDirectChecked(l);
}
#define mdmaBeginDirect(l,c,irq)   mdmaBeginDirect_  ((l),(c),(irq) MDMA_SRCARG)
#define mdmaBeginDirectHL(l,c,irq) mdmaBeginDirectHL_((l),(c),(irq) MDMA_SRCARG)

/* GIF tag. Argument order follows SCE_GIF_SET_TAG; `flg` is GIF_PACKED,
 * GIF_REGLIST or GIF_IMAGE and `regs` is the GIF_* descriptors packed four
 * bits each from the bottom.
 *
 * The bare form writes one qword of data and counts nothing — which is what
 * you want when the tag is bound for VU memory through an UNPACK rather than
 * for the GIF. The Begin/End form counts the loops that follow: `nloop` or
 * MDMA_AUTO, where AUTO divides what was written by nreg, so nreg has to be
 * right, and AUTO with GIF_REGLIST needs the body written in 64-bit adds. */
void mdmaGifTag(mdmaList*, uint32_t nloop, uint32_t eop, uint32_t pre,
                uint32_t prim, uint32_t flg, uint32_t nreg, uint64_t regs);
MDMA_INLINE void
mdmaBeginGifTag_(mdmaList *l, int32_t nloop, uint32_t eop, uint32_t pre,
                 uint32_t prim, uint32_t flg, uint32_t nreg,
                 uint64_t regs MDMA_SRC)
{
    if(MDMA_WANTSCOPE(nloop))
        mdmaOpenGifTag_(l, nloop, eop, pre, prim, flg, nreg, regs MDMA_SRCPASS);
    else
        mdmaAddD(l, SCE_GIF_SET_TAG((uint32_t)nloop, eop, pre, prim, flg,
                                    nreg), regs);
}

MDMA_INLINE void
mdmaEndGifTag(mdmaList *l)
{
    if(MDMA_HASSCOPE(l, MDMA_SC_GIFTAG))
        mdmaEndGifTagChecked(l);
}
#define mdmaBeginGifTag(l,nloop,eop,pre,prim,flg,nreg,regs) \
    mdmaBeginGifTag_((l),(nloop),(eop),(pre),(prim),(flg),(nreg),(regs) \
                     MDMA_SRCARG)

/* the overwhelmingly common case: PACKED, one A+D register per loop */
void mdmaGifTagAD(mdmaList*, uint32_t nloop);

MDMA_INLINE void
mdmaBeginGifTagAD_(mdmaList *l, int32_t nloop MDMA_SRC)
{
    mdmaBeginGifTag_(l, nloop, 1, 0, 0, GIF_PACKED, 1, GIF_AD MDMA_SRCPASS);
}
#define mdmaBeginGifTagAD(l,nloop) mdmaBeginGifTagAD_((l),(nloop) MDMA_SRCARG)

/* microprogram upload; handles the 256-instruction MPG chunking. Opens and
 * closes its own tags, so call it outside any scope. */
void mdmaMpg(mdmaList*, uint32_t vuaddr, void *code, uint32_t qwc);

/* ------------------------------------------------------------------ *
 * 6. data
 * ------------------------------------------------------------------ */

void   mdmaAddN(mdmaList*, const void *src, uint32_t nq);

/* sub-qword writes, for UNPACK formats narrower than V4_32 and for REGLIST.
 * mdma tracks the partial qword and pads it on close. */
void   mdmaAddU64(mdmaList*, uint64_t);
void   mdmaAddW1(mdmaList*, uint32_t);
void   mdmaAddW2(mdmaList*, uint32_t, uint32_t);
void   mdmaAddW3(mdmaList*, uint32_t, uint32_t, uint32_t);
void   mdmaAddF1(mdmaList*, float);
void   mdmaAddF2(mdmaList*, float, float);
void   mdmaAddF3(mdmaList*, float, float, float);
void   mdmaAddH2(mdmaList*, int, int);
void   mdmaAddH3(mdmaList*, int, int, int);
void   mdmaAddH4(mdmaList*, int, int, int, int);
void   mdmaAddB3(mdmaList*, int, int, int);
void   mdmaAddB4(mdmaList*, int, int, int, int);

/* current write position, e.g. to record where a sub-chain starts so an
 * interrupt handler can kick it (librw's texListBuild/MARK mechanism) */
MDMA_INLINE uint128_t *
mdmaHere(mdmaList *l)
{
    if(l->pendb)
        mdmaPadPend(l);
    return &l->p[l->size];
}

/* VIF MARK, and the tag's IRQ bit with it, so the handler that reads MARK
 * also gets woken. mdmaVifMark() is the code on its own. */
void   mdmaMark(mdmaList*, uint32_t id, int irq);

/* ------------------------------------------------------------------ *
 * 9. kicking
 * ------------------------------------------------------------------ */

#if MDMA_HAVE_DMAC
extern mdmaChan *mdmaVIF1, *mdmaVIF0, *mdmaGIF, *mdmaSPRfrom, *mdmaSPRto;

void mdmaInit(void);
void mdmaKick    (mdmaChan*, mdmaList*);    /* FlushCache if cached, send */
void mdmaKickAddr(mdmaChan*, void *chain);
void mdmaSync    (mdmaChan*);
int  mdmaBusy    (mdmaChan*);
#endif

/* The physical address the DMAC wants for this list, with the SPR bit and
 * 0x3ff0 masking applied if the arena is scratchpad. Always available: it is
 * pointer arithmetic, and worth being able to test. */
void *mdmaKickAddrOf(mdmaList*);

/* ------------------------------------------------------------------ *
 * 11. debug
 *
 * With MDMA_DEBUG:
 *  - explicit counts are verified on close; a mismatch reports
 *    "mdma: file.c:123: cnt declared 12, wrote 13 (patched)"
 *  - MDMA_AUTO in an MDMA_MEM_UCA or _SPR arena warns once per site, and so
 *    does a qword add at an unaligned cursor there
 *  - unbalanced scopes, a third VIF code on a `ref`, a partial qword where a
 *    whole one is required, and arena overflow all report and abort
 * ------------------------------------------------------------------ */

/* Interpret a chain the way the hardware would and print it. Shared with the
 * host-side GIF interpreter from the DECI2 capture tool, if that gets built. */
void mdmaDisasm(const uint128_t *chain, uint32_t qwc, FILE *out);
void mdmaDump(mdmaList*, FILE *out);

/* counters, for tests and for finding out whether AUTO is costing anything */
typedef struct mdmaStats mdmaStats;
struct mdmaStats {
    uint32_t patches;       /* out-of-order stores made by AUTO           */
    uint32_t mismatches;    /* explicit counts that were wrong            */
};
extern mdmaStats mdmaStatCounters;

#ifdef __cplusplus
}
#endif
#endif /* MDMA_H */

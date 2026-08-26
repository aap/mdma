/*
 * mdma — implementation of §§0-6, §9, §11 of mdma.h
 *
 * The whole file is one idea: a write cursor into an arena, plus a small
 * stack of open scopes, each of which remembers the word holding a count it
 * may have to check or patch.
 *
 * Invariants worth knowing while reading:
 *  - l->size counts whole qwords; l->pendb bytes of a partial one are staged
 *    in l->pend, so the store into the arena is always one whole qword.
 *  - DMA tags do not nest. The stack is tag > (unpack|direct) > giftag.
 *  - l->tagOff is the current tag, or -1. A tag is one VIF word stream:
 *    mdmaStreamWord() puts the first l->slots words into the tag's spare 64 bits
 *    and the rest into the body, which is the order the VIF reads them in.
 */

#include "mdma.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if MDMA_TARGET
#include <libgraph.h>
#endif

mdmaStats mdmaStatCounters;

void *(*mdmaMalloc)(size_t) = malloc;
void  (*mdmaFree)(void*) = free;

/* ------------------------------------------------------------------ *
 * reporting
 *
 * Two sources of location: a scope-opening call knows its own (MDMA_SRC), and
 * everything else attributes to the innermost open scope — which is the more
 * useful answer anyway, since a bad close is the open's fault.
 * ------------------------------------------------------------------ */

static void
defaultFail(const char *msg, const char *file, int line, int fatal)
{
    if(file)
        printf("mdma: %s:%d: %s\n", file, line, msg);
    else
        printf("mdma: %s\n", msg);
    if(fatal)
        abort();
}

void (*mdmaFail)(const char*, const char*, int, int) = defaultFail;

/* The strings live here, in the hook, not at the hundreds of call sites. */
static void
defaultPanic(int code)
{
    static const char *what[] = {
        "?", "arena overflow", "scopes out of order",
        "a VIF code with nowhere to go", "a count out of range"
    };
    printf("mdma: %s\n", code > 0 && code < 5 ? what[code] : "?");
    abort();
}

void (*mdmaPanic)(int) = defaultPanic;

/* Below level 2 a failure is a code and nothing else: no string, no format,
 * no stack frame at the site. REPORT vanishes entirely — a count mismatch is
 * still repaired, just silently. */
#if MDMA_DEBUG >= 2
#define REPORT(msg, file, line)      mdmaFail((msg), (file), (line), 0)
#define FATAL(code, msg, file, line) mdmaFail((msg), (file), (line), 1)
#else
#define REPORT(msg, file, line)      ((void)0)
#define FATAL(code, msg, file, line) mdmaPanic(code)
#endif

#if MDMA_DEBUG >= 2
#define SRCFILE     mdmaFile_
#define SRCLINE     mdmaLine_

static const char *
curFile(mdmaList *l)
{
    return l->depth > 0 ? l->stack[l->depth-1].file : (const char*)0;
}

static int
curLine(mdmaList *l)
{
    return l->depth > 0 ? l->stack[l->depth-1].line : 0;
}
#else
#define SRCFILE     ((const char*)0)
#define SRCLINE     0
#define curFile(l)  ((const char*)0)
#define curLine(l)  0
#endif

static const char *
kindName(uint32_t kind)
{
    switch(kind){
    case MDMA_SC_TAG:    return "tag";
    case MDMA_SC_UNPACK: return "unpack";
    case MDMA_SC_DIRECT: return "direct";
    case MDMA_SC_GIFTAG: return "giftag";
    }
    return "?";
}

/* ------------------------------------------------------------------ *
 * 2. arenas
 * ------------------------------------------------------------------ */

void
mdmaArenaInit(mdmaArena *a, void *mem, uint32_t qwSize, uint32_t nbuf,
              uint32_t flags)
{
    if(((size_t)mem & 0xf) != 0)
        mdmaFail("arena memory is not 16-byte aligned", 0, 0, 1);
    if(nbuf == 0)
        mdmaFail("arena needs at least one buffer", 0, 0, 1);
    if(flags & MDMA_ARENA_GROW)
        mdmaFail("MDMA_ARENA_GROW is not implemented yet (step 6)", 0, 0, 1);

#if MDMA_TARGET
    {
        uint32_t seg = 0;
        switch(flags & MDMA_MEM_MASK){
        case MDMA_MEM_UCA: seg = MDMA_SEG_UCA; break;
        case MDMA_MEM_SPR: seg = MDMA_SEG_SPR; break;
        }
        a->base = (uint128_t*)(MDMA_PHYS(mem) | seg);
    }
#else
    a->base = (uint128_t*)mem;      /* the host has no segments to speak of */
#endif
    a->size = qwSize;
    a->nbuf = nbuf;
    a->cur = 0;
    a->flags = flags;
}

uint128_t *
mdmaArenaBuf(mdmaArena *a, uint32_t i)
{
    return a->base + (size_t)i*a->size;
}

uint128_t *
mdmaArenaCurBuf(mdmaArena *a)
{
    return mdmaArenaBuf(a, a->cur);
}

void
mdmaArenaFlip(mdmaArena *a)
{
    a->cur++;
    if(a->cur >= a->nbuf)
        a->cur = 0;
}

/* ------------------------------------------------------------------ *
 * 3. the list
 * ------------------------------------------------------------------ */

void
mdmaListInit(mdmaList *l, mdmaArena *a)
{
    memset(l, 0, sizeof(*l));
    l->arena = a;
    l->p = mdmaArenaCurBuf(a);
    l->limit = a->size;
    l->tagOff = -1;
    l->tte = 1;
}

void
mdmaListReset(mdmaList *l)
{
    l->p = mdmaArenaCurBuf(l->arena);
    l->limit = l->arena->size;
    l->size = 0;
    l->pendb = 0;
    l->depth = 0;
    l->tagOff = -1;
    l->nslot = 0;
    l->slots = 0;
    l->numRelocs = 0;
}

void
mdmaListTTE(mdmaList *l, int on)
{
    l->tte = on ? 1u : 0u;
}

uint32_t
mdmaListSize(mdmaList *l)
{
    return l->size + (l->pendb ? 1 : 0);
}

uint128_t *
mdmaListData(mdmaList *l)
{
    return l->p;
}

void
mdmaListRelocs(mdmaList *l, uint32_t *buf, int32_t max)
{
    l->relocs = buf;
    l->maxRelocs = max;
    l->numRelocs = 0;
}

static void
addReloc(mdmaList *l, uint32_t off)
{
    if(l->relocs == 0)
        return;
    if(l->numRelocs >= l->maxRelocs){
        mdmaFail("relocation buffer full", curFile(l), curLine(l), 1);
        return;
    }
    l->relocs[l->numRelocs++] = off;
}

/* ------------------------------------------------------------------ *
 * raw writing
 * ------------------------------------------------------------------ */

static uint32_t *
wordAt(mdmaList *l, uint32_t off, uint32_t w)
{
    return (uint32_t*)&l->p[off] + w;
}

/* Patching a count is the one out-of-order store mdma ever makes; it is
 * counted so the cost of MDMA_AUTO stays visible.
 *
 * A VIF code lives wherever the stream put it, so the word being patched can
 * still be staged in l->pend — a small UNPACK whose body did not fill the
 * qword it started in. Written byte-wise there, because that is the form
 * flushPend reassembles from. */
static void
patch(mdmaList *l, uint32_t off, uint32_t w, uint32_t val)
{
    if(off == l->size)
        l->pend[w] = val;
    else
        *wordAt(l, off, w) = val;
    mdmaStatCounters.patches++;
}

/* cold: keeps the message and its buffer out of every qword written */
void
mdmaOverflow(mdmaList *l, uint32_t nq)
{
#if MDMA_DEBUG >= 2
    char buf[96];
    sprintf(buf, "arena overflow: %u of %u qwords used, need %u more",
            (unsigned)l->size, (unsigned)l->limit, (unsigned)nq);
    mdmaFail(buf, curFile(l), curLine(l), 1);
#else
    (void)l; (void)nq;
    mdmaPanic(MDMA_PANIC_OVERFLOW);
#endif
}

static void
room(mdmaList *l, uint32_t nq)
{
    if(l->size + nq > l->limit)
        mdmaOverflow(l, nq);
}

static void
pushQ(mdmaList *l, uint128_t q)
{
    room(l, 1);
    l->p[l->size++] = q;
}

void
mdmaFlushPend(mdmaList *l)
{
    uint128_t q;

    MAKEQ(q, l->pend[3], l->pend[2], l->pend[1], l->pend[0]);
    l->pendb = 0;
    pushQ(l, q);
}

/* Pad the partial qword with zeros — which VIF reads as NOP, so this doubles
 * as the alignment padding after inline VIF codes. */
void
mdmaPadPend(mdmaList *l)
{
    uint8_t *p;

    if(l->pendb == 0)
        return;
    p = (uint8_t*)l->pend;
    while(l->pendb < 16)
        p[l->pendb++] = 0;
    mdmaFlushPend(l);
}

static void
addBytes(mdmaList *l, const uint8_t *b, uint32_t n)
{
    uint8_t *p = (uint8_t*)l->pend;

    while(n--){
        p[l->pendb++] = *b++;
        if(l->pendb == 16)
            mdmaFlushPend(l);
    }
}

static void
addWords(mdmaList *l, const uint32_t *w, uint32_t n)
{
    uint8_t b[4];

    while(n--){
        b[0] = (uint8_t)*w;
        b[1] = (uint8_t)(*w>>8);
        b[2] = (uint8_t)(*w>>16);
        b[3] = (uint8_t)(*w>>24);
        addBytes(l, b, 4);
        w++;
    }
}

/* byte offset of the write cursor within the list */
static uint32_t
bytePos(mdmaList *l)
{
    return l->size*16 + l->pendb;
}

/* ------------------------------------------------------------------ *
 * scopes
 * ------------------------------------------------------------------ */

#if MDMA_DEBUG >= 2
/* MDMA_AUTO where back-patching is expensive: once per site, so a debug run
 * lists the places worth converting to explicit counts rather than shouting
 * every frame. */
enum { MDMA_MAXSITES = 64 };
static struct { const char *file; int line; } warnedSites[MDMA_MAXSITES];
static int numWarnedSites;

/* the memory where a per-qword cost is worth a word about, and the once-per
 * -site filter both warnings share */
static const char *
costlyMem(mdmaList *l)
{
    switch(l->arena->flags & MDMA_MEM_MASK){
    case MDMA_MEM_UCA: return "uncached-accelerated";
    case MDMA_MEM_SPR: return "scratchpad";
    }
    return 0;
}

static int
freshSite(const char *file, int line)
{
    int i;

    for(i = 0; i < numWarnedSites; i++)
        if(warnedSites[i].line == line && warnedSites[i].file == file)
            return 0;
    if(numWarnedSites < MDMA_MAXSITES){
        warnedSites[numWarnedSites].file = file;
        warnedSites[numWarnedSites].line = line;
        numWarnedSites++;
    }
    return 1;
}

static void
warnAuto(mdmaList *l, const char *file, int line)
{
    char buf[128];
    const char *mem;

    mem = costlyMem(l);
    if(mem == 0 || !freshSite(file, line))
        return;
    sprintf(buf, "MDMA_AUTO in %s memory breaks the write gather; "
                 "an explicit count would not", mem);
    mdmaFail(buf, file, line, 0);
}

/* A qword add at an unaligned cursor is correct but slow: it shuffles bytes
 * through the staging qword instead of doing one 128-bit store. Worth saying
 * once where a frame chain is being built; mdmaVifAlign() is the fix. The
 * location is the innermost open scope's, which is where the run of adds is. */
static void
warnUnaligned(mdmaList *l)
{
    char buf[128];
    const char *mem, *file;
    int line;

    mem = costlyMem(l);
    file = curFile(l);
    line = curLine(l);
    if(mem == 0 || file == 0 || !freshSite(file, line))
        return;
    sprintf(buf, "qword add at an unaligned cursor in %s memory is "
                 "byte-shuffled; mdmaVifAlign() first", mem);
    mdmaFail(buf, file, line, 0);
}
#else
#define warnUnaligned(l)    ((void)0)
#endif

/* `shift` is where the count sits in the header word: 0 for a tag qwc, a
 * DIRECT immediate and a GIF nloop, 16 for an UNPACK num. */
mdmaScope *
mdmaPush(mdmaList *l, uint32_t kind, uint32_t off, uint32_t word, uint32_t hdr,
     int32_t declared, uint32_t fmt MDMA_SRC)
{
    mdmaScope *s;

    if(l->depth >= MDMA_MAXDEPTH){
        FATAL(MDMA_PANIC_SCOPE, "scope stack overflow", SRCFILE, SRCLINE);
        return &l->stack[MDMA_MAXDEPTH-1];
    }
    s = &l->stack[l->depth++];
    s->kind = kind;
    s->off = off;
    s->word = word;
    s->hdr = hdr;
    s->declared = declared;
    s->start = l->size;
    s->startb = l->pendb;
    s->fmt = fmt;
#if MDMA_DEBUG >= 2
    s->file = SRCFILE;
    s->line = SRCLINE;
    if(declared == MDMA_AUTO)
        warnAuto(l, SRCFILE, SRCLINE);
#endif
    return s;
}

/* cold: the only two ways a close can be wrong, and their messages */
static mdmaScope *
badClose(mdmaList *l, uint32_t kind)
{
#if MDMA_DEBUG >= 2
    char buf[96];

    if(l->depth <= 0)
        sprintf(buf, "close of %s with no scope open", kindName(kind));
    else
        sprintf(buf, "close of %s, but the open scope is a %s",
                kindName(kind), kindName(l->stack[l->depth-1].kind));
    mdmaFail(buf, curFile(l), curLine(l), 1);
#else
    (void)l; (void)kind;
    mdmaPanic(MDMA_PANIC_SCOPE);
#endif
    return 0;
}

static mdmaScope *
top(mdmaList *l, uint32_t kind)
{
    mdmaScope *s;

    if(l->depth <= 0)
        return badClose(l, kind);
    s = &l->stack[l->depth-1];
    if(s->kind != kind)
        return badClose(l, kind);
    return s;
}

/* Settle the count. `shift` places it in the header word, `unit` names what it
 * counts for the message.
 *
 * The two builds differ exactly as §1 of the proposal asks: a debug build
 * verifies an explicit count and repairs it so the frame still runs, and a
 * release build does not look at it at all — the value was written once at
 * open time and is trusted, so an explicit count costs no read, no compare and
 * no out-of-order store. */
#if MDMA_DEBUG >= 2
/* cold: the message a mismatch deserves when you have asked for messages */
static void
reportCount(mdmaScope *s, int32_t actual, const char *unit)
{
    char buf[128];

    sprintf(buf, "%s declared %d %s, wrote %d (patched)",
            kindName(s->kind), (int)s->declared, unit, (int)actual);
    mdmaFail(buf, s->file, s->line, 0);
}
#endif

static void
settle(mdmaList *l, mdmaScope *s, int32_t actual, uint32_t shift,
       uint32_t mask, const char *unit)
{
    (void)unit;
#if MDMA_DEBUG >= 1
    if(s->declared != MDMA_AUTO){
        if(s->declared == actual)
            return;
        mdmaStatCounters.mismatches++;
#if MDMA_DEBUG >= 2
        reportCount(s, actual, unit);
#endif
    }
#else
    if(s->declared != MDMA_AUTO)
        return;
#endif
    patch(l, s->off, s->word, s->hdr | (((uint32_t)actual & mask) << shift));
}

/* cold: every scope close has the same complaint to make, so they share the
 * one function that knows how to say it. */
static void
badBody(mdmaList *l, const char *what, uint32_t bytes, uint32_t unit)
{
#if MDMA_DEBUG >= 2
    char buf[128];

    sprintf(buf, "%s wrote %u bytes, not a whole number of %u-byte units",
            what, (unsigned)bytes, (unsigned)unit);
    mdmaFail(buf, curFile(l), curLine(l), 1);
#else
    (void)l; (void)what; (void)bytes; (void)unit;
    mdmaPanic(MDMA_PANIC_RANGE);
#endif
}

static void
badRange(mdmaList *l, const char *what, int32_t n)
{
#if MDMA_DEBUG >= 2
    char buf[96];

    sprintf(buf, "%s of %d, out of range", what, (int)n);
    mdmaFail(buf, curFile(l), curLine(l), 1);
#else
    (void)l; (void)what; (void)n;
    mdmaPanic(MDMA_PANIC_RANGE);
#endif
}

/* ------------------------------------------------------------------ *
 * 4. tags
 * ------------------------------------------------------------------ */

uint32_t
mdmaAddr(void *p)
{
    return p ? MDMA_PHYS(p) : 0;
}

static mdmaTag *
openTagQ(mdmaList *l, uint32_t id, void *addr, int32_t qwc)
{
    uint128_t q;
    uint32_t off;

    mdmaPadPend(l);
    room(l, 1);
    off = l->size;
    MAKEQ(q, 0, 0,
               mdmaAddr(addr),
               id | (qwc == MDMA_AUTO ? 0 : (uint32_t)qwc & 0xffff));
    pushQ(l, q);
    l->tagOff = (int32_t)off;
    l->nslot = 0;
    l->slots = l->tte ? 2 : 0;
    if(addr)
        addReloc(l, off);
    return (mdmaTag*)&l->p[off];
}

static mdmaTag *
openTag(mdmaList *l, uint32_t id, void *addr, int32_t qwc MDMA_SRC)
{
    mdmaTag *t;

#if MDMA_DEBUG >= 1
    if(l->depth > 0){
        FATAL(MDMA_PANIC_SCOPE,
              "a tag cannot open inside another scope — missing a close?",
              SRCFILE, SRCLINE);
        return (mdmaTag*)l->p;
    }
#endif
    t = openTagQ(l, id, addr, qwc);
    /* a tag always gets a scope: mdmaCloseTag is where the qwords are
     * counted, and the qwc is the one count nothing else can settle */
    mdmaPush(l, MDMA_SC_TAG, (uint32_t)l->tagOff, 0, id, qwc, 0 MDMA_SRCPASS);
    return t;
}

mdmaTag *
mdmaCnt_(mdmaList *l, int32_t qwc MDMA_SRC)
{
    return openTag(l, DMAcnt, 0, qwc MDMA_SRCPASS);
}

mdmaTag *
mdmaRet_(mdmaList *l, int32_t qwc MDMA_SRC)
{
    return openTag(l, DMAret, 0, qwc MDMA_SRCPASS);
}

mdmaTag *
mdmaEnd_(mdmaList *l, int32_t qwc MDMA_SRC)
{
    return openTag(l, DMAend, 0, qwc MDMA_SRCPASS);
}

mdmaTag *
mdmaCall_(mdmaList *l, void *addr, int32_t qwc MDMA_SRC)
{
    return openTag(l, DMAcall, addr, qwc MDMA_SRCPASS);
}

mdmaTag *
mdmaNext_(mdmaList *l, void *addr, int32_t qwc MDMA_SRC)
{
    return openTag(l, DMAnext, addr, qwc MDMA_SRCPASS);
}

void
mdmaCloseTag(mdmaList *l)
{
    mdmaScope *s;

    mdmaPadPend(l);
    s = top(l, MDMA_SC_TAG);
    if(s == 0)
        return;
    settle(l, s, (int32_t)(l->size - s->start), 0, 0xffff, "qwords");
    l->depth--;
    l->tagOff = -1;
    l->nslot = 0;
    l->slots = 0;
}

/* "that tag continues here" — the same store as tag->addr = mdmaAddr(dst),
 * with the relocation recorded and, at level 2, a word about a tag that has
 * nowhere to put an address. */
void
mdmaSetTarget(mdmaList *l, mdmaTag *tag, void *dst)
{
#if MDMA_DEBUG >= 2
    uint32_t id = tag->id & 0x70000000;
    if(id != DMAnext && id != DMAcall && id != DMAref &&
       id != DMArefs && id != DMArefe)
        mdmaFail("mdmaSetTarget on a tag that holds no address",
                 curFile(l), curLine(l), 1);
#endif
    tag->addr = mdmaAddr(dst);
    mdmaStatCounters.patches++;
    addReloc(l, (uint32_t)(tag - (mdmaTag*)l->p));
}

mdmaTag *
mdmaCurTag(mdmaList *l)
{
    if(l->tagOff < 0){
        mdmaFail("mdmaCurTag with no tag written", curFile(l), curLine(l), 1);
        return (mdmaTag*)l->p;
    }
    return (mdmaTag*)&l->p[l->tagOff];
}

static mdmaTag *
refTag(mdmaList *l, uint32_t id, void *data, uint32_t qwc)
{
    /* No body, so no scope — but the spare words are still a VIF stream,
     * which is the whole point of mdmaRef(...); mdmaVifUnpack(...). */
    return openTagQ(l, id, data, (int32_t)qwc);
}

mdmaTag *mdmaRef (mdmaList *l, void *d, uint32_t n)
    { return refTag(l, DMAref,  d, n); }
mdmaTag *mdmaRefs(mdmaList *l, void *d, uint32_t n)
    { return refTag(l, DMArefs, d, n); }
mdmaTag *mdmaRefe(mdmaList *l, void *d, uint32_t n)
    { return refTag(l, DMArefe, d, n); }

void
mdmaTagWords(mdmaList *l, uint32_t w0, uint32_t w1)
{
    if(l->tagOff < 0){
        mdmaFail("mdmaTagWords with no tag to write into",
                 curFile(l), curLine(l), 1);
        return;
    }
    if(l->nslot != 0){
        mdmaFail("mdmaTagWords after the VIF stream already claimed a word",
                 curFile(l), curLine(l), 1);
        return;
    }
    *wordAt(l, (uint32_t)l->tagOff, 2) = w0;
    *wordAt(l, (uint32_t)l->tagOff, 3) = w1;
    l->slots = 0;               /* the stream starts in the body instead */
}

/* One word of the current tag's VIF stream: into the tag's spare 64 bits
 * while they are still in front of the cursor, otherwise into the body.
 * Returns where it landed, as qword*4 + word, for a scope to patch. */
uint32_t
mdmaStreamWord(mdmaList *l, uint32_t code)
{
    uint32_t off;

    if(l->nslot < l->slots && l->pendb == 0 &&
       (int32_t)l->size == l->tagOff + 1){
        off = (uint32_t)l->tagOff*4 + 2 + l->nslot;
        ((uint32_t*)&l->p[l->tagOff])[2 + l->nslot] = code;
        l->nslot++;
        return off;
    }
    if((l->pendb & 3) == 0 && l->tagOff >= 0 && MDMA_TAGHASBODY(l)){
        off = l->size*4 + (l->pendb >> 2);
        l->pend[l->pendb >> 2] = code;
        l->pendb += 4;
        if(l->pendb == 16)
            mdmaFlushPend(l);
        return off;
    }
    return mdmaStreamSlow(l, code);
}

/* A code whose data follows it in the stream cannot take the tag's first
 * spare word — the second would be read as the first word of that data. */
uint32_t
mdmaStreamData(mdmaList *l, uint32_t code)
{
    if(l->nslot == 0 && l->slots == 2 && l->pendb == 0 &&
       (int32_t)l->size == l->tagOff + 1)
        mdmaStreamWord(l, 0);           /* NOP */
    return mdmaStreamWord(l, code);
}

/* The cold half of mdmaStreamWord(): no tag, a ref with its two spare words
 * already spent, or a cursor left mid-word by a narrow UNPACK. The VIF reads
 * words, so round up to one before appending — the zero bytes are part of the
 * unpack data that was going to be padded anyway. */
uint32_t
mdmaStreamSlow(mdmaList *l, uint32_t code)
{
    uint32_t id, off;
    uint8_t *p;

    if(l->tagOff < 0){
        FATAL(MDMA_PANIC_STREAM, "a VIF code with no tag to attach to",
              curFile(l), curLine(l));
        return 0;
    }
    id = *wordAt(l, (uint32_t)l->tagOff, 0) & 0x70000000;
    if(id == DMAref || id == DMArefs || id == DMArefe){
        FATAL(MDMA_PANIC_STREAM,
              "a ref has no body for the VIF stream to continue in; "
              "open another tag", curFile(l), curLine(l));
        return 0;
    }
    p = (uint8_t*)l->pend;
    while(l->pendb & 3)
        p[l->pendb++] = 0;
    off = l->size*4 + (l->pendb >> 2);
    l->pend[l->pendb >> 2] = code;
    l->pendb += 4;
    if(l->pendb == 16)
        mdmaFlushPend(l);
    return off;
}

void
mdmaVifAlign(mdmaList *l)
{
    while(l->nslot < l->slots && bytePos(l) == ((uint32_t)l->tagOff + 1)*16)
        mdmaStreamWord(l, SCE_VIF1_SET_NOP(0));
    mdmaPadPend(l);
}

void
mdmaTagFlags(mdmaList *l, uint32_t bits)
{
    if(l->tagOff < 0){
        mdmaFail("mdmaTagFlags with no tag to flag", curFile(l), curLine(l), 1);
        return;
    }
    if(l->depth > 0 && l->stack[l->depth-1].kind == MDMA_SC_TAG)
        l->stack[l->depth-1].hdr |= bits;
    *wordAt(l, (uint32_t)l->tagOff, 0) |= bits;
}

/* ------------------------------------------------------------------ *
 *   counted VIF/GIF scopes
 * ------------------------------------------------------------------ */

/* The unit a scope's body is measured in — bytes per UNPACK element, or a
 * qword for DIRECT. Shared by the closers so "did it write what it said" is
 * one multiply. */
const uint8_t mdmaElemBytes[16] = {
    4, 2, 1, 2,  8, 4, 2, 2,  12, 6, 3, 2,  16, 8, 4, 2
};

/* The scoped openers. Only reached when the close will have something to
 * settle — an AUTO count to derive, or, in a checking build, any count to
 * verify. With an explicit count and MDMA_DEBUG 0 the inline half writes the
 * VIF code and that is the whole of it. */
void
mdmaOpenUnpack_(mdmaList *l, uint32_t vuaddr, int32_t num, uint32_t cmd,
                uint32_t irq MDMA_SRC)
{
    uint32_t hdr, pos;

#if MDMA_DEBUG >= 1
    if(num != MDMA_AUTO && (num < 1 || num > 256))
        badRange(l, "UNPACK num", num);
#endif
    /* NUM is 8 bits at 16, with 0 meaning 256 */
    hdr = SCE_VIF1_SET_UNPACK(vuaddr, 0, cmd, irq);
    pos = mdmaStreamData(l, hdr |
              (num == MDMA_AUTO ? 0 : ((uint32_t)num & 0xff) << 16));
    mdmaPush(l, MDMA_SC_UNPACK, pos/4, pos%4, hdr, num, cmd MDMA_SRCPASS);
}

void
mdmaOpenDirect_(mdmaList *l, uint32_t id, int32_t count MDMA_SRC)
{
    uint32_t pos;

    pos = mdmaStreamData(l, id |
              (count == MDMA_AUTO ? 0 : (uint32_t)count & 0xffff));
    mdmaPush(l, MDMA_SC_DIRECT, pos/4, pos%4, id, count, 0 MDMA_SRCPASS);
}

void
mdmaOpenGifTag_(mdmaList *l, int32_t nloop, uint32_t eop, uint32_t pre,
                uint32_t prim, uint32_t flg, uint32_t nreg,
                uint64_t regs MDMA_SRC)
{
    uint32_t pos;

#if MDMA_DEBUG >= 1
    if(l->pendb % 4 != 0)
        badRange(l, "a GIF tag off a word boundary at byte", (int32_t)l->pendb);
    if(nloop != MDMA_AUTO && (nloop < 0 || nloop > 0x7fff))
        badRange(l, "GIF nloop", nloop);
#endif
    pos = mdmaBytePos(l) / 4;
    mdmaAddD(l, SCE_GIF_SET_TAG(nloop == MDMA_AUTO ? 0 : (uint32_t)nloop,
                                eop, pre, prim, flg, nreg), regs);
    /* NLOOP is the low 15 bits of the tag's first word */
    mdmaPush(l, MDMA_SC_GIFTAG, pos/4, pos%4,
             (uint32_t)SCE_GIF_SET_TAG(0, eop, pre, prim, flg, nreg),
             nloop, (flg & 0xff) | (nreg & 0xf)<<8 MDMA_SRCPASS);
}

/* The checked half of mdmaEndUnpack(). Only reached when there is something to
 * work out: an AUTO count to derive, or an explicit one to verify. */
void
mdmaEndUnpackChecked(mdmaList *l)
{
    mdmaScope *s;
    uint32_t esz, bytes;
    int32_t n;

    s = top(l, MDMA_SC_UNPACK);
    if(s == 0)
        return;
    l->depth--;
    esz = mdmaElemBytes[s->fmt & 0xf];
    bytes = bytePos(l) - (s->start*16 + s->startb);
    /* the common case is that it was right: one multiply, no divide */
    if(s->declared != MDMA_AUTO && (uint32_t)s->declared * esz == bytes)
        return;
    if(bytes % esz != 0){
        badBody(l, "unpack", bytes, esz);
        return;
    }
    n = (int32_t)(bytes / esz);
    if(n < 1 || n > 256)
        badRange(l, "unpack elements", n);
    settle(l, s, n, 16, 0xff, "elements");
    /* deliberately no padding: the body ended where it ended, and the next
     * VIF code follows it in the next word. Only mdmaCloseTag pads, because
     * only the DMA transfer is measured in qwords. */
}

void
mdmaEndDirectChecked(mdmaList *l)
{
    mdmaScope *s;
    uint32_t bytes;

    s = top(l, MDMA_SC_DIRECT);
    if(s == 0)
        return;
    l->depth--;
    bytes = bytePos(l) - (s->start*16 + s->startb);
    if(s->declared != MDMA_AUTO && (uint32_t)s->declared * 16 == bytes)
        return;
    if(bytes % 16 != 0){
        badBody(l, "DIRECT", bytes, 16);
        return;
    }
    settle(l, s, (int32_t)(bytes/16), 0, 0xffff, "qwords");
}

/* One GIF tag qword. Written through mdmaAddD rather than a bare store so it
 * works wherever the cursor happens to be — a GIF tag is often data bound for
 * VU memory through an UNPACK rather than for the GIF at all. */
static void
gifTagQ(mdmaList *l, uint32_t nloop, uint32_t eop, uint32_t pre, uint32_t prim,
        uint32_t flg, uint32_t nreg, uint64_t regs)
{
    mdmaAddD(l, SCE_GIF_SET_TAG(nloop, eop, pre, prim, flg, nreg), regs);
}

void
mdmaGifTag(mdmaList *l, uint32_t nloop, uint32_t eop, uint32_t pre,
           uint32_t prim, uint32_t flg, uint32_t nreg, uint64_t regs)
{
    gifTagQ(l, nloop, eop, pre, prim, flg, nreg, regs);
}

void
mdmaGifTagAD(mdmaList *l, uint32_t nloop)
{
    gifTagQ(l, nloop, 1, 0, 0, GIF_PACKED, 1, GIF_AD);
}

void
mdmaEndGifTagChecked(mdmaList *l)
{
    mdmaScope *s;
    uint32_t flg, nreg, bytes, unit;
    int32_t n;

    s = top(l, MDMA_SC_GIFTAG);
    if(s == 0)
        return;
    l->depth--;
    flg = s->fmt & 0xff;
    nreg = (s->fmt >> 8) & 0xf;
    if(nreg == 0)
        nreg = 16;              /* NREG 0 encodes 16 */
    /* bytes one loop occupies: PACKED a qword per register, REGLIST a
     * doubleword per register, IMAGE a qword and no registers at all */
    unit = flg == GIF_REGLIST ? nreg*8 : (flg == GIF_IMAGE ? 16 : nreg*16);
    bytes = bytePos(l) - (s->start*16 + s->startb);
    if(s->declared != MDMA_AUTO && (uint32_t)s->declared * unit == bytes)
        return;
    if(unit == 0 || bytes % unit != 0){
        badBody(l, "giftag", bytes, unit);
        return;
    }
    n = (int32_t)(bytes / unit);
    settle(l, s, n, 0, 0x7fff, "loops");
}

void
mdmaMpg(mdmaList *l, uint32_t vuaddr, void *code, uint32_t qwc)
{
    uint8_t *p;

    p = (uint8_t*)code;
    while(qwc){
        uint32_t n;

        n = qwc > 128 ? 128 : qwc;      /* 128 qwords == 256 instructions */
        mdmaCnt(l, (int32_t)n);
            /* NUM counts double words, and 256 encodes as 0 */
            mdmaVifMpg(l, vuaddr & 0xffff, n*2, 0);
            mdmaAddN(l, p, n);
        mdmaCloseTag(l);
        vuaddr += n*2;
        p += n*16;
        qwc -= n;
    }
}

/* ------------------------------------------------------------------ *
 * 6. data
 * ------------------------------------------------------------------ */

/* The cold half of mdmaAdd(): the cursor is mid-qword, so the qword has to be
 * shuffled through the staging one a byte at a time. Correct, just slower —
 * mdmaVifAlign() before a run of qword adds is the fix. */
uint128_t *
mdmaSkip(mdmaList *l, uint32_t nq)
{
    uint128_t *p;

    if(l->pendb)
        mdmaPadPend(l);
    if(l->size + nq > l->limit)
        mdmaOverflow(l, nq);
    p = &l->p[l->size];
    l->size += nq;
    return p;
}

void
mdmaAddSlow(mdmaList *l, uint128_t q)
{
    uint8_t b[16];

    if(l->pendb == 0){
        pushQ(l, q);
        return;
    }
    warnUnaligned(l);
    memcpy(b, &q, 16);
    addBytes(l, b, 16);
}

void
mdmaAddN(mdmaList *l, const void *src, uint32_t nq)
{
    mdmaPadPend(l);
    room(l, nq);
    memcpy(&l->p[l->size], src, nq*16);
    l->size += nq;
}

void
mdmaAddU64(mdmaList *l, uint64_t v)
{
    uint32_t w[2];

    w[0] = (uint32_t)v;
    w[1] = (uint32_t)(v >> 32);
    addWords(l, w, 2);
}

void mdmaAddW1(mdmaList *l, uint32_t a)
    { addWords(l, &a, 1); }
void mdmaAddW2(mdmaList *l, uint32_t a, uint32_t b)
    { uint32_t w[2]; w[0]=a; w[1]=b; addWords(l, w, 2); }
void mdmaAddW3(mdmaList *l, uint32_t a, uint32_t b, uint32_t c)
    { uint32_t w[3]; w[0]=a; w[1]=b; w[2]=c; addWords(l, w, 3); }

void mdmaAddF1(mdmaList *l, float a)
    { mdmaAddW1(l, mdmaFbits(a)); }
void mdmaAddF2(mdmaList *l, float a, float b)
    { mdmaAddW2(l, mdmaFbits(a), mdmaFbits(b)); }
void mdmaAddF3(mdmaList *l, float a, float b, float c)
    { mdmaAddW3(l, mdmaFbits(a), mdmaFbits(b), mdmaFbits(c)); }

static void
addHalves(mdmaList *l, const int *v, uint32_t n)
{
    uint8_t b[2];

    while(n--){
        b[0] = (uint8_t)*v;
        b[1] = (uint8_t)((uint32_t)*v >> 8);
        addBytes(l, b, 2);
        v++;
    }
}

void mdmaAddH2(mdmaList *l, int a, int b)
    { int v[2]; v[0]=a; v[1]=b; addHalves(l, v, 2); }
void mdmaAddH3(mdmaList *l, int a, int b, int c)
    { int v[3]; v[0]=a; v[1]=b; v[2]=c; addHalves(l, v, 3); }
void mdmaAddH4(mdmaList *l, int a, int b, int c, int d)
    { int v[4]; v[0]=a; v[1]=b; v[2]=c; v[3]=d; addHalves(l, v, 4); }

void mdmaAddB3(mdmaList *l, int a, int b, int c)
    { uint8_t v[3]; v[0]=(uint8_t)a; v[1]=(uint8_t)b; v[2]=(uint8_t)c;
      addBytes(l, v, 3); }
void mdmaAddB4(mdmaList *l, int a, int b, int c, int d)
    { uint8_t v[4]; v[0]=(uint8_t)a; v[1]=(uint8_t)b; v[2]=(uint8_t)c;
      v[3]=(uint8_t)d; addBytes(l, v, 4); }

void
mdmaMark(mdmaList *l, uint32_t id, int irq)
{
    mdmaVifMark(l, id, 0);
    if(irq)
        mdmaTagFlags(l, IntFlg);
}

/* ------------------------------------------------------------------ *
 * 9. kicking
 * ------------------------------------------------------------------ */

void *
mdmaKickAddrOf(mdmaList *l)
{
    uint32_t a;

    a = MDMA_PHYS(l->p) | ((uint32_t)(size_t)l->p & MDMA_SEG_MASK);
    if((l->arena->flags & MDMA_MEM_MASK) == MDMA_MEM_SPR)
        return (void*)(size_t)((a & 0x3ff0) | 0x80000000u);
    return (void*)(size_t)MDMA_PHYS(a);
}

#if MDMA_HAVE_DMAC

mdmaChan *mdmaVIF1, *mdmaVIF0, *mdmaGIF, *mdmaSPRfrom, *mdmaSPRto;

void
mdmaInit(void)
{
    sceGsResetPath();
    sceDmaReset(1);

    mdmaGIF = sceDmaGetChan(SCE_DMA_GIF);
    mdmaGIF->chcr.TTE = 0;
    *GIF_MODE = 4;              /* IMT intermittent mode */

    mdmaVIF1 = sceDmaGetChan(SCE_DMA_VIF1);
    mdmaVIF1->chcr.TTE = 1;
    mdmaVIF1->chcr.TIE = 1;

    mdmaVIF0 = sceDmaGetChan(SCE_DMA_VIF0);
    mdmaSPRfrom = sceDmaGetChan(SCE_DMA_fromSPR);
    mdmaSPRto = sceDmaGetChan(SCE_DMA_toSPR);
}

void
mdmaKick(mdmaChan *chan, mdmaList *l)
{
    if(l->depth != 0)
        mdmaFail("kicking a list with a scope still open",
                 curFile(l), curLine(l), 1);
    if((l->arena->flags & MDMA_MEM_MASK) == MDMA_MEM_CACHED)
        MDMA_FLUSHCACHE();
    sceDmaSend(chan, mdmaKickAddrOf(l));
}

void
mdmaKickAddr(mdmaChan *chan, void *chain)
{
    MDMA_FLUSHCACHE();
    sceDmaSend(chan, (void*)(size_t)MDMA_PHYS(chain));
}

void
mdmaSync(mdmaChan *chan)
{
    sceDmaSync(chan, 0, 0);
}

int
mdmaBusy(mdmaChan *chan)
{
    return sceDmaSync(chan, 1, 0) != 0;
}

#endif /* MDMA_HAVE_DMAC */

/* ------------------------------------------------------------------ *
 * 11. debug
 * ------------------------------------------------------------------ */

void
mdmaDump(mdmaList *l, FILE *out)
{
    int32_t i;

    fprintf(out, "mdma list: %u/%u qwords", (unsigned)mdmaListSize(l),
            (unsigned)l->limit);
    if(l->pendb)
        fprintf(out, " (+%u partial bytes)", (unsigned)l->pendb);
    fprintf(out, ", depth %d, %d relocs\n", (int)l->depth, (int)l->numRelocs);
    for(i = 0; i < l->depth; i++)
        fprintf(out, "  open %s at qword %u, declared %d\n",
                kindName(l->stack[i].kind), (unsigned)l->stack[i].off,
                (int)l->stack[i].declared);
    mdmaDisasm(l->p, l->size, out);
}

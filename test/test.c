/*
 * mdma host tests.
 *
 * Everything in §§1-6 is memory writing, so it can be checked exactly: build
 * a chain, compare it word for word against what the hardware should see.
 * Where a test is about the *checking* rather than the output, it installs a
 * fail hook and looks at what got reported.
 */

#include "mdma.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

static int nfail;
static int ntest;

/* ---- fail hook ---------------------------------------------------------- */

static char   msgs[16][256];
static int    nmsgs;
static int    nfatal;
static jmp_buf fatalJmp;
static int    catching;

static void
testFail(const char *msg, const char *file, int line, int fatal)
{
    if(nmsgs < 16){
        snprintf(msgs[nmsgs], sizeof msgs[0], "%s:%d: %s",
                 file ? file : "?", line, msg);
        nmsgs++;
    }
    if(fatal){
        nfatal++;
        if(catching)
            longjmp(fatalJmp, 1);
        printf("  !! unexpected fatal: %s\n", msg);
        exit(1);
    }
}

/* Below level 2 the same conditions arrive as a bare code through mdmaPanic,
 * so the harness catches both the same way. */
static void
testPanic(int code)
{
    if(nmsgs < 16){
        snprintf(msgs[nmsgs], sizeof msgs[0], "panic %d", code);
        nmsgs++;
    }
    nfatal++;
    if(catching)
        longjmp(fatalJmp, 1);
    printf("  !! unexpected panic: %d\n", code);
    exit(1);
}

static void
clearMsgs(void)
{
    nmsgs = 0;
    nfatal = 0;
    mdmaStatCounters.patches = 0;
    mdmaStatCounters.mismatches = 0;
}

#define EXPECT_FATAL(stmt) do {                     \
        catching = 1;                               \
        if(setjmp(fatalJmp) == 0){                  \
            stmt;                                   \
            catching = 0;                           \
            fail("expected a fatal report from: " #stmt); \
        }else                                       \
            catching = 0;                           \
    } while(0)

/* ---- plumbing ---------------------------------------------------------- */

static const char *curTest = "?";

static void
fail(const char *what)
{
    printf("  FAIL %s: %s\n", curTest, what);
    nfail++;
}

static void
start(const char *name)
{
    curTest = name;
    ntest++;
    clearMsgs();
}

static uint128_t   arenaMem[512] __attribute__((aligned(16)));
static mdmaArena arena;
static mdmaList  list;

static void
newList(uint32_t memflags)
{
    mdmaArenaInit(&arena, arenaMem, 512, 1, memflags);
    mdmaListInit(&list, &arena);
    memset(arenaMem, 0xdd, sizeof arenaMem);
}

static uint32_t
word(uint32_t qw, uint32_t w)
{
    return ((const uint32_t*)arenaMem)[qw*4 + w];
}

static void
expectW(uint32_t qw, uint32_t w, uint32_t want)
{
    if(word(qw, w) != want){
        char buf[128];
        snprintf(buf, sizeof buf, "qword %u word %u is %08x, want %08x",
                 qw, w, word(qw, w), want);
        fail(buf);
    }
}

static void
expectSize(uint32_t want)
{
    if(mdmaListSize(&list) != want){
        char buf[96];
        snprintf(buf, sizeof buf, "list is %u qwords, want %u",
                 mdmaListSize(&list), want);
        fail(buf);
    }
}

static void
expectInt(const char *what, long got, long want)
{
    if(got != want){
        char buf[128];
        snprintf(buf, sizeof buf, "%s is %ld, want %ld", what, got, want);
        fail(buf);
    }
}

/* Only level 2 has messages to match; the checks themselves still run below
 * it, so every other expectation in these tests stays meaningful. */
static void
expectMsg(const char *needle)
{
    int i;

    if(MDMA_DEBUG < 2)
        return;

    for(i = 0; i < nmsgs; i++)
        if(strstr(msgs[i], needle))
            return;
    {
        char buf[200];
        snprintf(buf, sizeof buf, "no report matching \"%s\" (got %d)",
                 needle, nmsgs);
        fail(buf);
        for(i = 0; i < nmsgs; i++)
            printf("       had: %s\n", msgs[i]);
    }
}

/* ---- tests ------------------------------------------------------------- */

/* The shape from MDMA-PROPOSAL.md §4: STCYCL and an UNPACK in the tag's two
 * VIF slots, vertex data inline. */
static void
testCntUnpack(void)
{
    start("cnt + unpack, explicit counts");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 2);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 4, 0));
        mdmaBeginUnpack(&list, 0|UNPACK_DBLBUF, 2, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1.0f, 2.0f, 3.0f, 4.0f);
            mdmaAddF(&list, 5.0f, 6.0f, 7.0f, 8.0f);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    expectSize(3);
    expectW(0, 0, DMAcnt | 2);
    expectW(0, 1, 0);
    expectW(0, 2, SCE_VIF1_SET_STCYCL(1, 4, 0));
    expectW(0, 3, SCE_VIF1_SET_UNPACKR(0, 2, UNPACK_V4_32, 0));
    expectW(1, 0, 0x3f800000);                  /* 1.0f */
    expectW(2, 3, 0x41000000);                  /* 8.0f */
    /* explicit counts everywhere means not one out-of-order store */
    expectInt("patches", mdmaStatCounters.patches, 0);
    expectInt("reports", nmsgs, 0);
}

/* Same chain with MDMA_AUTO must come out identical, and cost two patches. */
static void
testAutoMatchesExplicit(void)
{
    uint32_t explicitCopy[3*4];

    start("AUTO produces the same chain as explicit");
    newList(MDMA_MEM_CACHED);
    mdmaCnt(&list, 2);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 4, 0));
        mdmaBeginUnpack(&list, 0|UNPACK_DBLBUF, 2, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1.0f, 2.0f, 3.0f, 4.0f);
            mdmaAddF(&list, 5.0f, 6.0f, 7.0f, 8.0f);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);
    memcpy(explicitCopy, arenaMem, sizeof explicitCopy);

    clearMsgs();
    newList(MDMA_MEM_CACHED);
    mdmaCnt(&list, MDMA_AUTO);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 4, 0));
        mdmaBeginUnpack(&list, 0|UNPACK_DBLBUF, MDMA_AUTO, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1.0f, 2.0f, 3.0f, 4.0f);
            mdmaAddF(&list, 5.0f, 6.0f, 7.0f, 8.0f);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    if(memcmp(explicitCopy, arenaMem, sizeof explicitCopy) != 0)
        fail("AUTO and explicit chains differ");
    expectInt("patches", mdmaStatCounters.patches, 2);
    expectInt("reports in cached memory", nmsgs, 0);
}

/* The whole point of §1: a wrong explicit count is a line number, not a
 * corrupt chain. */
static void
testMismatchReported(void)
{
    start("a wrong explicit count is reported and repaired");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 3);                  /* lying: only 2 qwords follow */
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 4, 0));
        mdmaBeginUnpack(&list, 0, 2, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 2, 3, 4);
            mdmaAddF(&list, 5, 6, 7, 8);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

#if MDMA_DEBUG
    expectInt("mismatches", mdmaStatCounters.mismatches, 1);
    expectMsg("tag declared 3 qwords, wrote 2 (patched)");
    expectMsg("test/test.c");
    expectW(0, 0, DMAcnt | 2);     /* repaired */
#else
    /* release trusts what it was told: no report, no compare, no repair */
    expectInt("reports", nmsgs, 0);
    expectInt("patches", mdmaStatCounters.patches, 0);
    expectW(0, 0, DMAcnt | 3);
#endif
}

static void
testUnpackNumMismatch(void)
{
    start("a wrong UNPACK num is reported in elements");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginUnpack(&list, 4, 3, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 2, 3, 4);
            mdmaAddF(&list, 5, 6, 7, 8);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

#if MDMA_DEBUG
    expectMsg("unpack declared 3 elements, wrote 2 (patched)");
    expectW(0, 3, SCE_VIF1_SET_UNPACK(4, 2, UNPACK_V4_32, 0));
#else
    expectW(0, 3, SCE_VIF1_SET_UNPACK(4, 3, UNPACK_V4_32, 0));
#endif
}

/* AUTO in UCA is the case the hybrid design exists to discourage. */
static void
testAutoWarnsInUca(void)
{
    start("AUTO in a UCA arena warns once per site");
    if(MDMA_DEBUG < 2)                  /* advice is a level-2 luxury */
        return;
    newList(MDMA_MEM_UCA);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 1, 0));
        mdmaBeginUnpack(&list, 0, 1, UNPACK_V4_32, 0);
            mdmaAddF(&list, 0, 0, 0, 1);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);
#if !MDMA_DEBUG
    expectInt("reports in a release build", nmsgs, 0);
    return;                     /* the warning is a debug-build service */
#endif
    expectMsg("breaks the write gather");
    expectInt("reports", nmsgs, 1);

    /* the same site again must stay quiet — one new AUTO site (the mdmaCnt),
     * warned once however many times the loop runs */
    {
        int before = nmsgs, i;
        for(i = 0; i < 3; i++){
            mdmaCnt(&list, MDMA_AUTO);
                mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 1, 0));
                mdmaBeginUnpack(&list, 0, 1, UNPACK_V4_32, 0);
                    mdmaAddF(&list, 0, 0, 0, 1);
                mdmaEndUnpack(&list);
            mdmaCloseTag(&list);
        }
        expectInt("extra reports for a repeated site", nmsgs - before, 1);
    }
}

/* Sub-qword adds: five UNPACK_V4_8 elements are 20 bytes, so the packet holds two
 * qwords with the tail zero-padded, and NUM is 5. */
static void
testNarrowUnpack(void)
{
    start("UNPACK_V4_8 unpack pads to a qword and counts elements");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginUnpack(&list, 10|UNPACK_USN, MDMA_AUTO, UNPACK_V4_8, 0);
            mdmaAddB4(&list, 1, 2, 3, 4);
            mdmaAddB4(&list, 5, 6, 7, 8);
            mdmaAddB4(&list, 9, 10, 11, 12);
            mdmaAddB4(&list, 13, 14, 15, 16);
            mdmaAddB4(&list, 17, 18, 19, 20);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    expectSize(3);
    expectW(0, 0, DMAcnt | 2);
    expectW(0, 3, (UNPACK_V4_8<<24) | UNPACK_USN | 5<<16 | 10);
    expectW(1, 0, 0x04030201);
    expectW(2, 0, 0x14131211);
    expectW(2, 1, 0);                   /* padding */
    expectW(2, 3, 0);
}

static void
testV3_32(void)
{
    start("UNPACK_V3_32 elements straddle qwords");
    newList(MDMA_MEM_CACHED);

    /* 3 elements * 12 bytes = 36 bytes = 2.25 qwords -> 3 qwords */
    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginUnpack(&list, 0, MDMA_AUTO, UNPACK_V3_32, 0);
            mdmaAddW3(&list, 1, 2, 3);
            mdmaAddW3(&list, 4, 5, 6);
            mdmaAddW3(&list, 7, 8, 9);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    expectSize(4);
    expectW(0, 0, DMAcnt | 3);
    expectW(0, 3, SCE_VIF1_SET_UNPACK(0, 3, UNPACK_V3_32, 0));
    expectW(1, 3, 4);                   /* second element starts mid-qword */
    expectW(3, 0, 9);
    expectW(3, 1, 0);
}

static void
testDirectGif(void)
{
    start("DIRECT + PACKED A+D giftag, all AUTO");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginDirect(&list, MDMA_AUTO, 0);
            mdmaBeginGifTagAD(&list, MDMA_AUTO);
                mdmaAddAD(&list, 0x47, 0x12345678);     /* TEST_1  */
                mdmaAddAD(&list, 0x42, 0x00000044);     /* ALPHA_1 */
            mdmaEndGifTag(&list);
        mdmaEndDirect(&list);
    mdmaCloseTag(&list);

    expectSize(4);
    expectW(0, 0, DMAcnt | 3);
    expectW(0, 2, SCE_VIF1_SET_NOP(0));   /* so the DIRECT is the last */
    expectW(0, 3, SCE_VIF1_SET_DIRECT(3, 0));
    expectW(1, 0, 2 | 1<<15);            /* nloop 2, eop */
    expectW(1, 1, 1u<<28);               /* nreg 1 */
    expectW(1, 2, 0xe);                  /* A+D */
    expectW(2, 0, 0x12345678);
    expectW(2, 2, 0x47);
    expectW(3, 2, 0x42);
}

static void
testGifImage(void)
{
    start("IMAGE giftag counts qwords");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginDirect(&list, MDMA_AUTO, 0);
            mdmaBeginGifTag(&list, MDMA_AUTO, 1, 0, 0, GIF_IMAGE, 0, 0);
                mdmaAddW(&list, 1, 2, 3, 4);
                mdmaAddW(&list, 5, 6, 7, 8);
                mdmaAddW(&list, 9, 10, 11, 12);
            mdmaEndGifTag(&list);
        mdmaEndDirect(&list);
    mdmaCloseTag(&list);

    expectW(1, 0, 3 | 1<<15);
    expectW(0, 3, SCE_VIF1_SET_DIRECT(4, 0));
}

static void
testGifReglist(void)
{
    start("REGLIST giftag counts register sets");
    newList(MDMA_MEM_CACHED);

    /* two registers per loop, three loops = six 64-bit values = 3 qwords */
    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginDirect(&list, MDMA_AUTO, 0);
            mdmaBeginGifTag(&list, MDMA_AUTO, 1, 1, 6 /*tristrip*/, GIF_REGLIST,
                       2, UINT64(0, SCE_GS_XYZ2<<4 | SCE_GS_RGBAQ));
                mdmaAddU64(&list, 0x1111);  mdmaAddU64(&list, 0x2222);
                mdmaAddU64(&list, 0x3333);  mdmaAddU64(&list, 0x4444);
                mdmaAddU64(&list, 0x5555);  mdmaAddU64(&list, 0x6666);
            mdmaEndGifTag(&list);
        mdmaEndDirect(&list);
    mdmaCloseTag(&list);

    expectSize(5);
    expectW(1, 0, 3 | 1<<15);
    expectW(0, 3, SCE_VIF1_SET_DIRECT(4, 0));
}

/* A tag is one VIF word stream: the first two words ride in the tag's spare
 * 64 bits and the rest continue in the body, in that order. */
static void
testVifStreamSpills(void)
{
    start("a tag's VIF stream spills from the tag into the body");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 1);
        mdmaVifStCycl(&list, 1, 1, 0);
        mdmaVifItop(&list, 4, 0);
        mdmaVifMsCal(&list, 0, 0);
        mdmaVifNop(&list, 0);
    mdmaCloseTag(&list);

    expectSize(2);
    expectW(0, 0, DMAcnt | 1);
    expectW(0, 2, SCE_VIF1_SET_STCYCL(1, 1, 0));
    expectW(0, 3, SCE_VIF1_SET_ITOP(4, 0));
    expectW(1, 0, SCE_VIF1_SET_MSCAL(0, 0));
    expectW(1, 1, SCE_VIF1_SET_NOP(0));
    expectW(1, 2, 0);                   /* mdmaCloseTag pads, and 0 is NOP */
    expectW(1, 3, 0);
    expectInt("reports", nmsgs, 0);
}

/* A ref has no body, so there the third word genuinely has nowhere to go. */
static void
testThirdVifOnRefFails(void)
{
    start("a third VIF code on a ref fails");
    if(MDMA_DEBUG < 1)                  /* level 0 takes your word for it */
        return;
    newList(MDMA_MEM_CACHED);

    mdmaRef(&list, arenaMem, 1);
        mdmaVifStCycl(&list, 1, 1, 0);
        mdmaVifItop(&list, 4, 0);
        EXPECT_FATAL(mdmaVifMsCal(&list, 0, 0));
    expectMsg("a ref has no body");
}

/* With TTE off the DMAC does not hand the tag to the VIF at all, so the same
 * calls put the whole stream in the body. */
static void
testStreamWithoutTTE(void)
{
    start("TTE off puts the whole VIF stream in the body");
    newList(MDMA_MEM_CACHED);
    mdmaListTTE(&list, 0);

    mdmaCnt(&list, 1);
        mdmaVifStCycl(&list, 1, 1, 0);
        mdmaVifItop(&list, 4, 0);
    mdmaCloseTag(&list);

    expectSize(2);
    expectW(0, 2, 0);                   /* untouched */
    expectW(0, 3, 0);
    expectW(1, 0, SCE_VIF1_SET_STCYCL(1, 1, 0));
    expectW(1, 1, SCE_VIF1_SET_ITOP(4, 0));
    expectInt("reports", nmsgs, 0);
}

/* mdmaTagWords is the same thing per tag: claim the spare words for something
 * of your own and the stream starts in the body. */
static void
testTagWords(void)
{
    start("mdmaTagWords claims the tag's spare words");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 1);
        mdmaTagWords(&list, 0xdeadbeef, 0x12345678);
        mdmaVifStCycl(&list, 1, 1, 0);
    mdmaCloseTag(&list);

    expectW(0, 2, 0xdeadbeef);
    expectW(0, 3, 0x12345678);
    expectW(1, 0, SCE_VIF1_SET_STCYCL(1, 1, 0));

    clearMsgs();
    mdmaCnt(&list, MDMA_AUTO);
        mdmaVifStCycl(&list, 1, 1, 0);
        EXPECT_FATAL(mdmaTagWords(&list, 1, 2));
    expectMsg("already claimed a word");
}

/* The VIF works in words, so an UNPACK body need not start on a qword
 * boundary — and its NUM is patched wherever the code landed, including in
 * the staging qword when the body is too small to have flushed it. */
static void
testUnalignedUnpack(void)
{
    start("an UNPACK body may start mid-qword");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 1);
        mdmaVifStCycl(&list, 1, 1, 0);
        mdmaVifNop(&list, 0);
        mdmaVifNop(&list, 0);                                   /* body wd 0 */
        mdmaBeginUnpack(&list, 0, MDMA_AUTO, UNPACK_V1_32, 0);  /* body wd 1 */
            mdmaAddW1(&list, 0x11111111);
            mdmaAddW1(&list, 0x22222222);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    expectSize(2);
    expectW(1, 0, SCE_VIF1_SET_NOP(0));
    expectW(1, 1, SCE_VIF1_SET_UNPACK(0, 2, UNPACK_V1_32, 0));
    expectW(1, 2, 0x11111111);
    expectW(1, 3, 0x22222222);
    expectInt("reports", nmsgs, 0);
}

/* mdmaVifAlign fills whatever is left — tag words included — so the next
 * whole-qword add is one store rather than a byte shuffle. */
static void
testVifAlign(void)
{
    start("mdmaVifAlign pads to the next qword");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 2);
        mdmaVifStCycl(&list, 1, 1, 0);
        mdmaVifAlign(&list);            /* one tag word left: NOP it */
        mdmaVifNop(&list, 0);           /* body word 0 */
        mdmaVifAlign(&list);            /* three body words to pad */
        mdmaAddW(&list, 1, 2, 3, 4);
    mdmaCloseTag(&list);

    expectSize(3);
    expectW(0, 3, SCE_VIF1_SET_NOP(0));
    expectW(1, 0, SCE_VIF1_SET_NOP(0));
    expectW(1, 3, 0);
    expectW(2, 0, 1);
    expectW(2, 3, 4);
    expectInt("reports", nmsgs, 0);
}

/* A code whose data follows it in the stream may not sit in the first of the
 * tag's two spare words: the second one would be read as the first word of
 * that data. mdma slips a NOP in so the code lands in the second word and the
 * data starts at the top of the body — while a code that takes no data is
 * free to use the first word. */
static void
testDataCodeNotFirstSlot(void)
{
    start("a data-carrying code never takes the tag's first word");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, 1);
        mdmaBeginDirect(&list, 1, 0);
            mdmaBeginGifTagAD(&list, 0);
            mdmaEndGifTag(&list);
        mdmaEndDirect(&list);
    mdmaCloseTag(&list);

    expectSize(2);
    expectW(0, 0, DMAcnt | 1);
    expectW(0, 2, SCE_VIF1_SET_NOP(0));
    expectW(0, 3, SCE_VIF1_SET_DIRECT(1, 0));
    expectW(1, 0, 1u<<15);              /* the GIF tag, at the top of the body */

    /* but a code with no data still gets the first word, and then the
     * data-carrying one needs no NOP of its own */
    newList(MDMA_MEM_CACHED);
    mdmaCnt(&list, 1);
        mdmaVifStCycl(&list, 1, 1, 0);
        mdmaBeginUnpack(&list, 0, 1, UNPACK_V4_32, 0);
            mdmaAddW(&list, 1, 2, 3, 4);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    expectSize(2);
    expectW(0, 2, SCE_VIF1_SET_STCYCL(1, 1, 0));
    expectW(0, 3, SCE_VIF1_SET_UNPACK(0, 1, UNPACK_V4_32, 0));
    expectW(1, 0, 1);
    expectInt("reports", nmsgs, 0);
}

/* xtc's default pipeline upload, which is what the stream model was designed
 * against: two FLUSHes in the tag words, then three STCYCL+UNPACK pairs whose
 * bodies fall where they fall. Written with whole qwords and no hand padding
 * it comes to 13 body qwords exactly — one less than the same packet built a
 * qword at a time, which had to spend NOPs squaring each UNPACK up. */
static void
testXtcUploadShape(void)
{
    int i;

    start("the xtc upload packet, unpadded");
    newList(MDMA_MEM_CACHED);

    mdmaNext(&list, 0, 13);
        mdmaVifFlush(&list, 0);                 /* tag word 0 */
        mdmaVifFlush(&list, 0);                 /* tag word 1 */
        mdmaVifBase(&list, 0, 0);               /* body, from here on */
        mdmaVifOffset(&list, 0x100, 0);
        mdmaVifStCycl(&list, 4, 4, 0);
        mdmaBeginUnpack(&list, 0, 7, UNPACK_V4_32, 0);
            for(i = 0; i < 7; i++)              /* matrix, scale, clip consts */
                mdmaAddF(&list, 1, 2, 3, 4);
        mdmaEndUnpack(&list);
        mdmaVifStCycl(&list, 4, 4, 0);
        mdmaBeginUnpack(&list, 8, 3, UNPACK_V4_32, 0);
            mdmaGifTag(&list, 0, 1, 1, 4, GIF_PACKED, 3,
                       GIF_ST | GIF_RGBAQ<<4 | GIF_XYZF2<<8);
            mdmaAddF(&list, 1, 1, 1, 1);        /* material colour */
            mdmaAddF(&list, 0, 0, 0, 0);        /* material params */
        mdmaEndUnpack(&list);
        mdmaVifStCycl(&list, 4, 4, 0);
        mdmaBeginUnpack(&list, 16, 1, UNPACK_V4_32, 0);
            mdmaAddW(&list, 0, 0, 0, 0);        /* microcode switch */
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    expectSize(14);                             /* the tag and 13 body qwords */
    expectW(0, 0, DMAnext | 13);
    expectW(0, 2, SCE_VIF1_SET_FLUSH(0));
    expectW(0, 3, SCE_VIF1_SET_FLUSH(0));
    expectW(1, 0, SCE_VIF1_SET_BASE(0, 0));
    expectW(1, 3, SCE_VIF1_SET_UNPACK(0, 7, UNPACK_V4_32, 0));
    /* the second pair lands mid-qword, which is legal and is the qword saved */
    expectW(9, 1, SCE_VIF1_SET_UNPACK(8, 3, UNPACK_V4_32, 0));
    expectW(12, 3, SCE_VIF1_SET_UNPACK(16, 1, UNPACK_V4_32, 0));
    expectInt("reports", nmsgs, 0);
}

static void
testTagInTagFails(void)
{
    start("a tag inside a tag fails");
    if(MDMA_DEBUG < 1)                  /* level 0 takes your word for it */
        return;
    newList(MDMA_MEM_CACHED);
    mdmaCnt(&list, MDMA_AUTO);
        EXPECT_FATAL(mdmaCnt(&list, MDMA_AUTO));
    expectMsg("cannot open inside another scope");
}

static void
testMismatchedCloseFails(void)
{
    start("closing the wrong kind of scope fails");
    newList(MDMA_MEM_CACHED);
    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginUnpack(&list, 0, MDMA_AUTO, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 2, 3, 4);
        EXPECT_FATAL(mdmaCloseTag(&list));
    expectMsg("close of tag, but the open scope is a unpack");
}

static void
testOverflowFails(void)
{
    start("arena overflow fails with a count");
    mdmaArenaInit(&arena, arenaMem, 4, 1, MDMA_MEM_CACHED);
    mdmaListInit(&list, &arena);
    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginUnpack(&list, 0, MDMA_AUTO, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 2, 3, 4);
            mdmaAddF(&list, 1, 2, 3, 4);
            mdmaAddF(&list, 1, 2, 3, 4);
            EXPECT_FATAL(mdmaAddF(&list, 1, 2, 3, 4));
    expectMsg("arena overflow: 4 of 4 qwords used");
}

/* xtc's zero-copy idiom: jump over the vertices with a next, then ref back
 * into them. The vertex count is not known until the end, which is the one
 * place AUTO genuinely earns its keep. */
static void
testNextOverInlineData(void)
{
    mdmaTag *tag;
    uint128_t *verts;

    start("jump over inline vertices, then ref back into them");
    newList(MDMA_MEM_CACHED);

    /* an upload: a next tag with two qwords of constants inline */
    mdmaNext(&list, 0, 2);
        tag = mdmaCurTag(&list);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 1, 0));
        mdmaBeginUnpack(&list, 8, 2, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 0, 0, 0);
            mdmaAddF(&list, 0, 1, 0, 0);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);

    /* vertices written straight into the chain buffer; no tag counts them */
    verts = mdmaSkip(&list, 3);
    memset(verts, 0x11, 3*16);

    /* the upload's next jumps past them ... */
    tag->addr = mdmaAddr(mdmaHere(&list));
    expectW(0, 0, DMAnext | 2);
    expectW(0, 1, MDMA_PHYS(&((uint128_t*)arenaMem)[6]));

    /* ... and a ref points back at them, zero copy */
    mdmaRef(&list, verts, 3);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 1, 0));
        mdmaVifUnpack(&list, 0|UNPACK_DBLBUF, 3, UNPACK_V4_32, 0);

    expectSize(7);
    expectW(6, 0, DMAref | 3);
    expectW(6, 1, MDMA_PHYS(&((uint128_t*)arenaMem)[3]));
    expectW(6, 2, SCE_VIF1_SET_STCYCL(1, 1, 0));
    expectW(6, 3, SCE_VIF1_SET_UNPACKR(0, 3, UNPACK_V4_32, 0));
    expectInt("reports", nmsgs, 0);
}

/* An inline VIF code costs a word instead of a whole tag qword, and the
 * padding to the next qword is NOPs, which is what xtc does today. */
static void
testInlineVifCodes(void)
{
    start("inline VIF codes and NOP padding");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 1, 0));
        mdmaBeginUnpack(&list, 0, MDMA_AUTO, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 2, 3, 4);
        mdmaEndUnpack(&list);
        mdmaVif(&list, SCE_VIF1_SET_ITOP(8, 0));
        mdmaVif(&list, SCE_VIF1_SET_MSCAL(0, 0));
    mdmaCloseTag(&list);

    expectSize(3);
    expectW(0, 0, DMAcnt | 2);
    expectW(2, 0, SCE_VIF1_SET_ITOP(8, 0));
    expectW(2, 1, SCE_VIF1_SET_MSCAL(0, 0));
    expectW(2, 2, 0);
    expectW(2, 3, 0);
}

static void
testMpgChunking(void)
{
    static uint128_t code[200];
    start("mdmaMpg splits at 256 instructions");
    newList(MDMA_MEM_CACHED);

    memset(code, 0x11, sizeof code);
    mdmaMpg(&list, 0, code, 200);

    /* 200 qwords = 400 instructions -> 128 + 72 qwords, two tags */
    expectSize(1 + 128 + 1 + 72);
    expectW(0, 0, DMAcnt | 128);
    expectW(0, 3, SCE_VIF1_SET_MPG(0, 0, 0));         /* num 256 encodes as 0 */
    expectW(129, 0, DMAcnt | 72);
    expectW(129, 3, SCE_VIF1_SET_MPG(256, 144, 0));   /* second half, addr 256 */
    expectInt("reports", nmsgs, 0);
}

static void
testRelocs(void)
{
    static uint32_t relocs[8];
    start("ref and call addresses are recorded for relocation");
    newList(MDMA_MEM_CACHED);
    mdmaListRelocs(&list, relocs, 8);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 1, 0));
        mdmaBeginUnpack(&list, 0, MDMA_AUTO, UNPACK_V4_32, 0);
            mdmaAddF(&list, 1, 2, 3, 4);
        mdmaEndUnpack(&list);
    mdmaCloseTag(&list);
    expectInt("relocs for a pure cnt chain", list.numRelocs, 0);

    mdmaCall(&list, (void*)0x00123450, 0);
    mdmaCloseTag(&list);
    expectInt("relocs after a call", list.numRelocs, 1);
    expectInt("reloc offset", relocs[0], 2);
}

static void
testFlipAndReset(void)
{
    start("double buffering flips the write pointer");
    mdmaArenaInit(&arena, arenaMem, 16, 2, MDMA_MEM_CACHED);
    mdmaListInit(&list, &arena);
    if(mdmaListData(&list) != (uint128_t*)arenaMem)
        fail("first list is not at the arena base");
    mdmaArenaFlip(&arena);
    mdmaListReset(&list);
    if(mdmaListData(&list) != (uint128_t*)arenaMem + 16)
        fail("flipped list is not at the second buffer");
    mdmaArenaFlip(&arena);
    mdmaListReset(&list);
    if(mdmaListData(&list) != (uint128_t*)arenaMem)
        fail("did not wrap back to the first buffer");
}

/* An end-to-end read of a realistic batch, printed by the disassembler. Not
 * an assertion so much as the thing you look at when something is wrong. */
static void
testDisasm(void)
{
    start("disassemble a realistic batch");
    newList(MDMA_MEM_CACHED);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaBeginDirect(&list, MDMA_AUTO, 0);
            mdmaBeginGifTagAD(&list, MDMA_AUTO);
                mdmaAddAD(&list, 0x47, 0x00050006);         /* TEST_1  */
                mdmaAddAD(&list, 0x4c, 0x00a01400);         /* FRAME_1 */
            mdmaEndGifTag(&list);
        mdmaEndDirect(&list);
    mdmaCloseTag(&list);

    mdmaCnt(&list, MDMA_AUTO);
        mdmaVif(&list, SCE_VIF1_SET_STCYCL(1, 3, 0));
        mdmaBeginUnpack(&list, 0|UNPACK_DBLBUF, MDMA_AUTO, UNPACK_V4_32, 0);
            mdmaAddF(&list, -1, -1, 0, 1);
            mdmaAddF(&list,  1, -1, 0, 1);
            mdmaAddF(&list,  0,  1, 0, 1);
        mdmaEndUnpack(&list);
        mdmaVif(&list, SCE_VIF1_SET_ITOP(3, 0));
        mdmaVif(&list, SCE_VIF1_SET_MSCAL(0, 0));
    mdmaCloseTag(&list);

    mdmaEnd(&list, 0);
    mdmaCloseTag(&list);

    expectInt("reports", nmsgs, 0);
    printf("\n--- disassembly ---\n");
    mdmaDisasm(mdmaListData(&list), mdmaListSize(&list), stdout);
    printf("--- end ---\n\n");
}

int
main(void)
{
    mdmaFail = testFail;
    mdmaPanic = testPanic;

    testCntUnpack();
    testAutoMatchesExplicit();
    testMismatchReported();
    testUnpackNumMismatch();
    testAutoWarnsInUca();
    testNarrowUnpack();
    testV3_32();
    testDirectGif();
    testGifImage();
    testGifReglist();
    testVifStreamSpills();
    testThirdVifOnRefFails();
    testStreamWithoutTTE();
    testTagWords();
    testUnalignedUnpack();
    testVifAlign();
    testDataCodeNotFirstSlot();
    testXtcUploadShape();
    testTagInTagFails();
    testMismatchedCloseFails();
    testOverflowFails();
    testNextOverInlineData();
    testInlineVifCodes();
    testMpgChunking();
    testRelocs();
    testFlipAndReset();
    testDisasm();

    printf("%d tests, %d failures\n", ntest, nfail);
    return nfail != 0;
}

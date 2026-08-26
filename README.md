# mdma

A PS2 DMA/VIF/GIF packet library: tags, VIF codes, GIF tags, counted scopes,
arenas, kicking. It has no opinion about rendering and nothing in it knows what
a vertex is.

It exists because the SCE SDK shipped six DMA-chain APIs that do not
interoperate and none of them is the one you want, and because both
[xtc](../xtc) and librw/ps2 had independently hand-rolled the same layer. See
`MDMA-PROPOSAL.md` and `XTC-DESIGN.md` §4 in `~/fun/scenanigans` for the
reasoning.

## The idea

A tag is one VIF word stream. Its first two words ride in the tag's spare 64
bits, which is where they cost nothing, and the rest continue in the body:

```c
mdmaCnt(&l, 2);                                    /* a tag, qwc 2 */
    mdmaVifStCycl(&l, 1, 4, 0);                    /* in the tag        */
    mdmaBeginUnpack(&l, 0|UNPACK_DBLBUF, 2, UNPACK_V4_32, 0);
        mdmaAddF(&l, 1, 2, 3, 4);
        mdmaAddF(&l, 5, 6, 7, 8);
    mdmaEndUnpack(&l);
    mdmaVifItop(&l, 8, 0);                         /* in the body       */
    mdmaVifMsCal(&l, 0, 0);
mdmaCloseTag(&l);
```

Where a word lands is mdma's business, not the caller's: a tag with TTE off,
or one whose spare words `mdmaTagWords` claimed for something else, takes the
same calls and simply starts in the body. Only a `ref` has nowhere to
continue, and there a third word is an error.

The one placement rule is that a code whose data follows it in the stream —
UNPACK, DIRECT, MPG, the ST* codes — cannot take the tag's *first* spare word,
since the second one would then be read as the first word of its data. mdma
puts a NOP there and the code takes the second word, which is what
`(VIFnop, VIFdirect)` in hand-written chains has always been for.

The VIF works in words. Qword alignment is a DMA property, so an UNPACK body
may start and end wherever it likes and nothing is padded except the tail of
the transfer, by `mdmaCloseTag`. `mdmaVifAlign` is there for when you *want*
the boundary — a run of whole-qword adds is one store each when the cursor is
aligned and a byte shuffle when it isn't.

Scopes nest the way the hardware does: tag > (unpack|direct) > giftag. The
bare `mdmaVifUnpack` / `mdmaGifTag` emit just the code or just the tag, for
when what they describe is not in this list — under a `ref`, or bound for VU
memory through an UNPACK; the `mdmaBegin*` forms count the body that follows.

Names and argument order are eestruct.h's `SCE_VIF1_SET_*` — a shorter
spelling, not a second dialect.

## Counts

Every counted thing takes the count you expect, or `MDMA_AUTO` to have it
patched on close.

Explicit is the default idiom, because chains are usually built in
uncached-accelerated memory whose write-gather buffer only coalesces
*sequential* stores; a back-patch is one out-of-order store that breaks the
gather in progress. `MDMA_AUTO` is for the two or three places where the count
is only knowable afterwards.

That trade is only safe because the counts are checked. `MDMA_DEBUG` is a
level rather than a flag, because the checking is cheap and the *reporting* is
not — strings, `sprintf`, the stack frames they need, and two extra arguments
on every scope-opening call:

| `MDMA_DEBUG` | explicit count | `MDMA_AUTO` |
|---|---|---|
| 0 (`NDEBUG`) | written once at open, never read again — no compare, no patch | patched |
| 1 (default) | verified and repaired, silently | patched |
| 2 | ...and reported with `file:line` | patched, and warned about once per site in UCA/SPR memory |

```
mdma: ps2render.cpp:127: tag declared 12 qwords, wrote 13 (patched)
```

That turns "I added a qword and forgot the count" from a corrupt chain into a
line number, which is most of the value of the scheme — and level 1 keeps the
repair without paying for the sentence. The few conditions no build can carry
on from go through `mdmaPanic(code)` instead, which passes a code and nothing
else, so the call site is a load-immediate in a branch never taken.

## Weight

Writing a qword and appending a VIF code are what a frame does thousands of
times. Both are a handful of instructions when nothing unusual is happening, so
both are `static __inline__` in the header, with the cold half out of line —
`mdmaAdd` is a compare and an `sq`, a VIF code is a compare and a store. The
one-line `mdmaVif*` family, `mdmaAddW/F/D/AD`, `mdmaSkip` and `mdmaHere` come
with them. Nothing that runs per qword keeps a stack frame for a branch it does
not take.

A scope is the other thing that used to cost more than it was worth. It exists
only to settle a count — to derive it for `MDMA_AUTO`, or to verify an explicit
one — so at level 0 an explicit count gets no scope at all: the opener writes
its VIF code and the close is a compare that finds nothing to close. That fold
only happens if the test on the count is the *first* thing in the inlined
opener; put any control flow in front of it and ee-gcc stops believing the
argument is a constant and emits `li 7 / li -1 / bne`.

For scale: xtc's default-pipeline upload, which builds a 15-qword packet, went
from 31 calls into mdma to 9, and at level 0 `mdmaBeginUnpack(l, vuMatrix, 7,
UNPACK_V4_32, 0)` is now a constant `lui/ori` and a `jal` to a leaf.

## Building

```
make test     # host unit tests — everything but §9, which needs a DMAC
make lib      # libmdma.a for the host
make ee       # libmdma.a for the EE, via freesce's ee-gcc 2.9
```

Four configurations are known to build clean: the host in debug and release,
the EE with freesce's ee-gcc 2.9 against freesce's headers, and the EE with
ps2dev's gcc 15 against the same headers. The stock SDK headers work too —
`mdmaplat.h` supplies the sized types itself when it does not see freesce's
`eetypes.h`, since Sony's has `u_long128` and nothing else and that toolchain
ships no `<stdint.h>` at all.

`MDMA_DEBUG` defaults from `NDEBUG` per translation unit, so build mdma and its
consumers at the same level — a level-2 libmdma.a linked into level-0 code
would disagree with the caller about the `__FILE__`/`__LINE__` arguments. Now
that most of the library is inline in the header, the level a *caller* is built
at is what decides what that caller pays for.

`mdmaplat.h` is the only file that knows what machine it is on. mdma speaks the
SCE runtime API (`sceDmaChan`, `sceDmaSend`, `sceGs*`, and eestruct.h's
`SCE_VIF0/1_SET_*` / `SCE_GIF_*` / `SCE_GS_*` constants), which is both what
`/usr/local/sce` provides and what [freesce](../freesce) reimplements for the
ps2dev toolchain. Only §9 needs the DMAC itself; on the host, freesce's headers
stand in for the SDK's, so the same source builds and is tested there — and the
tests verify chains built with the exact constants the EE will use.

mdma wraps that API rather than reaching around it, so a consumer needs no
eestruct.h of its own for anything mdma understands: the DMA tag ids, the
UNPACK formats (`UNPACK_V4_32` … `UNPACK_V1_8`) and their modifier bits, the
VIF codes as `mdmaVif*` functions, and the GIF tag fields (`GIF_PACKED`,
`GIF_AD`, …) — because mdma constructs GIF tags and so ought to name their
parts. It stops there: GS register numbers and the `SCE_GS_SET_*` value
constructors are the renderer's business.

## State

Implemented and tested: §§0-6 (tags, VIF/GIF scopes, data, arenas), §9
(kicking), §11 (count checking, `mdmaDisasm`).

Not yet, in the order `MDMA-PROPOSAL.md` §7 asks for them: deferred patch slots
(§7), sealed relocatable chains (§8), arena growth and MFIFO (§10). `mdmaRef`
and `mdmaCall` already record their relocations, so §8 is cheap when it comes.

Undecided, and marked so in the header: whether `STROW`/`STCOL`/`STMASK`
should take the data that follows them as arguments instead of leaving you to
write it — nothing has used them yet. `mdmaTagWords` trusts you to build a
tag that makes sense; checks for it can come later.

`mdmaDisasm` interprets a chain the way the hardware would and prints it. It is
the test oracle, and it is also the piece a host-side XGKICK-trace tool over
DECI2 would want, which is why it decodes GIF and depends on nothing.

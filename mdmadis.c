/*
 * mdmadis.c — interpret a chain the way the hardware would, and print it.
 *
 * This is mdma's test oracle: it is much easier to believe a chain is right
 * when something independent of the builder reads it back. It is also the
 * piece the DECI2 XGKICK-trace tool would want on the host side, which is why
 * it decodes GIF as well as VIF and has no dependency on the EE.
 *
 * It walks a buffer linearly rather than following next/call, which is what
 * you want when inspecting one: cnt-chains are linear anyway, and the
 * addresses in a chain built elsewhere mean nothing here.
 */

#include "mdma.h"
#include <stdio.h>

static const char *tagNames[8] = {
    "refe", "cnt", "next", "ref", "refs", "call", "ret", "end"
};

/* QWC is inline data after the tag for these, and a length at ADDR for the
 * ref forms. */
static int
inlineBody(uint32_t id)
{
    switch(id){
    case 1: case 2: case 5: case 6: case 7:     /* cnt next call ret end */
        return 1;
    }
    return 0;
}

/* PACKED/REGLIST register descriptors, which are not GS register numbers */
static const char *descNames[16] = {
    "PRIM", "RGBAQ", "ST", "UV", "XYZF2", "XYZ2", "TEX0_1", "TEX0_2",
    "CLAMP_1", "CLAMP_2", "FOG", "?", "XYZF3", "XYZ3", "A+D", "NOP"
};

static const char *
gsRegName(uint32_t r)
{
    switch(r){
    case 0x00: return "PRIM";       case 0x01: return "RGBAQ";
    case 0x02: return "ST";         case 0x03: return "UV";
    case 0x04: return "XYZF2";      case 0x05: return "XYZ2";
    case 0x06: return "TEX0_1";     case 0x07: return "TEX0_2";
    case 0x08: return "CLAMP_1";    case 0x09: return "CLAMP_2";
    case 0x0a: return "FOG";
    case 0x0c: return "XYZF3";      case 0x0d: return "XYZ3";
    case 0x14: return "TEX1_1";     case 0x15: return "TEX1_2";
    case 0x16: return "TEX2_1";     case 0x17: return "TEX2_2";
    case 0x18: return "XYOFFSET_1"; case 0x19: return "XYOFFSET_2";
    case 0x1a: return "PRMODECONT"; case 0x1b: return "PRMODE";
    case 0x1c: return "TEXCLUT";    case 0x22: return "SCANMSK";
    case 0x34: return "MIPTBP1_1";  case 0x35: return "MIPTBP1_2";
    case 0x36: return "MIPTBP2_1";  case 0x37: return "MIPTBP2_2";
    case 0x3b: return "TEXA";       case 0x3d: return "FOGCOL";
    case 0x3f: return "TEXFLUSH";
    case 0x40: return "SCISSOR_1";  case 0x41: return "SCISSOR_2";
    case 0x42: return "ALPHA_1";    case 0x43: return "ALPHA_2";
    case 0x44: return "DIMX";       case 0x45: return "DTHE";
    case 0x46: return "COLCLAMP";
    case 0x47: return "TEST_1";     case 0x48: return "TEST_2";
    case 0x49: return "PABE";
    case 0x4a: return "FBA_1";      case 0x4b: return "FBA_2";
    case 0x4c: return "FRAME_1";    case 0x4d: return "FRAME_2";
    case 0x4e: return "ZBUF_1";     case 0x4f: return "ZBUF_2";
    case 0x50: return "BITBLTBUF";  case 0x51: return "TRXPOS";
    case 0x52: return "TRXREG";     case 0x53: return "TRXDIR";
    case 0x54: return "HWREG";
    case 0x60: return "SIGNAL";     case 0x61: return "FINISH";
    case 0x62: return "LABEL";
    case 0x7f: return "NOP";
    }
    return "?";
}

static const char *unpackNames[16] = {
    "S_32", "S_16", "S_8", "?",
    "V2_32", "V2_16", "V2_8", "?",
    "V3_32", "V3_16", "V3_8", "?",
    "V4_32", "V4_16", "V4_8", "V4_5"
};

static uint32_t
unpackElemBytes(uint32_t cmd)
{
    uint32_t nc, ll;

    nc = ((cmd >> 2) & 3) + 1;
    ll = cmd & 3;
    switch(ll){
    case 0: return nc*4;
    case 1: return nc*2;
    case 2: return nc;
    }
    return 2;
}

/* GIF state while walking the words of a DIRECT transfer */
typedef struct GifState GifState;
struct GifState {
    int      inTag;             /* 0 = expecting a tag, 1 = in data */
    uint32_t  nloop, nreg, flg, regnum;
    uint64_t  regs;
};

static void
gifQword(GifState *g, const uint32_t *w, FILE *out, const char *ind)
{
    if(!g->inTag){
        uint32_t nloop, eop, pre, prim, flg, nreg;

        nloop = w[0] & 0x7fff;
        eop = w[0]>>15 & 1;
        pre = w[1]>>14 & 1;
        prim = w[1]>>15 & 0x7ff;
        flg = w[1]>>26 & 3;
        nreg = w[1]>>28 & 0xf;
        g->regs = (uint64_t)w[3]<<32 | w[2];
        g->nloop = nloop;
        g->nreg = nreg ? nreg : 16;
        g->flg = flg;
        g->regnum = 0;
        fprintf(out, "%sGIFtag nloop=%u eop=%u flg=%s nreg=%u",
                ind, (unsigned)nloop, (unsigned)eop,
                flg == 0 ? "PACKED" : flg == 1 ? "REGLIST" :
                flg == 2 ? "IMAGE" : "DISABLE", (unsigned)g->nreg);
        if(pre)
            fprintf(out, " pre prim=%#x", (unsigned)prim);
        if(flg == 0 || flg == 1){
            uint32_t i;
            fprintf(out, " regs=");
            for(i = 0; i < g->nreg; i++)
                fprintf(out, "%s%s", i ? "," : "",
                        descNames[g->regs >> i*4 & 0xf]);
        }
        fprintf(out, "\n");
        if(nloop)
            g->inTag = 1;
        return;
    }
    /* data */
    if(g->flg == 0){            /* PACKED, one descriptor per qword */
        uint32_t desc = (uint32_t)(g->regs >> g->regnum*4 & 0xf);
        if(desc == 0x0e)        /* A+D: the register is in the data */
            fprintf(out, "%s  A+D %-11s %08x_%08x\n", ind,
                    gsRegName(w[2] & 0xff), (unsigned)w[1], (unsigned)w[0]);
        else
            fprintf(out, "%s  %-15s %08x_%08x %08x_%08x\n", ind,
                    descNames[desc], (unsigned)w[3], (unsigned)w[2],
                    (unsigned)w[1], (unsigned)w[0]);
        g->regnum++;
        if(g->regnum >= g->nreg){
            g->regnum = 0;
            if(--g->nloop == 0)
                g->inTag = 0;
        }
    }else{
        fprintf(out, "%s  data %08x %08x %08x %08x\n", ind,
                (unsigned)w[0], (unsigned)w[1], (unsigned)w[2],
                (unsigned)w[3]);
        if(g->flg == 2){        /* IMAGE: nloop counts qwords */
            if(--g->nloop == 0)
                g->inTag = 0;
        }else{                  /* REGLIST: two registers per qword */
            g->regnum += 2;
            while(g->regnum >= g->nreg && g->nloop){
                g->regnum -= g->nreg;
                if(--g->nloop == 0)
                    g->inTag = 0;
            }
        }
    }
}

/* Decode one VIF code, print it, and return how many 32-bit data words follow
 * it in the stream. */
static uint32_t
vifCode(uint32_t c, FILE *out, const char *ind, int *isDirect)
{
    uint32_t cmd, num, imm;

    *isDirect = 0;
    cmd = c>>24 & 0x7f;
    num = c>>16 & 0xff;
    imm = c & 0xffff;

    if(cmd == 0x00){
        /* NOP is also what padding looks like; not worth a line each */
        return 0;
    }
    if((cmd & 0x60) == 0x60){   /* UNPACK */
        uint32_t n, bytes;
        n = num ? num : 256;
        bytes = n * unpackElemBytes(cmd & 0x0f);
        fprintf(out, "%sUNPACK %-6s num=%-4u addr=%u%s%s%s\n", ind,
                unpackNames[cmd & 0x0f], (unsigned)n, (unsigned)(imm & 0x3ff),
                imm & 0x8000 ? " dblbuf" : "",
                imm & 0x4000 ? " usn" : "",
                c & 0x10000000 ? " mask" : "");
        return (bytes + 3) / 4;
    }
    switch(cmd){
    case 0x01:
        fprintf(out, "%sSTCYCL cl=%u wl=%u\n", ind,
                (unsigned)(imm & 0xff), (unsigned)(imm>>8 & 0xff));
        return 0;
    case 0x02: fprintf(out, "%sOFFSET %u\n", ind, (unsigned)(imm & 0x3ff)); return 0;
    case 0x03: fprintf(out, "%sBASE %u\n", ind, (unsigned)(imm & 0x3ff)); return 0;
    case 0x04: fprintf(out, "%sITOP %u\n", ind, (unsigned)(imm & 0x3ff)); return 0;
    case 0x05: fprintf(out, "%sSTMOD %u\n", ind, (unsigned)(imm & 3)); return 0;
    case 0x06: fprintf(out, "%sMSKPATH3 %u\n", ind, (unsigned)(imm>>15 & 1)); return 0;
    case 0x07: fprintf(out, "%sMARK %u\n", ind, (unsigned)imm); return 0;
    case 0x10: fprintf(out, "%sFLUSHE\n", ind); return 0;
    case 0x11: fprintf(out, "%sFLUSH\n", ind); return 0;
    case 0x13: fprintf(out, "%sFLUSHA\n", ind); return 0;
    case 0x14: fprintf(out, "%sMSCAL %u\n", ind, (unsigned)imm); return 0;
    case 0x15: fprintf(out, "%sMSCALF %u\n", ind, (unsigned)imm); return 0;
    case 0x17: fprintf(out, "%sMSCNT\n", ind); return 0;
    case 0x20: fprintf(out, "%sSTMASK\n", ind); return 1;
    case 0x30: fprintf(out, "%sSTROW\n", ind); return 4;
    case 0x31: fprintf(out, "%sSTCOL\n", ind); return 4;
    case 0x4a:
        fprintf(out, "%sMPG num=%u addr=%u\n", ind,
                (unsigned)(num ? num : 256), (unsigned)imm);
        return (num ? num : 256) * 2;
    case 0x50:
    case 0x51:
        fprintf(out, "%s%s qwc=%u\n", ind, cmd == 0x50 ? "DIRECT" : "DIRECTHL",
                (unsigned)(imm ? imm : 65536));
        *isDirect = 1;
        return (imm ? imm : 65536) * 4;
    }
    fprintf(out, "%sVIF ?? %08x\n", ind, (unsigned)c);
    return 0;
}

/* The tag's two spare words and its body are one VIF word stream, but they are
 * not contiguous in memory. This is that stream, by index. */
static uint32_t
streamAt(const uint32_t *tag, const uint32_t *body, uint32_t i)
{
    return i < 2 ? tag[2+i] : body[i-2];
}

void
mdmaDisasm(const uint128_t *chain, uint32_t qwc, FILE *out)
{
    const uint32_t *base;
    uint32_t pos;

    base = (const uint32_t*)chain;
    pos = 0;
    while(pos < qwc){
        const uint32_t *t;
        uint32_t id, tqwc, addr, body, i, nw, dataw;
        int inDirect;
        GifState gif;

        t = base + pos*4;
        id = t[0]>>28 & 7;
        tqwc = t[0] & 0xffff;
        addr = t[1];
        fprintf(out, "%4u: %-4s qwc=%-5u", (unsigned)pos, tagNames[id],
                (unsigned)tqwc);
        if(id == 0 || id == 2 || id == 3 || id == 4 || id == 5)
            fprintf(out, " addr=%08x", (unsigned)addr);
        if(t[0] & IntFlg)
            fprintf(out, " irq");
        fprintf(out, "\n");
        pos++;

        /* Walk the tag's spare words and the body as VIF sees them: one
         * stream of 32-bit words, where anything not accounted for by a
         * pending data count is another VIF code. That is what makes an
         * inline ITOP/MSCAL legible — and it is also the only way to see a
         * DIRECT in the tag's first word swallowing its second. */
        dataw = 0;
        inDirect = 0;
        gif.inTag = 0;
        body = inlineBody(id) ? tqwc : 0;
        if(pos + body > qwc)
            body = qwc - pos;
        nw = 2 + body*4;

        i = 0;
        while(i < nw){
            const char *ind = i < 2 ? "        " : "      - ";
            uint32_t d[4], j, n;

            for(j = 0; j < 4; j++)
                d[j] = i+j < nw ? streamAt(t, base + pos*4, i+j) : 0;

            if(dataw == 0){
                int isdir;
                dataw += vifCode(d[0], out, ind, &isdir);
                if(isdir){
                    inDirect = 1;
                    gif.inTag = 0;
                }
                i++;
            }else if(inDirect){
                gifQword(&gif, d, out, "        ");
                i += 4;
                dataw -= dataw < 4 ? dataw : 4;
            }else{
                n = dataw < 4 ? dataw : 4;
                fprintf(out, "        data");
                for(j = 0; j < n; j++)
                    fprintf(out, " %08x", (unsigned)d[j]);
                fprintf(out, "\n");
                i += n;
                dataw -= n;
            }
        }

        if(!inlineBody(id)){
            if(tqwc)
                fprintf(out, "        (%u qwords at %08x)\n",
                        (unsigned)tqwc, (unsigned)addr);
            if(id == 0)
                break;                  /* refe ends the chain */
            continue;
        }
        pos += body;
        if(id == 7)
            break;                      /* end */
    }
}

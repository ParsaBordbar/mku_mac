/*
 * sceMpeg: the PSP's movie player library (PMF files = MPEG-PS with H.264
 * video and ATRAC3+ audio).
 *
 * For now movies are SKIPPED, but through the real API flow, so the game's
 * own player logic runs normally:
 *   - the game's ring buffer callback really reads the movie file
 *     (sceMpegRingbufferPut calls it, like the hardware does)
 *   - every "access unit" request swallows whatever data is buffered,
 *     "decoding" a black frame / silent audio
 *   - once the file is exhausted, GetAvcAu/GetAtracAu return NO_DATA and the
 *     game ends the movie itself.
 * Phase 9 will decode real frames (FFmpeg). Structures follow PPSSPP's
 * Core/HLE/sceMpeg.cpp.
 */
#include "kernel.h"

#include <string.h>

#define ERR_MPEG_NO_DATA 0x80618001u
#define ERR_MPEG_INVALID_VALUE 0x806101FEu
#define PSMF_MAGIC 0x464D5350u   /* "PSMF" */

/* Log each call with its arguments and result (MKU_TRACE=1). */
#undef RET
#define RET(v) do { uint32_t r_ = (uint32_t)(v); TRACE("(0x%X, 0x%X, 0x%X, 0x%X) -> 0x%X", A0, A1, A2, A3, r_); c->r[2] = r_; return; } while (0)

/* SceMpegRingBuffer, in guest memory */
enum { RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_PACKETSIZE = 16, RB_DATA = 20,
       RB_CALLBACK = 24, RB_CBARGS = 28, RB_UPPER = 32, RB_SEMA = 36, RB_MPEG = 40 };

static uint32_t g_stream_ids = 0x100;
static int g_movies;

static uint32_t be32(uint32_t addr) { return __builtin_bswap32(MEM_LW(addr)); }

/* Call a guest function from inside an HLE call, on the caller's thread, with
 * a copy of its registers just below its stack. */
static uint32_t call_guest(Ctx *c, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2) {
    Ctx cc = *c;
    cc.r[29] = (c->r[29] - 256) & ~15u;
    cc.r[4] = a0; cc.r[5] = a1; cc.r[6] = a2;
    cc.r[31] = 0;
    rt_call_indirect(&cc, func);
    return cc.r[2];
}

void hle_sceMpegInit(Ctx *c) { RET(0); }
void hle_sceMpegFinish(Ctx *c) { RET(0); }
void hle_sceMpegQueryMemSize(Ctx *c) { RET(0x10000); }
void hle_sceMpegRingbufferQueryMemSize(Ctx *c) { RET(A0 * (104 + 2048)); }

void hle_sceMpegRingbufferConstruct(Ctx *c) {
    uint32_t rb = A0, packets = A1, data = A2;
    MEM_SW(rb + RB_PACKETS, packets);
    MEM_SW(rb + RB_READ, 0);
    MEM_SW(rb + RB_WRITEPOS, 0);
    MEM_SW(rb + RB_AVAIL, 0);
    MEM_SW(rb + RB_PACKETSIZE, 2048);
    MEM_SW(rb + RB_DATA, data);
    MEM_SW(rb + RB_CALLBACK, T0);   /* args: rb, packets, data, size, callback, callback arg */
    MEM_SW(rb + RB_CBARGS, T1);
    MEM_SW(rb + RB_UPPER, data + packets * 2048);
    MEM_SW(rb + RB_MPEG, 0);
    RET(0);
}
void hle_sceMpegRingbufferDestruct(Ctx *c) { RET(0); }

void hle_sceMpegCreate(Ctx *c) {
    uint32_t mpeg = A0, data = A1, rb = A3;
    uint32_t handle = data + 0x30;
    MEM_SW(mpeg, handle);
    memcpy(rt_ptr(handle), "LIBMPEG\0001\0", 12);
    MEM_SW(handle + 12, 0xFFFFFFFFu);
    if (rb) {
        MEM_SW(handle + 16, rb);
        MEM_SW(handle + 20, MEM_LW(rb + RB_UPPER));
        MEM_SW(rb + RB_MPEG, mpeg);
        MEM_SW(rb + RB_AVAIL, 0);
    }
    g_movies++;
    TRACE("(#%d) -- movie will be skipped (no video decoding yet)", g_movies);
    RET(0);
}
void hle_sceMpegDelete(Ctx *c) { RET(0); }

void hle_sceMpegQueryStreamOffset(Ctx *c) {
    uint32_t buf = A1, out = A2;
    if (MEM_LW(buf) != PSMF_MAGIC) { MEM_SW(out, 0); RET(ERR_MPEG_INVALID_VALUE); }
    MEM_SW(out, be32(buf + 8));
    RET(0);
}

void hle_sceMpegQueryStreamSize(Ctx *c) {
    uint32_t buf = A0, out = A1;
    if (MEM_LW(buf) != PSMF_MAGIC) { MEM_SW(out, 0); RET(ERR_MPEG_INVALID_VALUE); }
    MEM_SW(out, be32(buf + 12));
    RET(0);
}

void hle_sceMpegRegistStream(Ctx *c) { RET(g_stream_ids++); }
void hle_sceMpegUnRegistStream(Ctx *c) { RET(0); }
void hle_sceMpegMallocAvcEsBuf(Ctx *c) { RET(1); }
void hle_sceMpegFreeAvcEsBuf(Ctx *c) { RET(0); }
void hle_sceMpegFlushAllStream(Ctx *c) { RET(0); }

/* SceMpegAu: pts (8), dts (8), esBuffer, esSize */
void hle_sceMpegInitAu(Ctx *c) {
    uint32_t au = A2;
    memset(rt_ptr(au), 0, 16);
    MEM_SW(au + 16, A1);
    MEM_SW(au + 20, A1 >= 1 && A1 <= 2 ? 2048 : 2112);
    RET(0);
}

void hle_sceMpegQueryAtracEsSize(Ctx *c) {
    if (A1) MEM_SW(A1, 2112);
    if (A2) MEM_SW(A2, 8192);
    RET(0);
}

void hle_sceMpegRingbufferAvailableSize(Ctx *c) {
    uint32_t rb = A0;
    RET(MEM_LW(rb + RB_PACKETS) - MEM_LW(rb + RB_AVAIL));
}

/* Ask the game's callback to read movie data into the ring buffer. */
void hle_sceMpegRingbufferPut(Ctx *c) {
    uint32_t rb = A0;
    int32_t want = (int32_t)A1, avail = (int32_t)A2;
    int32_t packets = (int32_t)MEM_LW(rb + RB_PACKETS);
    int32_t free_packets = packets - (int32_t)MEM_LW(rb + RB_AVAIL);
    if (want > avail) want = avail;
    if (want > free_packets) want = free_packets;
    uint32_t cb = MEM_LW(rb + RB_CALLBACK);
    if (want <= 0 || !cb) RET(0);
    int32_t total = 0;
    while (want > 0) {
        int32_t pos = (int32_t)MEM_LW(rb + RB_WRITEPOS) % packets;
        int32_t chunk = want < packets - pos ? want : packets - pos;
        int32_t got = (int32_t)call_guest(c, cb, MEM_LW(rb + RB_DATA) + (uint32_t)pos * 2048, (uint32_t)chunk, MEM_LW(rb + RB_CBARGS));
        if (got <= 0) {
            if (!total) RET(got);
            break;
        }
        MEM_SW(rb + RB_READ, MEM_LW(rb + RB_READ) + (uint32_t)got);
        MEM_SW(rb + RB_WRITEPOS, MEM_LW(rb + RB_WRITEPOS) + (uint32_t)got);
        MEM_SW(rb + RB_AVAIL, MEM_LW(rb + RB_AVAIL) + (uint32_t)got);
        total += got;
        want -= got;
        if (got < chunk) break;
    }
    RET(total);
}

static uint32_t ring_of(uint32_t mpeg) { return MEM_LW(MEM_LW(mpeg) + 16); }

/* "Decode" the next unit: swallow everything buffered. With nothing left the
 * stream has ended and the game stops the movie. */
static uint32_t take_unit(uint32_t mpeg, uint32_t au) {
    uint32_t rb = ring_of(mpeg);
    if (!rb || MEM_LW(rb + RB_READ) == 0 || MEM_LW(rb + RB_AVAIL) == 0) return ERR_MPEG_NO_DATA;
    MEM_SW(rb + RB_AVAIL, 0);
    if (au) {   /* advance the timestamps by one frame (90 kHz clock, ~29.97 fps) */
        uint64_t pts = ((uint64_t)MEM_LW(au) << 32 | MEM_LW(au + 4)) + 3003;
        MEM_SW(au, (uint32_t)(pts >> 32)); MEM_SW(au + 4, (uint32_t)pts);
        MEM_SW(au + 8, (uint32_t)(pts >> 32)); MEM_SW(au + 12, (uint32_t)pts);
    }
    return 0;
}

void hle_sceMpegGetAvcAu(Ctx *c) {
    uint32_t r = take_unit(A0, A2);
    if (!r && A3) MEM_SW(A3, 1);
    RET(r);
}

void hle_sceMpegGetAtracAu(Ctx *c) {
    uint32_t r = take_unit(A0, A2);
    if (!r && A3) MEM_SW(A3, 1);
    RET(r);
}

/* A black frame: args mpeg, au, frame width, pointer to buffer address, pointer to "frame ready". */
void hle_sceMpegAvcDecode(Ctx *c) {
    uint32_t width = A2 ? A2 : 512, buf = A3 ? MEM_LW(A3) : 0;
    if (buf) memset(rt_ptr(buf), 0, (size_t)width * 272 * 4);
    if (T0) MEM_SW(T0, 1);
    RET(0);
}

void hle_sceMpegAvcDecodeStop(Ctx *c) {
    if (A3) MEM_SW(A3, 0);
    RET(0);
}

/* Silent audio: args mpeg, au, output buffer, init. */
void hle_sceMpegAtracDecode(Ctx *c) {
    if (A2) memset(rt_ptr(A2), 0, 8192);
    RET(0);
}

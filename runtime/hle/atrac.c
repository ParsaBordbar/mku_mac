/*
 * sceAtrac3plus: Sony's ATRAC music decoder library. MK's music (.MUS files)
 * is ATRAC3 in a RIFF/WAVE container (format tag 0x270, 1024 samples per frame).
 *
 * Streaming API, as the game uses it:
 *   SetDataAndGetID(buf, size)       the first part of the file is in buf
 *   GetStreamDataInfo -> where/how much/from which file offset to read next
 *   AddStreamData(n)                 the game read n more bytes there
 *   DecodeData(out, &n, &end, &remain)   one frame of 16-bit stereo PCM
 * We copy every byte the game streams into our own buffer, so the game may
 * always write at the start of its buffer, and decode with FFmpeg's ATRAC3
 * decoder. Loops come from the WAV "smpl" chunk. Error codes follow PPSSPP.
 */
#include "kernel.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <string.h>

#define ERR_BAD_ID         0x80630005u
#define ERR_NO_ID          0x80630003u
#define ERR_UNKNOWN_FORMAT 0x80630006u
#define ERR_BUFFER_EMPTY   0x80630023u
#define ERR_ALL_DECODED    0x80630024u
#define ERR_NO_SECOND_BUF  0x80630022u
#define ERR_NO_LOOP        0x80630021u
#define ALL_DATA_ON_MEMORY 0xFFFFFFFFu   /* "remain frames" when the whole file is loaded */

#define SAMPLES_PER_FRAME 1024
#define MAX_IDS 6

typedef struct {
    int used;
    uint8_t *file;               /* everything received so far, at its file offset */
    uint32_t file_size, received;
    uint32_t data_off, data_size, block_align;
    int channels;
    uint32_t guest_buf, guest_buf_size;
    int end_sample, loop_start, loop_end, loop_num;
    int next_frame, total_frames, finished;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    uint8_t packet_buf[4096 + AV_INPUT_BUFFER_PADDING_SIZE];
} Atrac;

static Atrac g_at[MAX_IDS];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static Atrac *get(uint32_t id) { return id < MAX_IDS && g_at[id].used ? &g_at[id] : NULL; }

static void release(Atrac *a) {
    if (a->dec) avcodec_free_context(&a->dec);
    if (a->pkt) av_packet_free(&a->pkt);
    if (a->frame) av_frame_free(&a->frame);
    free(a->file);
    memset(a, 0, sizeof *a);
}

/* Parse RIFF/WAVE: fmt (codec params), fact (length), smpl (loop), data. */
static int parse_header(Atrac *a, const uint8_t *p, uint32_t avail) {
    if (avail < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return 0;
    a->file_size = le32(p + 4) + 8;
    const uint8_t *extra = NULL;
    int extra_size = 0, fmt_tag = 0;
    a->end_sample = -1;
    a->loop_start = a->loop_end = -1;
    for (uint32_t off = 12; off + 8 <= avail;) {
        const uint8_t *ck = p + off;
        uint32_t size = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            fmt_tag = le16(ck + 8);
            a->channels = le16(ck + 10);
            a->block_align = le16(ck + 20);
            if (size > 18) { extra_size = le16(ck + 24); extra = ck + 26; }
        } else if (!memcmp(ck, "fact", 4)) {
            a->end_sample = (int)le32(ck + 8);
        } else if (!memcmp(ck, "smpl", 4) && size >= 60 && le32(ck + 8 + 28) > 0) {
            /* smpl: 9 header words (loop count at +28), then loops of
             * {cue id, type, start, end, fraction, play count} */
            a->loop_start = (int)le32(ck + 8 + 36 + 8);
            a->loop_end = (int)le32(ck + 8 + 36 + 12);
        } else if (!memcmp(ck, "data", 4)) {
            a->data_off = off + 8;
            a->data_size = size;
            break;
        }
        off += 8 + size + (size & 1);
    }
    if (fmt_tag != 0x270 || !a->data_off || !a->block_align || a->block_align > 4096) return 0;
    a->total_frames = (int)(a->data_size / a->block_align);
    if (a->end_sample < 0) a->end_sample = a->total_frames * SAMPLES_PER_FRAME - 1;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3);
    if (!codec) return 0;
    a->dec = avcodec_alloc_context3(codec);
    a->dec->sample_rate = 44100;
    av_channel_layout_default(&a->dec->ch_layout, a->channels);
    a->dec->block_align = (int)a->block_align;
    if (extra && extra_size > 0) {
        a->dec->extradata = av_mallocz((size_t)extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(a->dec->extradata, extra, (size_t)extra_size);
        a->dec->extradata_size = extra_size;
    }
    if (avcodec_open2(a->dec, codec, NULL) < 0) return 0;
    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    return 1;
}

void hle_sceAtracSetDataAndGetID(Ctx *c) {
    uint32_t buf = A0, size = A1;
    int id;
    for (id = 0; id < MAX_IDS && g_at[id].used; id++) {}
    if (id == MAX_IDS) RET(ERR_NO_ID);
    Atrac *a = &g_at[id];
    memset(a, 0, sizeof *a);
    if (!parse_header(a, rt_ptr(buf), size)) {
        LOG("(0x%08X, %u): not an ATRAC3 WAV this decoder handles", buf, size);
        release(a);
        RET(ERR_UNKNOWN_FORMAT);
    }
    a->used = 1;
    a->file = calloc(1, a->file_size);
    a->received = size < a->file_size ? size : a->file_size;
    memcpy(a->file, rt_ptr(buf), a->received);
    a->guest_buf = buf;
    a->guest_buf_size = size;
    TRACE("(0x%08X, %u) -> id %d: %u bytes, %d frames, loop %d-%d", buf, size, id, a->file_size,
          a->total_frames, a->loop_start, a->loop_end);
    RET(id);
}

void hle_sceAtracReleaseAtracID(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    release(a);
    RET(0);
}

/* Where the game should put the next chunk of the file. */
void hle_sceAtracGetStreamDataInfo(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    uint32_t left = a->file_size - a->received;
    if (A1) MEM_SW(A1, a->guest_buf);
    if (A2) MEM_SW(A2, left < a->guest_buf_size ? left : a->guest_buf_size);
    if (A3) MEM_SW(A3, a->received);
    RET(0);
}

void hle_sceAtracAddStreamData(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    uint32_t n = A1;
    if (n > a->file_size - a->received) n = a->file_size - a->received;
    memcpy(a->file + a->received, rt_ptr(a->guest_buf), n);
    a->received += n;
    RET(0);
}

static uint32_t remain_frames(const Atrac *a) {
    if (a->received >= a->file_size) return ALL_DATA_ON_MEMORY;
    int have = (int)((a->received - a->data_off) / a->block_align) - a->next_frame;
    return (uint32_t)(have < 0 ? 0 : have);
}

/* Decode the next frame into out (16-bit, interleaved). */
static int decode_frame(Atrac *a, int16_t *out) {
    uint32_t off = a->data_off + (uint32_t)a->next_frame * a->block_align;
    memcpy(a->packet_buf, a->file + off, a->block_align);
    memset(a->packet_buf + a->block_align, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    a->pkt->data = a->packet_buf;
    a->pkt->size = (int)a->block_align;
    int got = 0;
    if (avcodec_send_packet(a->dec, a->pkt) == 0 && avcodec_receive_frame(a->dec, a->frame) == 0) {
        got = a->frame->nb_samples;
        int ch = a->frame->ch_layout.nb_channels;
        for (int i = 0; i < got; i++)
            for (int k = 0; k < a->channels; k++) {
                float s = ((const float *)a->frame->extended_data[k < ch ? k : 0])[i];
                int v = (int)(s * 32767.f);
                out[i * a->channels + k] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            }
    }
    a->next_frame++;
    return got;
}

/* args: id, output buffer, &samples, &finished, &remain frames */
void hle_sceAtracDecodeData(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    uint32_t out = A1, samples_p = A2, finish_p = A3, remain_p = T0;
    if (a->finished) {
        if (samples_p) MEM_SW(samples_p, 0);
        if (finish_p) MEM_SW(finish_p, 1);
        RET(ERR_ALL_DECODED);
    }
    if (a->data_off + (uint32_t)(a->next_frame + 1) * a->block_align > a->received) {
        if (samples_p) MEM_SW(samples_p, 0);
        RET(ERR_BUFFER_EMPTY);   /* the game has to stream more first */
    }
    static int16_t pcm[4096 * 2];
    int n = decode_frame(a, pcm);
    if (out) memcpy(rt_ptr(out), pcm, (size_t)n * (size_t)a->channels * 2);

    int at_end = a->next_frame >= a->total_frames || a->next_frame * SAMPLES_PER_FRAME > a->end_sample;
    int at_loop_end = a->loop_end >= 0 && a->next_frame * SAMPLES_PER_FRAME > a->loop_end;
    if ((at_loop_end || at_end) && a->loop_num != 0 && a->loop_start >= 0) {
        a->next_frame = a->loop_start / SAMPLES_PER_FRAME;   /* jump back to the loop start */
        if (a->loop_num > 0) a->loop_num--;
        avcodec_flush_buffers(a->dec);
    } else if (at_end) {
        a->finished = 1;
    }
    if (samples_p) MEM_SW(samples_p, (uint32_t)n);
    if (finish_p) MEM_SW(finish_p, (uint32_t)a->finished);
    if (remain_p) MEM_SW(remain_p, remain_frames(a));
    RET(0);
}

void hle_sceAtracGetRemainFrame(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    if (A1) MEM_SW(A1, remain_frames(a));
    RET(0);
}

void hle_sceAtracGetNextSample(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    if (A1) MEM_SW(A1, a->finished ? 0 : SAMPLES_PER_FRAME);
    RET(0);
}

void hle_sceAtracSetLoopNum(Ctx *c) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    if (a->loop_start < 0) RET(ERR_NO_LOOP);
    a->loop_num = (int)A1;   /* -1 = forever */
    RET(0);
}

void hle_sceAtracGetSoundSample(Ctx *c) {   /* id, &end sample, &loop start, &loop end */
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    if (A1) MEM_SW(A1, (uint32_t)a->end_sample);
    if (A2) MEM_SW(A2, (uint32_t)a->loop_start);
    if (A3) MEM_SW(A3, (uint32_t)a->loop_end);
    RET(0);
}

void hle_sceAtracGetSecondBufferInfo(Ctx *c) {
    if (A1) MEM_SW(A1, 0);
    if (A2) MEM_SW(A2, 0);
    RET(get(A0) ? ERR_NO_SECOND_BUF : ERR_BAD_ID);
}

void hle_sceAtracGetInternalErrorInfo(Ctx *c) {
    if (A1) MEM_SW(A1, 0);
    RET(get(A0) ? 0 : ERR_BAD_ID);
}

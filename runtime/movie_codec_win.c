// movie_codec.h on Windows: Media Foundation's H.264 decoder (CLSID_CMSH264DecoderMFT, NV12 out) and AAC decoder
// (CLSID_CMSAACDecMFT, 16-bit PCM out), both part of Windows, driven synchronously as IMFTransforms.
//
// - H.264 input is Annex B, which the transform wants: a sample starting with a 4-byte big-endian length (the MP4
//   layout) is rewritten with start codes. The SPS and PPS from csd-0/csd-1 go in front of the first sample after
//   open and after every flush.
// - The decoder may hold a few frames (reordering): mc_feed returns 0 while it has output to give, and output times
//   are the decoder's, not the input's.
// - NV12 comes out with the decoder's stride and its coded height (1088 for 1080p); mc_pull packs it to width x height.
// - AAC input is raw access units with the AudioSpecificConfig in the media type (MF_MT_USER_DATA after the 12-byte
//   HEAACWAVEINFO tail).
// COM is initialised multithreaded on each calling thread (the game's thread for open, the movie worker for decoding).
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "movie_codec.h"

struct MovieCodec {
    int nfeed, npull;       // first calls logged (TOTK_MOVIE_LOG-free diagnostics: 6 each)
    int kind;
    IMFTransform *t;
    int w, h;               // H.264: display size
    int aw, ah, stride;     // H.264: the output type's coded size and stride
    int provides;           // the transform allocates its output samples
    DWORD out_size;         // otherwise the size of the sample to hand it
    uint8_t *csd;           // H.264: SPS and PPS with start codes, sent before the next sample
    int ncsd, csd_due;
    uint8_t *conv;          // H.264: length-prefixed sample rewritten with start codes
    int nconv;
    char err[160];
};

static volatile LONG g_started;
static void com_init(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;   // S_FALSE: already initialised on this thread; RPC_E_CHANGED_MODE: an STA thread still works for MFTs
    if (!InterlockedCompareExchange(&g_started, 1, 0)) MFStartup(MF_VERSION, MFSTARTUP_LITE);
}

static int fail(MovieCodec *c, const char *what, HRESULT hr) {
    snprintf(c->err, sizeof c->err, "%s failed (hr %#lx)", what, (unsigned long)hr);
    return -1;
}

const char *mc_backend(void) { return "Media Foundation"; }
const char *mc_error(const MovieCodec *c) { return c ? c->err : "no codec"; }

// ---- H.264 helpers
static int start_code(const uint8_t *p, int n) {
    return (n >= 4 && !p[0] && !p[1] && !p[2] && p[3] == 1) || (n >= 3 && !p[0] && !p[1] && p[2] == 1);
}
// csd-N: Annex B already, or a bare NAL unit (stagefright passes SPS/PPS with start codes; avcC-style without).
static void csd_add(MovieCodec *c, const uint8_t *p, int n) {
    if (!p || n <= 0) return;
    int add = start_code(p, n) ? n : n + 4;
    uint8_t *q = realloc(c->csd, (size_t)(c->ncsd + add));
    if (!q) return;
    c->csd = q;
    if (!start_code(p, n)) {
        static const uint8_t sc[4] = {0, 0, 0, 1};
        memcpy(c->csd + c->ncsd, sc, 4);
        memcpy(c->csd + c->ncsd + 4, p, (size_t)n);
    } else {
        memcpy(c->csd + c->ncsd, p, (size_t)n);
    }
    c->ncsd += add;
}
// A sample of 4-byte big-endian NAL lengths, rewritten in place of the lengths with start codes. 0 if it is not one.
static int to_annexb(MovieCodec *c, const uint8_t *p, int n) {
    for (int off = 0; off < n;) {
        if (off + 4 > n) return 0;
        uint32_t len = (uint32_t)p[off] << 24 | (uint32_t)p[off + 1] << 16 | (uint32_t)p[off + 2] << 8 | p[off + 3];
        if (!len || len > (uint32_t)(n - off - 4)) return 0;
        off += 4 + (int)len;
    }
    if (c->nconv < n) {
        uint8_t *q = realloc(c->conv, (size_t)n);
        if (!q) return 0;
        c->conv = q, c->nconv = n;
    }
    memcpy(c->conv, p, (size_t)n);
    for (int off = 0; off < n;) {
        uint32_t len = (uint32_t)p[off] << 24 | (uint32_t)p[off + 1] << 16 | (uint32_t)p[off + 2] << 8 | p[off + 3];
        c->conv[off] = c->conv[off + 1] = c->conv[off + 2] = 0, c->conv[off + 3] = 1;
        off += 4 + (int)len;
    }
    return 1;
}

static HRESULT pick_output(MovieCodec *c) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        HRESULT hr = IMFTransform_GetOutputAvailableType(c->t, 0, i, &t);
        if (FAILED(hr)) return hr;
        GUID sub;
        UINT32 bits = 0;
        int want = 0;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub))) {
            if (c->kind == MC_H264) want = IsEqualGUID(&sub, &MFVideoFormat_NV12);
            else want = IsEqualGUID(&sub, &MFAudioFormat_PCM) &&
                        SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits == 16;
        }
        if (want) {
            hr = IMFTransform_SetOutputType(c->t, 0, t, 0);
            if (SUCCEEDED(hr) && c->kind == MC_H264) {
                UINT64 fs = 0;
                UINT32 stride = 0;
                IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &fs);
                c->aw = (int)(fs >> 32), c->ah = (int)(uint32_t)fs;
                if (FAILED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &stride)) || (int)stride < c->aw) stride = (UINT32)c->aw;
                c->stride = (int)stride;
            }
            IMFMediaType_Release(t);
            if (FAILED(hr)) return hr;
            MFT_OUTPUT_STREAM_INFO si = {0};
            hr = IMFTransform_GetOutputStreamInfo(c->t, 0, &si);
            if (FAILED(hr)) return hr;
            c->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            c->out_size = si.cbSize ? si.cbSize : 1 << 20;
            return S_OK;
        }
        IMFMediaType_Release(t);
    }
}

MovieCodec *mc_open(int kind, int a, int b, const uint8_t *csd0, int n0, const uint8_t *csd1, int n1, char *err,
                    int errn) {
    com_init();
    MovieCodec *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, (size_t)errn, "out of memory"); return NULL; }
    c->kind = kind;
    IMFMediaType *in = NULL;
    HRESULT hr = CoCreateInstance(kind == MC_H264 ? &CLSID_CMSH264DecoderMFT : &CLSID_CMSAACDecMFT, NULL,
                                  CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&c->t);
    if (FAILED(hr)) { fail(c, kind == MC_H264 ? "creating the H.264 decoder" : "creating the AAC decoder", hr); goto bad; }
    if (FAILED(hr = MFCreateMediaType(&in))) { fail(c, "MFCreateMediaType", hr); goto bad; }
    if (kind == MC_H264) {
        c->w = a, c->h = b;
        IMFAttributes *attr = NULL;
        if (SUCCEEDED(IMFTransform_GetAttributes(c->t, &attr)) && attr) {
            IMFAttributes_SetUINT32(attr, &MF_LOW_LATENCY, TRUE);   // a frame out per frame in where the stream allows
            IMFAttributes_Release(attr);
        }
        IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
        IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, (UINT64)(uint32_t)a << 32 | (uint32_t)b);
        IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        csd_add(c, csd0, n0);
        csd_add(c, csd1, n1);
        c->csd_due = c->ncsd > 0;
    } else {
        // AudioSpecificConfig: the game's, else AAC-LC at this rate and channel count
        static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
        uint8_t asc[2];
        const uint8_t *cfg = csd0;
        int ncfg = n0;
        if (!cfg || ncfg < 2 || ncfg > 64) {
            int sri = 3;
            for (int i = 0; i < 12; i++)
                if (rates[i] == a) sri = i;
            uint16_t v = (uint16_t)(2 << 11 | sri << 7 | (b & 15) << 3);
            asc[0] = (uint8_t)(v >> 8), asc[1] = (uint8_t)v;
            cfg = asc, ncfg = 2;
        }
        uint8_t ud[12 + 64] = {0};   // HEAACWAVEINFO after WAVEFORMATEX: payload type 0 (raw), profile 0xFE (unknown)
        ud[2] = 0xFE;
        memcpy(ud + 12, cfg, (size_t)ncfg);
        IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)a);
        IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)b);
        IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(in, &MF_MT_AAC_PAYLOAD_TYPE, 0);
        IMFMediaType_SetBlob(in, &MF_MT_USER_DATA, ud, (UINT32)(12 + ncfg));
    }
    if (FAILED(hr = IMFTransform_SetInputType(c->t, 0, in, 0))) { fail(c, "SetInputType", hr); goto bad; }
    IMFMediaType_Release(in);
    in = NULL;
    if (FAILED(hr = pick_output(c))) { fail(c, "SetOutputType", hr); goto bad; }
    IMFTransform_ProcessMessage(c->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(c->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return c;
bad:
    snprintf(err, (size_t)errn, "%s", c->err);
    if (in) IMFMediaType_Release(in);
    mc_close(c);
    return NULL;
}

static IMFSample *make_sample(const uint8_t *a, int na, const uint8_t *b, int nb, int64_t us) {
    IMFMediaBuffer *buf = NULL;
    IMFSample *s = NULL;
    BYTE *p;
    if (FAILED(MFCreateMemoryBuffer((DWORD)(na + nb), &buf))) return NULL;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
        if (na) memcpy(p, a, (size_t)na);
        if (nb) memcpy(p + na, b, (size_t)nb);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, (DWORD)(na + nb));
    }
    if (SUCCEEDED(MFCreateSample(&s))) {
        IMFSample_AddBuffer(s, buf);
        IMFSample_SetSampleTime(s, us * 10);
    }
    IMFMediaBuffer_Release(buf);
    return s;
}

// The NAL unit types of an Annex B access unit with their sizes ("9:2 7:25 8:5 5:98000"), for the log.
static void nal_list(const uint8_t *p, int n, char *out, int cap) {
    int k = 0, start = -1, type = -1;
    out[0] = 0;
    for (int i = 0; i + 3 <= n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
        if (start >= 0 && k < cap - 16) k += snprintf(out + k, (size_t)(cap - k), "%d:%d ", type, i - start);
        start = i + 3;
        type = i + 3 < n ? p[i + 3] & 31 : -1;
        i += 2;
    }
    if (start >= 0 && k < cap - 16) snprintf(out + k, (size_t)(cap - k), "%d:%d", type, n - start);
}

int mc_feed(MovieCodec *c, const uint8_t *p, int n, int64_t us) {
    com_init();
    if (c->kind == MC_H264 && !start_code(p, n) && to_annexb(c, p, n)) p = c->conv;
    if (c->kind == MC_H264 && c->nfeed < 6) {
        char nals[200];
        nal_list(p, n, nals, sizeof nals);
        rt_log("movie_codec: H.264 access unit at %lld us, NAL type:size %s", (long long)us, nals);
    }
    int pre = c->kind == MC_H264 && c->csd_due ? c->ncsd : 0;
    IMFSample *s = make_sample(c->csd, pre, p, n, us);
    if (!s) return fail(c, "making an input sample", E_OUTOFMEMORY);
    HRESULT hr = IMFTransform_ProcessInput(c->t, 0, s, 0);
    IMFSample_Release(s);
    if (c->nfeed++ < 6)
        rt_log("movie_codec: %s ProcessInput %d bytes (+%d csd) at %lld us: hr %#lx", c->kind == MC_H264 ? "H.264" : "AAC",
               n, pre, (long long)us, (unsigned long)hr);
    if (hr == MF_E_NOTACCEPTING) return 0;
    if (FAILED(hr)) return fail(c, "ProcessInput", hr);
    if (pre) c->csd_due = 0;
    return 1;
}

static void pack_nv12(const MovieCodec *c, const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < c->h; y++) memcpy(dst + (size_t)y * c->w, src + (size_t)y * c->stride, (size_t)c->w);
    const uint8_t *uv = src + (size_t)c->stride * c->ah;
    uint8_t *duv = dst + (size_t)c->w * c->h;
    for (int y = 0; y < c->h / 2; y++) memcpy(duv + (size_t)y * c->w, uv + (size_t)y * c->stride, (size_t)c->w);
}

int mc_pull(MovieCodec *c, uint8_t *dst, int cap, int *bytes, int64_t *us) {
    com_init();
    for (int tries = 0; tries < 4; tries++) {
        MFT_OUTPUT_DATA_BUFFER out = {0};
        IMFSample *mine = NULL;
        if (!c->provides) {
            IMFMediaBuffer *buf = NULL;
            if (FAILED(MFCreateMemoryBuffer(c->out_size, &buf))) return fail(c, "an output buffer", E_OUTOFMEMORY);
            MFCreateSample(&mine);
            IMFSample_AddBuffer(mine, buf);
            IMFMediaBuffer_Release(buf);
            out.pSample = mine;
        }
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(c->t, 0, 1, &out, &status);
        if (out.pEvents) IMFCollection_Release(out.pEvents);
        if (c->npull++ < 6)
            rt_log("movie_codec: %s ProcessOutput (sample %s, %lu bytes): hr %#lx", c->kind == MC_H264 ? "H.264" : "AAC",
                   c->provides ? "theirs" : "ours", (unsigned long)c->out_size, (unsigned long)hr);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (mine) IMFSample_Release(mine);
            return 0;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {   // the decoder found the real format: choose the output again
            if (mine) IMFSample_Release(mine);
            if (FAILED(hr = pick_output(c))) return fail(c, "SetOutputType after a format change", hr);
            continue;
        }
        if (FAILED(hr)) {
            if (mine) IMFSample_Release(mine);
            if (out.pSample && out.pSample != mine) IMFSample_Release(out.pSample);
            return fail(c, "ProcessOutput", hr);
        }
        IMFSample *s = out.pSample;
        LONGLONG t = 0;
        IMFSample_GetSampleTime(s, &t);
        IMFMediaBuffer *buf = NULL;
        int r = -1;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &buf))) {
            BYTE *p;
            DWORD len = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
                if (c->kind == MC_H264) {
                    int need = c->w * c->h * 3 / 2;
                    if ((uint64_t)c->stride * c->ah * 3 / 2 > len || c->h > c->ah || c->w > c->stride) {
                        snprintf(c->err, sizeof c->err, "NV12 frame of %lu bytes for %dx%d (stride %d, height %d)",
                                 (unsigned long)len, c->w, c->h, c->stride, c->ah);
                    } else if (need > cap) {
                        snprintf(c->err, sizeof c->err, "NV12 frame of %d bytes, buffer %d", need, cap);
                    } else {
                        pack_nv12(c, p, dst);
                        *bytes = need;
                        r = 1;
                    }
                } else if ((int)len > cap) {
                    snprintf(c->err, sizeof c->err, "PCM block of %lu bytes, buffer %d", (unsigned long)len, cap);
                } else {
                    memcpy(dst, p, len);
                    *bytes = (int)len;
                    r = 1;
                }
                IMFMediaBuffer_Unlock(buf);
            }
            IMFMediaBuffer_Release(buf);
        }
        IMFSample_Release(s);
        if (mine && mine != s) IMFSample_Release(mine);
        *us = t / 10;
        return r;
    }
    return fail(c, "ProcessOutput (format changes)", E_FAIL);
}

void mc_drain(MovieCodec *c) {
    com_init();
    IMFTransform_ProcessMessage(c->t, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    IMFTransform_ProcessMessage(c->t, MFT_MESSAGE_COMMAND_DRAIN, 0);
}

void mc_flush(MovieCodec *c) {
    com_init();
    IMFTransform_ProcessMessage(c->t, MFT_MESSAGE_COMMAND_FLUSH, 0);
    c->csd_due = c->ncsd > 0;
}

void mc_close(MovieCodec *c) {
    if (!c) return;
    com_init();
    if (c->t) {
        IMFTransform_ProcessMessage(c->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(c->t);
    }
    free(c->csd);
    free(c->conv);
    free(c);
}

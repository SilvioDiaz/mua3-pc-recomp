// nn::audio::AudioOut on the host: the simple PCM output (audout:u on the Switch) that FMOD and the movie library use,
// played through an SDL3 audio stream per AudioOut. SDL mixes these streams with the audio renderer's (audio_mixer.c)
// on the same device. AudioIn (microphones) reports none.
//
// The game appends AudioOutBuffers and polls GetReleasedAudioOutBuffer, or waits on the SystemEvent it passed to
// OpenDefaultAudioOut. Here SDL's audio thread pulls the appended buffers in order, copies their PCM16 samples from
// guest memory (guest addresses are host addresses), and when a buffer is played out moves it to the released list and
// signals the event, as the Switch does when the hardware finished it. Without buffers, or before Start, it plays
// silence. GetAudioOutPlayedSampleCount counts the frames taken from buffers.
//
// Layouts read from the SDK (MUA3 and Cadence of Hyrule link the same sdk):
//   AudioOutParameter: int32 sample rate +0, uint16 channel count +4 (0 for both: 48000 Hz stereo), uint16 0xCAFE +6
//   AudioOutBuffer: +0 next (the SDK's), +8 data pointer, +0x10 buffer size, +0x18 data size (SetAudioOutBufferInfo)
//   AudioOut: opaque to the game; +0 holds the host object here.
// TOTK_AUDIO=none (or TOTK_AUDIO_OFF): no device; appended buffers are released at once.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define QUEUE 64                       // appended or released buffers per AudioOut at most
#define SAMPLE_FORMAT_PCM16 2          // nn::audio::SampleFormat_PcmInt16
#define R_AUDIO_INVALID 0x1f499u       // audio module 153: an argument the host cannot take
#define R_AUDIO_NO_INPUT 0x3299u       // audio module 153: no audio input device

typedef struct {
    uint64_t guest;                    // the AudioOut object
    int rate, channels, started, device;
    uint64_t event;                    // SystemEvent * from OpenDefaultAudioOut, 0 if none
    SDL_AudioStream *stream;
    PlatMutex mu;
    uint64_t queued[QUEUE];            // AudioOutBuffer * appended, not played yet (FIFO)
    int qhead, qn;
    uint64_t offset;                   // bytes of queued[qhead] already played
    uint64_t released[QUEUE];          // played, not taken back by GetReleasedAudioOutBuffer yet
    int rhead, rn;
    uint64_t played_frames;
} AudioOutHost;

static AudioOutHost *host_of(Cpu *c, int arg, const char *fn) {
    uint64_t obj = arg_x(c, arg);
    AudioOutHost *a = obj ? (AudioOutHost *)(uintptr_t)LD64(obj) : NULL;
    if (!a || a->guest != obj) rt_fatal("nn::audio::%s: AudioOut %#llx was not opened", fn, (unsigned long long)obj);
    return a;
}

static uint64_t buf_data(uint64_t b) { return LD64(b + 8); }
static uint64_t buf_size(uint64_t b) { return LD64(b + 0x18); }

// Caller holds a->mu. The finished buffer goes to the released list; the game's event says so.
static void release_head(AudioOutHost *a) {
    uint64_t b = a->queued[a->qhead];
    a->qhead = (a->qhead + 1) % QUEUE, a->qn--;
    a->offset = 0;
    if (a->rn == QUEUE) rt_fatal("nn::audio: AudioOut %#llx: %d played buffers not taken back", (unsigned long long)a->guest, QUEUE);
    a->released[(a->rhead + a->rn) % QUEUE] = b, a->rn++;
    if (a->event) kern_signal_system_event(a->event);
}

static void SDLCALL pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    AudioOutHost *a = user;
    static const uint8_t zeros[4096];
    plat_mutex_lock(&a->mu);
    while (additional > 0) {
        if (!a->started || !a->qn) {   // nothing to play: silence for what the device asks
            int n = additional < (int)sizeof zeros ? additional : (int)sizeof zeros;
            SDL_PutAudioStreamData(stream, zeros, n);
            additional -= n;
            continue;
        }
        uint64_t b = a->queued[a->qhead], size = buf_size(b), left = size > a->offset ? size - a->offset : 0;
        uint64_t n = left < (uint64_t)additional ? left : (uint64_t)additional;
        if (n) {
            SDL_PutAudioStreamData(stream, (const void *)(uintptr_t)(buf_data(b) + a->offset), (int)n);
            a->offset += n;
            additional -= (int)n;
            a->played_frames += n / (uint64_t)(2 * a->channels);
        }
        if (a->offset >= size) release_head(a);
    }
    plat_mutex_unlock(&a->mu);
}

static int audio_off(void) {
    const char *mode = cfg_get("TOTK_AUDIO");
    return (mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF");
}

// Result OpenDefaultAudioOut(AudioOut *out, [os::SystemEvent *event,] const AudioOutParameter &param)
static void open_out(Cpu *c, uint64_t event, uint64_t param) {
    uint64_t obj = arg_x(c, 0);
    if (!obj || !param) { ret_x(c, R_AUDIO_INVALID); return; }
    int rate = (int32_t)LD32(param), channels = (int)(uint16_t)LD16(param + 4);
    if (!rate) rate = 48000;
    if (!channels) channels = 2;
    if (rate != 48000 || (channels != 1 && channels != 2 && channels != 6)) {
        rt_log("audout: OpenDefaultAudioOut %d Hz x%d refused (48000 Hz, 1, 2 or 6 channels)", rate, channels);
        ret_x(c, R_AUDIO_INVALID);
        return;
    }
    AudioOutHost *a = calloc(1, sizeof *a);
    if (!a) rt_fatal("audout: allocation");
    a->guest = obj, a->rate = rate, a->channels = channels;
    plat_mutex_init(&a->mu);
    if (event) {
        kern_bind_system_event(c, event, 1);
        a->event = event;
    }
    if (!audio_off() && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, rate};
        a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, a);
        if (a->stream && SDL_ResumeAudioStreamDevice(a->stream)) a->device = 1;
        else rt_log("audout: no output device (%s): buffers are released unplayed", SDL_GetError());
    }
    ST64(obj, (uint64_t)(uintptr_t)a);
    char at[96];
    rt_log("audout: AudioOut %#llx open, %d Hz x%d PCM16%s, from %s", (unsigned long long)obj, rate, channels,
           event ? ", with a release event" : "", ldr_describe(c->x[30] - 4, at, sizeof at));
    ret_x(c, 0);
}
static void ov_open(Cpu *c) { open_out(c, 0, arg_x(c, 1)); }
static void ov_open_event(Cpu *c) { open_out(c, arg_x(c, 1), arg_x(c, 2)); }

static void ov_start(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "StartAudioOut");
    plat_mutex_lock(&a->mu);
    a->started = 1;
    plat_mutex_unlock(&a->mu);
    ret_x(c, 0);
}

static void ov_stop(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "StopAudioOut");
    plat_mutex_lock(&a->mu);
    a->started = 0;
    plat_mutex_unlock(&a->mu);
}

static void ov_close(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "CloseAudioOut");
    if (a->stream) SDL_DestroyAudioStream(a->stream);   // stops the callback before it returns
    rt_log("audout: AudioOut %#llx closed after %llu frames", (unsigned long long)a->guest,
           (unsigned long long)a->played_frames);
    ST64(a->guest, 0);
    free(a);
}

// bool AppendAudioOutBuffer(AudioOut *out, AudioOutBuffer *buffer)
static void ov_append(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "AppendAudioOutBuffer");
    uint64_t b = arg_x(c, 1);
    if (!b || !buf_data(b)) { ret_x(c, 0); return; }
    plat_mutex_lock(&a->mu);
    int ok = a->qn < QUEUE;
    if (ok) {
        a->queued[(a->qhead + a->qn) % QUEUE] = b, a->qn++;
        if (!a->device) {   // no device: played at once
            a->played_frames += buf_size(b) / (uint64_t)(2 * a->channels);
            release_head(a);
        }
    }
    plat_mutex_unlock(&a->mu);
    ret_x(c, (uint64_t)ok);
}

// AudioOutBuffer *GetReleasedAudioOutBuffer(AudioOut *out): the oldest played buffer, NULL if none
static void ov_released(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "GetReleasedAudioOutBuffer");
    uint64_t b = 0;
    plat_mutex_lock(&a->mu);
    if (a->rn) b = a->released[a->rhead], a->rhead = (a->rhead + 1) % QUEUE, a->rn--;
    plat_mutex_unlock(&a->mu);
    ret_x(c, b);
}

static void ov_volume(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "SetAudioOutVolume");
    float v = arg_s(c, 0);
    if (a->stream) SDL_SetAudioStreamGain(a->stream, v < 0 ? 0 : v);
}

static void ov_rate(Cpu *c) { ret_x(c, (uint64_t)host_of(c, 0, "GetAudioOutSampleRate")->rate); }
static void ov_channels(Cpu *c) { ret_x(c, (uint64_t)host_of(c, 0, "GetAudioOutChannelCount")->channels); }
static void ov_format(Cpu *c) { (void)host_of(c, 0, "GetAudioOutSampleFormat"); ret_x(c, SAMPLE_FORMAT_PCM16); }
static void ov_played(Cpu *c) {
    AudioOutHost *a = host_of(c, 0, "GetAudioOutPlayedSampleCount");
    plat_mutex_lock(&a->mu);
    uint64_t n = a->played_frames;
    plat_mutex_unlock(&a->mu);
    ret_x(c, n);
}

// ---- AudioIn: the host offers no microphone
static void ov_list_ins(Cpu *c) { ret_x(c, 0); }
static void ov_open_in(Cpu *c) {
    rt_log("audout: OpenAudioIn: no audio input on the host");
    ret_x(c, R_AUDIO_NO_INPUT);
}
static void ov_in_zero(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", ov_open);
HOST_OVERRIDE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE",
              ov_open_event);
HOST_OVERRIDE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", ov_start);
HOST_OVERRIDE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", ov_stop);
HOST_OVERRIDE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", ov_close);
HOST_OVERRIDE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", ov_append);
HOST_OVERRIDE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", ov_released);
HOST_OVERRIDE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", ov_volume);
HOST_OVERRIDE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", ov_rate);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", ov_channels);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", ov_format);
HOST_OVERRIDE("_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE", ov_played);

HOST_OVERRIDE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", ov_list_ins);
HOST_OVERRIDE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", ov_open_in);
HOST_OVERRIDE("_ZN2nn5audio12StartAudioInEPNS0_7AudioInE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio11StopAudioInEPNS0_7AudioInE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio12CloseAudioInEPNS0_7AudioInE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio19AppendAudioInBufferEPNS0_7AudioInEPNS0_13AudioInBufferE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio24GetReleasedAudioInBufferEPNS0_7AudioInE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio20GetAudioInSampleRateEPKNS0_7AudioInE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio22GetAudioInChannelCountEPKNS0_7AudioInE", ov_in_zero);
HOST_OVERRIDE("_ZN2nn5audio22GetAudioInSampleFormatEPKNS0_7AudioInE", ov_in_zero);

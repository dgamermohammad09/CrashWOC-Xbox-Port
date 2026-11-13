#include "platform/audio.h"

#include <SDL3/SDL.h>

#include "audio/spu2.h"

#ifdef NW_UWP
/* Xbox: SDL's WASAPI device never finishes opening in this app (its activation waits on the UI thread), so the
   stream has no device and a thread of ours hands it to XAudio2, 10 ms at a time. */
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#define NW_XA_BUFFERS 4
#define NW_XA_FRAMES 480

static struct {
    SDL_Thread *thread;
    SDL_AtomicInt run;
} nw_xa;
#endif

#define NW_AUDIO_RATE 48000
#define NW_AUDIO_FRAME (2 * sizeof(float))
#define NW_AUDIO_TARGET_MS 60 /* queued ahead of the device */
#define NW_AUDIO_MAX_MS 250   /* past this the queue is dropped */
#define NW_AUDIO_TRIM 0.005f  /* the most the playback rate is nudged to hold the target */
#define NW_AUDIO_CHUNK 256
#define NW_AUDIO_VOICE 4 /* the channel GameSfx plays voices on (gamesfx_channel) */

/* The SPU runs on the game's EE threads, the device on its own; the lock lets close race the game thread. */
static struct {
    SDL_Mutex *lock;
    SDL_AudioStream *stream; /* under lock */
    float master;            /* under lock once there is one */
    float dc_in[2], dc_out[2];
} nw_audio = { .master = 1.0f };

static unsigned nw_audio_queued_ms(void)
{
    return (unsigned)((uint64_t)SDL_GetAudioStreamQueued(nw_audio.stream) / NW_AUDIO_FRAME * 1000 / NW_AUDIO_RATE);
}

/* After an underrun the queue starts again at the target, with silence. */
static void nw_audio_prime(void)
{
    static const float silence[NW_AUDIO_CHUNK * 2];
    unsigned frames = NW_AUDIO_RATE * NW_AUDIO_TARGET_MS / 1000;
    while (frames) {
        unsigned n = frames < NW_AUDIO_CHUNK ? frames : NW_AUDIO_CHUNK;
        SDL_PutAudioStreamData(nw_audio.stream, silence, (int)(n * NW_AUDIO_FRAME));
        frames -= n;
    }
}

/* The PS2's analog out is AC coupled, so a paused voice's held sample is no offset (PCSX2's DCFilter). */
static void nw_audio_out(void *user, const int16_t *frames, unsigned count)
{
    float buf[NW_AUDIO_CHUNK * 2];
    (void)user;

    SDL_LockMutex(nw_audio.lock);
    if (nw_audio.stream) {
        if (SDL_GetAudioStreamQueued(nw_audio.stream) == 0) {
            nw_audio_prime();
        }
        while (count) {
            unsigned n = count < NW_AUDIO_CHUNK ? count : NW_AUDIO_CHUNK;
            for (unsigned i = 0; i < n * 2; i++) {
                float in = frames[i] / 32767.0f, *last = &nw_audio.dc_in[i & 1], *out = &nw_audio.dc_out[i & 1];
                *out = in - *last + 0.995f * *out;
                *last = in;
                buf[i] = *out;
            }
            SDL_PutAudioStreamData(nw_audio.stream, buf, (int)(n * NW_AUDIO_FRAME));
            frames += n * 2;
            count -= n;
        }
        unsigned ms = nw_audio_queued_ms();
        if (ms > NW_AUDIO_MAX_MS) {
            SDL_ClearAudioStream(nw_audio.stream);
        } else {
            float trim = ((float)ms - NW_AUDIO_TARGET_MS) / NW_AUDIO_TARGET_MS * NW_AUDIO_TRIM;
            trim = trim < -NW_AUDIO_TRIM ? -NW_AUDIO_TRIM : trim > NW_AUDIO_TRIM ? NW_AUDIO_TRIM : trim;
            SDL_SetAudioStreamFrequencyRatio(nw_audio.stream, 1.0f + trim);
        }
    }
    SDL_UnlockMutex(nw_audio.lock);
}

#ifdef NW_UWP
static int SDLCALL nw_xa_feed(void *data)
{
    static float bufs[NW_XA_BUFFERS][NW_XA_FRAMES * 2];
    const WAVEFORMATEX fmt = { WAVE_FORMAT_IEEE_FLOAT, 2, NW_AUDIO_RATE, NW_AUDIO_RATE * NW_AUDIO_FRAME,
                               NW_AUDIO_FRAME, 32, 0 };
    IXAudio2 *xa = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *voice = NULL;
    HRESULT hr;
    int next = 0;
    (void)data;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(hr = IXAudio2_CreateMasteringVoice(xa, &master, 2, NW_AUDIO_RATE, 0, NULL, NULL,
                                                  AudioCategory_GameEffects)) ||
        FAILED(hr = IXAudio2_CreateSourceVoice(xa, &voice, &fmt, 0, 2.0f, NULL, NULL, NULL)) ||
        FAILED(hr = IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW))) {
        SDL_Log("audio: XAudio2 failed (0x%08lx), no sound", (unsigned long)hr);
        goto done;
    }
    SDL_Log("audio: XAudio2, %d Hz", NW_AUDIO_RATE);
    while (SDL_GetAtomicInt(&nw_xa.run)) {
        XAUDIO2_VOICE_STATE state;
        IXAudio2SourceVoice_GetState(voice, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued >= NW_XA_BUFFERS - 1) {
            SDL_Delay(2);
            continue;
        }
        float *b = bufs[next];
        int got = 0;
        SDL_LockMutex(nw_audio.lock);
        float gain = nw_audio.master;
        if (nw_audio.stream) {
            got = SDL_GetAudioStreamData(nw_audio.stream, b, (int)sizeof(bufs[0]));
        }
        SDL_UnlockMutex(nw_audio.lock);
        got = got < 0 ? 0 : got;
        SDL_memset((char *)b + got, 0, sizeof(bufs[0]) - (size_t)got);
        for (int i = 0; i < NW_XA_FRAMES * 2; i++) {
            b[i] *= gain;
        }
        XAUDIO2_BUFFER xb;
        SDL_zero(xb);
        xb.AudioBytes = sizeof(bufs[0]);
        xb.pAudioData = (const BYTE *)b;
        IXAudio2SourceVoice_SubmitSourceBuffer(voice, &xb, NULL);
        next = (next + 1) % NW_XA_BUFFERS;
    }
done:
    if (voice) {
        IXAudio2SourceVoice_DestroyVoice(voice);
    }
    if (master) {
        IXAudio2MasteringVoice_DestroyVoice(master);
    }
    if (xa) {
        IXAudio2_Release(xa);
    }
    CoUninitialize();
    return 0;
}
#endif

/* Before the game thread starts: the SPU's output is set once. */
bool nw_audio_open(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, NW_AUDIO_RATE };

#ifdef NW_UWP
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"); /* streams only, no WASAPI */
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return false;
    }
    if (!nw_audio.lock && !(nw_audio.lock = SDL_CreateMutex())) {
        return false;
    }
#ifdef NW_UWP
    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, &spec);
#else
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
#endif
    if (!stream) {
        return false;
    }
    SDL_LockMutex(nw_audio.lock);
    nw_audio.stream = stream;
#ifndef NW_UWP
    SDL_SetAudioStreamGain(stream, nw_audio.master);
#endif
    SDL_UnlockMutex(nw_audio.lock);
#ifdef NW_UWP
    if (!nw_xa.thread) {
        SDL_SetAtomicInt(&nw_xa.run, 1);
        nw_xa.thread = SDL_CreateThread(nw_xa_feed, "nw_xaudio2", NULL);
    }
#else
    SDL_ResumeAudioStreamDevice(stream);
#endif
    nw_spu2_set_output(nw_audio_out, NULL);
    return true;
}

void nw_audio_set_buses(int master, int voice)
{
    nw_spu2_set_gain(NW_AUDIO_VOICE, SDL_clamp(voice, 0, 100) * NW_SPU2_UNITY / 100);
    if (nw_audio.lock) {
        SDL_LockMutex(nw_audio.lock);
    }
    nw_audio.master = (float)SDL_clamp(master, 0, 100) / 100.0f;
#ifndef NW_UWP /* the XAudio2 feeder applies it */
    if (nw_audio.stream) {
        SDL_SetAudioStreamGain(nw_audio.stream, nw_audio.master);
    }
#endif
    if (nw_audio.lock) {
        SDL_UnlockMutex(nw_audio.lock);
    }
}

/* The game thread may still run: the output stays set and goes nowhere. */
void nw_audio_close(void)
{
    if (!nw_audio.lock) {
        return;
    }
#ifdef NW_UWP
    if (nw_xa.thread) {
        SDL_SetAtomicInt(&nw_xa.run, 0);
        SDL_WaitThread(nw_xa.thread, NULL);
        nw_xa.thread = NULL;
    }
#endif
    SDL_LockMutex(nw_audio.lock);
    SDL_DestroyAudioStream(nw_audio.stream);
    nw_audio.stream = NULL;
    SDL_UnlockMutex(nw_audio.lock);
}

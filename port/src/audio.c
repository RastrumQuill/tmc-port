/**
 * @file audio.c
 * @brief Audio output: the DirectSound block mixed by the m4a engine plus an
 * emulation of the four Game Boy PSG channels, resampled by SDL.
 *
 * The PSG is driven by the sound registers the m4a CgbSound routine writes.
 * A write of the trigger bit (NRx4 bit 7) restarts a channel; the bit is
 * cleared again afterwards since it is write only on hardware. The channel
 * status bits in NR52 are kept up to date because the sound driver reads them.
 */
#include "port.h"

#include <SDL.h>
#include <string.h>

#include "global.h"

bool M4a_TakeMixedBlock(const s8** samples, int* count, int* freq);

#define OUT_RATE 48000

static SDL_AudioDeviceID sDevice;
static FILE* sWav;
static uint32_t sWavSamples;
static int sWavRate;

static void WavHeader(FILE* f, uint32_t samples, int rate) {
    uint32_t data = samples * 2;
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0 };
    uint32_t v;
    v = 36 + data;
    memcpy(h + 4, &v, 4);
    v = rate;
    memcpy(h + 24, &v, 4);
    v = rate * 2;
    memcpy(h + 28, &v, 4);
    h[32] = 2;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

/** Record the game audio into a WAV file (testing aid, --wav). */
void Audio_StartDump(const char* path) {
    sWav = fopen(path, "wb");
    if (sWav != NULL)
        WavHeader(sWav, 0, 15768);
}

void Audio_StopDump(void) {
    if (sWav == NULL)
        return;
    WavHeader(sWav, sWavSamples, sWavRate ? sWavRate : 15768);
    fclose(sWav);
    sWav = NULL;
}
static SDL_AudioStream* sStream;
static int sStreamRate;

/* ---- PSG ---- */

typedef struct {
    bool on;
    int volume;      /* 0..15 */
    int envDir;      /* +1 / -1 */
    int envPeriod;   /* 0 = off */
    int envTimer;
    int length;      /* remaining length ticks */
    bool lengthOn;
    double phase;    /* 0..1 within a waveform period */
    /* ch1 sweep */
    int sweepPeriod, sweepTimer, sweepShift;
    bool sweepNeg;
    int shadowFreq;
    /* ch4 */
    uint16_t lfsr;
} PsgChannel;

static PsgChannel sPsg[4];
static double sFrameSeqPos; /* 512 Hz frame sequencer */
static int sFrameSeqStep;

#define NR(off) (*(volatile uint8_t*)(uintptr_t)(PORT_IO_ADDR + (off)))

static const uint8_t sDuty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

static int FreqReg(int ch) {
    static const int lo[4] = { 0x64, 0x6C, 0x74, 0x7C };
    (void)lo;
    switch (ch) {
        case 0:
            return NR(0x64) | ((NR(0x65) & 7) << 8);
        case 1:
            return NR(0x6C) | ((NR(0x6D) & 7) << 8);
        case 2:
            return NR(0x74) | ((NR(0x75) & 7) << 8);
    }
    return 0;
}

static void SetNr52(int ch, bool on) {
    if (on)
        NR(0x84) |= 1 << ch;
    else
        NR(0x84) &= ~(1 << ch);
}

static void Trigger(int ch) {
    PsgChannel* c = &sPsg[ch];
    static const int envReg[4] = { 0x63, 0x69, 0, 0x79 };
    static const int lenReg[4] = { 0x62, 0x68, 0x72, 0x78 };
    c->on = true;
    if (ch != 2) {
        uint8_t env = NR(envReg[ch]);
        c->volume = env >> 4;
        c->envDir = (env & 8) ? 1 : -1;
        c->envPeriod = env & 7;
        c->envTimer = c->envPeriod;
        if ((env & 0xF8) == 0)
            c->on = false; /* DAC off */
    } else if (!(NR(0x70) & 0x80)) {
        c->on = false;
    }
    if (c->length == 0)
        c->length = (ch == 2) ? 256 - NR(lenReg[ch]) : 64 - (NR(lenReg[ch]) & 0x3F);
    if (ch == 0) {
        uint8_t sw = NR(0x60);
        c->sweepPeriod = (sw >> 4) & 7;
        c->sweepTimer = c->sweepPeriod;
        c->sweepShift = sw & 7;
        c->sweepNeg = (sw & 8) != 0;
        c->shadowFreq = FreqReg(0);
    }
    if (ch == 3)
        c->lfsr = 0x7FFF;
    c->phase = 0;
    SetNr52(ch, c->on);
}

/* called once per frame after the sound driver wrote the registers */
static void PsgLatch(void) {
    static const int ctrl[4] = { 0x65, 0x6D, 0x75, 0x7D };
    static const int lenReg[4] = { 0x62, 0x68, 0x72, 0x78 };
    int ch;
    for (ch = 0; ch < 4; ch++) {
        uint8_t v = NR(ctrl[ch]);
        sPsg[ch].lengthOn = (v & 0x40) != 0;
        if (v & 0x80) {
            sPsg[ch].length = 0;
            Trigger(ch);
            /* the length register was written with the trigger */
            sPsg[ch].length = (ch == 2) ? 256 - NR(lenReg[ch]) : 64 - (NR(lenReg[ch]) & 0x3F);
            NR(ctrl[ch]) = v & 0x7F;
        }
    }
    if (!(NR(0x70) & 0x80)) {
        sPsg[2].on = false;
        SetNr52(2, false);
    }
    if (!(NR(0x84) & 0x80)) {
        /* master off */
        for (ch = 0; ch < 4; ch++)
            sPsg[ch].on = false;
    }
}

static void FrameSequencerStep(void) {
    int ch;
    sFrameSeqStep = (sFrameSeqStep + 1) & 7;
    /* length: 256 Hz */
    if ((sFrameSeqStep & 1) == 0) {
        for (ch = 0; ch < 4; ch++) {
            PsgChannel* c = &sPsg[ch];
            if (c->lengthOn && c->length > 0) {
                if (--c->length == 0) {
                    c->on = false;
                    SetNr52(ch, false);
                }
            }
        }
    }
    /* sweep: 128 Hz */
    if (sFrameSeqStep == 2 || sFrameSeqStep == 6) {
        PsgChannel* c = &sPsg[0];
        if (c->on && c->sweepPeriod != 0 && --c->sweepTimer <= 0) {
            int delta = c->shadowFreq >> c->sweepShift;
            int f = c->sweepNeg ? c->shadowFreq - delta : c->shadowFreq + delta;
            c->sweepTimer = c->sweepPeriod;
            if (f > 2047) {
                c->on = false;
                SetNr52(0, false);
            } else if (c->sweepShift != 0) {
                c->shadowFreq = f;
                NR(0x64) = f & 0xFF;
                NR(0x65) = (NR(0x65) & ~7) | ((f >> 8) & 7);
            }
        }
    }
    /* envelope: 64 Hz */
    if (sFrameSeqStep == 7) {
        for (ch = 0; ch < 4; ch++) {
            PsgChannel* c = &sPsg[ch];
            if (ch == 2 || c->envPeriod == 0)
                continue;
            if (--c->envTimer <= 0) {
                int v = c->volume + c->envDir;
                c->envTimer = c->envPeriod;
                if (v >= 0 && v <= 15)
                    c->volume = v;
            }
        }
    }
}

/* returns a sample in -15..15 for a channel */
static int PsgSample(int ch, int rate) {
    PsgChannel* c = &sPsg[ch];
    double hz;
    int out = 0;
    if (!c->on)
        return 0;
    switch (ch) {
        case 0:
        case 1: {
            int f = (ch == 0) ? c->shadowFreq : FreqReg(1);
            int duty = ((ch == 0 ? NR(0x62) : NR(0x68)) >> 6) & 3;
            if (ch == 0 && c->sweepPeriod == 0)
                f = FreqReg(0);
            hz = 131072.0 / (2048 - f);
            out = sDuty[duty][(int)(c->phase * 8) & 7] ? c->volume : -c->volume;
            c->phase += hz / rate;
            break;
        }
        case 2: {
            static const int shift[4] = { 4, 0, 1, 2 };
            int f = FreqReg(2);
            int pos;
            uint8_t b;
            int s, volCode = (NR(0x73) >> 5) & 3;
            hz = 2097152.0 / (2048 - f) / 32.0;
            pos = (int)(c->phase * 32) & 31;
            b = NR(0x90 + (pos >> 1));
            s = (pos & 1) ? (b & 0xF) : (b >> 4);
            if (NR(0x73) & 0x80)
                s = s * 3 / 4;
            else
                s >>= shift[volCode];
            out = s * 2 - 15;
            if (volCode == 0 && !(NR(0x73) & 0x80))
                out = 0;
            c->phase += hz / rate;
            break;
        }
        case 3: {
            uint8_t poly = NR(0x7C);
            double r = (poly & 7) ? (poly & 7) : 0.5;
            int s = poly >> 4;
            hz = 524288.0 / r / (double)(2 << s);
            c->phase += hz / rate;
            while (c->phase >= 1.0) {
                int bit = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                c->lfsr = (c->lfsr >> 1) | (bit << 14);
                if (poly & 8)
                    c->lfsr = (c->lfsr & ~0x40) | (bit << 6);
                c->phase -= 1.0;
            }
            out = (c->lfsr & 1) ? -c->volume : c->volume;
            break;
        }
    }
    while (c->phase >= 1.0)
        c->phase -= 1.0;
    return out;
}

static void RenderFrame(const s8* dsound, int n, int rate, int16_t* out) {
    uint16_t cntL = PORT_IO16(0x80);
    uint16_t cntH = PORT_IO16(0x82);
    int masterR = (cntL & 7) + 1;
    int masterL = ((cntL >> 4) & 7) + 1;
    int psgShift = 2 - (cntH & 3); /* 0 = 25% (>>2), 1 = 50%, 2 = 100% */
    int dmaScale = (cntH & 4) ? 4 : 2;
    bool dmaOn = (cntH & 0x300) != 0;
    int i, ch;
    if (psgShift < 0)
        psgShift = 0;
    for (i = 0; i < n; i++) {
        int psg = 0;
        int v;
        sFrameSeqPos += 512.0 / rate;
        while (sFrameSeqPos >= 1.0) {
            sFrameSeqPos -= 1.0;
            FrameSequencerStep();
        }
        for (ch = 0; ch < 4; ch++) {
            int s = PsgSample(ch, rate);
            int amp = 0;
            if (cntL & (0x100 << ch))
                amp += s * masterR;
            if (cntL & (0x1000 << ch))
                amp += s * masterL;
            psg += amp;
        }
        psg = (psg / 2) >> psgShift;
        v = (dmaOn ? dsound[i] * dmaScale : 0) + psg;
        v *= 32;
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        out[i] = (int16_t)v;
    }
}

/* ---- SDL ---- */

void Audio_Init(void) {
    SDL_AudioSpec want, have;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Port_Log("no audio: %s", SDL_GetError());
        return;
    }
    memset(&want, 0, sizeof(want));
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    sDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sDevice == 0) {
        Port_Log("no audio device: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(sDevice, 0);
}

void Audio_Shutdown(void) {
    if (sStream != NULL)
        SDL_FreeAudioStream(sStream);
    if (sDevice != 0)
        SDL_CloseAudioDevice(sDevice);
    sStream = NULL;
    sDevice = 0;
}

static void Submit(const int16_t* samples, int n, int rate) {
    static int16_t converted[16384];
    int avail;
    if (sDevice == 0)
        return;
    if (sStream == NULL || sStreamRate != rate) {
        if (sStream != NULL)
            SDL_FreeAudioStream(sStream);
        sStream = SDL_NewAudioStream(AUDIO_S16SYS, 1, rate, AUDIO_S16SYS, 2, OUT_RATE);
        sStreamRate = rate;
        if (sStream == NULL)
            return;
    }
    SDL_AudioStreamPut(sStream, samples, n * 2);
    avail = SDL_AudioStreamAvailable(sStream);
    if (avail > (int)sizeof(converted))
        avail = sizeof(converted);
    avail = SDL_AudioStreamGet(sStream, converted, avail);
    /* keep the latency bounded (about 100 ms) */
    if (avail > 0 && SDL_GetQueuedAudioSize(sDevice) < OUT_RATE * 4 / 10)
        SDL_QueueAudio(sDevice, converted, avail);
}

void Audio_SubmitFrame(const int8_t* left, const int8_t* right, int samples, int sampleRate) {
    (void)left;
    (void)right;
    (void)samples;
    (void)sampleRate;
}

/** Called once per frame after the sound driver ran. */
void Audio_Frame(void) {
    static int16_t mixed[2048];
    const s8* block;
    int n, rate;
    PsgLatch();
    if (!M4a_TakeMixedBlock(&block, &n, &rate))
        return;
    if (rate <= 0 || n <= 0 || n > (int)(sizeof(mixed) / sizeof(mixed[0])))
        return;
    RenderFrame(block, n, rate, mixed);
    if (sWav != NULL) {
        fwrite(mixed, 2, n, sWav);
        sWavSamples += n;
        sWavRate = rate;
    }
    Submit(mixed, n, rate);
}

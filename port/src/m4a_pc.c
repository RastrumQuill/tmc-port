/**
 * @file m4a_pc.c
 * @brief C implementation of asm/lib/m4a_asm.s: the MusicPlayer2000 (m4a)
 * sequencer core and the DirectSound mixer.
 *
 * The Minish Cap mixes a single (mono) DirectSound channel. The mixer works
 * like the original: 8 bit samples, linear interpolation for pitched
 * instruments, wrapping 8 bit accumulation.
 */
#include "port.h"

#include <string.h>

#include "global.h"
#include "gba/m4a.h"

#define ID_NUMBER 0x68736D53

/* SoundInfo field offsets (the struct is private to src/gba/m4a.c) */
#define SI_IDENT 0x00
#define SI_PCM_DMA_COUNTER 0x04
#define SI_REVERB 0x05
#define SI_MAX_CHANS 0x06
#define SI_MASTER_VOLUME 0x07
#define SI_PCM_DMA_PERIOD 0x0b
#define SI_SAMPLES_PER_VBLANK 0x10
#define SI_PCM_FREQ 0x14
#define SI_DIV_FREQ 0x18
#define SI_CGB_CHANS 0x1c
#define SI_MPLAY_MAIN_HEAD 0x20
#define SI_INTP 0x24
#define SI_CGB_SOUND 0x28
#define SI_CGB_OSC_OFF 0x2c
#define SI_MIDI_KEY_TO_CGB_FREQ 0x30
#define SI_MPLAY_JUMP_TABLE 0x34
#define SI_PLYNOTE 0x38
#define SI_CHANS 0x50
#define SI_PCM_BUFFER 0x350
#define PCM_DMA_BUF_SIZE 1584

#define SOUND_INFO (*(u8**)0x03007FF0)
#define SI8(si, off) (*(u8*)((si) + (off)))
#define SI32(si, off) (*(u32*)((si) + (off)))
#define SIPTR(si, off) (*(void**)((si) + (off)))

/* CgbChannel is 0x40 bytes; only these fields are touched here */
#define CGB_PRIORITY 0x13
#define CGB_TRACK 0x2c
#define CGB_MODIFY 0x1d
#define CGB_LENGTH 0x1e
#define CGB_SWEEP 0x1f
#define CGB_FREQUENCY 0x20

#define CHN_ACTIVE 0xC7
#define CHN_START 0x80
#define CHN_STOP 0x40
#define CHN_LOOP 0x10
#define CHN_IEC 0x04
#define CHN_ENV 0x03

extern const u8 gClockTable[];
extern void* const gMPlayJumpTableTemplate[];
extern u32 MidiKeyToFreq(WaveData* wav, u8 key, u8 fineAdjust);
extern void TrkVolPitSet(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track);
extern void FadeOutBody(MusicPlayerInfo* mplayInfo);
extern void ClearChain(void* x);
extern void Clear64byte(void* addr);

typedef void (*TrackFunc)(MusicPlayerInfo*, MusicPlayerTrack*);
typedef void (*PlyNoteFunc)(u32, MusicPlayerInfo*, MusicPlayerTrack*);
typedef void (*CgbOscOffFunc)(u8);
typedef u32 (*MidiKeyToCgbFreqFunc)(u8, u8, u8);
typedef void (*VoidFunc)(void);

/* the last mixed block, for the audio output */
static const s8* sLastBlock;
static int sLastBlockSize;
static int sLastFreq;

u32 umul3232H32(u32 a, u32 b) {
    return (u32)(((u64)a * b) >> 32);
}

void MPlayJumpTableCopy(void** table) {
    int i;
    for (i = 0; i < 0x24; i++)
        table[i] = gMPlayJumpTableTemplate[i];
}

/* ---- channel lists ---- */

void RealClearChain(void* x) {
    SoundChannel* chan = x;
    MusicPlayerTrack* track = chan->track;
    SoundChannel* next;
    SoundChannel* prev;
    if (track == NULL)
        return;
    next = (SoundChannel*)chan->next;
    prev = (SoundChannel*)chan->prev;
    if (prev != NULL)
        prev->next = (u32)(uintptr_t)next;
    else
        track->chan = next;
    if (next != NULL)
        next->prev = (u32)(uintptr_t)prev;
    chan->track = NULL;
}

void SoundMainBTM(void* p) {
    memset(p, 0, 64);
}

/* walk the channel list of a track (the original guards against self links) */
static SoundChannel* NextChannel(SoundChannel* chan) {
    SoundChannel* next = (SoundChannel*)chan->next;
    if (next == chan) {
        chan->next = 0;
        next = NULL;
    }
    return next;
}

/* ---- sequencer commands ---- */

static u8 ReadByte(MusicPlayerTrack* track) {
    return *track->cmdPtr++;
}

void ply_fine(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    SoundChannel* chan = track->chan;
    (void)info;
    while (chan != NULL) {
        if (chan->statusFlags & CHN_ACTIVE)
            chan->statusFlags |= CHN_STOP;
        RealClearChain(chan);
        chan = NextChannel(chan);
    }
    track->flags = 0;
}

void ply_goto(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8* p = track->cmdPtr;
    (void)info;
    track->cmdPtr = (u8*)(uintptr_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}

void ply_patt(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    if (track->patternLevel >= 3) {
        ply_fine(info, track);
        return;
    }
    track->patternStack[track->patternLevel] = track->cmdPtr + 4;
    track->patternLevel++;
    ply_goto(info, track);
}

void ply_pend(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    if (track->patternLevel != 0) {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8* p = track->cmdPtr;
    if (*p == 0) {
        track->cmdPtr = p + 1;
        ply_goto(info, track);
        return;
    }
    track->repN++;
    track->cmdPtr = p + 1;
    if (track->repN < *p) {
        ply_goto(info, track);
    } else {
        track->repN = 0;
        track->cmdPtr = p + 5;
    }
}

void ply_prio(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->priority = ReadByte(track);
}

void ply_tempo(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u32 t = ReadByte(track) << 1;
    info->tempoD = t;
    info->tempoI = (t * info->tempoU) >> 8;
}

void ply_keysh(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->keyShift = ReadByte(track);
    track->flags |= 0xC;
}

void ply_voice(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8 index = ReadByte(track);
    memcpy(&track->tone, (u8*)info->tone + index * 12, 12);
}

void ply_vol(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->vol = ReadByte(track);
    track->flags |= 3;
}

void ply_pan(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->pan = ReadByte(track) - 0x40;
    track->flags |= 3;
}

void ply_bend(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->bend = ReadByte(track) - 0x40;
    track->flags |= 0xC;
}

void ply_bendr(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->bendRange = ReadByte(track);
    track->flags |= 0xC;
}

void ply_lfodl(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->lfoDelay = ReadByte(track);
}

void ply_modt(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8 v = ReadByte(track);
    (void)info;
    if (track->modT != v) {
        track->modT = v;
        track->flags |= 0xF;
    }
}

void ply_tune(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->tune = ReadByte(track) - 0x40;
    track->flags |= 0xC;
}

void ply_port(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8 reg = ReadByte(track);
    u8 value = ReadByte(track);
    (void)info;
    *(vu8*)(uintptr_t)(0x04000060 + reg) = value;
}

static void clear_modM(MusicPlayerTrack* track) {
    track->modM = 0;
    track->lfoSpeedC = 0;
    track->flags |= (track->modT == 0) ? 0xC : 3;
}

void ply_lfos(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->lfoSpeed = ReadByte(track);
    if (track->lfoSpeed == 0)
        clear_modM(track);
}

void ply_mod(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    (void)info;
    track->mod = ReadByte(track);
    if (track->mod == 0)
        clear_modM(track);
}

void ply_endtie(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8 key;
    SoundChannel* chan;
    (void)info;
    if (*track->cmdPtr < 0x80) {
        key = *track->cmdPtr++;
        track->key = key;
    } else {
        key = track->key;
    }
    for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
        if ((chan->statusFlags & 0x83) && !(chan->statusFlags & CHN_STOP) && chan->midiKey == key) {
            chan->statusFlags |= CHN_STOP;
            return;
        }
    }
}

void TrackStop(MusicPlayerInfo* info, MusicPlayerTrack* track) {
    SoundChannel* chan;
    (void)info;
    if (!(track->flags & 0x80))
        return;
    for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
        if (chan->statusFlags != 0) {
            u8 cgb = chan->type & 7;
            if (cgb != 0)
                ((CgbOscOffFunc)SIPTR(SOUND_INFO, SI_CGB_OSC_OFF))(cgb);
            chan->statusFlags = 0;
        }
        chan->track = NULL;
    }
    track->chan = NULL;
}

static void ChnVolSetAsm(SoundChannel* chan, MusicPlayerTrack* track) {
    s32 pan = (s8)chan->rhythmPan;
    u32 v = chan->velocity;
    u32 r = (track->volMR * ((0x80 + pan) * v)) >> 14;
    u32 l = (track->volML * ((0x7F - pan) * v)) >> 14;
    chan->rightVolume = r > 0xFF ? 0xFF : r;
    chan->leftVolume = l > 0xFF ? 0xFF : l;
}

void ply_note(u32 noteCmd, MusicPlayerInfo* info, MusicPlayerTrack* track) {
    u8* si = SOUND_INFO;
    u8* p;
    ToneData* tone;
    u32 key, priority, cgbType;
    s32 rhythmPan = 0;
    SoundChannel* chan;
    s32 k;

    track->gateTime = gClockTable[noteCmd];
    p = track->cmdPtr;
    if (*p < 0x80) {
        track->key = *p++;
        if (*p < 0x80) {
            track->velocity = *p++;
            if (*p < 0x80) {
                track->gateTime += *p++;
            }
        }
        track->cmdPtr = p;
    }

    tone = &track->tone;
    if (tone->type & 0xC0) {
        u32 index = track->key;
        if (tone->type & 0x40)
            index = ((u8*)(uintptr_t) * (u32*)&tone->attack)[track->key];
        tone = (ToneData*)((u8*)tone->wav + index * 12);
        if (tone->type & 0xC0)
            return;
        if (track->tone.type & 0x80) {
            if (tone->pan_sweep & 0x80)
                rhythmPan = ((s32)tone->pan_sweep - 0xC0) * 2;
            key = tone->key;
        } else {
            key = track->key;
        }
    } else {
        key = track->key;
    }

    priority = info->priority + track->priority;
    if (priority > 0xFF)
        priority = 0xFF;

    cgbType = tone->type & 7;
    if (cgbType != 0) {
        u8* cgb = SIPTR(si, SI_CGB_CHANS);
        if (cgb == NULL)
            return;
        chan = (SoundChannel*)(cgb + (cgbType - 1) * 0x40);
        if ((chan->statusFlags & CHN_ACTIVE) && !(chan->statusFlags & CHN_STOP)) {
            if (chan->priority > priority)
                return;
            if (chan->priority == priority && (uintptr_t)chan->track < (uintptr_t)track)
                return;
        }
    } else {
        u32 bestPrio = priority;
        MusicPlayerTrack* bestTrack = track;
        bool foundStopping = false;
        SoundChannel* best = NULL;
        SoundChannel* c = (SoundChannel*)(si + SI_CHANS);
        u32 n = SI8(si, SI_MAX_CHANS);
        chan = NULL;
        for (; n > 0; n--, c++) {
            if (!(c->statusFlags & CHN_ACTIVE)) {
                chan = c;
                break;
            }
            if (c->statusFlags & CHN_STOP) {
                if (!foundStopping) {
                    foundStopping = true;
                    bestPrio = c->priority;
                    bestTrack = c->track;
                    best = c;
                    continue;
                }
            } else if (foundStopping) {
                continue;
            }
            if (c->priority < bestPrio) {
                bestPrio = c->priority;
                bestTrack = c->track;
                best = c;
            } else if (c->priority == bestPrio) {
                if ((uintptr_t)c->track > (uintptr_t)bestTrack) {
                    bestTrack = c->track;
                    best = c;
                } else if (c->track == bestTrack) {
                    best = c;
                }
            }
        }
        if (chan == NULL) {
            chan = best;
            if (chan == NULL)
                return;
        }
    }

    ClearChain(chan);
    chan->prev = 0;
    chan->next = (u32)(uintptr_t)track->chan;
    if (track->chan != NULL)
        track->chan->prev = (u32)(uintptr_t)chan;
    track->chan = chan;
    chan->track = track;
    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        clear_modM(track);
    TrkVolPitSet(info, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;
    chan->key = key;
    chan->rhythmPan = rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->echoVolume = track->echoVolume;
    chan->echoLength = track->echoLength;
    ChnVolSetAsm(chan, track);

    k = chan->key + track->keyM;
    if (k < 0)
        k = 0;
    if (cgbType != 0) {
        u8 sweep = tone->pan_sweep;
        ((u8*)chan)[CGB_LENGTH] = tone->length;
        if ((sweep & 0x80) || !(sweep & 0x70))
            sweep = 8;
        ((u8*)chan)[CGB_SWEEP] = sweep;
        chan->frequency = ((MidiKeyToCgbFreqFunc)SIPTR(si, SI_MIDI_KEY_TO_CGB_FREQ))(cgbType, k, track->pitM);
    } else {
        chan->frequency = MidiKeyToFreq(chan->wav, k, track->pitM);
    }
    chan->statusFlags = CHN_START;
    track->flags &= 0xF0;
}

/* ---- sequencer main loop ---- */

void MPlayMain(MusicPlayerInfo* info) {
    u8* si;
    if (info->ident != ID_NUMBER)
        return;
    info->ident++;
    if (info->func != NULL)
        info->func((MusicPlayerInfo*)info->intp);

    if ((s32)info->status < 0)
        goto done;
    si = SOUND_INFO;
    FadeOutBody(info);
    if ((s32)info->status < 0)
        goto done;

    info->tempoC += info->tempoI;
    while (info->tempoC >= 150) {
        u32 n = info->trackCount;
        MusicPlayerTrack* track = info->tracks;
        u32 bit = 1;
        u32 active = 0;
        for (; n > 0; n--, track++, bit <<= 1) {
            SoundChannel* chan;
            if (!(track->flags & 0x80))
                continue;
            active |= bit;
            for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
                if (chan->statusFlags & CHN_ACTIVE) {
                    if (chan->gateTime != 0 && --chan->gateTime == 0)
                        chan->statusFlags |= CHN_STOP;
                } else {
                    ClearChain(chan);
                }
            }
            if (track->flags & 0x40) {
                Clear64byte(track);
                track->flags = 0x80;
                track->bendRange = 2;
                track->volX = 0x40;
                track->lfoSpeed = 0x16;
                track->tone.type = 1;
            }
            for (;;) {
                u32 cmd;
                if (track->wait != 0)
                    break;
                cmd = *track->cmdPtr;
                if (cmd < 0x80) {
                    cmd = track->runningStatus;
                } else {
                    track->cmdPtr++;
                    if (cmd >= 0xBD)
                        track->runningStatus = cmd;
                }
                if (cmd >= 0xCF) {
                    ((PlyNoteFunc)SIPTR(si, SI_PLYNOTE))(cmd - 0xCF, info, track);
                } else if (cmd > 0xB0) {
                    info->cmd = cmd - 0xB1;
                    ((TrackFunc)((void**)SIPTR(si, SI_MPLAY_JUMP_TABLE))[cmd - 0xB1])(info, track);
                    if (track->flags == 0)
                        goto nextTrack;
                } else {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }
            track->wait--;
            if (track->lfoSpeed != 0 && track->mod != 0) {
                if (track->lfoDelayC != 0) {
                    track->lfoDelayC--;
                } else {
                    u8 c = track->lfoSpeedC + track->lfoSpeed;
                    s32 m;
                    track->lfoSpeedC = c;
                    if ((s8)(c - 0x40) < 0)
                        m = (s8)c;
                    else
                        m = 0x80 - c;
                    m = (track->mod * m) >> 6;
                    if ((u8)(track->modM ^ m) != 0) {
                        track->modM = m;
                        track->flags |= (track->modT == 0) ? 0xC : 3;
                    }
                }
            }
        nextTrack:;
        }
        info->clock++;
        if (active == 0) {
            info->status = 0x80000000;
            goto done;
        }
        info->status = active;
        info->tempoC -= 150;
    }

    {
        u32 n = info->trackCount;
        MusicPlayerTrack* track = info->tracks;
        for (; n > 0; n--, track++) {
            SoundChannel* chan;
            if (!(track->flags & 0x80) || !(track->flags & 0xF))
                continue;
            TrkVolPitSet(info, track);
            for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
                u32 cgbType;
                if (!(chan->statusFlags & CHN_ACTIVE)) {
                    ClearChain(chan);
                    continue;
                }
                cgbType = chan->type & 7;
                if (track->flags & 3) {
                    ChnVolSetAsm(chan, track);
                    if (cgbType != 0)
                        ((u8*)chan)[CGB_MODIFY] |= 1;
                }
                if (track->flags & 0xC) {
                    s32 k = chan->key + track->keyM;
                    if (k < 0)
                        k = 0;
                    if (cgbType != 0) {
                        chan->frequency =
                            ((MidiKeyToCgbFreqFunc)SIPTR(si, SI_MIDI_KEY_TO_CGB_FREQ))(cgbType, k, track->pitM);
                        ((u8*)chan)[CGB_MODIFY] |= 2;
                    } else {
                        chan->frequency = MidiKeyToFreq(chan->wav, k, track->pitM);
                    }
                }
            }
            track->flags &= 0xF0;
        }
    }
done:
    info->ident = ID_NUMBER;
}

/* ---- mixer ---- */

static void MixChannel(u8* si, SoundChannel* chan, s8* out, u32 samples) {
    WaveData* wav = chan->wav;
    u8 status = chan->statusFlags;
    u32 env;
    u32 vol;
    const s8* loopStart = NULL;
    s32 loopLen = 0;
    const s8* cur;
    s32 count;
    u32 i;

    if (!(status & CHN_ACTIVE))
        return;
    if (status & CHN_START) {
        if (status & CHN_STOP) {
            chan->statusFlags = 0;
            return;
        }
        status = CHN_ENV; /* attack */
        chan->currentPointer = (u32)(uintptr_t)wav->data;
        chan->count = wav->size;
        chan->envelopeVolume = 0;
        chan->fw = 0;
        if (((u8*)wav)[3] & 0xC0)
            status |= CHN_LOOP;
        env = 0;
        goto attack;
    }
    env = chan->envelopeVolume;
    if (status & CHN_IEC) {
        u8 len = chan->echoLength;
        chan->echoLength = len - 1;
        if (len <= 1) {
            chan->statusFlags = 0;
            return;
        }
    } else if (status & CHN_STOP) {
        env = (env * chan->release) >> 8;
        if (env <= chan->echoVolume) {
        echo:
            env = chan->echoVolume;
            if (env == 0) {
                chan->statusFlags = 0;
                return;
            }
            status |= CHN_IEC;
        }
    } else if ((status & CHN_ENV) == 2) { /* decay */
        env = (env * chan->decay) >> 8;
        if (env <= chan->sustain) {
            env = chan->sustain;
            if (env == 0)
                goto echo;
            status--;
        }
    } else if ((status & CHN_ENV) == 3) {
    attack:
        env += chan->attack;
        if (env >= 0xFF) {
            env = 0xFF;
            status--;
        }
    }
    chan->statusFlags = status;
    chan->envelopeVolume = env;
    env = ((SI8(si, SI_MASTER_VOLUME) + 1) * env) >> 4;
    vol = ((chan->rightVolume + chan->leftVolume) * env) >> 9;
    chan->envelopeVolumeRight = vol;

    if (status & CHN_LOOP) {
        loopStart = wav->data + wav->loopStart;
        loopLen = wav->size - wav->loopStart;
    }

    cur = (const s8*)(uintptr_t)chan->currentPointer;
    count = chan->count;

    if (chan->type & 8) {
        /* fixed frequency: one source sample per output sample */
        for (i = 0; i < samples; i++) {
            out[i] = (s8)(out[i] + ((s32)(vol * *cur++) >> 8));
            if (--count <= 0) {
                if (loopLen != 0) {
                    cur = loopStart;
                    count = loopLen;
                } else {
                    chan->statusFlags = 0;
                    return;
                }
            }
        }
    } else {
        u32 fw = chan->fw;
        u32 step = SI32(si, SI_DIV_FREQ) * chan->frequency;
        s32 s0 = cur[0];
        s32 d = cur[1] - s0;
        for (i = 0; i < samples; i++) {
            s32 v = s0 + ((s32)(fw * d) >> 23);
            u32 adv;
            out[i] = (s8)(out[i] + ((s32)(vol * v) >> 8));
            fw += step;
            adv = fw >> 23;
            if (adv != 0) {
                fw &= 0x7FFFFF;
                count -= adv;
                if (count <= 0) {
                    if (loopLen == 0) {
                        chan->statusFlags = 0;
                        return;
                    }
                    {
                        s32 over = -count;
                        count += loopLen;
                        while (count <= 0) {
                            over -= loopLen;
                            count += loopLen;
                        }
                        cur = loopStart + over;
                    }
                } else {
                    cur += adv;
                }
                s0 = cur[0];
                d = cur[1] - s0;
            }
        }
        chan->fw = fw;
    }
    chan->count = count;
    chan->currentPointer = (u32)(uintptr_t)cur;
}

void SoundMain(void) {
    u8* si = SOUND_INFO;
    u32 samples, counter, reverb, n;
    s8* buf;
    SoundChannel* chan;

    if (si == NULL || SI32(si, SI_IDENT) != ID_NUMBER)
        return;
    SI32(si, SI_IDENT)++;

    if (SIPTR(si, SI_MPLAY_MAIN_HEAD) != NULL)
        ((void (*)(void*))SIPTR(si, SI_MPLAY_MAIN_HEAD))(SIPTR(si, SI_INTP));
    ((VoidFunc)SIPTR(si, SI_CGB_SOUND))();

    samples = SI32(si, SI_SAMPLES_PER_VBLANK);
    buf = (s8*)(si + SI_PCM_BUFFER);
    counter = SI8(si, SI_PCM_DMA_COUNTER);
    if (counter > 1)
        buf += (SI8(si, SI_PCM_DMA_PERIOD) - (counter - 1)) * samples;
    if (buf + samples > (s8*)(si + SI_PCM_BUFFER) + PCM_DMA_BUF_SIZE)
        buf = (s8*)(si + SI_PCM_BUFFER);

    reverb = SI8(si, SI_REVERB);
    if (reverb != 0) {
        const s8* other = (counter == 2) ? (const s8*)(si + SI_PCM_BUFFER) : buf + samples;
        u32 i;
        for (i = 0; i < samples; i++) {
            s32 v = ((buf[i] + other[i]) * (s32)reverb) >> 8;
            if (v & 0x80)
                v++;
            buf[i] = (s8)v;
        }
    } else {
        memset(buf, 0, samples);
    }

    chan = (SoundChannel*)(si + SI_CHANS);
    for (n = SI8(si, SI_MAX_CHANS); n > 0; n--, chan++)
        MixChannel(si, chan, buf, samples);

    sLastBlock = buf;
    sLastBlockSize = samples;
    sLastFreq = SI32(si, SI_PCM_FREQ);
    SI32(si, SI_IDENT) = ID_NUMBER;
}

void SoundMainRAM(void) {
    /* the mixer lives in SoundMain on PC */
}

/** The block mixed by the last SoundMain call (consumed once). */
bool M4a_TakeMixedBlock(const s8** samples, int* count, int* freq) {
    if (sLastBlock == NULL)
        return false;
    *samples = sLastBlock;
    *count = sLastBlockSize;
    *freq = sLastFreq;
    sLastBlock = NULL;
    return true;
}

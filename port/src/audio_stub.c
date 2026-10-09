/**
 * @file audio_stub.c
 * @brief Placeholder for the m4a assembly (sound mixer and sequencer).
 * Replaced by port/src/m4a_pc.c once audio is implemented.
 */
#include "port.h"

#include "global.h"
#include "gba/m4a.h"

void SoundMain(void) {
}
void SoundMainRAM(void) {
}
void SoundMainBTM(void) {
}
void RealClearChain(void* x) {
    (void)x;
}
void MPlayMain(MusicPlayerInfo* mplayInfo) {
    (void)mplayInfo;
}
void TrackStop(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    (void)mplayInfo;
    (void)track;
}
void MPlayJumpTableCopy(void** mplayJumpTable) {
    (void)mplayJumpTable;
}
u32 umul3232H32(u32 a, u32 b) {
    return (u32)(((u64)a * b) >> 32);
}

#define PLY_STUB(name) \
    void name(MusicPlayerInfo* a, MusicPlayerTrack* b) { (void)a; (void)b; }
PLY_STUB(ply_fine)
PLY_STUB(ply_goto)
PLY_STUB(ply_patt)
PLY_STUB(ply_pend)
PLY_STUB(ply_rept)
PLY_STUB(ply_prio)
PLY_STUB(ply_tempo)
PLY_STUB(ply_keysh)
PLY_STUB(ply_voice)
PLY_STUB(ply_vol)
PLY_STUB(ply_pan)
PLY_STUB(ply_bend)
PLY_STUB(ply_bendr)
PLY_STUB(ply_lfodl)
PLY_STUB(ply_modt)
PLY_STUB(ply_tune)
PLY_STUB(ply_port)
PLY_STUB(ply_note)
PLY_STUB(ply_endtie)
PLY_STUB(ply_lfos)
PLY_STUB(ply_mod)

void Audio_Init(void) {
}
void Audio_Shutdown(void) {
}
void Audio_SubmitFrame(const int8_t* left, const int8_t* right, int samples, int sampleRate) {
    (void)left;
    (void)right;
    (void)samples;
    (void)sampleRate;
}

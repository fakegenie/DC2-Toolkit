#include "sound.hpp"

extern "C" {
#include "audio.h"
#include "midi.h"
#include "spu2.h"
}

PORT_OVERRIDE void CSound::StopVoice(int core) { spu_voices_stop(core, 0xFFFFFF); }

PORT_OVERRIDE void CSound::SndInReverb(bool enable) {
    spu_core_mixer(0, enable ? -4 : -0x34);
    spu_core_mixer(1, enable ? -4 : -0x34);
}

PORT_OVERRIDE void CSound::SetReverb(int core, int mode, int depth) { spu_core_reverb(core, mode, depth); }

PORT_OVERRIDE void set_spu(int mode0, int mode1, int depth0, int depth1) {
    spu_reset();
    spu_core_reverb(0, mode0, depth0);
    spu_core_reverb(1, mode1, depth1);
}

PORT_OVERRIDE int CSound::Init(int mode0, int mode1, int depth0, int depth1) {
    if (midi_opened()) {
        return 0;
    }

    set_spu(mode0, mode1, depth0, depth1);
    midi_open();
    return 0;
}

PORT_OVERRIDE int CSound::Exit() {
    midi_close();
    return 0;
}

PORT_OVERRIDE void CSound::DEL_PORT(int port) { midi_port_delete(port); }

PORT_OVERRIDE void CSound::SQ_Play(int port, int seq_no, int volume) { midi_sequence_play(port, seq_no, volume); }

PORT_OVERRIDE void CSound::SQ_RePlay(int port) { midi_sequence_resume(port); }

PORT_OVERRIDE void CSound::SE_Play(int port, int bank, int program, int key, int pan, int velocity, int volume, int pitch, int id) {
    midi_effect_play(port, bank, program, key, pan, velocity, volume, id);
}

PORT_OVERRIDE void CSound::SE_SetVol(int port, int bank, int program, int key, int volume, int id) {
    midi_effect_volume(port, bank, program, key, volume, id);
}

PORT_OVERRIDE void CSound::SE_SetPan(int port, int bank, int program, int key, int pan, int id) {
    midi_effect_pan(port, bank, program, key, pan, id);
}

PORT_OVERRIDE void CSound::SE_Stop(int port, int bank, int program, int key, int id) {
    midi_effect_stop(port, bank, program, key, id);
}

PORT_OVERRIDE void CSound::SE_SetPitch(int port, int bank, int program, int key, int pitch, int id) {
    midi_effect_pitch(port, bank, program, key, pitch, id);
}

PORT_OVERRIDE void CSound::Stop(int port) { midi_stop(port); }

PORT_OVERRIDE void CSound::SetVol(int port, int volume) { midi_volume(port, volume); }

PORT_OVERRIDE void CSound::SetStereoMode(int mode) { midi_stereo(mode); }

PORT_OVERRIDE void CSound::SetMasterVol(int core, int volume) { spu_core_master_volume(core, volume); }

PORT_OVERRIDE void CSound::LoadHdBd2(int port, uptr hd, int hd_size, uptr bd, int bd_size) {
    midi_bank_first(port, (const void *) hd, hd_size, (const void *) bd, bd_size);
}

PORT_OVERRIDE int CSound::LoadHdBdAdd(int port, uptr hd, int hd_size, uptr bd, int bd_size) {
    return midi_bank_add(port, (const void *) hd, hd_size, (const void *) bd, bd_size);
}

PORT_OVERRIDE int CSound::LoadSeq(int port, uptr address, int size) {
    return midi_sequence_load(port, (const void *) address, size);
}

PORT_OVERRIDE void CSound::StreamOpenFast(int channel, char *name) {
    audio_stream_volume(channel, 0, 0);
    audio_stream_open(channel, name);
}

PORT_OVERRIDE void CSound::StreamOpenFromFPLFast(int channel, char *name, char *pack_name) {
    audio_stream_volume(channel, 0, 0);
    audio_stream_open_pack(channel, name, pack_name);
}

PORT_OVERRIDE void CSound::StreamPlay(int channel) { audio_stream_play(channel); }

PORT_OVERRIDE void CSound::StreamStop(int channel) { audio_stream_stop(channel); }

PORT_OVERRIDE void CSound::StreamClose(int channel) {
    audio_stream_stop(channel);
    audio_stream_rewind(channel);
    audio_stream_close(channel);
}

PORT_OVERRIDE void CSound::StreamEND(int channel) {
    audio_stream_stop(channel);
    audio_stream_close(channel);
}

PORT_OVERRIDE void CSound::StreamPause(int channel) { audio_stream_stop(channel); }

PORT_OVERRIDE void CSound::StreamRePlay(int channel) { audio_stream_play(channel); }

PORT_OVERRIDE void CSound::StreamSetVol(int channel, int left, int right) { audio_stream_volume(channel, left, right); }

PORT_OVERRIDE int CSound::StreamGetState(int channel) { return audio_stream_state(channel) & ~0xFFF; }

PORT_OVERRIDE int CSound::StreamGetLevel(int channel) { return audio_stream_level(channel); }

PORT_OVERRIDE void CSound::StreamStandBy(int channel) {}

PORT_OVERRIDE int CSound::TransBdState(int channel) { return 1; }

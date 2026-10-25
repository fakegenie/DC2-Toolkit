#pragma once
#include <stdint.h>
#include "spu2.h"

#define SCE_CREATOR "IECS"
#define SCE_TABLE_NONE UINT32_MAX

enum { SCE_LFO_PITCH, SCE_LFO_AMP, SCE_LFOS };
enum { SCE_LFO_POSITIVE, SCE_LFO_NEGATIVE, SCE_LFO_SIDES };
enum { SCE_LFO_TRIGGER_KEY_ON = 0x1, SCE_LFO_TRIGGER_KEY_OFF = 0x2, SCE_LFO_TRIGGER_BOTH = 0x4 };
enum { SCE_LFO_WAVE_OFF, SCE_LFO_WAVE_SAW_UP, SCE_LFO_WAVE_SAW_DOWN, SCE_LFO_WAVE_TRIANGLE, SCE_LFO_WAVE_SQUARE, SCE_LFO_WAVE_NOISE, SCE_LFO_WAVE_SINE };
enum { SCE_SAMPLE_CORE_ANY, SCE_SAMPLE_CORE_0, SCE_SAMPLE_CORE_1, SCE_SAMPLE_CORE_EITHER };
enum { SCE_VELOCITY_CURVE_LINEAR = 0, SCE_VELOCITY_CURVE_SQUARE = 2 };
enum { SCE_PROGRAM_PAN_CIRCULAR = 0x1 };
enum { SCE_SQ_DIVISION_SMPTE = 0x8000 };

struct sce_chunk {
    char creator[4];
    char type[4];
    uint32_t size;
};
_Static_assert(sizeof(struct sce_chunk) == 12, "Sony chunk header");

struct sce_vers {
    struct sce_chunk chunk;
    uint16_t reserved;
    uint8_t version_major;
    uint8_t version_minor;
};
_Static_assert(sizeof(struct sce_vers) == 16, "Vers chunk");

struct sce_hd_head {
    struct sce_chunk chunk;
    uint32_t header_size;
    uint32_t body_size;
    uint32_t program_chunk;
    uint32_t sample_set_chunk;
    uint32_t sample_chunk;
    uint32_t vag_info_chunk;
    uint32_t se_timbre_chunk;
    uint8_t reserved[24];
};
_Static_assert(sizeof(struct sce_hd_head) == 64, "HD Head chunk");

struct sce_hd_header {
    struct sce_vers vers;
    struct sce_hd_head head;
};
_Static_assert(sizeof(struct sce_hd_header) == 80, "HD file header");

struct sce_table {
    struct sce_chunk chunk;
    uint32_t max_index;
    uint32_t offsets[];
};
_Static_assert(sizeof(struct sce_table) == 16, "HD table chunk");

struct sce_vag_info {
    uint32_t offset;
    uint16_t rate;
    uint8_t attribute;
    uint8_t reserved;
};
_Static_assert(sizeof(struct sce_vag_info) == 8, "Vagi record");

struct sce_key_follow {
    int8_t amount;
    uint8_t center;
};

struct sce_lfo_attribute {
    uint8_t pitch : 3;
    uint8_t pitch_unused : 1;
    uint8_t amp : 3;
    uint8_t amp_unused : 1;
};

struct sce_spu_attribute {
    uint8_t route : 4;
    uint8_t core : 2;
    uint8_t unused : 2;
};

struct sce_sample {
    uint16_t vag_index;
    uint8_t vel_range_low;
    uint8_t vel_cross_fade;
    uint8_t vel_range_high;
    int8_t vel_follow_pitch;
    uint8_t vel_follow_pitch_center;
    uint8_t vel_follow_pitch_curve;
    int8_t vel_follow_amp;
    uint8_t vel_follow_amp_center;
    uint8_t vel_follow_amp_curve;
    uint8_t base_note;
    int8_t detune;
    uint8_t panpot;
    uint8_t group;
    uint8_t priority;
    uint8_t volume;
    uint8_t reserved;
    struct spu_adsr1 adsr1;
    struct spu_adsr2 adsr2;
    struct sce_key_follow key_follow_ar;
    struct sce_key_follow key_follow_dr;
    struct sce_key_follow key_follow_sr;
    struct sce_key_follow key_follow_rr;
    struct sce_key_follow key_follow_sl;
    uint16_t pitch_lfo_delay;
    uint16_t pitch_lfo_fade;
    uint16_t amp_lfo_delay;
    uint16_t amp_lfo_fade;
    struct sce_lfo_attribute lfo_attribute;
    struct sce_spu_attribute spu_attribute;
};
_Static_assert(sizeof(struct sce_sample) == 42, "Smpl record");

struct sce_split {
    uint16_t sample_set;
    uint8_t range_low;
    uint8_t cross_fade;
    uint8_t range_high;
    uint8_t number;
    uint16_t bend_range_low;
    uint16_t bend_range_high;
    struct sce_key_follow key_follow_pitch;
    struct sce_key_follow key_follow_amp;
    struct sce_key_follow key_follow_pan;
    uint8_t volume;
    uint8_t panpot;
    int8_t transpose;
    int8_t detune;
};
_Static_assert(sizeof(struct sce_split) == 20, "Prog split block");

struct sce_program {
    uint32_t split_block_addr;
    uint8_t n_split;
    uint8_t split_block_size;
    uint8_t volume;
    uint8_t panpot;
    int8_t transpose;
    int8_t detune;
    struct sce_key_follow key_follow_pan;
    uint8_t attribute;
    uint8_t reserved;
    uint8_t lfo_wave[SCE_LFOS];
    uint8_t lfo_start_phase[SCE_LFOS];
    uint8_t lfo_phase_random[SCE_LFOS];
    uint16_t lfo_cycle[SCE_LFOS];
    int16_t lfo_pitch_depth[SCE_LFO_SIDES];
    int16_t lfo_midi_pitch_depth[SCE_LFO_SIDES];
    int8_t lfo_amp_depth[SCE_LFO_SIDES];
    int8_t lfo_midi_amp_depth[SCE_LFO_SIDES];
};
_Static_assert(sizeof(struct sce_program) == 36, "Prog record");

struct sce_sample_set {
    uint8_t vel_curve;
    uint8_t vel_limit_low;
    uint8_t vel_limit_high;
    uint8_t sample_count;
    uint16_t samples[UINT8_MAX];
};

struct sce_sq_sequ {
    struct sce_chunk chunk;
    uint32_t file_size;
    uint32_t song_chunk;
    uint32_t midi_chunk;
    uint32_t se_sequence_chunk;
    uint32_t se_song_chunk;
};
_Static_assert(sizeof(struct sce_sq_sequ) == 32, "SQ Sequ chunk");

struct sce_sq_midi {
    struct sce_chunk chunk;
    uint32_t max_data_index;
    uint32_t data_offset;
};
_Static_assert(sizeof(struct sce_sq_midi) == 20, "SQ Midi chunk");

struct sce_sq_head {
    struct sce_vers vers;
    struct sce_sq_sequ sequ;
    struct sce_sq_midi midi;
};
_Static_assert(sizeof(struct sce_sq_head) == 0x44, "SQ file header");

struct __attribute__((packed)) sce_sq_midi_block {
    uint32_t sequence_data_offset;
    uint16_t division;
};
_Static_assert(sizeof(struct sce_sq_midi_block) == 6, "SQ Midi data block");

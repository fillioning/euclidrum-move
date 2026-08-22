/*
 * Euclidrum — 8-Lane Generative Euclidean Drum Sequencer for Schwung
 *
 * Purpose-built to drive Weird Dreams (8-voice drum synth) on Ableton Move.
 * Based on Eucalypso's Euclidean engine, stripped of melodic machinery and
 * rebuilt for autonomous generative drum patterns.
 *
 * Architecture:
 *   - 8 lanes, each hardwired to MIDI notes 36-43 (Weird Dreams voices 0-7)
 *   - Transport-triggered: fires on play, no held notes required
 *   - Per-lane rate divisor for polymetric patterns
 *   - Per-lane accent, fill, drop probabilities
 *   - Global pattern mutation/drift evolving patterns over time
 *   - Per-lane freq (CC 70-77) and decay (CC 80-87) output
 *   - Live MIDI pass-through for manual layering
 *
 * CC mapping (Euclidrum → Weird Dreams):
 *   CC 70-77  →  v1_freq through v8_freq  (0-127 → 20-20000 Hz exp)
 *   CC 80-87  →  v1_decay through v8_decay (0-127 → 0.0001-4.0s exp)
 *
 * License: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "host/plugin_api_v1.h"
#include "host/midi_fx_api_v1.h"

/* ── Constants ── */
#define MAX_LANES           8
#define MAX_VOICES          64
#define DEFAULT_BPM         120
#define DEFAULT_SAMPLE_RATE 44100
#define DRUM_NOTE_BASE      36      /* C2 = voice 0 .. G#2 = voice 7 */
#define CC_FREQ_BASE        70      /* CC 70-77 = v1_freq .. v8_freq */
#define CC_DECAY_BASE       80      /* CC 80-87 = v1_decay .. v8_decay */

#define EUCLIDRUM_DEBUG_LOG 0
#define EUCLIDRUM_LOG_PATH  "/data/UserData/schwung/euclidrum.log"

/* ── Enums ── */
typedef enum { SYNC_INTERNAL = 0, SYNC_CLOCK } sync_mode_t;

typedef enum {
    RATE_1_32 = 0, RATE_1_16T, RATE_1_16, RATE_1_8T, RATE_1_8,
    RATE_1_4T, RATE_1_4, RATE_1_2, RATE_1_1
} rate_t;

typedef enum {
    LANE_RATE_X1 = 0,   /* same as global */
    LANE_RATE_X2,       /* double speed */
    LANE_RATE_X4,       /* quadruple speed */
    LANE_RATE_D2,       /* half speed */
    LANE_RATE_D4,       /* quarter speed */
    LANE_RATE_X3,       /* triplet (3x) */
    LANE_RATE_D3        /* 1/3 speed */
} lane_rate_t;

/* ── Lane ── */
typedef struct {
    int enabled;
    int steps;          /* 1-64 */
    int pulses;         /* 0-steps */
    int rotation;       /* 0-63 */
    lane_rate_t rate;   /* per-lane rate multiplier */
    int drop;           /* drop probability 0-100 */
    int drop_seed;
    int velocity;       /* 0 = use global, 1-127 = override */
    int accent;         /* accent probability 0-100 */
    int accent_amt;     /* accent velocity boost 0-64 */
    int fill;           /* fill probability 0-100 — extra hits between euclidean pulses */
    int fill_seed;
    int gate;           /* 0 = use global, 1-1600 = override */
    int freq;           /* base freq CC value 0-127 (64 = no change) */
    int freq_rnd;       /* freq random deviation 0-64 */
    int freq_seed;
    int decay;          /* base decay CC value 0-127 (64 = no change) */
    int decay_rnd;      /* decay random deviation 0-64 */
    int decay_seed;
} lane_t;

/* ── Preset: compact lane snapshot ── */
/* en=enabled, st=steps, pu=pulses, ro=rotation, rt=lane_rate,
   dr=drop%, ac=accent%, fi=fill%, vel=velocity, fq=freq CC, dk=decay CC */
typedef struct {
    uint8_t en, st, pu, ro, rt, dr, ac, fi, vel, fq, dk;
} preset_lane_t;

typedef struct {
    const char *name;
    preset_lane_t lanes[MAX_LANES];
    int vel_rnd;    /* global velocity randomization */
    int swing;      /* global swing */
} preset_t;

#define NUM_PRESETS 32

/* ── Instance ── */
typedef struct {
    sync_mode_t sync_mode;
    rate_t rate;
    int bpm;
    int swing;
    int max_voices;
    int global_velocity;
    int global_gate;
    int global_rnd_seed;
    int rand_cycle;
    int mutation;       /* 0-100: probability of pulse/rotation shift per cycle */
    int mutation_seed;
    int global_vel_rnd; /* 0-64: random velocity deviation */
    int passthrough;    /* 1 = forward incoming notes alongside generated */
    int current_preset; /* 0-31 */
    lane_t lanes[MAX_LANES];

    /* Timing — internal sync */
    int sample_rate;
    int timing_dirty;
    double step_interval_base_f;
    double samples_until_step_f;
    uint64_t internal_sample_total;
    int swing_phase;

    /* Timing — external clock sync */
    int clock_counter;
    int clocks_per_step;
    int clock_running;
    int midi_transport_started;
    uint64_t clock_tick_total;
    int pending_step_triggers;

    /* Step tracking */
    uint64_t anchor_step;
    uint64_t mutation_cycle_count;

    /* Voice tracking (for gate-off scheduling) */
    uint8_t voice_notes[MAX_VOICES];
    int voice_clock_left[MAX_VOICES];
    int voice_sample_left[MAX_VOICES];
    int voice_count;

    /* UI state */
    int current_level;      /* 0=root, 1=global, 2-9=lane1-lane8 */

    /* Debug */
    FILE *debug_fp;
    uint64_t debug_seq;

    /* Chain params cache (generated programmatically) */
    char chain_params_json[16384];
    int chain_params_len;
} euclidrum_instance_t;

static const host_api_v1_t *g_host = NULL;

/* ════════════════════════════════════════════════════════════════════════════
 * Presets — 32 rhythm patterns
 *
 * Lane order: Kick, Snare, HH Cls, HH Opn, Tom Lo, Tom Hi, Perc, FX
 * Fields: en, st, pu, ro, rt(lane_rate), dr, ac%, fi%, vel, fq(CC), dk(CC)
 * fq/dk: 64 = no change, <64 = lower, >64 = higher
 * rt: 0=x1, 1=x2, 2=x4, 3=/2, 4=/4, 5=x3, 6=/3
 * ════════════════════════════════════════════════════════════════════════════ */
static const preset_t g_presets[NUM_PRESETS] = {
    /* 0: Init — blank slate */
    {"Init", {
        {1,16,4,0, 0, 0, 0, 0, 100, 64,64},
        {1,16,2,4, 0, 0, 0, 0, 100, 64,64},
        {1,16,8,0, 0, 0, 0, 0,  85, 64,64},
        {1,16,3,2, 0, 0, 0, 0,  70, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 0, 0},

    /* 1: Four on Floor — classic house/techno */
    {"4 Floor", {
        {1,16,4,0, 0, 0, 0, 0, 110, 64,64}, /* kick: 4/4 */
        {1,16,4,4, 0, 0,30, 0, 100, 64,64}, /* snare: backbeat */
        {1,16,8,0, 0, 0, 0, 0,  85, 64,48}, /* hh cls: 8ths */
        {1,16,2,2, 0, 0, 0, 0,  70, 64,64}, /* hh opn: offbeat */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,3,0, 0,30, 0, 0,  80, 64,64}, /* perc: ghost */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 5, 0},

    /* 2: Boom Bap — hip hop */
    {"BoomBap", {
        {1,16,3,0, 0, 0,40, 0, 120, 64,80}, /* kick: heavy */
        {1,16,2,4, 0, 0,50, 0, 115, 64,64}, /* snare */
        {1,16,6,0, 1, 0, 0,15,  80, 64,48}, /* hh: x2 shuffle */
        {1,16,1,6, 0,40, 0, 0,  65, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,2,3, 0,50, 0,10,  70, 64,50}, /* perc: shaker */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 12, 55},

    /* 2: Trap 808 */
    {"Trap808", {
        {1,16,2,0, 0, 0,60, 0, 127, 64,80}, /* kick: sub bass */
        {1,16,2,4, 0, 0,40, 0, 110, 64, 64}, /* snare */
        {1,32,12,0,2, 0, 0,25,  75, 64,48}, /* hh: x4 rolls */
        {1,16,1,8, 0,60, 0, 0,  60, 64, 64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {1,16,1,0, 3,70, 0, 0,  90, 64, 64}, /* fx: sparse /2 */
    }, 8, 0},

    /* 3: Minimal Techno */
    {"MinTech", {
        {1,16,4,0, 0, 0, 0, 0, 100, 64,64}, /* kick: steady */
        {1,32,3,5, 0,25, 0, 0,  90, 64,50}, /* snare: sparse */
        {1,16,6,1, 0, 0,20, 0,  70, 64,48}, /* hh */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,24,3,7, 0,40, 0, 0,  80, 64,70}, /* tom lo: polymetric */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,2,3, 0,50, 0,10,  60, 64,48}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 3, 0},

    /* 4: Breakbeat */
    {"Break", {
        {1,16,5,0, 0, 0,35, 0, 110, 64,70}, /* kick: syncopated */
        {1,16,3,2, 0, 0,50,10, 105, 64,64}, /* snare */
        {1,16,7,1, 0,10,15, 0,  80, 64,48}, /* hh */
        {1,16,2,5, 0,30, 0, 0,  65, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,4,3, 0,20,25, 0,  75, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 10, 40},

    /* 5: Dub Techno */
    {"DubTech", {
        {1,16,3,0, 0, 0, 0, 0, 100, 64,80}, /* kick: long decay */
        {1,16,2,4, 0,20, 0, 0,  85, 64,80}, /* snare: dub */
        {1,16,5,2, 0,15, 0, 0,  60, 64,48}, /* hh */
        {1,16,1,6, 0,50, 0, 0,  50, 64,80}, /* hh opn: reverb */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,12,2,1, 0,40, 0, 0,  55, 64,70}, /* perc: 12-step */
        {1,16,1,0, 3,60, 0, 0,  70, 64,80},/* fx: /2 wash */
    }, 8, 30},

    /* 6: Afrobeat */
    {"Afro", {
        {1,16,5,0, 0, 0,20, 0, 105, 64,64}, /* kick */
        {1,16,3,4, 0, 0,40, 0, 100, 64,64}, /* snare */
        {1,12,7,0, 0, 0,10, 0,  80, 64,48}, /* hh: 12/8 feel */
        {1,12,3,3, 0,20, 0, 0,  65, 64,64}, /* hh opn */
        {1,16,4,2, 0,15,30, 0,  90, 64,60}, /* tom lo */
        {1,16,3,5, 0,20,25, 0,  85, 64,55}, /* tom hi */
        {1,12,5,1, 0,10,20, 0,  75, 64,48}, /* perc: bell */
        {1,16,2,7, 0,30, 0, 0,  70, 64,48}, /* fx: shaker */
    }, 6, 20},

    /* 7: Industrial */
    {"Indstrl", {
        {1,16,6,0, 0, 0,50, 0, 127, 64,80}, /* kick: distorted */
        {1,16,4,2, 0, 0,60, 0, 120, 64, 80}, /* snare: harsh */
        {1,16,8,0, 1, 0,20,20,  90, 64,48}, /* hh: x2 */
        {1,16,3,5, 0,20,30, 0,  80, 64, 64}, /* hh opn */
        {1, 8,3,1, 0,30,40, 0, 110, 64,80}, /* tom: 8-step */
        {1,12,2,4, 0,40,35, 0, 100, 64,80}, /* tom hi */
        {1,16,5,3, 0,15,45, 0,  95, 64,48}, /* perc: metallic */
        {1, 6,2,0, 0,50,50, 0, 115, 64,80}, /* fx: 6-step noise */
    }, 15, 10},

    /* 8: Reggaeton */
    {"Reggton", {
        {1,16,3,0, 0, 0,30, 0, 115, 64,75}, /* kick */
        {1,16,4,2, 0, 0,40, 0, 105, 64,64}, /* snare: dembow */
        {1,16,6,1, 0, 0, 0, 0,  75, 64,48}, /* hh */
        {1,16,2,4, 0,20, 0, 0,  60, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,3,5, 0,30,20, 0,  80, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 4, 35},

    /* 9: Polyrhythm */
    {"Poly", {
        {1,16,4,0, 0, 0, 0, 0, 100, 64,64}, /* kick: 4/4 */
        {1,12,3,0, 0, 0,30, 0,  95, 64,64}, /* snare: 12-step */
        {1, 7,3,0, 0, 0,15, 0,  80, 64,48}, /* hh: 7-step */
        {1, 5,2,0, 0,20, 0, 0,  65, 64,64}, /* hh opn: 5-step */
        {1, 9,4,0, 0,10,20, 0,  85, 64,60}, /* tom: 9-step */
        {1,11,3,0, 0,15,25, 0,  80, 64,55}, /* tom: 11-step */
        {1,13,5,0, 0, 5,10, 0,  75, 64,48}, /* perc: 13-step */
        {1, 3,1,0, 0,30, 0, 0,  70, 64,80}, /* fx: 3-step */
    }, 5, 0},

    /* 10: Sparse — generative ambient */
    {"Sparse", {
        {1,32,2,0, 3, 0, 0, 0,  80, 64,80}, /* kick: /2 sparse */
        {1,24,1,7, 0,50, 0, 0,  70, 64,80}, /* snare */
        {1,16,3,2, 0,40, 0,10,  50, 64,48}, /* hh */
        {1,32,1,11,3,60, 0, 0,  45, 64,80}, /* hh opn */
        {1,20,1,5, 0,70, 0, 0,  60, 64,80}, /* tom */
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {1,16,2,9, 0,55, 0, 5,  55, 64,60}, /* perc */
        {1, 7,1,0, 4,40, 0, 0,  65, 64,80}, /* fx: /4 */
    }, 20, 15},

    /* 11: Drum & Bass */
    {"DnB", {
        {1,16,3,0, 0, 0,45, 0, 115, 64,70}, /* kick */
        {1,16,4,2, 0, 0,50,10, 110, 64,55}, /* snare: fast */
        {1,16,8,0, 1, 0,10,15,  85, 64,48}, /* hh: x2 */
        {1,16,2,5, 0,25, 0, 0,  65, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,5,3, 0,20,30, 0,  80, 64,48}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 8, 25},

    /* 12: Samba */
    {"Samba", {
        {1,16,4,0, 0, 0,20, 0, 100, 64,65}, /* kick: surdo */
        {1,16,5,2, 0,10,30, 0,  90, 64,50}, /* snare: caixa */
        {1,16,8,0, 0, 0,15, 0,  70, 64,48}, /* hh: ganza */
        {1,16,3,4, 0,20, 0, 0,  60, 64,64}, /* hh opn */
        {1,16,5,1, 0, 5,25, 0,  85, 64,60}, /* tom: repinique */
        {1,16,4,3, 0,10,20, 0,  80, 64,55}, /* tom: tamborim */
        {1,16,7,2, 0, 0,10, 0,  75, 64,48}, /* perc: agogo */
        {1,16,3,5, 0,25,15, 0,  70, 64,48},/* fx: cuica */
    }, 4, 30},

    /* 13: Electro */
    {"Electro", {
        {1,16,3,0, 0, 0,50, 0, 120, 64,80}, /* kick: 808 */
        {1,16,4,4, 0, 0,40,10, 110, 64,60}, /* snare: clap */
        {1,16,6,0, 0, 0,10, 0,  80, 64,48}, /* hh */
        {1,16,2,6, 0,30, 0, 0,  65, 64,64}, /* hh opn */
        {1,16,2,1, 0,40,30, 0,  95, 64,75}, /* tom */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,3,3, 0,20,25, 0,  85, 64,48}, /* perc: cowbell */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 6, 0},

    /* 14: Waltz (3/4) */
    {"Waltz", {
        {1,12,3,0, 0, 0, 0, 0, 100, 64,70}, /* kick: 3/4 */
        {1,12,1,4, 0, 0,30, 0,  90, 64,64}, /* snare */
        {1,12,6,0, 0, 0,10, 0,  70, 64,48}, /* hh */
        {1,12,2,3, 0,30, 0, 0,  55, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,12,4,1, 0,20,15, 0,  75, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 3, 20},

    /* 15: Gabber */
    {"Gabber", {
        {1,16,8,0, 0, 0,60, 0, 127, 64,80}, /* kick: every 8th */
        {1,16,4,4, 0, 0,50, 0, 120, 64, 70}, /* snare */
        {1,16,8,0, 1, 0,30,20, 100, 64,48}, /* hh: x2 relentless */
        {1,16,4,2, 0,15,20, 0,  85, 64, 64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {1, 8,3,1, 0,25,40, 0, 110, 64,60}, /* perc: stab */
        {1,16,2,0, 0,50,55, 0, 115, 64,80}, /* fx: noise */
    }, 10, 0},

    /* 16: Bossa Nova */
    {"Bossa", {
        {1,16,3,0, 0, 0, 0, 0,  90, 64,70}, /* kick */
        {1,16,5,3, 0,10,20, 0,  80, 64,55}, /* snare: brush */
        {1,16,8,0, 0, 0, 5, 0,  55, 64,48}, /* hh: soft */
        {1,16,2,5, 0,30, 0, 0,  45, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,5,1, 0,15,10, 0,  65, 64,48}, /* perc: rim */
        {1,16,3,4, 0,25, 0, 0,  60, 64,48},/* fx: shaker */
    }, 8, 45},

    /* 17: UK Garage */
    {"UKGarage", {
        {1,16,3,0, 0, 0,25, 0, 110, 64,75}, /* kick */
        {1,16,4,4, 0, 0,35,15, 100, 64,60}, /* snare: skippy */
        {1,16,7,1, 1, 0,10,10,  75, 64,48}, /* hh: x2 shuffle */
        {1,16,2,6, 0,20, 0, 0,  60, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,3,3, 0,35,20, 0,  80, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 6, 50},

    /* 18: Acid */
    {"Acid", {
        {1,16,4,0, 0, 0,40, 0, 115, 64,80}, /* kick: 909 */
        {1,16,4,4, 0,10,30, 0, 100, 64,64}, /* snare */
        {1,16,6,0, 0, 0,15,10,  80, 64,48}, /* hh */
        {1,16,3,3, 0,20,10, 0,  65, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,5,2, 0,15,25, 0,  85, 64,48}, /* perc: cowbell */
        {1,16,2,7, 0,40,20, 0,  70, 64,70}, /* fx: clav */
    }, 5, 15},

    /* 19: Half Time */
    {"HalfTm", {
        {1,16,2,0, 0, 0,30, 0, 115, 64,80}, /* kick: half */
        {1,16,1,8, 0, 0,50, 0, 110, 64,64}, /* snare: beat 3 */
        {1,16,4,0, 0, 0, 0,10,  75, 64,48}, /* hh */
        {1,16,1,4, 0,40, 0, 0,  55, 64,70}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,3,2, 0,30,15, 0,  70, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 10, 20},

    /* 20: Funk */
    {"Funk", {
        {1,16,5,0, 0, 0,30, 0, 110, 64,65}, /* kick: syncopated */
        {1,16,4,4, 0, 0,45,15, 105, 64,55}, /* snare: ghost notes */
        {1,16,8,0, 0, 0,10, 5,  80, 64,48}, /* hh */
        {1,16,2,5, 0,15, 0, 0,  60, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,6,1, 0,10,20,10,  75, 64,48}, /* perc: clav */
        {1,16,3,3, 0,30,15, 0,  70, 64,48}, /* fx: cowbell */
    }, 8, 35},

    /* 21: Shuffle */
    {"Shuffle", {
        {1,12,4,0, 0, 0,20, 0, 105, 64,70}, /* kick */
        {1,12,3,3, 0, 0,35, 0,  95, 64,60}, /* snare */
        {1,12,8,0, 0, 0,10, 0,  75, 64,48}, /* hh: triplet */
        {1,12,2,4, 0,25, 0, 0,  60, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,12,5,1, 0,15,20, 0,  80, 64,48}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 5, 55},

    /* 22: Glitch */
    {"Glitch", {
        {1,16,5,0, 0,30,40,20, 100, 64,60}, /* kick: chaotic */
        {1,14,4,3, 0,25,35,25,  90, 64,50}, /* snare: odd length */
        {1,16,7,2, 2,15,20,30,  70, 64,48}, /* hh: x4 fills */
        {1,10,3,1, 0,35,15,15,  60, 64,64}, /* hh opn: 10-step */
        {1, 6,2,0, 0,40,30,20,  85, 64,70}, /* tom: 6-step */
        {1, 9,3,4, 0,45,25,15,  80, 64,55}, /* tom: 9-step */
        {1,11,5,2, 0,20,30,25,  75, 64,48}, /* perc: 11-step */
        {1, 5,2,1, 0,50,40,20,  90, 64,80}, /* fx: 5-step */
    }, 20, 10},

    /* 23: Dub */
    {"Dub", {
        {1,16,3,0, 0, 0,20, 0, 100, 64,80}, /* kick: deep */
        {1,16,2,8, 0,20,30, 0,  85, 64,80}, /* snare: rimshot */
        {1,16,5,1, 0,15, 0, 0,  55, 64,48}, /* hh */
        {1,16,1,5, 0,45, 0, 0,  45, 64,80}, /* hh opn: delay */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,3,3, 0,35,15, 0,  65, 64,55}, /* perc: woodblock */
        {1,16,1,7, 3,50, 0, 0,  70, 64,80},/* fx: /2 echo */
    }, 10, 40},

    /* 24: Lo-Fi */
    {"Lo-Fi", {
        {1,16,3,0, 0,15,25, 0,  95, 64,80}, /* kick: dusty */
        {1,16,2,4, 0,10,35, 0,  85, 64,65}, /* snare */
        {1,16,5,2, 0,20, 5,10,  60, 64,48}, /* hh: crackle */
        {1,16,1,6, 0,40, 0, 0,  50, 64,64}, /* hh opn */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,3,1, 0,30,15,10,  70, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 15, 50},

    /* 25: Machine — relentless 8th note grid */
    {"Machine", {
        {1,16,8,0, 0, 0,30, 0, 110, 64,60}, /* kick: 8ths */
        {1,16,8,4, 0, 0,25, 0, 100, 64,55}, /* snare: 8ths offset */
        {1,16,16,0,0, 0,10, 0,  85, 64,48}, /* hh: 16ths */
        {1,16,4,2, 0,20,15, 0,  70, 64,64}, /* hh opn */
        {1,16,8,1, 0,10,20, 0,  90, 64,50}, /* tom */
        {1,16,8,5, 0,15,20, 0,  85, 64,48}, /* tom */
        {1,16,8,3, 0,20,25, 0,  80, 64,48}, /* perc */
        {1,16,8,7, 0,25,30, 0,  75, 64,64}, /* fx */
    }, 0, 0},

    /* 26: Ritual */
    {"Ritual", {
        {1,16,3,0, 3, 0,20, 0, 105, 64,80}, /* kick: /2 deep */
        {1, 7,2,0, 0,20,30, 0,  90, 64,70}, /* snare: 7-step */
        {1,16,5,3, 0,10,10, 0,  65, 64,48}, /* hh */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,11,4,0, 0,15,25, 0,  85, 64,65}, /* tom: 11-step */
        {1, 9,3,2, 0,20,20, 0,  80, 64,60}, /* tom: 9-step */
        {1,13,6,1, 0, 5,15, 0,  75, 64,48}, /* perc: 13-step */
        {1, 5,2,0, 0,35,10, 0,  70, 64,55},/* fx: 5-step */
    }, 8, 10},

    /* 27: Pulse — minimal pulse */
    {"Pulse", {
        {1,16,4,0, 0, 0, 0, 0,  95, 64,50}, /* kick: 4/4 */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,4,0, 0, 0, 0, 0,  60, 64,48}, /* hh: sync */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,4,2, 0,40, 0, 0,  75, 64,60}, /* tom */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1,16,4,4, 0,50, 0, 0,  65, 64,48}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 2, 0},

    /* 28: Chaos — maximum generative */
    {"Chaos", {
        {1,16,5,0, 0,20,50,30, 100, 64,70}, /* kick */
        {1,14,4,3, 0,25,45,25,  95, 64,60}, /* snare */
        {1,11,6,2, 1,15,30,35,  80, 64,48}, /* hh: x2 */
        {1, 9,3,1, 0,30,25,20,  65, 64,64}, /* hh opn */
        {1, 7,3,0, 0,35,40,25,  85, 64,75}, /* tom */
        {1,13,4,4, 0,30,35,20,  80, 64,55}, /* tom */
        {1, 5,2,0, 0,40,45,30,  75, 64,48}, /* perc */
        {1, 3,1,0, 0,50,50,25,  90, 64,80}, /* fx */
    }, 25, 15},

    /* 29: Tresillo — 3+3+2 pattern */
    {"Tresllo", {
        {1, 8,3,0, 0, 0,25, 0, 110, 64,70}, /* kick: 3+3+2 */
        {1,16,2,4, 0, 0,35, 0, 100, 64,60}, /* snare */
        {1,16,6,0, 0, 0,10, 0,  75, 64,48}, /* hh */
        {1,16,2,5, 0,25, 0, 0,  60, 64,64}, /* hh opn */
        {1, 8,3,2, 0,20,20, 0,  85, 64,65}, /* tom: offset tresillo */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
        {1, 8,3,4, 0,15,15, 0,  80, 64,50}, /* perc */
        {0,16,0,0, 0, 0, 0, 0,   0, 64,64},
    }, 5, 25},

    /* 30: Zen — meditative */
    {"Zen", {
        {1,32,1,0, 3, 0, 0, 0,  70, 64,80}, /* kick: rare, deep */
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {1,16,2,5, 0,60, 0, 0,  40, 64,48}, /* hh: rare taps */
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {1,24,1,7, 0,70, 0, 0,  50, 64,80}, /* tom: very rare */
        {0,16,0,0, 0, 0, 0, 0,   0, 64, 64},
        {1,20,2,3, 0,55, 0, 5,  45, 64,70}, /* perc: bell */
        {1, 7,1,0, 4,50, 0, 0,  55, 64,80},/* fx: rare wash */
    }, 15, 0},

};

/* Forward declaration */
static int clamp_int(int v, int lo, int hi);

/* ════════════════════════════════════════════════════════════════════════════
 * Preset loading
 * ════════════════════════════════════════════════════════════════════════════ */

static void load_preset(euclidrum_instance_t *inst, int idx) {
    int i;
    const preset_t *p;
    if (!inst || idx < 0 || idx >= NUM_PRESETS) return;
    p = &g_presets[idx];
    inst->current_preset = idx;
    inst->global_vel_rnd = p->vel_rnd;
    inst->swing = p->swing;
    for (i = 0; i < MAX_LANES; i++) {
        const preset_lane_t *pl = &p->lanes[i];
        lane_t *lane = &inst->lanes[i];
        lane->enabled   = pl->en;
        lane->steps     = clamp_int(pl->st, 1, 64);
        lane->pulses    = clamp_int(pl->pu, 0, lane->steps);
        lane->rotation  = clamp_int(pl->ro, 0, 63);
        lane->rate      = (lane_rate_t)clamp_int(pl->rt, 0, (int)LANE_RATE_D3);
        lane->drop      = clamp_int(pl->dr, 0, 100);
        lane->accent    = clamp_int(pl->ac, 0, 100);
        lane->fill      = clamp_int(pl->fi, 0, 100);
        lane->velocity  = clamp_int(pl->vel, 0, 127);
        lane->freq      = clamp_int(pl->fq, 0, 127);
        lane->decay     = clamp_int(pl->dk, 0, 127);
        /* Keep existing seeds, accent_amt, freq_rnd, decay_rnd, gate */
    }
}

/* Generate a completely random preset using seeded LCG */
static void generate_random_preset(euclidrum_instance_t *inst) {
    int i;
    uint32_t rng;
    if (!inst) return;
    /* Seed from current RNG state so each turn gives a new result */
    rng = (uint32_t)(inst->global_rnd_seed + 1) * 2654435761u + (uint32_t)inst->current_preset;
    inst->current_preset = (inst->current_preset + 1) % NUM_PRESETS;

    /* Global params */
    rng = rng * 1664525u + 1013904223u;
    inst->swing = (int)(rng % 61u);              /* 0-60 */
    rng = rng * 1664525u + 1013904223u;
    inst->global_vel_rnd = (int)(rng % 25u);     /* 0-24 */

    for (i = 0; i < MAX_LANES; i++) {
        lane_t *lane = &inst->lanes[i];

        rng = rng * 1664525u + 1013904223u;
        /* First 4 lanes more likely enabled, last 4 less likely */
        lane->enabled = (i < 4) ? ((rng % 100u) < 85u) : ((rng % 100u) < 40u);

        rng = rng * 1664525u + 1013904223u;
        /* Steps: bias toward common values (8,12,16,24,32) */
        {
            static const int common_steps[] = {4,6,7,8,10,12,14,16,20,24,28,32,48,64};
            lane->steps = common_steps[rng % 14u];
        }

        rng = rng * 1664525u + 1013904223u;
        lane->pulses = (int)(rng % (uint32_t)(lane->steps + 1));

        rng = rng * 1664525u + 1013904223u;
        lane->rotation = (int)(rng % (uint32_t)lane->steps);

        rng = rng * 1664525u + 1013904223u;
        lane->rate = (lane_rate_t)(rng % 7u);

        rng = rng * 1664525u + 1013904223u;
        lane->drop = (int)(rng % 51u);           /* 0-50 */

        rng = rng * 1664525u + 1013904223u;
        lane->accent = (int)(rng % 71u);         /* 0-70 */

        rng = rng * 1664525u + 1013904223u;
        lane->fill = (int)(rng % 41u);           /* 0-40 */

        rng = rng * 1664525u + 1013904223u;
        lane->velocity = 60 + (int)(rng % 68u);  /* 60-127 */

        rng = rng * 1664525u + 1013904223u;
        lane->freq = 32 + (int)(rng % 65u);      /* 32-96 (centered) */

        rng = rng * 1664525u + 1013904223u;
        lane->decay = 32 + (int)(rng % 65u);     /* 32-96 (centered) */
    }

    /* Advance the global seed so next random preset is different */
    inst->global_rnd_seed = (inst->global_rnd_seed + 1) & 0xFFFF;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Utility
 * ════════════════════════════════════════════════════════════════════════════ */

#if EUCLIDRUM_DEBUG_LOG
static void dlog(euclidrum_instance_t *inst, const char *fmt, ...) {
    va_list ap;
    if (!inst) return;
    if (!inst->debug_fp) {
        inst->debug_fp = fopen(EUCLIDRUM_LOG_PATH, "a");
        if (!inst->debug_fp) return;
        setvbuf(inst->debug_fp, NULL, _IOLBF, 0);
    }
    fprintf(inst->debug_fp, "[%llu] ", (unsigned long long)inst->debug_seq++);
    va_start(ap, fmt);
    vfprintf(inst->debug_fp, fmt, ap);
    va_end(ap);
    fputc('\n', inst->debug_fp);
}
#else
static void dlog(euclidrum_instance_t *inst, const char *fmt, ...) {
    (void)inst; (void)fmt;
}
#endif

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* ── Deterministic seeded random ── */
static uint32_t mix_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static uint32_t step_rand_u32(uint32_t seed, uint64_t step, uint32_t salt) {
    uint32_t lo = (uint32_t)(step & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)((step >> 32) & 0xFFFFFFFFu);
    uint32_t s = seed ? seed : 1u;
    return mix_u32(s ^ lo ^ mix_u32(hi ^ salt) ^ salt);
}

static int rand_offset_signed(uint32_t r, int amount) {
    int span;
    if (amount <= 0) return 0;
    span = amount * 2 + 1;
    return (int)(r % (uint32_t)span) - amount;
}

static int chance_hit(uint32_t r, int pct) {
    pct = clamp_int(pct, 0, 100);
    if (pct <= 0) return 0;
    if (pct >= 100) return 1;
    return (int)(r % 100u) < pct;
}

static uint64_t rand_cycle_step(const euclidrum_instance_t *inst, uint64_t step) {
    int cycle;
    if (!inst) return step;
    cycle = clamp_int(inst->rand_cycle, 1, 128);
    return step % (uint64_t)cycle;
}

static uint32_t lane_seed(const euclidrum_instance_t *inst, int lane_idx, uint32_t offset) {
    uint32_t seed = 1u;
    if (inst) seed = (uint32_t)(inst->global_rnd_seed + 1);
    return seed + (uint32_t)((lane_idx + 1) * 1000) + offset;
}

/* ── MIDI emit helper ── */
static int emit3(uint8_t out_msgs[][3], int out_lens[], int max_out, int *count,
                 uint8_t s, uint8_t d1, uint8_t d2) {
    if (!out_msgs || !out_lens || !count || *count >= max_out) return 0;
    out_msgs[*count][0] = s;
    out_msgs[*count][1] = d1;
    out_msgs[*count][2] = d2;
    out_lens[*count] = 3;
    (*count)++;
    return 1;
}

/* ── JSON helpers ── */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64]; const char *pos, *colon, *end; int len;
    if (!json || !key || !out || out_len < 1) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search); if (!pos) return 0;
    colon = strchr(pos, ':'); if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0; colon++;
    end = strchr(colon, '"'); if (!end) return 0;
    len = (int)(end - colon); if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len); out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char search[64]; const char *pos, *colon;
    if (!json || !key || !out) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search); if (!pos) return 0;
    colon = strchr(pos, ':'); if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

static int appendf(char *buf, int buf_len, int *pos, const char *fmt, ...) {
    va_list ap; int wrote;
    if (!buf || !pos || *pos >= buf_len) return 0;
    va_start(ap, fmt);
    wrote = vsnprintf(buf + *pos, (size_t)(buf_len - *pos), fmt, ap);
    va_end(ap);
    if (wrote < 0 || *pos + wrote >= buf_len) { *pos = buf_len; return 0; }
    *pos += wrote;
    return 1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Rate / Timing
 * ════════════════════════════════════════════════════════════════════════════ */

static const char *rate_to_string(rate_t r) {
    switch (r) {
        case RATE_1_32: return "1/32"; case RATE_1_16T: return "1/16T";
        case RATE_1_16: return "1/16"; case RATE_1_8T: return "1/8T";
        case RATE_1_8: return "1/8";   case RATE_1_4T: return "1/4T";
        case RATE_1_4: return "1/4";   case RATE_1_2: return "1/2";
        case RATE_1_1: default: return "1";
    }
}

static rate_t parse_rate(const char *val) {
    int n;
    if (!val) return RATE_1_16;
    if (strcmp(val, "1/32") == 0) return RATE_1_32;
    if (strcmp(val, "1/16T") == 0) return RATE_1_16T;
    if (strcmp(val, "1/16") == 0) return RATE_1_16;
    if (strcmp(val, "1/8T") == 0) return RATE_1_8T;
    if (strcmp(val, "1/8") == 0) return RATE_1_8;
    if (strcmp(val, "1/4T") == 0) return RATE_1_4T;
    if (strcmp(val, "1/4") == 0) return RATE_1_4;
    if (strcmp(val, "1/2") == 0) return RATE_1_2;
    if (strcmp(val, "1") == 0) return RATE_1_1;
    /* Numeric fallback */
    n = atoi(val);
    if (n >= 0 && n <= (int)RATE_1_1) return (rate_t)n;
    return RATE_1_16;
}

static double rate_notes_per_beat(rate_t r) {
    switch (r) {
        case RATE_1_32: return 8.0;  case RATE_1_16T: return 6.0;
        case RATE_1_16: return 4.0;  case RATE_1_8T: return 3.0;
        case RATE_1_8: return 2.0;   case RATE_1_4T: return 1.5;
        case RATE_1_4: return 1.0;   case RATE_1_2: return 0.5;
        case RATE_1_1: default: return 0.25;
    }
}

static const char *lane_rate_to_string(lane_rate_t r) {
    switch (r) {
        case LANE_RATE_X1: return "x1"; case LANE_RATE_X2: return "x2";
        case LANE_RATE_X4: return "x4"; case LANE_RATE_D2: return "/2";
        case LANE_RATE_D4: return "/4"; case LANE_RATE_X3: return "x3";
        case LANE_RATE_D3: return "/3"; default: return "x1";
    }
}

static lane_rate_t parse_lane_rate(const char *val) {
    int n;
    if (!val) return LANE_RATE_X1;
    if (strcmp(val, "x1") == 0) return LANE_RATE_X1;
    if (strcmp(val, "x2") == 0) return LANE_RATE_X2;
    if (strcmp(val, "x4") == 0) return LANE_RATE_X4;
    if (strcmp(val, "/2") == 0) return LANE_RATE_D2;
    if (strcmp(val, "/4") == 0) return LANE_RATE_D4;
    if (strcmp(val, "x3") == 0) return LANE_RATE_X3;
    if (strcmp(val, "/3") == 0) return LANE_RATE_D3;
    /* Numeric fallback */
    n = atoi(val);
    if (n >= 0 && n <= (int)LANE_RATE_D3) return (lane_rate_t)n;
    return LANE_RATE_X1;
}

/* Convert global anchor step to lane-local step using lane rate multiplier */
static uint64_t lane_step(uint64_t anchor, lane_rate_t lr) {
    switch (lr) {
        case LANE_RATE_X2: return anchor * 2;
        case LANE_RATE_X4: return anchor * 4;
        case LANE_RATE_X3: return anchor * 3;
        case LANE_RATE_D2: return anchor / 2;
        case LANE_RATE_D4: return anchor / 4;
        case LANE_RATE_D3: return anchor / 3;
        case LANE_RATE_X1: default: return anchor;
    }
}

/* For divided rates, only fire on steps that align */
static int lane_step_active(uint64_t anchor, lane_rate_t lr) {
    switch (lr) {
        case LANE_RATE_D2: return (anchor % 2) == 0;
        case LANE_RATE_D4: return (anchor % 4) == 0;
        case LANE_RATE_D3: return (anchor % 3) == 0;
        default: return 1;  /* x1, x2, x3, x4 always active at global tick */
    }
}

/* For multiplied rates, how many sub-steps per global step */
static int lane_sub_steps(lane_rate_t lr) {
    switch (lr) {
        case LANE_RATE_X2: return 2;
        case LANE_RATE_X4: return 4;
        case LANE_RATE_X3: return 3;
        default: return 1;
    }
}

static void recalc_clock_timing(euclidrum_instance_t *inst) {
    double npb; int clocks;
    if (!inst) return;
    npb = rate_notes_per_beat(inst->rate);
    if (npb <= 0.0) npb = 4.0;
    clocks = (int)(24.0 / npb + 0.5);
    if (clocks < 1) clocks = 1;
    inst->clocks_per_step = clocks;
}

static void recalc_internal_timing(euclidrum_instance_t *inst, int sample_rate) {
    double npb, step_samples;
    if (!inst || sample_rate <= 0) return;
    inst->bpm = clamp_int(inst->bpm, 10, 500);
    npb = rate_notes_per_beat(inst->rate);
    if (npb <= 0.0) npb = 4.0;
    step_samples = ((double)sample_rate * 60.0) / ((double)inst->bpm * npb);
    if (step_samples < 1.0) step_samples = 1.0;
    inst->sample_rate = sample_rate;
    inst->step_interval_base_f = step_samples;
    if (inst->samples_until_step_f <= 0.0 || inst->samples_until_step_f > step_samples) {
        inst->samples_until_step_f = step_samples;
    }
    inst->timing_dirty = 0;
}

static double next_internal_interval(euclidrum_instance_t *inst) {
    double base, delta; int swing;
    if (!inst) return 1.0;
    base = inst->step_interval_base_f > 0.0 ? inst->step_interval_base_f : 1.0;
    swing = clamp_int(inst->swing, 0, 100);
    if (swing <= 0) return base;
    delta = (base * (double)swing) / 200.0;
    if (inst->swing_phase == 0) { inst->swing_phase = 1; return base + delta; }
    inst->swing_phase = 0;
    return (base - delta) < 1.0 ? 1.0 : (base - delta);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Voice management (gate-off scheduling)
 * ════════════════════════════════════════════════════════════════════════════ */

static void voice_remove_at(euclidrum_instance_t *inst, int idx) {
    int i;
    if (!inst || idx < 0 || idx >= inst->voice_count) return;
    for (i = idx; i < inst->voice_count - 1; i++) {
        inst->voice_notes[i] = inst->voice_notes[i + 1];
        inst->voice_clock_left[i] = inst->voice_clock_left[i + 1];
        inst->voice_sample_left[i] = inst->voice_sample_left[i + 1];
    }
    inst->voice_count--;
}

static int voice_note_off(euclidrum_instance_t *inst, int idx,
                          uint8_t out[][3], int lens[], int max, int *cnt) {
    uint8_t note;
    if (!inst || idx < 0 || idx >= inst->voice_count) return 0;
    note = inst->voice_notes[idx];
    if (!emit3(out, lens, max, cnt, 0x80, note, 0)) return 0;
    voice_remove_at(inst, idx);
    return 1;
}

static int flush_all_voices(euclidrum_instance_t *inst,
                            uint8_t out[][3], int lens[], int max, int *cnt) {
    int n = 0;
    if (!inst || !cnt) return 0;
    while (inst->voice_count > 0) {
        if (!voice_note_off(inst, 0, out, lens, max, cnt)) break;
        n++;
    }
    return n;
}

static int kill_voice_notes(euclidrum_instance_t *inst, uint8_t note,
                            uint8_t out[][3], int lens[], int max, int *cnt) {
    int i = 0, killed = 0;
    if (!inst || !cnt) return 0;
    while (i < inst->voice_count) {
        if (inst->voice_notes[i] == note) {
            if (!voice_note_off(inst, i, out, lens, max, cnt)) break;
            killed++;
        } else { i++; }
    }
    return killed;
}

static void voice_add(euclidrum_instance_t *inst, uint8_t note, int gate_pct) {
    int idx;
    if (!inst || inst->voice_count >= MAX_VOICES) return;
    idx = inst->voice_count++;
    inst->voice_notes[idx] = note;
    inst->voice_clock_left[idx] = 0;
    inst->voice_sample_left[idx] = 0;
    gate_pct = clamp_int(gate_pct, 0, 1600);
    if (inst->sync_mode == SYNC_CLOCK) {
        /* Use sample-based timers for fine gate resolution even in clock mode */
        /* Estimate step interval from BPM and rate */
        double npb = rate_notes_per_beat(inst->rate);
        int sr = inst->sample_rate > 0 ? inst->sample_rate : 44100;
        double step_samples = ((double)sr * 60.0) / ((double)inst->bpm * (npb > 0 ? npb : 4.0));
        int samples = (int)(step_samples * (double)gate_pct / 100.0);
        if (samples < 1) samples = 1;
        inst->voice_sample_left[idx] = samples;
    } else {
        int samples = (int)(inst->step_interval_base_f * (double)gate_pct / 100.0);
        if (samples < 1) samples = 1;
        inst->voice_sample_left[idx] = samples;
    }
}

static int advance_voice_timers_clock(euclidrum_instance_t *inst,
                                      uint8_t out[][3], int lens[], int max, int *cnt) {
    int i = 0, n = 0;
    if (!inst || !cnt) return 0;
    while (i < inst->voice_count) {
        if (inst->voice_clock_left[i] > 0) inst->voice_clock_left[i]--;
        if (inst->voice_clock_left[i] <= 0) {
            if (!voice_note_off(inst, i, out, lens, max, cnt)) break;
            n++;
        } else { i++; }
    }
    return n;
}

static int advance_voice_timers_samples(euclidrum_instance_t *inst, int frames,
                                        uint8_t out[][3], int lens[], int max, int *cnt) {
    int i = 0, n = 0;
    if (!inst || !cnt) return 0;
    while (i < inst->voice_count) {
        if (inst->voice_sample_left[i] > 0) inst->voice_sample_left[i] -= frames;
        if (inst->voice_sample_left[i] <= 0) {
            if (!voice_note_off(inst, i, out, lens, max, cnt)) break;
            n++;
        } else { i++; }
    }
    return n;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Euclidean algorithm
 * ════════════════════════════════════════════════════════════════════════════ */

static int euclidean_trigger(uint64_t step, int steps, int pulses, int rotation) {
    int pos;
    if (steps <= 0) return 0;
    pulses = clamp_int(pulses, 0, steps);
    if (pulses <= 0) return 0;
    if (pulses >= steps) return 1;
    pos = (int)(step % (uint64_t)steps);
    rotation %= steps;
    if (rotation < 0) rotation += steps;
    pos = (pos + rotation) % steps;
    return ((pos * pulses) % steps) < pulses;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Mutation / Drift
 *
 * Each rand_cycle boundary, there's a chance (mutation%) to shift a lane's
 * pulses ±1 or rotation ±1. This makes patterns slowly evolve without manual
 * tweaking. The changes are deterministic (seeded) so they're reproducible.
 * ════════════════════════════════════════════════════════════════════════════ */

static void maybe_mutate(euclidrum_instance_t *inst) {
    int i;
    uint32_t r;
    if (!inst || inst->mutation <= 0) return;
    for (i = 0; i < MAX_LANES; i++) {
        lane_t *lane = &inst->lanes[i];
        if (!lane->enabled) continue;
        r = step_rand_u32((uint32_t)(inst->mutation_seed + 1),
                          inst->mutation_cycle_count,
                          0x9000u + (uint32_t)i);
        if (!chance_hit(r, inst->mutation)) continue;
        /* Choose what to mutate: 50% pulses, 50% rotation */
        if ((r >> 8) & 1) {
            int delta = ((r >> 16) & 1) ? 1 : -1;
            lane->pulses = clamp_int(lane->pulses + delta, 0, lane->steps);
        } else {
            int delta = ((r >> 16) & 1) ? 1 : -1;
            lane->rotation = clamp_int(lane->rotation + delta, 0, lane->steps - 1);
        }
    }
    inst->mutation_cycle_count++;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Note scheduling
 * ════════════════════════════════════════════════════════════════════════════ */

static int schedule_drum_hit(euclidrum_instance_t *inst, int lane_idx,
                             int velocity, int gate_pct,
                             uint8_t out[][3], int lens[], int max, int *cnt) {
    uint8_t note;
    int voice_limit;
    if (!inst || !cnt) return 0;
    note = (uint8_t)(DRUM_NOTE_BASE + lane_idx);
    velocity = clamp_int(velocity, 1, 127);
    gate_pct = clamp_int(gate_pct, 0, 1600);
    voice_limit = clamp_int(inst->max_voices, 1, MAX_VOICES);

    /* Kill existing voice on same note */
    (void)kill_voice_notes(inst, note, out, lens, max, cnt);
    while (inst->voice_count >= voice_limit) {
        if (!voice_note_off(inst, 0, out, lens, max, cnt)) return 0;
    }
    /* Emit note-on */
    if (!emit3(out, lens, max, cnt, 0x90, note, (uint8_t)velocity)) return 0;
    if (gate_pct <= 0) {
        return emit3(out, lens, max, cnt, 0x80, note, 0);
    }
    voice_add(inst, note, gate_pct);
    return 1;
}

/* Emit CC for freq and/or decay before the note-on */
static void emit_lane_cc(euclidrum_instance_t *inst, const lane_t *lane, int lane_idx,
                         uint64_t rhythm_step,
                         uint8_t out[][3], int lens[], int max, int *cnt) {
    uint64_t cycle_step;
    if (!inst || !lane || !cnt) return;
    cycle_step = rand_cycle_step(inst, rhythm_step);

    /* Frequency CC */
    if (lane->freq != 64 || lane->freq_rnd > 0) {
        int val = lane->freq;
        if (lane->freq_rnd > 0) {
            uint32_t r = step_rand_u32((uint32_t)(lane->freq_seed + 1), cycle_step,
                                       0xA000u + (uint32_t)lane_idx);
            val += rand_offset_signed(r, lane->freq_rnd);
        }
        val = clamp_int(val, 0, 127);
        emit3(out, lens, max, cnt, 0xB0, (uint8_t)(CC_FREQ_BASE + lane_idx), (uint8_t)val);
    }

    /* Decay CC */
    if (lane->decay != 64 || lane->decay_rnd > 0) {
        int val = lane->decay;
        if (lane->decay_rnd > 0) {
            uint32_t r = step_rand_u32((uint32_t)(lane->decay_seed + 1), cycle_step,
                                       0xB000u + (uint32_t)lane_idx);
            val += rand_offset_signed(r, lane->decay_rnd);
        }
        val = clamp_int(val, 0, 127);
        emit3(out, lens, max, cnt, 0xB0, (uint8_t)(CC_DECAY_BASE + lane_idx), (uint8_t)val);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Step emission — core lane evaluation
 * ════════════════════════════════════════════════════════════════════════════ */

static int emit_lane_step(euclidrum_instance_t *inst, int lane_idx, uint64_t step_id,
                          uint8_t out[][3], int lens[], int max, int *cnt) {
    lane_t *lane;
    uint64_t cycle_step;
    int velocity, gate_pct;
    uint32_t r;

    if (!inst || !cnt || lane_idx < 0 || lane_idx >= MAX_LANES) return 0;
    lane = &inst->lanes[lane_idx];
    if (!lane->enabled) return 0;

    cycle_step = rand_cycle_step(inst, step_id);

    /* Check Euclidean trigger */
    int triggered = euclidean_trigger(step_id,
                                      clamp_int(lane->steps, 1, 64),
                                      clamp_int(lane->pulses, 0, 64),
                                      lane->rotation);

    /* Fill: chance to trigger even when Euclidean says no */
    if (!triggered && lane->fill > 0) {
        r = step_rand_u32(lane_seed(inst, lane_idx, 0x7000u) + (uint32_t)(lane->fill_seed + 1),
                          cycle_step, 0x7000u);
        if (chance_hit(r, lane->fill)) triggered = 1;
    }

    if (!triggered) return 0;

    /* Drop: chance to skip */
    if (lane->drop > 0) {
        r = step_rand_u32((uint32_t)(lane->drop_seed + 1), cycle_step,
                          0x1000u + (uint32_t)lane_idx);
        if (chance_hit(r, lane->drop)) return 0;
    }

    /* Velocity */
    velocity = lane->velocity > 0 ? lane->velocity : inst->global_velocity;
    velocity = clamp_int(velocity, 1, 127);

    /* Global velocity randomization */
    if (inst->global_vel_rnd > 0) {
        r = step_rand_u32(lane_seed(inst, lane_idx, 0xC000u), cycle_step, 0xC000u);
        velocity += rand_offset_signed(r, inst->global_vel_rnd);
        velocity = clamp_int(velocity, 1, 127);
    }

    /* Accent: chance to boost velocity */
    if (lane->accent > 0) {
        r = step_rand_u32(lane_seed(inst, lane_idx, 0x8000u), cycle_step, 0x8000u);
        if (chance_hit(r, lane->accent)) {
            velocity = clamp_int(velocity + clamp_int(lane->accent_amt, 0, 64), 1, 127);
        }
    }

    /* Gate */
    gate_pct = lane->gate > 0 ? lane->gate : inst->global_gate;
    gate_pct = clamp_int(gate_pct, 0, 1600);

    /* Emit CC (freq/decay) before note */
    emit_lane_cc(inst, lane, lane_idx, step_id, out, lens, max, cnt);

    /* Emit drum hit */
    return schedule_drum_hit(inst, lane_idx, velocity, gate_pct, out, lens, max, cnt);
}

/* Process one anchor step: evaluate all 8 lanes */
static int emit_anchor_step(euclidrum_instance_t *inst, uint64_t step_id,
                            uint8_t out[][3], int lens[], int max) {
    int count = 0, lane_idx, sub, nsubs;
    if (!inst || max < 1) return 0;

    /* Check if we've completed a rand_cycle — time to mutate */
    if (inst->rand_cycle > 0 && step_id > 0 && (step_id % (uint64_t)inst->rand_cycle) == 0) {
        maybe_mutate(inst);
    }

    for (lane_idx = 0; lane_idx < MAX_LANES && count < max; lane_idx++) {
        lane_t *lane = &inst->lanes[lane_idx];
        if (!lane->enabled) continue;

        nsubs = lane_sub_steps(lane->rate);
        if (nsubs <= 1) {
            /* x1, /2, /4, /3: evaluate at global step rate (possibly skipping) */
            int local_cnt = 0;
            uint64_t ls;
            if (!lane_step_active(step_id, lane->rate)) continue;
            ls = lane_step(step_id, lane->rate);
            (void)emit_lane_step(inst, lane_idx, ls, out + count, lens + count, max - count, &local_cnt);
            count += local_cnt;
        } else {
            /* x2, x3, x4: multiple sub-steps per global step */
            uint64_t base = step_id * (uint64_t)nsubs;
            for (sub = 0; sub < nsubs && count < max; sub++) {
                int local_cnt = 0;
                (void)emit_lane_step(inst, lane_idx, base + (uint64_t)sub,
                                     out + count, lens + count, max - count, &local_cnt);
                count += local_cnt;
            }
        }
    }

    dlog(inst, "step=%llu out=%d", (unsigned long long)step_id, count);
    return count;
}

static int run_anchor_step(euclidrum_instance_t *inst,
                           uint8_t out[][3], int lens[], int max) {
    int count;
    if (!inst || max < 1) return 0;
    count = emit_anchor_step(inst, inst->anchor_step, out, lens, max);
    inst->anchor_step++;
    return count;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Clock / Transport
 * ════════════════════════════════════════════════════════════════════════════ */

static int process_clock_tick(euclidrum_instance_t *inst,
                              uint8_t out[][3], int lens[], int max) {
    int count = 0;
    int threshold;
    if (!inst || max < 1) return 0;
    (void)advance_voice_timers_clock(inst, out, lens, max, &count);
    inst->clock_tick_total++;
    if (inst->clocks_per_step < 1) inst->clocks_per_step = 1;
    inst->clock_counter++;

    /* Swing: alternate step thresholds (even steps longer, odd steps shorter) */
    threshold = inst->clocks_per_step;
    if (inst->swing > 0 && inst->clocks_per_step >= 2) {
        int swing_ticks = (inst->clocks_per_step * inst->swing) / 200;
        if (inst->swing_phase == 0)
            threshold = inst->clocks_per_step + swing_ticks;  /* delay odd step */
        else
            threshold = inst->clocks_per_step - swing_ticks;  /* advance even step */
        if (threshold < 1) threshold = 1;
    }

    if (inst->clock_counter >= threshold) {
        inst->clock_counter = 0;
        inst->swing_phase = inst->swing_phase ? 0 : 1;
        inst->pending_step_triggers++;
    }
    return count;
}

static int handle_transport_stop(euclidrum_instance_t *inst,
                                 uint8_t out[][3], int lens[], int max) {
    int count = 0;
    if (!inst) return 0;
    (void)flush_all_voices(inst, out, lens, max, &count);
    inst->pending_step_triggers = 0;
    inst->clock_counter = 0;
    inst->clock_tick_total = 0;
    inst->anchor_step = 0;
    inst->internal_sample_total = 0;
    inst->samples_until_step_f = inst->step_interval_base_f > 0.0 ? inst->step_interval_base_f : 1.0;
    inst->swing_phase = 0;
    inst->clock_running = 0;    /* Wait for next transport start */
    inst->midi_transport_started = 0;
    inst->mutation_cycle_count = 0;
    return count;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Instance lifecycle
 * ════════════════════════════════════════════════════════════════════════════ */

static void apply_defaults(euclidrum_instance_t *inst) {
    int i;
    if (!inst) return;
    memset(inst, 0, sizeof(*inst));
    inst->sync_mode = SYNC_CLOCK;
    inst->rate = RATE_1_16;
    inst->bpm = DEFAULT_BPM;
    inst->swing = 0;
    inst->max_voices = 16;
    inst->global_velocity = 100;
    inst->global_gate = 80;
    inst->global_rnd_seed = 0;
    inst->rand_cycle = 16;
    inst->mutation = 0;
    inst->mutation_seed = 42;
    inst->global_vel_rnd = 0;
    inst->passthrough = 1;

    for (i = 0; i < MAX_LANES; i++) {
        lane_t *lane = &inst->lanes[i];
        lane->enabled = (i < 4) ? 1 : 0;  /* Enable kick/snare/hh/perc by default */
        lane->steps = 16;
        lane->pulses = (i == 0) ? 4 : (i == 1) ? 2 : (i == 2) ? 8 : 3;
        lane->rotation = 0;
        lane->rate = LANE_RATE_X1;
        lane->drop = 0;
        lane->drop_seed = 0;
        lane->velocity = 0;
        lane->accent = 0;
        lane->accent_amt = 20;
        lane->fill = 0;
        lane->fill_seed = 0;
        lane->gate = 0;
        lane->freq = 64;       /* 64 = "no change" / center */
        lane->freq_rnd = 0;
        lane->freq_seed = 0;
        lane->decay = 64;      /* 64 = "no change" / center */
        lane->decay_rnd = 0;
        lane->decay_seed = 0;
    }

    inst->timing_dirty = 1;
    inst->step_interval_base_f = 1.0;
    inst->samples_until_step_f = 1.0;
    inst->clock_running = 0;    /* Wait for transport start (0xFA/0xFB) */
    inst->clocks_per_step = 6;
    recalc_clock_timing(inst);
}

static void build_chain_params(euclidrum_instance_t *inst) {
    int pos = 0, i;
    char *buf;
    int buf_len;
    if (!inst) return;
    buf = inst->chain_params_json;
    buf_len = (int)sizeof(inst->chain_params_json);

    appendf(buf, buf_len, &pos, "[");

    /* Global params */
    appendf(buf, buf_len, &pos,
        "{\"key\":\"rnd_preset\",\"name\":\"Rnd Preset\",\"type\":\"enum\",\"access\":\"write\",\"options\":[\"\\u2014\",\"Rnd!\"]},");
    appendf(buf, buf_len, &pos,
        "{\"key\":\"rate\",\"name\":\"Rate\",\"type\":\"enum\",\"options\":[\"1/32\",\"1/16T\",\"1/16\",\"1/8T\",\"1/8\",\"1/4T\",\"1/4\",\"1/2\",\"1\"]},"
        "{\"key\":\"sync\",\"name\":\"Sync\",\"type\":\"enum\",\"options\":[\"internal\",\"clock\"]},"
        "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":10,\"max\":500,\"step\":1},"
        "{\"key\":\"swing\",\"name\":\"Swing\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1},"
        "{\"key\":\"max_voices\",\"name\":\"Voices\",\"type\":\"int\",\"min\":1,\"max\":64,\"step\":1},"
        "{\"key\":\"global_velocity\",\"name\":\"Vel\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
        "{\"key\":\"global_gate\",\"name\":\"Gate\",\"type\":\"int\",\"min\":1,\"max\":1600,\"step\":1},"
        "{\"key\":\"global_rnd_seed\",\"name\":\"Rnd Seed\",\"type\":\"int\",\"min\":0,\"max\":65535,\"step\":1},"
        "{\"key\":\"rand_cycle\",\"name\":\"Rnd Cyc\",\"type\":\"int\",\"min\":1,\"max\":128,\"step\":1},"
        "{\"key\":\"mutation\",\"name\":\"Mutate\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1},"
        "{\"key\":\"mutation_seed\",\"name\":\"Mut Seed\",\"type\":\"int\",\"min\":0,\"max\":65535,\"step\":1},"
        "{\"key\":\"global_vel_rnd\",\"name\":\"Vel Rnd\",\"type\":\"int\",\"min\":0,\"max\":64,\"step\":1},"
        "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"enum\",\"options\":[");
    /* Emit all preset names as enum options */
    {
        int pi;
        for (pi = 0; pi < NUM_PRESETS; pi++) {
            appendf(buf, buf_len, &pos, "%s\"%s\"", pi ? "," : "", g_presets[pi].name);
        }
    }
    appendf(buf, buf_len, &pos,
        "]},"
        "{\"key\":\"passthrough\",\"name\":\"Passthru\",\"type\":\"enum\",\"options\":[\"off\",\"on\"]}");

    /* Per-lane params — clean names (no lane prefix, context is the page) */
    for (i = 1; i <= MAX_LANES; i++) {
        appendf(buf, buf_len, &pos,
            ",{\"key\":\"lane%d_enabled\",\"name\":\"L%d\",\"type\":\"enum\",\"options\":[\"off\",\"on\"]}"
            ",{\"key\":\"lane%d_steps\",\"name\":\"Steps\",\"type\":\"int\",\"min\":1,\"max\":64,\"step\":1}"
            ",{\"key\":\"lane%d_pulses\",\"name\":\"Pulses\",\"type\":\"int\",\"min\":0,\"max\":64,\"step\":1}"
            ",{\"key\":\"lane%d_rotation\",\"name\":\"Rotation\",\"type\":\"int\",\"min\":0,\"max\":63,\"step\":1}"
            ",{\"key\":\"lane%d_rate\",\"name\":\"Rate\",\"type\":\"enum\",\"options\":[\"x1\",\"x2\",\"x4\",\"/2\",\"/4\",\"x3\",\"/3\"]}"
            ",{\"key\":\"lane%d_drop\",\"name\":\"Drop\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1}"
            ",{\"key\":\"lane%d_velocity\",\"name\":\"Velocity\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1}"
            ",{\"key\":\"lane%d_accent\",\"name\":\"Accent\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1}"
            ",{\"key\":\"lane%d_accent_amt\",\"name\":\"Accent Amt\",\"type\":\"int\",\"min\":0,\"max\":64,\"step\":1}"
            ",{\"key\":\"lane%d_fill\",\"name\":\"Fill\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1}"
            ",{\"key\":\"lane%d_gate\",\"name\":\"Gate\",\"type\":\"int\",\"min\":0,\"max\":1600,\"step\":1}"
            ",{\"key\":\"lane%d_freq\",\"name\":\"Freq\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1}"
            ",{\"key\":\"lane%d_freq_rnd\",\"name\":\"Freq Rnd\",\"type\":\"int\",\"min\":0,\"max\":64,\"step\":1}"
            ",{\"key\":\"lane%d_decay\",\"name\":\"Decay\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1}"
            ",{\"key\":\"lane%d_decay_rnd\",\"name\":\"Decay Rnd\",\"type\":\"int\",\"min\":0,\"max\":64,\"step\":1}",
            i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
    }

    appendf(buf, buf_len, &pos, "]");
    inst->chain_params_len = pos;
}

static void *euclidrum_create_instance(const char *module_dir, const char *config_json) {
    euclidrum_instance_t *inst;
    (void)config_json;
    inst = (euclidrum_instance_t *)calloc(1, sizeof(euclidrum_instance_t));
    if (!inst) return NULL;
    apply_defaults(inst);
    (void)module_dir;  /* chain_params built programmatically */
    build_chain_params(inst);
    dlog(inst, "create sync=%d cps=%d", (int)inst->sync_mode, inst->clocks_per_step);
    return inst;
}

static void euclidrum_destroy_instance(void *instance) {
    euclidrum_instance_t *inst = (euclidrum_instance_t *)instance;
    if (!inst) return;
    if (inst->debug_fp) { fclose(inst->debug_fp); inst->debug_fp = NULL; }
    free(inst);
}

/* ════════════════════════════════════════════════════════════════════════════
 * set_param / get_param
 * ════════════════════════════════════════════════════════════════════════════ */

static int parse_lane_key(const char *key, int *idx, const char **suffix) {
    int n, consumed = 0;
    if (!key || !idx || !suffix) return 0;
    if (sscanf(key, "lane%d_%n", &n, &consumed) != 1) return 0;
    if (n < 1 || n > MAX_LANES) return 0;
    *idx = n - 1;
    *suffix = key + consumed;
    return 1;
}

static int parse_on_off(const char *val) {
    if (!val) return 0;
    if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) return 1;
    return 0;
}

static void set_lane_param(lane_t *lane, const char *suffix, const char *val) {
    if (!lane || !suffix || !val) return;
    if (strcmp(suffix, "enabled") == 0) lane->enabled = parse_on_off(val);
    else if (strcmp(suffix, "steps") == 0) { lane->steps = clamp_int(atoi(val), 1, 64); lane->pulses = clamp_int(lane->pulses, 0, lane->steps); }
    else if (strcmp(suffix, "pulses") == 0) lane->pulses = clamp_int(atoi(val), 0, 64);
    else if (strcmp(suffix, "rotation") == 0) lane->rotation = clamp_int(atoi(val), 0, 63);
    else if (strcmp(suffix, "rate") == 0) lane->rate = parse_lane_rate(val);
    else if (strcmp(suffix, "drop") == 0) lane->drop = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "drop_seed") == 0) lane->drop_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "velocity") == 0) lane->velocity = clamp_int(atoi(val), 0, 127);
    else if (strcmp(suffix, "accent") == 0) lane->accent = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "accent_amt") == 0) lane->accent_amt = clamp_int(atoi(val), 0, 64);
    else if (strcmp(suffix, "fill") == 0) lane->fill = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "fill_seed") == 0) lane->fill_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "gate") == 0) lane->gate = clamp_int(atoi(val), 0, 1600);
    else if (strcmp(suffix, "freq") == 0) lane->freq = clamp_int(atoi(val), 0, 127);
    else if (strcmp(suffix, "freq_rnd") == 0) lane->freq_rnd = clamp_int(atoi(val), 0, 64);
    else if (strcmp(suffix, "freq_seed") == 0) lane->freq_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "decay") == 0) lane->decay = clamp_int(atoi(val), 0, 127);
    else if (strcmp(suffix, "decay_rnd") == 0) lane->decay_rnd = clamp_int(atoi(val), 0, 64);
    else if (strcmp(suffix, "decay_seed") == 0) lane->decay_seed = clamp_int(atoi(val), 0, 65535);
}

/* Forward declarations for knob overlay */
static int euclidrum_get_param(void *instance, const char *key, char *buf, int buf_len);
static void euclidrum_set_param(void *instance, const char *key, const char *val);

/* ── Knob overlay: mapping tables ── */

/* Root level: knobs 1-8 = lane1_enabled .. lane8_enabled */
/* Global level: rate, bpm, swing, global_velocity, global_gate, mutation, rand_cycle, passthrough */
/* Lane levels: enabled, steps, pulses, rotation, rate, accent, freq, decay */

static const char *g_root_knob_keys[8] = {
    "lane1_enabled","lane2_enabled","lane3_enabled","lane4_enabled",
    "lane5_enabled","lane6_enabled","lane7_enabled","lane8_enabled"
};
static const char *g_root_knob_names[8] = {
    "L1","L2","L3","L4","L5","L6","L7","L8"
};

static const char *g_global_knob_keys[8] = {
    "preset","rnd_preset","rate","swing","global_gate","mutation","rand_cycle","global_vel_rnd"
};
static const char *g_global_knob_names[8] = {
    "Preset","Rnd Preset","Rate","Swing","Gate","Mutation","Rnd Cyc","Vel Rnd"
};

/* Per-lane knob suffixes (appended to "laneN_") */
static const char *g_lane_knob_suffixes[8] = {
    "enabled","steps","pulses","rotation","rate","accent","freq","decay"
};
static const char *g_lane_knob_names[8] = {
    "Enabled","Steps","Pulses","Rotation","Rate","Accent%","Freq CC","Decay CC"
};

static void knob_adjust_param(euclidrum_instance_t *inst, const char *param_key, int delta) {
    char buf[64]; int cur; const char *suffix;
    int lane_idx;

    if (!inst || !param_key) return;

    /* Handle enum params specially */
    if (strcmp(param_key, "rate") == 0) {
        int r = (int)inst->rate + delta;
        inst->rate = (rate_t)clamp_int(r, 0, (int)RATE_1_1);
        inst->timing_dirty = 1;
        recalc_clock_timing(inst);
        if (inst->sync_mode == SYNC_INTERNAL && inst->sample_rate > 0)
            recalc_internal_timing(inst, inst->sample_rate);
        return;
    }
    if (strcmp(param_key, "sync") == 0) {
        int s = (int)inst->sync_mode + delta;
        inst->sync_mode = (sync_mode_t)clamp_int(s, 0, 1);
        inst->timing_dirty = 1;
        recalc_clock_timing(inst);
        return;
    }
    if (strcmp(param_key, "passthrough") == 0) {
        inst->passthrough = clamp_int(inst->passthrough + delta, 0, 1);
        return;
    }
    if (strcmp(param_key, "preset") == 0) {
        load_preset(inst, clamp_int(inst->current_preset + delta, 0, NUM_PRESETS - 1));
        return;
    }
    if (strcmp(param_key, "rnd_preset") == 0) {
        generate_random_preset(inst);
        return;
    }

    /* Lane enum: enabled, rate */
    if (parse_lane_key(param_key, &lane_idx, &suffix)) {
        lane_t *lane = &inst->lanes[lane_idx];
        if (strcmp(suffix, "enabled") == 0) {
            lane->enabled = clamp_int(lane->enabled + delta, 0, 1);
            return;
        }
        if (strcmp(suffix, "rate") == 0) {
            int r = (int)lane->rate + delta;
            lane->rate = (lane_rate_t)clamp_int(r, 0, (int)LANE_RATE_D3);
            return;
        }
    }

    /* For int params: get current value, add delta, set back */
    if (euclidrum_get_param(inst, param_key, buf, sizeof(buf)) > 0) {
        cur = atoi(buf);
        snprintf(buf, sizeof(buf), "%d", cur + delta);
        euclidrum_set_param(inst, param_key, buf);
    }
}

static void euclidrum_set_param(void *instance, const char *key, const char *val) {
    euclidrum_instance_t *inst = (euclidrum_instance_t *)instance;
    int lane_idx; const char *suffix;
    if (!inst || !key || !val) return;

    /* ── Knob overlay: adjust ── */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_adjust")) {
        int knob_num = atoi(key + 5);  /* knob_1_adjust → 1 */
        int delta = atoi(val);
        const char *param_key = NULL;
        if (knob_num < 1 || knob_num > 8 || delta == 0) return;

        if (inst->current_level == 0) {
            param_key = g_root_knob_keys[knob_num - 1];
        } else if (inst->current_level == 1) {
            param_key = g_global_knob_keys[knob_num - 1];
        } else if (inst->current_level >= 2 && inst->current_level <= 9) {
            char full_key[64];
            int li = inst->current_level - 2;
            snprintf(full_key, sizeof(full_key), "lane%d_%s", li + 1, g_lane_knob_suffixes[knob_num - 1]);
            knob_adjust_param(inst, full_key, delta);
            return;
        }
        if (param_key) knob_adjust_param(inst, param_key, delta);
        return;
    }

    /* ── Level navigation ── */
    if (strcmp(key, "current_level") == 0) {
        if (strcmp(val, "root") == 0) inst->current_level = 0;
        else if (strcmp(val, "global") == 0) inst->current_level = 1;
        else if (strncmp(val, "lane", 4) == 0) {
            int n = atoi(val + 4);
            if (n >= 1 && n <= 8) inst->current_level = n + 1;
        }
        return;
    }

    if (parse_lane_key(key, &lane_idx, &suffix)) {
        set_lane_param(&inst->lanes[lane_idx], suffix, val);
        return;
    }

    if (strcmp(key, "rate") == 0) {
        inst->rate = parse_rate(val);
        inst->timing_dirty = 1;
        recalc_clock_timing(inst);
        if (inst->sync_mode == SYNC_INTERNAL && inst->sample_rate > 0)
            recalc_internal_timing(inst, inst->sample_rate);
    }
    else if (strcmp(key, "sync") == 0) {
        inst->sync_mode = (strcmp(val, "clock") == 0 || strcmp(val, "1") == 0) ? SYNC_CLOCK : SYNC_INTERNAL;
        inst->timing_dirty = 1;
        recalc_clock_timing(inst);
        /* Keep clock_running state — only transport start/stop should change it */
    }
    else if (strcmp(key, "bpm") == 0) {
        inst->bpm = clamp_int(atoi(val), 10, 500);
        inst->timing_dirty = 1;
        if (inst->sync_mode == SYNC_INTERNAL && inst->sample_rate > 0)
            recalc_internal_timing(inst, inst->sample_rate);
    }
    else if (strcmp(key, "swing") == 0) inst->swing = clamp_int(atoi(val), 0, 100);
    else if (strcmp(key, "max_voices") == 0) inst->max_voices = clamp_int(atoi(val), 1, MAX_VOICES);
    else if (strcmp(key, "global_velocity") == 0) inst->global_velocity = clamp_int(atoi(val), 1, 127);
    else if (strcmp(key, "global_gate") == 0) inst->global_gate = clamp_int(atoi(val), 1, 1600);
    else if (strcmp(key, "global_rnd_seed") == 0) inst->global_rnd_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(key, "rand_cycle") == 0) inst->rand_cycle = clamp_int(atoi(val), 1, 128);
    else if (strcmp(key, "mutation") == 0) inst->mutation = clamp_int(atoi(val), 0, 100);
    else if (strcmp(key, "mutation_seed") == 0) inst->mutation_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(key, "global_vel_rnd") == 0) inst->global_vel_rnd = clamp_int(atoi(val), 0, 64);
    else if (strcmp(key, "preset") == 0) {
        int pi, found = 0;
        for (pi = 0; pi < NUM_PRESETS; pi++) {
            if (strcmp(val, g_presets[pi].name) == 0) { load_preset(inst, pi); found = 1; break; }
        }
        if (!found) load_preset(inst, clamp_int(atoi(val), 0, NUM_PRESETS - 1));
    }
    else if (strcmp(key, "rnd_preset") == 0) {
        /*
         * Fire ONLY on the explicit fire value.
         *
         * This was `strcmp(val, "\xe2\x80\x94") != 0` — fire on anything that
         * is not the em-dash. But the em-dash is option 0, so a host or a patch
         * restore writing the INDEX "0" — which MEANS "do nothing" — randomised
         * all eight lanes and destroyed the kit. The safe spelling was the one
         * value that could not be sent by anything working in indices.
         *
         * Both conventions are accepted, since get_param reports the name and a
         * caller that has not learned that yet will send an index.
         */
        if (val && (strcmp(val, "Rnd!") == 0 || strcmp(val, "1") == 0))
            generate_random_preset(inst);
    }
    else if (strcmp(key, "passthrough") == 0) inst->passthrough = parse_on_off(val);
    else if (strcmp(key, "state") == 0) {
        /* Bulk state restore */
        char s[64]; int parsed, i;
        if (json_get_string(val, "rate", s, sizeof(s))) euclidrum_set_param(inst, "rate", s);
        if (json_get_string(val, "sync", s, sizeof(s))) euclidrum_set_param(inst, "sync", s);
        if (json_get_int(val, "bpm", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "bpm", s); }
        if (json_get_int(val, "swing", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "swing", s); }
        if (json_get_int(val, "max_voices", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "max_voices", s); }
        if (json_get_int(val, "global_velocity", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "global_velocity", s); }
        if (json_get_int(val, "global_gate", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "global_gate", s); }
        if (json_get_int(val, "global_rnd_seed", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "global_rnd_seed", s); }
        if (json_get_int(val, "rand_cycle", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "rand_cycle", s); }
        if (json_get_int(val, "mutation", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "mutation", s); }
        if (json_get_int(val, "mutation_seed", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "mutation_seed", s); }
        if (json_get_int(val, "global_vel_rnd", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "global_vel_rnd", s); }
        if (json_get_int(val, "preset", &parsed)) { snprintf(s, sizeof(s), "%d", parsed); euclidrum_set_param(inst, "preset", s); }
        if (json_get_string(val, "passthrough", s, sizeof(s))) euclidrum_set_param(inst, "passthrough", s);
        for (i = 0; i < MAX_LANES; i++) {
            static const char *lane_fields[] = {
                "enabled", "steps", "pulses", "rotation", "rate",
                "drop", "drop_seed", "velocity", "accent", "accent_amt",
                "fill", "fill_seed", "gate",
                "freq", "freq_rnd", "freq_seed",
                "decay", "decay_rnd", "decay_seed"
            };
            int f;
            for (f = 0; f < (int)(sizeof(lane_fields) / sizeof(lane_fields[0])); f++) {
                char k[64];
                snprintf(k, sizeof(k), "lane%d_%s", i + 1, lane_fields[f]);
                if (strcmp(lane_fields[f], "enabled") == 0 || strcmp(lane_fields[f], "rate") == 0) {
                    if (json_get_string(val, k, s, sizeof(s))) euclidrum_set_param(inst, k, s);
                } else if (json_get_int(val, k, &parsed)) {
                    snprintf(s, sizeof(s), "%d", parsed);
                    euclidrum_set_param(inst, k, s);
                }
            }
        }
    }
}

static int get_lane_param(const lane_t *lane, const char *suffix, char *buf, int buf_len) {
    if (!lane || !suffix || !buf || buf_len < 1) return -1;
    if (strcmp(suffix, "enabled") == 0) return snprintf(buf, buf_len, "%s", lane->enabled ? "on" : "off");
    if (strcmp(suffix, "steps") == 0) return snprintf(buf, buf_len, "%d", lane->steps);
    if (strcmp(suffix, "pulses") == 0) return snprintf(buf, buf_len, "%d", lane->pulses);
    if (strcmp(suffix, "rotation") == 0) return snprintf(buf, buf_len, "%d", lane->rotation);
    if (strcmp(suffix, "rate") == 0) return snprintf(buf, buf_len, "%s", lane_rate_to_string(lane->rate));
    if (strcmp(suffix, "drop") == 0) return snprintf(buf, buf_len, "%d", lane->drop);
    if (strcmp(suffix, "drop_seed") == 0) return snprintf(buf, buf_len, "%d", lane->drop_seed);
    if (strcmp(suffix, "velocity") == 0) return snprintf(buf, buf_len, "%d", lane->velocity);
    if (strcmp(suffix, "accent") == 0) return snprintf(buf, buf_len, "%d", lane->accent);
    if (strcmp(suffix, "accent_amt") == 0) return snprintf(buf, buf_len, "%d", lane->accent_amt);
    if (strcmp(suffix, "fill") == 0) return snprintf(buf, buf_len, "%d", lane->fill);
    if (strcmp(suffix, "fill_seed") == 0) return snprintf(buf, buf_len, "%d", lane->fill_seed);
    if (strcmp(suffix, "gate") == 0) return snprintf(buf, buf_len, "%d", lane->gate);
    if (strcmp(suffix, "freq") == 0) return snprintf(buf, buf_len, "%d", lane->freq);
    if (strcmp(suffix, "freq_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->freq_rnd);
    if (strcmp(suffix, "freq_seed") == 0) return snprintf(buf, buf_len, "%d", lane->freq_seed);
    if (strcmp(suffix, "decay") == 0) return snprintf(buf, buf_len, "%d", lane->decay);
    if (strcmp(suffix, "decay_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->decay_rnd);
    if (strcmp(suffix, "decay_seed") == 0) return snprintf(buf, buf_len, "%d", lane->decay_seed);
    return -1;
}

static int euclidrum_get_param(void *instance, const char *key, char *buf, int buf_len) {
    euclidrum_instance_t *inst = (euclidrum_instance_t *)instance;
    int lane_idx; const char *suffix; int pos = 0, i;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    /* ── Knob overlay: name / value ── */
    if (strncmp(key, "knob_", 5) == 0) {
        int knob_num = atoi(key + 5);
        int is_name = strstr(key, "_name") != NULL;
        int is_value = strstr(key, "_value") != NULL;
        if (knob_num >= 1 && knob_num <= 8 && (is_name || is_value)) {
            int ki = knob_num - 1;

            if (inst->current_level == 0) {
                /* Root: lane enable toggles */
                if (is_name) return snprintf(buf, buf_len, "%s", g_root_knob_names[ki]);
                return snprintf(buf, buf_len, "%s", inst->lanes[ki].enabled ? "On" : "Off");
            }
            if (inst->current_level == 1) {
                /* Global page */
                if (is_name) return snprintf(buf, buf_len, "%s", g_global_knob_names[ki]);
                return euclidrum_get_param(inst, g_global_knob_keys[ki], buf, buf_len);
            }
            if (inst->current_level >= 2 && inst->current_level <= 9) {
                /* Lane page */
                int li = inst->current_level - 2;
                char full_key[64];
                if (is_name) return snprintf(buf, buf_len, "%s", g_lane_knob_names[ki]);
                snprintf(full_key, sizeof(full_key), "lane%d_%s", li + 1, g_lane_knob_suffixes[ki]);
                return euclidrum_get_param(inst, full_key, buf, buf_len);
            }
        }
        return -1;
    }

    if (parse_lane_key(key, &lane_idx, &suffix))
        return get_lane_param(&inst->lanes[lane_idx], suffix, buf, buf_len);

    if (strcmp(key, "rate") == 0) return snprintf(buf, buf_len, "%s", rate_to_string(inst->rate));
    if (strcmp(key, "sync") == 0) return snprintf(buf, buf_len, "%s", inst->sync_mode == SYNC_CLOCK ? "clock" : "internal");
    if (strcmp(key, "bpm") == 0) return snprintf(buf, buf_len, "%d", inst->bpm);
    if (strcmp(key, "swing") == 0) return snprintf(buf, buf_len, "%d", inst->swing);
    if (strcmp(key, "max_voices") == 0) return snprintf(buf, buf_len, "%d", inst->max_voices);
    if (strcmp(key, "global_velocity") == 0) return snprintf(buf, buf_len, "%d", inst->global_velocity);
    if (strcmp(key, "global_gate") == 0) return snprintf(buf, buf_len, "%d", inst->global_gate);
    if (strcmp(key, "global_rnd_seed") == 0) return snprintf(buf, buf_len, "%d", inst->global_rnd_seed);
    if (strcmp(key, "rand_cycle") == 0) return snprintf(buf, buf_len, "%d", inst->rand_cycle);
    if (strcmp(key, "mutation") == 0) return snprintf(buf, buf_len, "%d", inst->mutation);
    if (strcmp(key, "mutation_seed") == 0) return snprintf(buf, buf_len, "%d", inst->mutation_seed);
    if (strcmp(key, "global_vel_rnd") == 0) return snprintf(buf, buf_len, "%d", inst->global_vel_rnd);
    if (strcmp(key, "preset") == 0) {
        int p = clamp_int(inst->current_preset, 0, NUM_PRESETS - 1);
        return snprintf(buf, buf_len, "%s", g_presets[p].name);
    }
    if (strcmp(key, "preset_name") == 0) {
        int p = clamp_int(inst->current_preset, 0, NUM_PRESETS - 1);
        return snprintf(buf, buf_len, "%s", g_presets[p].name);
    }
    if (strcmp(key, "rnd_preset") == 0) return snprintf(buf, buf_len, "%s", "\xe2\x80\x94");
    if (strcmp(key, "passthrough") == 0) return snprintf(buf, buf_len, "%s", inst->passthrough ? "on" : "off");
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "EDrum");
    if (strcmp(key, "bank_name") == 0) return snprintf(buf, buf_len, "Factory");
    if (strcmp(key, "error") == 0) {
        if (inst->sync_mode == SYNC_CLOCK && g_host && g_host->get_clock_status) {
            int st = g_host->get_clock_status();
            if (st == MOVE_CLOCK_STATUS_UNAVAILABLE)
                return snprintf(buf, buf_len, "Enable MIDI Clock Out in Move settings");
            if (st == MOVE_CLOCK_STATUS_STOPPED)
                return snprintf(buf, buf_len, "Clock out enabled, transport stopped");
        }
        buf[0] = '\0'; return 0;
    }
    if (strcmp(key, "chain_params") == 0) {
        if (inst->chain_params_len > 0) {
            if (inst->chain_params_len >= buf_len) return -1;
            memcpy(buf, inst->chain_params_json, (size_t)inst->chain_params_len);
            buf[inst->chain_params_len] = '\0';
            return inst->chain_params_len;
        }
        return -1;
    }

    /* Full state serialization */
    if (strcmp(key, "state") == 0) {
        if (!appendf(buf, buf_len, &pos, "{")) return -1;
        if (!appendf(buf, buf_len, &pos,
                     "\"rate\":\"%s\",\"sync\":\"%s\",\"bpm\":%d,\"swing\":%d,"
                     "\"max_voices\":%d,\"global_velocity\":%d,\"global_gate\":%d,"
                     "\"global_rnd_seed\":%d,\"rand_cycle\":%d,"
                     "\"mutation\":%d,\"mutation_seed\":%d,\"global_vel_rnd\":%d,\"preset\":%d,\"passthrough\":\"%s\"",
                     rate_to_string(inst->rate),
                     inst->sync_mode == SYNC_CLOCK ? "clock" : "internal",
                     inst->bpm, inst->swing, inst->max_voices,
                     inst->global_velocity, inst->global_gate,
                     inst->global_rnd_seed, inst->rand_cycle,
                     inst->mutation, inst->mutation_seed,
                     inst->global_vel_rnd, inst->current_preset,
                     inst->passthrough ? "on" : "off")) return -1;
        for (i = 0; i < MAX_LANES; i++) {
            lane_t *lane = &inst->lanes[i];
            if (!appendf(buf, buf_len, &pos,
                         ",\"lane%d_enabled\":\"%s\",\"lane%d_steps\":%d,\"lane%d_pulses\":%d,"
                         "\"lane%d_rotation\":%d,\"lane%d_rate\":\"%s\","
                         "\"lane%d_drop\":%d,\"lane%d_drop_seed\":%d,"
                         "\"lane%d_velocity\":%d,\"lane%d_accent\":%d,\"lane%d_accent_amt\":%d,"
                         "\"lane%d_fill\":%d,\"lane%d_fill_seed\":%d,\"lane%d_gate\":%d,"
                         "\"lane%d_freq\":%d,\"lane%d_freq_rnd\":%d,\"lane%d_freq_seed\":%d,"
                         "\"lane%d_decay\":%d,\"lane%d_decay_rnd\":%d,\"lane%d_decay_seed\":%d",
                         i+1, lane->enabled ? "on" : "off",
                         i+1, lane->steps, i+1, lane->pulses,
                         i+1, lane->rotation, i+1, lane_rate_to_string(lane->rate),
                         i+1, lane->drop, i+1, lane->drop_seed,
                         i+1, lane->velocity, i+1, lane->accent, i+1, lane->accent_amt,
                         i+1, lane->fill, i+1, lane->fill_seed, i+1, lane->gate,
                         i+1, lane->freq, i+1, lane->freq_rnd, i+1, lane->freq_seed,
                         i+1, lane->decay, i+1, lane->decay_rnd, i+1, lane->decay_seed))
                return -1;
        }
        if (!appendf(buf, buf_len, &pos, "}")) return -1;
        return pos;
    }

    return -1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * process_midi — transport, clock, live pass-through
 * ════════════════════════════════════════════════════════════════════════════ */

static int euclidrum_process_midi(void *instance, const uint8_t *in_msg, int in_len,
                                  uint8_t out_msgs[][3], int out_lens[], int max_out) {
    euclidrum_instance_t *inst = (euclidrum_instance_t *)instance;
    uint8_t status, type;
    int count = 0;
    if (!inst || !in_msg || in_len < 1) return 0;

    status = in_msg[0];
    type = status & 0xF0;

    /* ── Transport messages ── */
    if (inst->sync_mode == SYNC_CLOCK) {
        if (status == 0xFA) { /* MIDI Start */
            inst->clock_running = 1;
            inst->midi_transport_started = 1;
            inst->clock_counter = 0;
            inst->clock_tick_total = 0;
            inst->pending_step_triggers = 1;
            inst->anchor_step = 0;
            inst->swing_phase = 0;
            inst->mutation_cycle_count = 0;
            return 0;
        }
        if (status == 0xFB) { /* MIDI Continue */
            inst->clock_running = 1;
            inst->midi_transport_started = 1;
            return 0;
        }
        if (status == 0xFC) { /* MIDI Stop */
            return handle_transport_stop(inst, out_msgs, out_lens, max_out);
        }
        if (status == 0xF8) { /* Clock tick */
            if (!inst->clock_running) return 0;
            return process_clock_tick(inst, out_msgs, out_lens, max_out);
        }
    } else {
        if (status == 0xFA || status == 0xFB) {
            if (inst->timing_dirty || inst->sample_rate <= 0)
                recalc_internal_timing(inst, inst->sample_rate > 0 ? inst->sample_rate : DEFAULT_SAMPLE_RATE);
            inst->clock_running = 1;
            inst->midi_transport_started = 1;
            inst->internal_sample_total = 0;
            inst->samples_until_step_f = 0.0;
            inst->anchor_step = 0;
            inst->swing_phase = 0;
            inst->mutation_cycle_count = 0;
            return 0;
        }
        if (status == 0xFC) {
            return handle_transport_stop(inst, out_msgs, out_lens, max_out);
        }
    }


    /* ── Live MIDI pass-through ── */
    if (inst->passthrough && (type == 0x90 || type == 0x80 || type == 0xB0) && in_len >= 3) {
        if (max_out > count) {
            out_msgs[count][0] = in_msg[0];
            out_msgs[count][1] = in_msg[1];
            out_msgs[count][2] = in_msg[2];
            out_lens[count] = 3;
            count++;
        }
        return count;
    }

    /* Forward other MIDI messages */
    if (max_out > count) {
        out_msgs[count][0] = in_msg[0];
        out_msgs[count][1] = in_len > 1 ? in_msg[1] : 0;
        out_msgs[count][2] = in_len > 2 ? in_msg[2] : 0;
        out_lens[count] = in_len > 3 ? 3 : in_len;
        count++;
    }
    return count;
}

/* ════════════════════════════════════════════════════════════════════════════
 * tick — per-block timing engine
 * ════════════════════════════════════════════════════════════════════════════ */

static int euclidrum_tick(void *instance, int frames, int sample_rate,
                          uint8_t out_msgs[][3], int out_lens[], int max_out) {
    euclidrum_instance_t *inst = (euclidrum_instance_t *)instance;
    int count = 0;
    if (!inst || frames < 0 || max_out < 1) return 0;

    if (inst->timing_dirty || inst->sample_rate != sample_rate)
        recalc_internal_timing(inst, sample_rate);

    if (inst->sync_mode == SYNC_INTERNAL) {
        (void)advance_voice_timers_samples(inst, frames, out_msgs, out_lens, max_out, &count);
        if (count >= max_out || !inst->clock_running) return count;

        inst->internal_sample_total += (uint64_t)frames;
        inst->samples_until_step_f -= (double)frames;
        while (inst->samples_until_step_f <= 0.0 && count < max_out) {
            count += run_anchor_step(inst, out_msgs + count, out_lens + count, max_out - count);
            inst->samples_until_step_f += next_internal_interval(inst);
            if (inst->samples_until_step_f < 1.0) inst->samples_until_step_f = 1.0;
        }
        return count;
    }

    /* External clock: advance sample-based voice timers for fine gate resolution */
    (void)advance_voice_timers_samples(inst, frames, out_msgs, out_lens, max_out, &count);

    /* External clock: drain pending steps */
    if (inst->pending_step_triggers > 0) {
        while (inst->pending_step_triggers > 0 && count < max_out) {
            count += run_anchor_step(inst, out_msgs + count, out_lens + count, max_out - count);
            inst->pending_step_triggers--;
        }
    }
    return count;
}

/* ════════════════════════════════════════════════════════════════════════════
 * API export
 * ════════════════════════════════════════════════════════════════════════════ */

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = euclidrum_create_instance,
    .destroy_instance = euclidrum_destroy_instance,
    .process_midi = euclidrum_process_midi,
    .tick = euclidrum_tick,
    .set_param = euclidrum_set_param,
    .get_param = euclidrum_get_param
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}

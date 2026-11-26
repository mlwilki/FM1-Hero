/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-Hero: Falling-note MIDI piano trainer, rhythm game & 2-Op FM synth for M-VAVE FM-1.
 *
 * Features & controls:
 *   27 Keys / MIDI In   Play notes & hit falling Green (Right Hand) / Blue (Left Hand) notes
 *   Physical Key LEDs   Light up the keys you need to press next on the FM-1 keybed
 *   PLAY                Play / Pause song (or play again from Song Complete screen)
 *   HOME                Rewind song to Measure 1
 *   SELECT / PRESETS    Browse 16 built-in songs + Slot 17 (Custom .MID from web library)
 *   ALGORITHM / SEL     Practice Mode:
 *                         MELODY (Wait-for-Me: pauses at note until you press the lit key)
 *                         RHYTHM (Continuous scroll, timing grades & combo multiplier)
 *                         WATCH  (Watch & Listen auto-play jukebox + free play-along)
 *   ENV / LFO           Hand Select: BOTH HANDS, RIGHT HAND (Green, LH auto-plays), LEFT HAND (Blue, RH auto-plays)
 *   EDIT                A-B Loop Bookmark (1st press = set A, 2nd = loop A..B, 3rd = clear loop)
 *   GLO                 Toggle Metronome click on beats
 *   FX / KNOB2          Cycle 6 FM Synth Patches (DX E-PIANO, GRAND PIANO, CHIPTUNE, FM BELLS, BRASS, PAD)
 *   REC                 Record your own song from keys/MIDI into Custom Slot 17
 *   SAVE                Save scores, settings & custom .MID to flash
 *   OCT- / OCT+         Transpose keyboard octave (-2 .. +2)
 *   KNOB1               Tempo / Speed % (25% .. 200%)
 *   KNOB3               FM Brightness / Timbre (0 .. 127)
 *   KNOB4               Stereo Echo FX Mix (0 .. 127)
 */
#include "fm1.h"
#include "songs.h"

/* ---- Constants & Limits */
#define VOICES 6
#define MAX_SONG_NOTES 8192
#define MAX_CUSTOM_NOTES 8192
#define FLASH_CUSTOM_NOTES 900 /* fits within 4 KiB user flash sector alongside scores/settings */
#define TOTAL_SONGS (BUILTIN_SONG_COUNT + 1) /* 0..BUILTIN_SONG_COUNT-1 built-in, last = CUSTOM .MID */
#define CUSTOM_SLOT BUILTIN_SONG_COUNT

/* Screen Layout (240 x 240 — Full-Screen Keysight Layout, zero permanent bars) */
#define TIMELINE_H 2
#define HWY_Y0 2
#define HWY_Y1 188
#define HWY_H (HWY_Y1 - HWY_Y0) /* 186 px edge-to-edge falling-note highway */
#define FELT_Y 188              /* 3 px luminous strike bar above keys */
#define KB_Y 191
#define KB_WHITE_H 49           /* flush to bottom edge (191 + 49 = 240) */
#define KB_BLACK_H 30
#define LOOKAHEAD_MS 2200

/* Practice & Game Modes */
enum {
  MODE_WAIT = 0, /* Melody Practice ("Wait for me") */
  MODE_GAME = 1, /* Rhythm Practice / Recital */
  MODE_AUTO = 2, /* Watch and Listen */
  MODE_COUNT = 3
};

/* 10 Waterfall Visual Themes */
enum {
  THEME_SPACE = 0,    /* 1. Outer Space: Galaxy + Comets + Supernova Flash */
  THEME_CARTOON = 1,  /* 2. Cartoon: Sky & Clouds + Inked Jelly Notes + POW! */
  THEME_CYBER = 2,    /* 3. Cyberpunk / Neon: Synthwave Grid + Laser Trails + Glitch */
  THEME_MEDIEVAL = 3, /* 4. Medieval / Fantasy: Castle Bricks + Swords/Scrolls + Runes */
  THEME_ARCADE = 4,   /* 5. Retro / Arcade: Magenta Wireframe + Gems/Bricks + Pixel Boom */
  THEME_TIEDYE = 5,   /* 6. Tie-Dye / Blacklight: Psychedelic Swirl + Rainbow Notes + Peace */
  THEME_LAVA = 6,     /* 7. Lava Lamp: Rising Bubbles + Glossy Blobs + Liquid Splash */
  THEME_FLOWER = 7,   /* 8. Flower Power: Retro Forest + Daisy Notes + Blossom Burst */
  THEME_MANDALA = 8,  /* 9. Cosmic Mandala: Sacred Geometry + Prismatic Crystals + Rings */
  THEME_GROOVY = 9,   /* 10. 70s Groovy: Vintage Stripes + Retro Pills + Golden Flare */
  THEME_COUNT = 10
};

/* Hand Practice Selection (RH / LH split) */
enum {
  HAND_BOTH = 0,  /* Player plays both hands (Green + Blue) */
  HAND_RIGHT = 1, /* Player plays Right Hand (Green); Left Hand (Blue) plays automatically */
  HAND_LEFT = 2,  /* Player plays Left Hand (Blue); Right Hand (Green) plays automatically */
  HAND_COUNT = 3
};

/* Judgement Grades */
enum {
  JUDGE_NONE = 0,
  JUDGE_WAIT,
  JUDGE_PERFECT,
  JUDGE_GREAT,
  JUDGE_GOOD,
  JUDGE_MISS
};

/* Note runtime state flags */
#define NF_DONE    1u
#define NF_HIT     2u
#define NF_MISSED  4u
#define NF_AUTO_ON 8u
#define NF_LH      16u /* Left Hand (Blue) when set, Right Hand (Green) when clear */

/* ---- Persistent Flash Storage */
#define SAVED_MAGIC 0x53594E33u /* "SYN3" */
typedef struct {
  uint32_t magic;
  int8_t octave;
  uint8_t song_idx;
  uint8_t mode;
  uint8_t hand;
  uint8_t patch;
  uint8_t metronome;
  uint8_t speed_pct; /* 25..200 */
  uint8_t fm_bright; /* 0..127 */
  uint8_t echo_mix;  /* 0..127 */
  uint8_t theme;     /* 0..THEME_COUNT-1 (fits in alignment padding) */
  uint32_t high_score[TOTAL_SONGS];
  char custom_title[16];
  uint16_t custom_bpm;
  uint16_t custom_count;
  song_ev_t custom_events[FLASH_CUSTOM_NOTES];
} saved_data_t;

static saved_data_t saved;
/* Full-length custom .MID / recorded song buffer in POOL RAM (up to MAX_CUSTOM_NOTES) */
static song_ev_t custom_events[MAX_CUSTOM_NOTES] POOL;
static uint16_t custom_count;

/* ---- Unpacked Active Song in POOL RAM */
typedef struct {
  int32_t t0_ms;
  int32_t t1_ms;
  uint8_t key;   /* 0..26 key index on FM-1 (F3..G5) */
  uint8_t note;  /* MIDI note 53..79 */
  uint8_t flags; /* NF_* */
  uint8_t pad;
} active_note_t;

static active_note_t song_notes[MAX_SONG_NOTES] POOL;
static int32_t song_note_count;
static int32_t song_total_ms;
static uint16_t song_base_bpm = 120;
static const char *song_title = "COMING TO TOWN";

/* ---- 6-Voice 2-Operator FM Synth + Metronome Click + Stereo Echo */
typedef struct {
  const char *name;
  uint8_t ratio_num;
  uint8_t ratio_den;
  uint8_t mod_base;
  uint8_t mod_decay;
  uint8_t atk_step;
  uint8_t dec_rate;
  uint8_t rel_rate;
  uint8_t feedback;
} fm_patch_t;

#define PATCH_COUNT 6
static const fm_patch_t PATCHES[PATCH_COUNT] = {
  {"GRAND PIANO", 1, 1, 44, 22, 115, 5, 16, 2},
  {"DX E-PIANO",  1, 1, 58, 18, 96,  3, 14, 3},
  {"CHIPTUNE",    1, 1, 95, 4,  127, 1, 28, 1},
  {"FM BELLS",    7, 2, 82, 10, 120, 4, 8,  0},
  {"SYNTH BRASS", 1, 1, 76, 6,  32,  1, 16, 2},
  {"CRYSTAL PAD", 2, 1, 40, 8,  20,  1, 8,  3},
};

static struct {
  volatile uint32_t c_inc;
  volatile uint32_t m_inc;
  volatile int32_t gate;
  volatile int32_t mod_env;
  int32_t amp;
  uint32_t c_ph;
  uint32_t m_ph;
  int32_t m_prev;
  uint8_t note;
  uint8_t from_song;
} voice[VOICES];

/* Metronome woodblock click state */
static volatile int32_t metro_amp;
static volatile uint32_t metro_inc;
static uint32_t metro_ph;

#define DELAY_LEN 8192u
static int16_t delay_l[DELAY_LEN] POOL;
static int16_t delay_r[DELAY_LEN] POOL;
static uint32_t delay_pos;

static volatile uint8_t v_patch;
static volatile uint8_t v_fm_bright = 64;
static volatile uint8_t v_echo_mix = 32;

/* ---- Tiny no-libc string helpers */
static char *str_cat(char *dst, const char *src) {
  while (*src) {
    *dst++ = *src++;
  }
  *dst = 0;
  return dst;
}

static char *str_cat_int(char *dst, int32_t v) {
  char tmp[12];
  format_int(tmp, v);
  return str_cat(dst, tmp);
}

static uint8_t fold_note_to_fm1(int note) {
  while (note < 53) {
    note += 12;
  }
  while (note > 79) {
    note -= 12;
  }
  return (uint8_t)note;
}

/* ---- 27-Key Geometry (F3..G5 across 240 px) */
static const uint8_t IS_BLACK[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};
static uint8_t key_x[KEY_COUNT];
static uint8_t key_w[KEY_COUNT];
static uint8_t key_black[KEY_COUNT];

static void init_key_geometry(void) {
  int x = 8;
  for (int k = 0; k < KEY_COUNT; k++) {
    int blk = IS_BLACK[k % 12];
    key_black[k] = (uint8_t)blk;
    if (!blk) {
      key_x[k] = (uint8_t)x;
      key_w[k] = 13;
      x += 14;
    } else {
      key_x[k] = (uint8_t)(x - 5);
      key_w[k] = 9;
    }
  }
}

/* ---- Audio Interrupt */
void audio(int32_t *out, uint32_t frames) {
  const fm_patch_t *p = &PATCHES[v_patch % PATCH_COUNT];
  int32_t bright = v_fm_bright;
  int32_t echo = v_echo_mix;
  int32_t atk_inc = (int32_t)p->atk_step * 8;
  int32_t dec_step = (int32_t)p->dec_rate;
  int32_t rel_step = (int32_t)p->rel_rate * 2;
  int32_t mod_dec = (int32_t)p->mod_decay * 2;
  int32_t base_idx = ((int32_t)p->mod_base * (bright + 32)) >> 6;
  uint8_t fb_shift = p->feedback;

#if FOMA_USB_AUDIO
  const int32_t *in = audio_in();
  for (uint32_t i = 0; in && i < 2u * frames; i++) {
    out[i] += in[i];
  }
#endif

  for (int v = 0; v < VOICES; v++) {
    int32_t amp = voice[v].amp;
    int32_t gate = voice[v].gate;
    int32_t menv = voice[v].mod_env;
    uint32_t c_ph = voice[v].c_ph, c_inc = voice[v].c_inc;
    uint32_t m_ph = voice[v].m_ph, m_inc = voice[v].m_inc;
    int32_t m_prev = voice[v].m_prev;
    if (!amp && !gate) {
      continue;
    }
    int pan_l = (v & 1) ? 14 : 18;
    int pan_r = (v & 1) ? 18 : 14;
    for (uint32_t i = 0; i < frames; i++) {
      if (amp < gate) {
        amp += atk_inc;
        if (amp > gate) {
          amp = gate;
        }
      } else if (gate > 0) {
        int32_t floor_g = gate >> 1;
        if (amp > floor_g && (i & 7u) == 0u) {
          amp -= dec_step;
          if (amp < floor_g) {
            amp = floor_g;
          }
        }
      } else {
        amp -= rel_step;
        if (amp < 0) {
          amp = 0;
        }
      }
      if (menv > 0 && (i & 3u) == 0u) {
        menv -= mod_dec;
        if (menv < 0) {
          menv = 0;
        }
      }
      m_ph += m_inc;
      uint32_t fb_off = fb_shift ? (uint32_t)(m_prev << (14 - fb_shift)) : 0u;
      int32_t mod = sine(m_ph + fb_off);
      m_prev = mod;
      int32_t idx = base_idx + (menv >> 8);
      uint32_t pm = (uint32_t)(mod * idx) << 9;
      c_ph += c_inc;
      int32_t sig = (sine(c_ph + pm) * amp) >> 15;
      out[2u * i] += (sig * pan_l) >> 4;
      out[2u * i + 1u] += (sig * pan_r) >> 4;
    }
    voice[v].amp = amp;
    voice[v].mod_env = menv;
    voice[v].c_ph = c_ph;
    voice[v].m_ph = m_ph;
    voice[v].m_prev = m_prev;
  }

  /* Optional Metronome click */
  int32_t ma = metro_amp;
  if (ma > 0) {
    uint32_t mi = metro_inc, mp = metro_ph;
    for (uint32_t i = 0; i < frames && ma > 0; i++) {
      mp += mi;
      int32_t cs = (sine(mp) * ma) >> 15;
      out[2u * i] += cs;
      out[2u * i + 1u] += cs;
      ma -= 18;
    }
    metro_ph = mp;
    metro_amp = ma > 0 ? ma : 0;
  }

  /* Stereo ping-pong echo */
  uint32_t dp = delay_pos;
  for (uint32_t i = 0; i < frames; i++) {
    int32_t l = out[2u * i];
    int32_t r = out[2u * i + 1u];
    int32_t dl = delay_l[dp];
    int32_t dr = delay_r[dp];
    if (echo > 0) {
      out[2u * i] = l + ((dl * echo) >> 7);
      out[2u * i + 1u] = r + ((dr * echo) >> 7);
    }
    int32_t nl = (l >> 1) + ((dr * 50) >> 7);
    int32_t nr = (r >> 1) + ((dl * 50) >> 7);
    delay_l[dp] = (int16_t)(nl > 32000 ? 32000 : nl < -32000 ? -32000 : nl);
    delay_r[dp] = (int16_t)(nr > 32000 ? 32000 : nr < -32000 ? -32000 : nr);
    dp = (dp + 1u) & (DELAY_LEN - 1u);
  }
  delay_pos = dp;
}

static void synth_note_on(int note, int velocity, int from_song) {
  const fm_patch_t *p = &PATCHES[saved.patch % PATCH_COUNT];
  int pick = 0;
  for (int v = 0; v < VOICES; v++) {
    if (voice[v].note == note && voice[v].gate) {
      pick = v;
      break;
    }
    if (voice[v].amp + voice[v].gate < voice[pick].amp + voice[pick].gate) {
      pick = v;
    }
  }
  uint32_t ci = note_inc(note);
  uint32_t mi = p->ratio_den == 2 ? (ci * p->ratio_num) >> 1 : ci * p->ratio_num;
  voice[pick].note = (uint8_t)note;
  voice[pick].from_song = (uint8_t)from_song;
  voice[pick].c_inc = ci;
  voice[pick].m_inc = mi;
  voice[pick].mod_env = 24000;
  voice[pick].gate = velocity * 85;
}

static void synth_note_off(int note, int from_song) {
  for (int v = 0; v < VOICES; v++) {
    if (voice[v].note == note && voice[v].from_song == from_song) {
      voice[v].gate = 0;
    }
  }
}

static void synth_all_off(void) {
  for (int v = 0; v < VOICES; v++) {
    voice[v].gate = 0;
  }
}

/* ---- Game & Sequencer State */
static int32_t song_pos_ms;
static uint32_t song_frac_acc;
static uint8_t is_playing;
static uint8_t is_finished;
static uint8_t is_recording;
static uint32_t rec_last_ms;

/* A-B Bookmark Loop state */
static uint8_t loop_state; /* 0 = off, 1 = A set, 2 = looping A..B */
static int32_t loop_a_ms;
static int32_t loop_b_ms;

static uint32_t score;
static uint16_t combo;
static uint16_t max_combo;
static uint16_t hits_perfect;
static uint16_t hits_good;
static uint16_t hits_missed;
static uint8_t last_judge;
static uint32_t judge_start_ms;
static uint32_t judge_until_ms;
static int32_t last_metro_beat = -1;

/* ---- Lightweight Zero-Allocation Keysight FX & GLO Menu State (Pure Integer) */
#define SPARK_MAX 48
typedef struct {
  int16_t x8;
  int16_t y8;
  int16_t vx8;
  int16_t vy8;
  uint16_t col_hot;
  uint16_t col_mid;
  uint8_t life;
  uint8_t max_life;
} spark_t;

static spark_t sparks[SPARK_MAX];
static uint8_t lane_flash_ms[KEY_COUNT]; /* 0..200 ms strike bloom & rising light plume */
static uint8_t lane_flash_lh[KEY_COUNT];
static uint32_t cur_held_mask;
static uint32_t combo_pop_until_ms;
static uint32_t finish_start_ms;

/* GLO Modal Menu Window */
#define MENU_ITEMS 10
static uint8_t menu_open;
static uint8_t menu_cursor;
static uint32_t menu_open_ms;

/* Knob / parameter pill popup (1.2s auto-dismiss) */
static uint32_t popup_start_ms;
static uint32_t popup_end_ms;
static uint16_t popup_col;
static uint8_t popup_pct;
static char popup_label[12];
static char popup_val[14];

/* Song browser drawer (1.5s auto-dismiss on SELECT/PRESETS turn) */
static uint32_t browser_start_ms;
static uint32_t browser_until_ms;

/* MIDI Channel 16 (.mid upload) state */
static uint16_t upload_dt_acc;
static uint8_t upload_title_len;
static uint8_t uploading_mid;

static uint8_t hud_dirty = 1;
static uint8_t bot_dirty = 1;
static uint8_t kb_dirty = 1;
static uint32_t last_frame_ms;

static int32_t ticks_to_ms(uint32_t ticks, uint32_t bpm) {
  if (bpm < 30u) {
    bpm = 120u;
  }
  return (int32_t)((ticks * 2500u) / bpm);
}

/* Check if a note belongs to the player's active hand(s) or auto-plays */
static int note_is_player_hand(uint8_t flags) {
  if (saved.mode == MODE_AUTO) {
    return 0;
  }
  if (saved.hand == HAND_RIGHT) {
    return !(flags & NF_LH);
  }
  if (saved.hand == HAND_LEFT) {
    return (flags & NF_LH) != 0;
  }
  return 1;
}

static void reset_notes_range(int32_t from_ms) {
  for (int32_t i = 0; i < song_note_count; i++) {
    if (song_notes[i].t0_ms >= from_ms) {
      song_notes[i].flags &= NF_LH;
    }
  }
}

static void load_song(int idx) {
  synth_all_off();
  if (idx < 0) {
    idx = TOTAL_SONGS - 1;
  }
  if (idx >= TOTAL_SONGS) {
    idx = 0;
  }
  saved.song_idx = (uint8_t)idx;

  const song_ev_t *evs;
  uint16_t cnt;
  if (idx == CUSTOM_SLOT) {
    song_title = saved.custom_title[0] ? saved.custom_title : "CUSTOM .MID";
    song_base_bpm = saved.custom_bpm ? saved.custom_bpm : 120;
    evs = custom_events;
    cnt = custom_count;
  } else {
    song_title = BUILTIN_SONGS[idx].title;
    song_base_bpm = BUILTIN_SONGS[idx].bpm;
    evs = BUILTIN_SONGS[idx].events;
    cnt = BUILTIN_SONGS[idx].count;
  }

  if (cnt > MAX_SONG_NOTES) {
    cnt = MAX_SONG_NOTES;
  }
  uint32_t cur_ticks = 0;
  int32_t end_ms = 0;
  for (uint16_t i = 0; i < cnt; i++) {
    cur_ticks += evs[i].dt;
    uint8_t raw_n = evs[i].note;
    uint8_t is_lh = (raw_n & 0x80u) ? NF_LH : 0u;
    uint8_t n = fold_note_to_fm1(raw_n & 0x7Fu);
    uint8_t dur = evs[i].dur ? evs[i].dur : 12;
    int32_t t0 = ticks_to_ms(cur_ticks, song_base_bpm);
    int32_t t1 = ticks_to_ms(cur_ticks + dur, song_base_bpm);
    if (t1 <= t0 + 40) {
      t1 = t0 + 40;
    }
    song_notes[i].t0_ms = t0;
    song_notes[i].t1_ms = t1;
    song_notes[i].note = n;
    song_notes[i].key = (uint8_t)(n - KEY_FIRST_NOTE);
    song_notes[i].flags = is_lh;
    if (t1 > end_ms) {
      end_ms = t1;
    }
  }
  song_note_count = cnt;
  song_total_ms = end_ms + 800;
  song_pos_ms = -800;
  song_frac_acc = 0;
  last_metro_beat = -1;
  loop_state = 0;
  is_finished = 0;
  score = 0;
  combo = 0;
  max_combo = 0;
  hits_perfect = 0;
  hits_good = 0;
  hits_missed = 0;
  last_judge = JUDGE_NONE;
  hud_dirty = 1;
  bot_dirty = 1;
  kb_dirty = 1;
}

static void restart_song(void) {
  load_song(saved.song_idx);
  is_playing = 1;
}

/* ---- Keysight Neon-on-Void Palette (RGB565) */
#define C_BG_VOID      RGB(5, 7, 12)      /* #05070C deep Keysight void */
#define C_SURF_RECESS  RGB(15, 19, 30)    /* #0F131E modal menu recessed header */
#define C_SURF_ELEV    RGB(22, 28, 42)    /* #161C2A modal menu card */
#define C_OUTLINE_IDLE RGB(46, 54, 70)    /* #2E3646 crisp idle border */
#define C_FM1_ORANGE   RGB(255, 136, 28)  /* #FF881C warm amber/orange accent */
#define C_GOLD         RGB(255, 220, 72)  /* #FFDC48 S-rank / combos / wait */
#define C_CRIMSON      RGB(245, 66, 78)   /* #F5424E REC / MISS */
#define C_TEXT_PRI     RGB(243, 245, 248) /* #F3F5F8 primary text */
#define C_TEXT_DIM     RGB(148, 160, 178) /* #94A0B2 muted text */

/* Subtle Highway Guides */
#define C_GUIDE_C      RGB(24, 32, 46)
#define C_GUIDE_F      RGB(14, 19, 28)
#define C_MEASURE      RGB(18, 24, 36)

/* Right Hand (Vibrant Neon Green / Emerald) */
#define C_RH_GLOW_DIM  RGB(8, 28, 14)
#define C_RH_CORE      RGB(16, 62, 26)
#define C_RH_CORE_B    RGB(10, 44, 18)
#define C_RH_EDGE      RGB(58, 242, 108)  /* luminous neon green */
#define C_RH_HOT       RGB(205, 255, 215) /* white-hot specular */

/* Left Hand (Vibrant Electric Cyan / Azure) */
#define C_LH_GLOW_DIM  RGB(6, 22, 38)
#define C_LH_CORE      RGB(12, 52, 86)
#define C_LH_CORE_B    RGB(8, 36, 64)
#define C_LH_EDGE      RGB(36, 208, 255)  /* luminous electric cyan */
#define C_LH_HOT       RGB(195, 244, 255) /* white-hot specular */

/* Keybed Surfaces */
#define C_KEY_W_TOP    RGB(232, 236, 244)
#define C_KEY_W_MID    RGB(210, 216, 228)
#define C_KEY_W_BOT    RGB(168, 176, 192)
#define C_KEY_B_BODY   RGB(16, 20, 28)
#define C_KEY_B_HI     RGB(54, 64, 84)
#define C_MISS_CORE    RGB(56, 16, 20)
#define C_MISS_EDGE    RGB(195, 56, 68)

/* ---- Fixed-Point Animation & UI Primitives (No FPU) */
static int32_t ease_out_quad(int32_t t, int32_t dur) {
  if (t <= 0 || dur <= 0) {
    return 0;
  }
  if (t >= dur) {
    return 256;
  }
  int32_t u = (t * 256) / dur;
  int32_t inv = 256 - u;
  return 256 - ((inv * inv) >> 8);
}

static void draw_outlined_box(int x, int y, int w, int h, uint16_t bg, uint16_t border, uint16_t corner) {
  rect(x, y, w, h, bg);
  frame(x, y, w, h, border);
  rect(x, y, 3, 1, corner);
  rect(x, y, 1, 3, corner);
  rect(x + w - 3, y, 3, 1, corner);
  rect(x + w - 1, y, 1, 3, corner);
  rect(x, y + h - 1, 3, 1, corner);
  rect(x, y + h - 3, 1, 3, corner);
  rect(x + w - 3, y + h - 1, 3, 1, corner);
  rect(x + w - 1, y + h - 3, 1, 3, corner);
}

static void str_copy_lim(char *dst, const char *src, int max_len) {
  int i = 0;
  while (src[i] && i < max_len - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

static void trigger_popup(const char *label, const char *val, uint8_t pct, uint16_t col) {
  uint32_t now = millis();
  str_copy_lim(popup_label, label, sizeof popup_label);
  str_copy_lim(popup_val, val, sizeof popup_val);
  popup_pct = pct > 100 ? 100 : pct;
  popup_col = col;
  if (now >= popup_end_ms) {
    popup_start_ms = now;
  } else if (now > popup_start_ms + 120u) {
    popup_start_ms = now - 120u;
  }
  popup_end_ms = now + 1200u;
}

static void spawn_sparks(int k, int is_lh, int count, int boost) {
  if (k < 0 || k >= KEY_COUNT) {
    return;
  }
  int cx = key_x[k] + (key_w[k] >> 1);
  uint16_t hot = boost ? C_GOLD : (is_lh ? C_LH_HOT : C_RH_HOT);
  uint16_t mid = is_lh ? C_LH_EDGE : C_RH_EDGE;
  int spawned = 0;
  for (int i = 0; i < SPARK_MAX && spawned < count; i++) {
    if (sparks[i].life == 0) {
      uint32_t r = random32();
      int ox = (int)((r & 7u)) - 3;
      sparks[i].x8 = (int16_t)((cx + ox) << 8);
      sparks[i].y8 = (int16_t)((FELT_Y - 1) << 8);
      sparks[i].vx8 = (int16_t)(((int32_t)((r >> 3) & 0xFFu) - 128) * 3);
      sparks[i].vy8 = (int16_t)(-300 - (int32_t)((r >> 11) & 0xFFu) * 3);
      sparks[i].col_hot = (spawned == 0) ? WHITE : hot;
      sparks[i].col_mid = mid;
      uint8_t lf = (uint8_t)(10u + ((r >> 19) & 7u));
      sparks[i].life = lf;
      sparks[i].max_life = lf;
      spawned++;
    }
  }
}

static void trigger_hit_fx(int k, int is_lh, uint8_t judge) {
  if (k < 0 || k >= KEY_COUNT) {
    return;
  }
  lane_flash_ms[k] = 200u;
  lane_flash_lh[k] = (uint8_t)is_lh;
  spawn_sparks(k, is_lh, 6, judge == JUDGE_PERFECT);
}

static void update_fx(uint32_t dt_ms) {
  for (int k = 0; k < KEY_COUNT; k++) {
    if ((cur_held_mask >> k) & 1u) {
      /* Keep a steady luminous plume while key is held */
      if (lane_flash_ms[k] < 110u) {
        lane_flash_ms[k] = 110u;
      }
      if ((random32() & 7u) == 0u) {
        spawn_sparks(k, lane_flash_lh[k], 1, 0);
      }
    } else if (lane_flash_ms[k] > dt_ms) {
      lane_flash_ms[k] = (uint8_t)(lane_flash_ms[k] - dt_ms);
    } else {
      lane_flash_ms[k] = 0;
    }
  }
  for (int i = 0; i < SPARK_MAX; i++) {
    if (sparks[i].life) {
      sparks[i].x8 = (int16_t)(sparks[i].x8 + sparks[i].vx8);
      sparks[i].y8 = (int16_t)(sparks[i].y8 + sparks[i].vy8);
      /* Keysight upward float + gentle drag */
      sparks[i].vx8 = (int16_t)((sparks[i].vx8 * 15) >> 4);
      sparks[i].vy8 = (int16_t)((sparks[i].vy8 * 15) >> 4);
      sparks[i].life--;
    }
  }
}

/* ---- Drawing Functions */
static void draw_timeline(void) {
  rect(0, 0, SCREEN_W, TIMELINE_H, RGB(12, 16, 24));
  if (song_total_ms > 0 && song_pos_ms > 0) {
    int pw = (int)((song_pos_ms * SCREEN_W) / song_total_ms);
    if (pw > SCREEN_W) {
      pw = SCREEN_W;
    }
    if (pw > 0) {
      rect(0, 0, pw, TIMELINE_H, C_RH_EDGE);
      if (pw >= 2) {
        rect(pw - 2, 0, 2, TIMELINE_H, WHITE);
      }
    }
  }
  if (loop_state >= 1 && song_total_ms > 0) {
    int xa = (int)((loop_a_ms * SCREEN_W) / song_total_ms);
    int xb = loop_state == 2 ? (int)((loop_b_ms * SCREEN_W) / song_total_ms) : xa + 4;
    if (xa < 0) {
      xa = 0;
    }
    if (xb > SCREEN_W) {
      xb = SCREEN_W;
    }
    if (xb > xa) {
      rect(xa, 0, xb - xa, TIMELINE_H, C_FM1_ORANGE);
    }
  }
}

/* ---- 5-Theme Procedural Waterfall, Impact & Keyboard Renderers (Pure Integer) */
static const char *const THEME_SHORT[THEME_COUNT] = {
  "SPACE", "CARTOON", "CYBER", "FANTASY", "ARCADE",
  "TIEDYE", "LAVA", "FLOWER", "MANDALA", "70S"
};

static uint16_t theme_accent_col(void) {
  switch (saved.theme % THEME_COUNT) {
    case THEME_SPACE:    return C_LH_EDGE;
    case THEME_CARTOON:  return C_GOLD;
    case THEME_CYBER:    return RGB(255, 48, 210);
    case THEME_MEDIEVAL: return C_FM1_ORANGE;
    case THEME_ARCADE:   return RGB(190, 56, 255);
    case THEME_TIEDYE:   return RGB(255, 64, 210);
    case THEME_LAVA:     return RGB(140, 255, 48);
    case THEME_FLOWER:   return RGB(96, 232, 112);
    case THEME_MANDALA:  return RGB(178, 108, 255);
    case THEME_GROOVY:   return RGB(242, 168, 42);
    default:             return C_FM1_ORANGE;
  }
}

/* Deterministic 32-starfield for Outer Space */
static const uint8_t STAR_X[32] = {
  14, 38, 62, 88, 118, 146, 174, 206, 228, 22, 54, 78, 108, 134, 166, 198,
  218, 10, 44, 72, 98, 128, 158, 186, 212, 30, 66, 94, 124, 152, 182, 224
};
static const uint8_t STAR_Y[32] = {
  12, 28, 16, 42, 20, 36, 14, 32, 48, 64, 52, 74, 60, 82, 56, 72,
  92, 108, 96, 118, 104, 126, 98, 114, 134, 148, 138, 160, 144, 166, 152, 172
};

static void draw_bg_space(uint32_t now) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(7, 5, 20));
  /* Diagonal cosmic nebula bands */
  for (int band = 0; band < 18; band++) {
    int y = HWY_Y0 + band * 10;
    int nx = 180 - band * 9;
    if (nx < 0) nx = 0;
    int nw = 68;
    if (nx + nw > SCREEN_W) nw = SCREEN_W - nx;
    rect(nx, y, nw, 10, (band & 1) ? RGB(20, 10, 42) : RGB(28, 12, 50));
    if (nx + 14 < SCREEN_W) {
      int iw = nw > 28 ? nw - 28 : 8;
      rect(nx + 14, y + 2, iw, 6, (band & 1) ? RGB(42, 16, 68) : RGB(18, 28, 62));
    }
  }
  /* Subtle lane guides */
  for (int k = 0; k < KEY_COUNT; k++) {
    int semi = k % 12;
    if (semi == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(26, 24, 52));
  }
  /* Twinkling stars */
  uint32_t tw = now >> 8;
  for (int i = 0; i < 32; i++) {
    int sx = STAR_X[i];
    int sy = STAR_Y[i];
    uint16_t sc = ((i + tw) & 3u) == 0 ? WHITE : ((i & 1) ? RGB(140, 180, 255) : RGB(200, 150, 255));
    pixel(sx, sy, sc);
    if ((i & 7) == 0 && ((i + tw) & 1u)) {
      pixel(sx - 1, sy, RGB(80, 110, 190));
      pixel(sx + 1, sy, RGB(80, 110, 190));
      pixel(sx, sy - 1, RGB(80, 110, 190));
      pixel(sx, sy + 1, RGB(80, 110, 190));
    }
  }
}

static void draw_bg_cartoon(uint32_t now) {
  /* Bright sky gradient */
  rect(0, HWY_Y0, SCREEN_W, 64, RGB(135, 218, 246));
  rect(0, HWY_Y0 + 64, SCREEN_W, 66, RGB(172, 232, 250));
  rect(0, HWY_Y0 + 130, SCREEN_W, HWY_H - 130, RGB(206, 244, 252));
  /* Soft drifting fluffy clouds */
  int drift = (int)((now >> 6) % 240u);
  for (int c = 0; c < 4; c++) {
    int cx = ((c * 68 + drift) % 270) - 30;
    int cy = 22 + c * 32;
    int cw = 36 + (c & 1) * 12;
    if (cx + cw > 0 && cx < SCREEN_W) {
      int x0 = cx < 0 ? 0 : cx;
      int w0 = (cx + cw > SCREEN_W ? SCREEN_W : cx + cw) - x0;
      rect(x0, cy + 4, w0, 10, WHITE);
      if (w0 > 8) rect(x0 + 4, cy, w0 - 8, 6, WHITE);
      rect(x0, cy + 12, w0, 2, RGB(214, 236, 246));
    }
  }
  /* Green cartoon hills at bottom above keys */
  rect(0, HWY_Y1 - 16, SCREEN_W, 16, RGB(118, 206, 86));
  rect(0, HWY_Y1 - 17, SCREEN_W, 1, RGB(52, 116, 42));
  /* Subtle vertical guides */
  for (int k = 0; k < KEY_COUNT; k++) {
    if ((k % 12) == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(110, 180, 204));
  }
}

static void draw_bg_cyber(void) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(7, 5, 22));
  /* Subtle horizontal scanlines */
  for (int y = HWY_Y0; y < HWY_Y1; y += 6) {
    rect(0, y, SCREEN_W, 1, RGB(14, 10, 38));
  }
  /* Dark cyberpunk skyline silhouettes */
  static const uint8_t BLD_H[12] = {54, 72, 46, 84, 60, 42, 78, 52, 68, 44, 74, 58};
  for (int i = 0; i < 12; i++) {
    int bx = i * 20;
    int bh = BLD_H[i];
    rect(bx + 1, HWY_Y1 - bh, 18, bh, RGB(12, 16, 40));
    frame(bx + 1, HWY_Y1 - bh, 18, bh, RGB(24, 38, 78));
  }
  /* Perspective synthwave floor grid (lower 42px) */
  for (int gy = HWY_Y1 - 40; gy < HWY_Y1; gy += 8) {
    rect(0, gy, SCREEN_W, 1, RGB(44, 24, 96));
  }
  for (int k = 0; k < KEY_COUNT; k++) {
    int semi = k % 12;
    if (semi == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(34, 48, 94));
    else if (semi == 0) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(20, 26, 56));
  }
}

static void draw_bg_medieval(uint32_t now) {
  /* Stone castle brick wall */
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(48, 36, 28));
  for (int row = 0; row < 16; row++) {
    int y = HWY_Y0 + row * 12;
    if (y >= HWY_Y1) break;
    rect(0, y, SCREEN_W, 1, RGB(24, 18, 14));
    int off = (row & 1) ? 12 : 0;
    for (int x = off; x < SCREEN_W; x += 24) {
      rect(x, y, 1, 12, RGB(24, 18, 14));
      if (((x + row) & 3) == 0 && x + 22 < SCREEN_W) {
        rect(x + 2, y + 2, 20, 8, RGB(58, 44, 34));
      }
    }
  }
  /* Crimson banners & flickering wall torches on left/right edges */
  uint8_t flk = (uint8_t)((now >> 6) & 3u);
  static const uint8_t TY[2] = {54, 132};
  for (int t = 0; t < 2; t++) {
    int y = TY[t];
    /* Left & right torch sconces */
    rect(4, y + 6, 4, 10, RGB(70, 56, 46));
    rect(SCREEN_W - 8, y + 6, 4, 10, RGB(70, 56, 46));
    /* Animated torch flame */
    int fh = 6 + (flk & 1);
    rect(3, y + 6 - fh, 6, fh, C_FM1_ORANGE);
    rect(4, y + 7 - fh, 4, fh - 2, C_GOLD);
    pixel(5, y + 5 - fh, WHITE);
    rect(SCREEN_W - 9, y + 6 - fh, 6, fh, C_FM1_ORANGE);
    rect(SCREEN_W - 8, y + 7 - fh, 4, fh - 2, C_GOLD);
    pixel(SCREEN_W - 6, y + 5 - fh, WHITE);
  }
}

static void draw_bg_arcade(void) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, BLACK);
  /* Hot magenta retro perspective grid */
  for (int y = HWY_Y0 + 16; y < HWY_Y1; y += 22) {
    rect(0, y, SCREEN_W, 1, RGB(96, 18, 138));
  }
  for (int k = 0; k < KEY_COUNT; k++) {
    int semi = k % 12;
    if (semi == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(130, 28, 185));
    else if (semi == 0) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(64, 12, 92));
  }
}

static void draw_bg_tiedye(uint32_t now) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(8, 4, 18));
  static const uint16_t RING_COLS[5] = {
    RGB(38, 10, 48), RGB(12, 34, 48), RGB(14, 42, 22), RGB(44, 28, 10), RGB(48, 12, 34)
  };
  int shift = (int)((now >> 7) % 5u);
  int cx = 120, cy = 96;
  for (int r = 7; r >= 1; r--) {
    int sz = r * 14;
    uint16_t col = RING_COLS[(r + shift) % 5];
    frame(cx - sz, cy - sz, sz * 2, sz * 2, col);
    frame(cx - sz + 2, cy - sz + 2, sz * 2 - 4, sz * 2 - 4, col);
  }
  for (int k = 0; k < KEY_COUNT; k++) {
    if ((k % 12) == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(28, 20, 48));
  }
}

static void draw_bg_lava(uint32_t now) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(18, 8, 28));
  /* Subtle warm bottom glow */
  rect(0, HWY_Y1 - 24, SCREEN_W, 24, RGB(26, 12, 38));
  /* Rising micro-bubbles */
  int rise = (int)((now >> 5) % (uint32_t)HWY_H);
  for (int i = 0; i < 20; i++) {
    int bx = STAR_X[i];
    int by = HWY_Y0 + ((STAR_Y[i] + HWY_H - rise) % HWY_H);
    uint16_t bc = (i & 1) ? RGB(54, 96, 118) : RGB(84, 52, 112);
    if ((i & 3) == 0 && by < HWY_Y1 - 3) {
      frame(bx, by, 4, 4, bc);
    } else {
      pixel(bx, by, bc);
    }
  }
}

static void draw_bg_flower(void) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(10, 26, 16));
  /* Subtle retro floral wallpaper silhouettes */
  uint16_t fcol = RGB(16, 40, 24);
  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 5; col++) {
      int fx = 20 + col * 48 + ((row & 1) ? 20 : 0);
      int fy = 22 + row * 36;
      if (fx < SCREEN_W - 10 && fy < HWY_Y1 - 10) {
        rect(fx - 5, fy - 2, 11, 5, fcol);
        rect(fx - 2, fy - 5, 5, 11, fcol);
        rect(fx - 1, fy - 1, 3, 3, RGB(28, 52, 26));
      }
    }
  }
  for (int k = 0; k < KEY_COUNT; k++) {
    if ((k % 12) == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(20, 52, 30));
  }
}

static void draw_bg_mandala(uint32_t now) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(14, 6, 32));
  int cx = 120, cy = 96;
  uint16_t m1 = RGB(48, 22, 86);
  uint16_t m2 = RGB(74, 34, 124);
  /* Concentric sacred-geometry squares & rotated diamonds */
  for (int r = 18; r <= 78; r += 20) {
    frame(cx - r, cy - r, r * 2, r * 2, m1);
    line(cx, cy - r, cx + r, cy, m2);
    line(cx + r, cy, cx, cy + r, m2);
    line(cx, cy + r, cx - r, cy, m2);
    line(cx - r, cy, cx, cy - r, m2);
  }
  /* Subtle golden stardust */
  uint32_t tw = now >> 8;
  for (int i = 0; i < 16; i++) {
    if (((i + tw) & 1u) == 0) {
      pixel(STAR_X[i], STAR_Y[i], C_GOLD);
    }
  }
}

static void draw_bg_groovy(void) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, RGB(28, 18, 11));
  /* Iconic 1970s layered retro stripes along left & right sides */
  static const uint16_t GCOLS[4] = {
    RGB(110, 44, 20), RGB(138, 84, 22), RGB(92, 98, 26), RGB(26, 86, 84)
  };
  for (int s = 0; s < 4; s++) {
    int y = 62 + s * 10;
    rect(0, y, 44 - s * 6, 8, GCOLS[s]);
    rect(44 - s * 6, y, 8, HWY_Y1 - y, GCOLS[s]);
    int rx = SCREEN_W - (44 - s * 6);
    rect(rx, y - 24, 44 - s * 6, 8, GCOLS[s]);
    rect(rx - 8, y - 24, 8, HWY_Y1 - (y - 24), GCOLS[s]);
  }
  for (int k = 0; k < KEY_COUNT; k++) {
    if ((k % 12) == 7) rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, RGB(58, 40, 26));
  }
}

static void draw_themed_note(int x, int y_top, int w, int h, int is_lh, int blk, uint8_t flags, uint32_t now) {
  int y_bot = y_top + h;
  int theme = saved.theme % THEME_COUNT;
  uint8_t pulse_on = ((now >> 7) & 1u) != 0;
  int is_wait = (last_judge == JUDGE_WAIT && !(flags & NF_DONE) && note_is_player_hand(flags));
  int is_hit = (flags & NF_HIT) != 0;
  int is_miss = (flags & NF_MISSED) != 0;

  if (is_miss) {
    rect(x, y_top, w, h, C_MISS_CORE);
    frame(x, y_top, w, h, C_MISS_EDGE);
    return;
  }

  if (theme == THEME_SPACE) {
    /* Falling Comet: tapered flickering plasma tail + glowing icy/emerald head at y_bot */
    uint16_t edge = is_wait ? (pulse_on ? WHITE : C_GOLD) : (is_lh ? C_LH_EDGE : C_RH_EDGE);
    uint16_t hot  = is_wait ? WHITE : (is_lh ? C_LH_HOT : C_RH_HOT);
    uint16_t dim  = is_lh ? C_LH_CORE : C_RH_CORE;
    int head_h = h < 9 ? h : 9;
    int tail_h = h - head_h;
    if (tail_h > 0) {
      int flick = (int)(((now >> 5) + x) & 1u);
      int tw = w > 4 ? w - 4 : 2;
      rect(x + ((w - tw) >> 1), y_top, tw, tail_h, dim);
      if (tail_h > 4) {
        rect(x + ((w - 2) >> 1), y_top + 2 + flick, 2, tail_h - 2 - flick, edge);
      }
    }
    int hy = y_bot - head_h;
    rect(x + 1, hy, w - 2, head_h, edge);
    rect(x, hy + 1, w, head_h - 2, edge);
    if (w >= 5 && head_h >= 5) {
      rect(x + 2, hy + 2, w - 4, head_h - 4, is_hit ? WHITE : hot);
    }
    return;
  }

  if (theme == THEME_CARTOON) {
    /* Glossy cartoon jelly capsule with black ink outline & tiny cute face */
    uint16_t body = is_wait ? C_GOLD : (is_lh ? (blk ? RGB(158, 86, 232) : RGB(58, 168, 248)) : (blk ? RGB(255, 186, 36) : RGB(84, 212, 54)));
    if (is_hit) body = WHITE;
    rect(x + 1, y_top + 1, w - 2, h - 2, body);
    /* Crisp black cartoon ink outline */
    rect(x + 1, y_top, w - 2, 1, BLACK);
    rect(x + 1, y_bot - 1, w - 2, 1, BLACK);
    rect(x, y_top + 1, 1, h - 2, BLACK);
    rect(x + w - 1, y_top + 1, 1, h - 2, BLACK);
    /* Specular shine strip */
    if (h >= 7 && w >= 6) {
      rect(x + 2, y_top + 2, 1, h - 4, WHITE);
    }
    /* Cute 2-dot cartoon eyes + smile near bottom/top on taller notes */
    if (h >= 12 && w >= 7) {
      int fy = y_bot - 8;
      pixel(x + 2, fy, BLACK);
      pixel(x + w - 3, fy, BLACK);
      rect(x + 3, fy + 2, w - 6 > 1 ? w - 6 : 2, 1, BLACK);
    }
    return;
  }

  if (theme == THEME_CYBER) {
    /* Cyberpunk Laser Trail + Glowing Square Neon Core */
    uint16_t neon = is_wait ? C_GOLD : (is_lh ? RGB(255, 42, 212) : RGB(0, 234, 255));
    uint16_t hot  = is_lh ? RGB(255, 190, 245) : RGB(185, 252, 255);
    int head_h = h < 9 ? h : 9;
    int tail_h = h - head_h;
    if (tail_h > 0) {
      int cx = x + (w >> 1);
      rect(cx - 1, y_top, 3, tail_h, neon);
      rect(cx, y_top, 1, tail_h, WHITE);
    }
    int hy = y_bot - head_h;
    rect(x, hy, w, head_h, is_lh ? RGB(42, 10, 48) : RGB(8, 34, 48));
    frame(x, hy, w, head_h, is_hit ? WHITE : neon);
    if (w >= 5 && head_h >= 5) {
      rect(x + 2, hy + 2, w - 4, head_h - 4, hot);
    }
    return;
  }

  if (theme == THEME_MEDIEVAL) {
    if (!is_lh) {
      /* RH: Glowing Pixel Sword (steel-cyan blade + golden crossguard + pommel) */
      int cx = x + (w >> 1);
      uint16_t blade = is_wait ? C_GOLD : (is_hit ? WHITE : RGB(120, 224, 255));
      int guard_y = y_top + 4;
      if (guard_y > y_bot - 4) guard_y = y_top + 1;
      /* Pommel & hilt */
      rect(cx - 1, y_top, 3, guard_y - y_top, C_FM1_ORANGE);
      /* Golden crossguard */
      rect(x, guard_y, w, 2, C_GOLD);
      /* Glowing blade extending down to y_bot */
      int bh = y_bot - (guard_y + 2);
      if (bh > 0) {
        rect(cx - 2, guard_y + 2, 5 <= w ? 5 : w, bh, blade);
        rect(cx, guard_y + 2, 1, bh, WHITE);
      }
    } else {
      /* LH: Magic Rune Scroll (parchment + wooden rollers + glowing cyan rune) */
      uint16_t parch = is_wait ? C_GOLD : RGB(224, 198, 146);
      rect(x + 1, y_top + 1, w - 2, h - 2, parch);
      rect(x, y_top, w, 2, RGB(128, 82, 42));
      rect(x, y_bot - 2, w, 2, RGB(128, 82, 42));
      if (h >= 8 && w >= 6) {
        int cy = y_top + (h >> 1);
        int cx = x + (w >> 1);
        rect(cx - 1, cy - 2, 3, 5, is_hit ? WHITE : C_LH_EDGE);
      }
    }
    return;
  }

  if (theme == THEME_TIEDYE) {
    /* Blacklight Neon Border + Animated Psychedelic Tie-Dye Stripes */
    static const uint16_t TD[6] = {
      RGB(255, 42, 190), RGB(255, 190, 24), RGB(56, 245, 96),
      RGB(32, 224, 255), RGB(148, 64, 255), RGB(255, 72, 72)
    };
    int phase = (int)((now >> 6) + (uint32_t)x) % 6;
    for (int py = y_top + 1; py < y_bot - 1; py += 2) {
      int sh = (py - y_top) / 2;
      uint16_t c = TD[(sh + phase) % 6];
      rect(x + 1, py, w - 2, (py + 2 <= y_bot - 1) ? 2 : 1, c);
    }
    frame(x, y_top, w, h, is_hit ? WHITE : (is_lh ? C_LH_EDGE : C_RH_EDGE));
    return;
  }

  if (theme == THEME_LAVA) {
    /* Glossy Pinched-Waist Lava Lamp Blob */
    uint16_t body = is_wait ? C_GOLD : (is_lh ? RGB(28, 178, 255) : RGB(152, 242, 32));
    uint16_t edge = is_hit ? WHITE : (is_lh ? C_LH_HOT : RGB(220, 255, 140));
    uint16_t deep = is_lh ? RGB(10, 96, 176) : RGB(84, 154, 12);
    if (h >= 12 && w >= 6) {
      /* Bulbous top & bottom with pinched waist in middle */
      int mid_y = y_top + (h >> 1);
      rect(x + 2, y_top + 3, w - 4, h - 6, body);
      rect(x + 1, y_top + 1, w - 2, 5, body);
      rect(x + 1, y_bot - 6, w - 2, 5, body);
      rect(x + 2, y_top, w - 4, 1, edge);
      rect(x + 2, y_bot - 1, w - 4, 1, deep);
      /* Specular liquid gloss dot */
      rect(x + 2, y_top + 2, 2, 3, WHITE);
      pixel(x + 2, mid_y, edge);
    } else {
      rect(x + 1, y_top, w - 2, h, body);
      rect(x, y_top + 1, w, h - 2, body);
      pixel(x + 2, y_top + 1, WHITE);
    }
    return;
  }

  if (theme == THEME_FLOWER) {
    /* Glossy Stem Bar capped with a Pixel White-and-Gold Daisy at leading edge */
    uint16_t stem = is_wait ? C_GOLD : (is_lh ? RGB(38, 168, 246) : RGB(56, 212, 82));
    uint16_t rim  = is_hit ? WHITE : (is_lh ? C_LH_HOT : C_RH_HOT);
    rect(x + 1, y_top, w - 2, h, stem);
    frame(x + 1, y_top, w - 2, h, rim);
    /* Daisy blossom at y_bot */
    int cx = x + (w >> 1);
    int cy = y_bot - 4;
    if (cy < y_top + 3) cy = y_top + (h >> 1);
    rect(cx - 3, cy - 1, 7 <= w ? 7 : w, 3, WHITE);
    rect(cx - 1, cy - 3, 3, 7 <= h ? 7 : h, WHITE);
    rect(cx - 1, cy - 1, 3, 3, C_GOLD);
    return;
  }

  if (theme == THEME_MANDALA) {
    /* Pointed Prismatic Crystal Shard */
    uint16_t c1 = is_wait ? C_GOLD : (is_lh ? RGB(42, 218, 255) : RGB(48, 240, 168));
    uint16_t c2 = is_lh ? RGB(164, 88, 255) : RGB(248, 224, 92);
    int cx = x + (w >> 1);
    if (h >= 10 && w >= 5) {
      rect(x + 1, y_top + 2, w - 2, h - 4, c1);
      rect(cx - 1, y_top, 3, h, c2);
      frame(x + 1, y_top + 2, w - 2, h - 4, is_hit ? WHITE : C_GOLD);
      pixel(cx, y_top, WHITE);
      pixel(cx, y_bot - 1, WHITE);
      rect(cx, y_top + 3, 1, h - 6, WHITE);
    } else {
      rect(x + 1, y_top, w - 2, h, c1);
      frame(x + 1, y_top, w - 2, h, C_GOLD);
    }
    return;
  }

  if (theme == THEME_GROOVY) {
    /* 70s Retro Glossy Pill (Avocado/Gold for RH, Turquoise/Amber for LH) */
    uint16_t outer = is_wait ? C_GOLD : (is_lh ? RGB(34, 198, 186) : RGB(156, 206, 36));
    uint16_t inner = is_lh ? RGB(246, 178, 42) : RGB(248, 210, 56);
    rect(x + 1, y_top, w - 2, h, outer);
    rect(x, y_top + 1, w, h - 2, outer);
    if (w >= 6 && h >= 6) {
      rect(x + 2, y_top + 2, w - 4, h - 4, is_hit ? WHITE : inner);
      rect(x + 2, y_top + 2, 1, h - 4, WHITE);
    }
    frame(x, y_top + 1, w, h - 2, RGB(58, 32, 14));
    return;
  }

  /* THEME_ARCADE: RH = Faceted Pixel Gem, LH = Classic 8-bit Brick Stack */
  if (!is_lh) {
    uint16_t gem = is_wait ? C_GOLD : (blk ? RGB(255, 160, 24) : RGB(245, 56, 140));
    uint16_t hi  = is_hit ? WHITE : RGB(255, 200, 230);
    rect(x + 1, y_top, w - 2, h, gem);
    rect(x, y_top + 1, w, h - 2, gem);
    if (w >= 5 && h >= 5) {
      rect(x + 2, y_top + 2, w - 4, h - 4, hi);
      rect(x + 3, y_top + 3, w - 6 > 1 ? w - 6 : 1, h - 6 > 1 ? h - 6 : 1, gem);
    }
  } else {
    uint16_t brk = blk ? RGB(56, 224, 96) : RGB(236, 76, 54);
    rect(x, y_top, w, h, brk);
    frame(x, y_top, w, h, is_hit ? WHITE : RGB(255, 200, 140));
    for (int by = y_top + 3; by < y_bot - 1; by += 4) {
      rect(x, by, w, 1, BLACK);
    }
  }
}

static void draw_themed_impacts(uint32_t now) {
  int theme = saved.theme % THEME_COUNT;
  for (int k = 0; k < KEY_COUNT; k++) {
    uint8_t fl = lane_flash_ms[k];
    if (fl == 0) continue;
    int is_lh = lane_flash_lh[k];
    int cx = key_x[k] + (key_w[k] >> 1);
    int r = (fl * 14) / 200;
    if (r < 3) r = 3;

    if (theme == THEME_SPACE) {
      /* Supernova starburst flash on impact */
      uint16_t col = is_lh ? C_LH_EDGE : C_GOLD;
      line(cx - r, HWY_Y1 - 4, cx + r, HWY_Y1 - 4, col);
      line(cx, HWY_Y1 - 4 - r, cx, HWY_Y1 - 2, WHITE);
      int d = (r * 7) / 10;
      line(cx - d, HWY_Y1 - 4 - d, cx + d, HWY_Y1 - 4 + (d >> 1), is_lh ? C_LH_HOT : C_FM1_ORANGE);
      line(cx + d, HWY_Y1 - 4 - d, cx - d, HWY_Y1 - 4 + (d >> 1), is_lh ? C_LH_HOT : C_FM1_ORANGE);
      rect(cx - 2, HWY_Y1 - 6, 5, 5, WHITE);
    } else if (theme == THEME_CARTOON) {
      /* Comic jagged starburst + POW! on big hits */
      rect(cx - r, HWY_Y1 - r - 2, r * 2, r + 1, C_GOLD);
      frame(cx - r, HWY_Y1 - r - 2, r * 2, r + 1, BLACK);
      if (r >= 6) {
        rect(cx - r + 2, HWY_Y1 - r, (r - 2) * 2, r - 3, C_FM1_ORANGE);
        if (fl > 130 && cx > 18 && cx < SCREEN_W - 22) {
          text(cx - 12, HWY_Y1 - 15, is_lh ? "BAM" : "POW", BLACK, FONT_SMALL);
        }
      }
    } else if (theme == THEME_CYBER) {
      /* Glitch bars + neon X lightning */
      uint16_t col = is_lh ? RGB(255, 42, 212) : RGB(0, 234, 255);
      line(cx - r, HWY_Y1 - r, cx + r, HWY_Y1 - 1, col);
      line(cx + r, HWY_Y1 - r, cx - r, HWY_Y1 - 1, WHITE);
      int gx = (int)(((now >> 4) & 3u) - 1) * 3;
      rect(cx - r + gx, HWY_Y1 - 8, r * 2, 2, RGB(255, 42, 212));
      rect(cx - r - gx, HWY_Y1 - 4, r * 2, 2, RGB(0, 234, 255));
    } else if (theme == THEME_MEDIEVAL) {
      /* Arcane magic rune ring */
      uint16_t col = is_lh ? RGB(178, 96, 255) : C_GOLD;
      frame(cx - r, HWY_Y1 - r - 3, r * 2, r + 2, col);
      if (r >= 5) {
        frame(cx - r + 2, HWY_Y1 - r - 1, (r - 2) * 2, r - 2, WHITE);
      }
    } else if (theme == THEME_TIEDYE) {
      /* Rainbow Peace-Sign Impact Burst */
      uint16_t pc = is_lh ? C_LH_EDGE : RGB(255, 64, 210);
      int cy = HWY_Y1 - r - 2;
      frame(cx - r, cy - r, r * 2, r * 2, pc);
      line(cx, cy - r, cx, cy + r, WHITE);
      line(cx, cy, cx - (r * 7) / 10, cy + (r * 7) / 10, C_GOLD);
      line(cx, cy, cx + (r * 7) / 10, cy + (r * 7) / 10, C_RH_EDGE);
    } else if (theme == THEME_LAVA) {
      /* Liquid Splash Crown + Concentric Ripples */
      uint16_t lc = is_lh ? C_LH_EDGE : RGB(152, 242, 32);
      rect(cx - r, HWY_Y1 - 3, r * 2, 2, lc);
      line(cx, HWY_Y1 - 2, cx - (r * 8) / 10, HWY_Y1 - r - 2, WHITE);
      line(cx, HWY_Y1 - 2, cx + (r * 8) / 10, HWY_Y1 - r - 2, lc);
      rect(cx - 2, HWY_Y1 - r, 4, r, lc);
    } else if (theme == THEME_FLOWER) {
      /* Blooming White-Petal Daisy Burst */
      int cy = HWY_Y1 - r - 1;
      rect(cx - r, cy - 2, r * 2, 5, WHITE);
      rect(cx - 2, cy - r, 5, r * 2, WHITE);
      rect(cx - 2, cy - 2, 5, 5, C_GOLD);
    } else if (theme == THEME_MANDALA) {
      /* Concentric Sacred-Geometry Neon Rings */
      int cy = HWY_Y1 - r - 1;
      frame(cx - r, cy - r, r * 2, r * 2, RGB(178, 108, 255));
      if (r >= 5) {
        int r2 = r - 3;
        frame(cx - r2, cy - r2, r2 * 2, r2 * 2, C_LH_EDGE);
        line(cx - r, cy, cx + r, cy, WHITE);
        line(cx, cy - r, cx, cy + r, C_GOLD);
      }
    } else if (theme == THEME_GROOVY) {
      /* Warm 70s Golden Starburst Lens Flare */
      line(cx - r - 2, HWY_Y1 - 4, cx + r + 2, HWY_Y1 - 4, C_FM1_ORANGE);
      line(cx, HWY_Y1 - 4 - r, cx, HWY_Y1 - 1, C_GOLD);
      int d = (r * 7) / 10;
      line(cx - d, HWY_Y1 - 4 - d, cx + d, HWY_Y1 - 4 + (d >> 1), C_GOLD);
      line(cx + d, HWY_Y1 - 4 - d, cx - d, HWY_Y1 - 4 + (d >> 1), C_GOLD);
      rect(cx - 2, HWY_Y1 - 6, 5, 5, WHITE);
    } else {
      /* Retro 8-bit square pixel burst */
      uint16_t col = is_lh ? C_LH_EDGE : C_GOLD;
      rect(cx - r, HWY_Y1 - r, 3, 3, col);
      rect(cx + r - 2, HWY_Y1 - r, 3, 3, RGB(255, 56, 180));
      rect(cx - (r >> 1), HWY_Y1 - r - 4, 3, 3, WHITE);
      rect(cx + (r >> 1), HWY_Y1 - (r >> 1) - 2, 3, 3, C_FM1_ORANGE);
    }
  }
}

static void draw_strike_line(void) {
  int theme = saved.theme % THEME_COUNT;
  uint16_t rail_bg = theme == THEME_CARTOON ? RGB(52, 116, 42) : theme == THEME_ARCADE ? BLACK : RGB(14, 18, 28);
  uint16_t rail_ln = theme_accent_col();
  if (theme == THEME_CARTOON) rail_ln = BLACK;
  rect(0, FELT_Y, SCREEN_W, 3, rail_bg);
  rect(0, FELT_Y + 1, SCREEN_W, 1, rail_ln);
  for (int k = 0; k < KEY_COUNT; k++) {
    uint8_t fl = lane_flash_ms[k];
    if (fl > 0) {
      int is_lh = lane_flash_lh[k];
      uint16_t edge = is_lh ? C_LH_EDGE : C_RH_EDGE;
      uint16_t hot  = fl > 130 ? WHITE : (is_lh ? C_LH_HOT : C_RH_HOT);
      int bx = key_x[k] - 3;
      int bw = key_w[k] + 6;
      if (bx < 0) bx = 0;
      if (bx + bw > SCREEN_W) bw = SCREEN_W - bx;
      rect(bx, FELT_Y, bw, 3, edge);
      rect(key_x[k], FELT_Y, key_w[k], 3, hot);
    }
  }
}

static void draw_keyboard(uint32_t held_mask, uint32_t rh_cue, uint32_t lh_cue) {
  int theme = saved.theme % THEME_COUNT;
  draw_strike_line();
  rect(0, KB_Y, SCREEN_W, KB_WHITE_H, BLACK);

  uint16_t w_top = (theme == THEME_MEDIEVAL || theme == THEME_MANDALA) ? RGB(218, 190, 142) : theme == THEME_GROOVY ? RGB(204, 164, 108) : theme == THEME_CYBER ? RGB(150, 162, 188) : C_KEY_W_TOP;
  uint16_t w_mid = (theme == THEME_MEDIEVAL || theme == THEME_MANDALA) ? RGB(190, 158, 110) : theme == THEME_GROOVY ? RGB(164, 124, 76)  : theme == THEME_CYBER ? RGB(114, 126, 154) : C_KEY_W_MID;
  uint16_t w_bot = (theme == THEME_MEDIEVAL || theme == THEME_MANDALA) ? RGB(144, 114, 74)  : theme == THEME_GROOVY ? RGB(110, 76, 44)   : theme == THEME_CYBER ? RGB(74, 84, 108)   : C_KEY_W_BOT;

  /* 16 White keys */
  for (int k = 0; k < KEY_COUNT; k++) {
    if (!key_black[k]) {
      int held = (held_mask >> k) & 1u;
      int rc = (rh_cue >> k) & 1u;
      int lc = (lh_cue >> k) & 1u;
      int fl = lane_flash_ms[k];
      int x = key_x[k], w = key_w[k];
      if (theme == THEME_ARCADE && !held && !rc && !lc && fl <= 70) {
        rect(x, KB_Y, w, KB_WHITE_H, BLACK);
        frame(x, KB_Y, w, KB_WHITE_H, RGB(140, 36, 200));
      } else if (held || fl > 70) {
        int is_lh = (lc && !rc) || (fl > 0 && lane_flash_lh[k] && !rc);
        rect(x, KB_Y, w, 6, WHITE);
        rect(x, KB_Y + 6, w, 14, is_lh ? C_LH_HOT : C_RH_HOT);
        rect(x, KB_Y + 20, w, KB_WHITE_H - 24, is_lh ? C_LH_EDGE : C_RH_EDGE);
        rect(x, KB_Y + KB_WHITE_H - 4, w, 4, is_lh ? C_LH_CORE : C_RH_CORE);
      } else if (rc || lc) {
        rect(x, KB_Y, w, 10, rc ? C_RH_HOT : C_LH_HOT);
        rect(x, KB_Y + 10, w, KB_WHITE_H - 14, rc ? C_RH_EDGE : C_LH_EDGE);
        rect(x, KB_Y + KB_WHITE_H - 4, w, 4, rc ? C_RH_CORE : C_LH_CORE);
      } else {
        rect(x, KB_Y, w, 18, w_top);
        rect(x, KB_Y + 18, w, KB_WHITE_H - 22, w_mid);
        rect(x, KB_Y + KB_WHITE_H - 4, w, 4, w_bot);
      }
    }
  }
  /* 11 Black keys */
  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_black[k]) {
      int held = (held_mask >> k) & 1u;
      int rc = (rh_cue >> k) & 1u;
      int lc = (lh_cue >> k) & 1u;
      int fl = lane_flash_ms[k];
      int x = key_x[k], w = key_w[k];
      rect(x - 1, KB_Y, w + 2, KB_BLACK_H + 1, BLACK);
      if (held || fl > 70) {
        int is_lh = (lc && !rc) || (fl > 0 && lane_flash_lh[k] && !rc);
        rect(x, KB_Y, w, 6, WHITE);
        rect(x, KB_Y + 6, w, KB_BLACK_H - 6, is_lh ? C_LH_EDGE : C_RH_EDGE);
        frame(x, KB_Y, w, KB_BLACK_H, is_lh ? C_LH_HOT : C_RH_HOT);
      } else if (rc || lc) {
        rect(x, KB_Y, w, KB_BLACK_H, rc ? C_RH_CORE : C_LH_CORE);
        frame(x, KB_Y, w, KB_BLACK_H, rc ? C_RH_EDGE : C_LH_EDGE);
        rect(x + 1, KB_Y + KB_BLACK_H - 4, w - 2, 3, rc ? C_RH_HOT : C_LH_HOT);
      } else if (theme == THEME_ARCADE) {
        rect(x, KB_Y, w, KB_BLACK_H, BLACK);
        frame(x, KB_Y, w, KB_BLACK_H, RGB(210, 56, 255));
      } else {
        rect(x, KB_Y, w, KB_BLACK_H, theme == THEME_MEDIEVAL ? RGB(42, 28, 18) : C_KEY_B_BODY);
        frame(x, KB_Y, w, KB_BLACK_H, theme == THEME_MEDIEVAL ? RGB(92, 66, 44) : C_KEY_B_HI);
        rect(x + 1, KB_Y + KB_BLACK_H - 3, w - 2, 2, RGB(78, 90, 114));
      }
    }
  }
}

static void draw_finish_overlay(uint32_t now) {
  rect(0, HWY_Y0, SCREEN_W, HWY_H, C_BG_VOID);

  uint32_t total = (uint32_t)hits_perfect + hits_good + hits_missed;
  uint32_t pct = total ? ((uint32_t)(hits_perfect * 100u + hits_good * 70u) / total) : 100u;
  const char *grade = pct >= 95 ? "S" : pct >= 85 ? "A" : pct >= 70 ? "B" : "C";
  uint16_t gcol = pct >= 95 ? C_GOLD : pct >= 85 ? C_RH_EDGE : C_LH_EDGE;

  int32_t el = (int32_t)(now - finish_start_ms);
  int32_t slide = ease_out_quad(el, 160);
  int y0 = 28 + (((256 - slide) * 16) >> 8);

  draw_outlined_box(12, y0, 216, 132, C_SURF_ELEV, gcol, WHITE);
  rect(13, y0 + 1, 214, 18, C_SURF_RECESS);
  rect(13, y0 + 19, 214, 1, C_OUTLINE_IDLE);

  const char *hdr = "RECITAL COMPLETE";
  text((SCREEN_W - text_width(hdr, FONT_SMALL)) / 2, y0 + 2, hdr, gcol, FONT_SMALL);

  char buf[28];
  if (el >= 80) {
    str_cat_int(str_cat(buf, "SCORE:  "), (int32_t)score);
    text(84, y0 + 28, buf, C_TEXT_PRI, FONT_SMALL);
  }
  if (el >= 180) {
    str_cat_int(str_cat(buf, "COMBO:  "), max_combo);
    text(84, y0 + 46, buf, C_RH_EDGE, FONT_SMALL);
  }
  if (el >= 280) {
    str_cat(str_cat_int(str_cat(buf, "ACCUR:  "), (int32_t)pct), "%");
    text(84, y0 + 64, buf, C_LH_EDGE, FONT_SMALL);
  }
  if (el >= 380) {
    str_cat_int(str_cat(buf, "BEST:   "), (int32_t)saved.high_score[saved.song_idx]);
    text(84, y0 + 82, buf, C_GOLD, FONT_SMALL);
  }
  if (el >= 400) {
    int pop = (el < 540) ? 2 : 0;
    draw_outlined_box(24 - pop, y0 + 30 - pop, 48 + pop * 2, 58 + pop * 2, C_SURF_RECESS, pop ? WHITE : gcol, gcol);
    text(32, y0 + 34, "RANK", C_TEXT_DIM, FONT_SMALL);
    text(40, y0 + 50, grade, gcol, FONT_BIG);
  }

  const char *foot = "PLAY:RETRY  GLO:MENU";
  text((SCREEN_W - text_width(foot, FONT_SMALL)) / 2, y0 + 110, foot, C_TEXT_DIM, FONT_SMALL);
}

static void draw_floating_judge(uint32_t now) {
  if (now >= judge_until_ms || last_judge < JUDGE_PERFECT) {
    return;
  }
  const char *jstr = "";
  uint16_t jcol = C_TEXT_PRI;
  switch (last_judge) {
    case JUDGE_PERFECT: jstr = "PERFECT!"; jcol = C_GOLD; break;
    case JUDGE_GREAT:   jstr = "GREAT!";   jcol = C_RH_EDGE; break;
    case JUDGE_GOOD:    jstr = "GOOD";     jcol = C_LH_EDGE; break;
    case JUDGE_MISS:    jstr = "MISS";     jcol = C_CRIMSON; break;
    default: return;
  }
  int32_t el = (int32_t)(now - judge_start_ms);
  int32_t ease = ease_out_quad(el, 320);
  int jy = (HWY_Y1 - 30) - ((ease * 18) >> 8);
  char jbuf[24];
  char *jp = str_cat(jbuf, jstr);
  if (combo > 1 && last_judge != JUDGE_MISS) {
    jp = str_cat(jp, " x");
    str_cat_int(jp, combo);
  }
  int jw = text_width(jbuf, FONT_SMALL) + 12;
  int jx = (SCREEN_W - jw) >> 1;
  rect(jx, jy, jw, 16, C_SURF_RECESS);
  frame(jx, jy, jw, 16, jcol);
  text(jx + 6, jy + 1, jbuf, jcol, FONT_SMALL);
}

static void draw_knob_popup(uint32_t now) {
  if (now >= popup_end_ms) {
    return;
  }
  int32_t anim = 256;
  int32_t since = (int32_t)(now - popup_start_ms);
  int32_t rem = (int32_t)(popup_end_ms - now);
  if (since < 120) {
    anim = ease_out_quad(since, 120);
  } else if (rem < 120) {
    anim = ease_out_quad(rem, 120);
  }
  int pw = 196, ph = 28;
  int px = (SCREEN_W - pw) >> 1;
  int py = (HWY_Y0 - ph) + (((ph + 8) * anim) >> 8);
  if (py + ph <= HWY_Y0) {
    return;
  }
  draw_outlined_box(px, py, pw, ph, C_SURF_ELEV, popup_col, WHITE);
  text(px + 8, py + 3, popup_label, C_TEXT_DIM, FONT_SMALL);
  text(px + pw - text_width(popup_val, FONT_SMALL) - 8, py + 3, popup_val, C_TEXT_PRI, FONT_SMALL);

  int bx = px + 8, by = py + 20, bw = pw - 16;
  rect(bx, by, bw, 4, C_BG_VOID);
  int fw = (bw * popup_pct) / 100;
  if (fw > 0) {
    rect(bx, by, fw, 4, popup_col);
  }
}

static void draw_song_browser(uint32_t now) {
  if (now >= browser_until_ms || menu_open) {
    return;
  }
  int32_t since = (int32_t)(now - browser_start_ms);
  int32_t rem = (int32_t)(browser_until_ms - now);
  int32_t anim = since < 120 ? ease_out_quad(since, 120) : (rem < 120 ? ease_out_quad(rem, 120) : 256);

  int bw = 212, bh = 98;
  int bx = (SCREEN_W - bw) >> 1;
  int by = HWY_Y0 + 40 + (((256 - anim) * 16) >> 8);
  draw_outlined_box(bx, by, bw, bh, C_SURF_ELEV, C_LH_EDGE, WHITE);
  rect(bx + 1, by + 1, bw - 2, 16, C_SURF_RECESS);
  text(bx + 6, by + 1, "SONG SELECT", C_LH_EDGE, FONT_SMALL);

  char cnt_buf[12];
  char *cp = str_cat_int(cnt_buf, saved.song_idx + 1);
  cp = str_cat(cp, "/");
  str_cat_int(cp, TOTAL_SONGS);
  text(bx + bw - text_width(cnt_buf, FONT_SMALL) - 6, by + 1, cnt_buf, C_TEXT_DIM, FONT_SMALL);

  for (int row = -2; row <= 2; row++) {
    int idx = (saved.song_idx + row + TOTAL_SONGS) % TOTAL_SONGS;
    int ry = by + 19 + (row + 2) * 15;
    int sel = (row == 0);
    if (sel) {
      rect(bx + 4, ry, bw - 8, 15, C_SURF_RECESS);
      frame(bx + 4, ry, bw - 8, 15, C_LH_EDGE);
      rect(bx + 4, ry, 3, 15, C_RH_EDGE);
    }
    const char *t = (idx == CUSTOM_SLOT) ? (saved.custom_title[0] ? saved.custom_title : "CUSTOM .MID") : BUILTIN_SONGS[idx].title;
    uint16_t bpm = (idx == CUSTOM_SLOT) ? (saved.custom_bpm ? saved.custom_bpm : 120) : BUILTIN_SONGS[idx].bpm;
    char lbuf[24];
    char *lp = str_cat_int(lbuf, idx + 1);
    lp = str_cat(lp, ".");
    str_cat(lp, t);
    if (text_width(lbuf, FONT_SMALL) > 146) {
      lbuf[18] = 0;
    }
    text(bx + 10, ry, lbuf, sel ? C_TEXT_PRI : C_TEXT_DIM, FONT_SMALL);
    char bbuf[8];
    format_int(bbuf, bpm);
    text(bx + bw - text_width(bbuf, FONT_SMALL) - 8, ry, bbuf, sel ? C_GOLD : C_OUTLINE_IDLE, FONT_SMALL);
  }
}

/* ---- GLO Modal Menu Window (with 5-Theme Selector!) */
static void draw_glo_menu(uint32_t now) {
  static const char *const MODE_NAMES[MODE_COUNT] = {"WAIT", "GAME", "AUTO"};
  static const char *const HAND_NAMES[HAND_COUNT] = {"BOTH", "RH", "LH"};
  static const char *const SHORT_PATCH[PATCH_COUNT] = {"GRAND", "E-PNO", "CHIP", "BELLS", "BRASS", "PAD"};

  int32_t anim = ease_out_quad((int32_t)(now - menu_open_ms), 140);
  int mw = 218, mh = 180;
  int mx = (SCREEN_W - mw) >> 1;
  int my = 4 + (((256 - anim) * 14) >> 8);
  uint16_t acc = theme_accent_col();

  draw_outlined_box(mx, my, mw, mh, C_SURF_ELEV, acc, WHITE);
  rect(mx + 1, my + 1, mw - 2, 17, C_SURF_RECESS);
  rect(mx + 1, my + 18, mw - 2, 1, C_OUTLINE_IDLE);
  text(mx + 6, my + 1, "FM1-HERO MENU", acc, FONT_SMALL);

  char sbuf[16];
  str_cat_int(str_cat(sbuf, "PTS:"), (int32_t)score);
  text(mx + mw - text_width(sbuf, FONT_SMALL) - 6, my + 1, sbuf, C_GOLD, FONT_SMALL);

  static const char *const LABELS[MENU_ITEMS] = {
    "SONG", "THEME", "MODE", "HAND", "PATCH", "TEMPO", "FM BRT", "ECHO", "METRO", "OCTAVE"
  };

  for (int i = 0; i < MENU_ITEMS; i++) {
    int ry = my + 20 + i * 14;
    int sel = (i == menu_cursor);
    char val[18];
    uint16_t vcol = C_TEXT_PRI;
    switch (i) {
      case 0: {
        char *p = str_cat_int(val, saved.song_idx + 1);
        p = str_cat(p, ".");
        str_copy_lim(p, song_title, 12);
        vcol = C_LH_EDGE;
        break;
      }
      case 1:
        str_copy_lim(val, THEME_SHORT[saved.theme % THEME_COUNT], sizeof val);
        vcol = acc;
        break;
      case 2:
        str_copy_lim(val, MODE_NAMES[saved.mode % MODE_COUNT], sizeof val);
        vcol = saved.mode == MODE_GAME ? C_RH_EDGE : saved.mode == MODE_AUTO ? C_LH_EDGE : C_FM1_ORANGE;
        break;
      case 3:
        str_copy_lim(val, HAND_NAMES[saved.hand % HAND_COUNT], sizeof val);
        vcol = saved.hand == HAND_RIGHT ? C_RH_EDGE : saved.hand == HAND_LEFT ? C_LH_EDGE : C_GOLD;
        break;
      case 4:
        str_copy_lim(val, SHORT_PATCH[saved.patch % PATCH_COUNT], sizeof val);
        vcol = C_LH_EDGE;
        break;
      case 5:
        str_cat(str_cat_int(val, saved.speed_pct), "%");
        vcol = C_RH_EDGE;
        break;
      case 6:
        format_int(val, saved.fm_bright);
        vcol = C_TEXT_PRI;
        break;
      case 7:
        format_int(val, saved.echo_mix);
        vcol = C_FM1_ORANGE;
        break;
      case 8:
        str_copy_lim(val, saved.metronome ? "ON" : "OFF", sizeof val);
        vcol = saved.metronome ? C_RH_EDGE : C_TEXT_DIM;
        break;
      case 9: {
        char *p = val;
        if (saved.octave > 0) *p++ = '+';
        format_int(p, saved.octave);
        vcol = C_GOLD;
        break;
      }
      default:
        val[0] = 0;
        break;
    }
    if (sel) {
      rect(mx + 5, ry, mw - 10, 14, C_SURF_RECESS);
      frame(mx + 5, ry, mw - 10, 14, acc);
      rect(mx + 5, ry, 3, 14, C_GOLD);
    }
    text(mx + 11, ry, LABELS[i], sel ? C_TEXT_PRI : C_TEXT_DIM, FONT_SMALL);
    text(mx + mw - text_width(val, FONT_SMALL) - 10, ry, val, vcol, FONT_SMALL);
  }

  rect(mx + 1, my + mh - 15, mw - 2, 14, C_SURF_RECESS);
  const char *hint = "SEL:NAV  K1:ADJ  GLO:OK";
  text((SCREEN_W - text_width(hint, FONT_SMALL)) / 2, my + mh - 15, hint, C_TEXT_DIM, FONT_SMALL);
}

static void adjust_menu_item(int dir) {
  switch (menu_cursor) {
    case 0:
      load_song((int)saved.song_idx + (dir > 0 ? 1 : -1));
      is_playing = 1;
      break;
    case 1:
      saved.theme = (uint8_t)(((int)saved.theme + (dir > 0 ? 1 : THEME_COUNT - 1)) % THEME_COUNT);
      break;
    case 2:
      saved.mode = (uint8_t)(((int)saved.mode + (dir > 0 ? 1 : MODE_COUNT - 1)) % MODE_COUNT);
      synth_all_off();
      last_judge = JUDGE_NONE;
      break;
    case 3:
      saved.hand = (uint8_t)(((int)saved.hand + (dir > 0 ? 1 : HAND_COUNT - 1)) % HAND_COUNT);
      synth_all_off();
      last_judge = JUDGE_NONE;
      break;
    case 4:
      saved.patch = (uint8_t)(((int)saved.patch + (dir > 0 ? 1 : PATCH_COUNT - 1)) % PATCH_COUNT);
      v_patch = saved.patch;
      break;
    case 5: {
      int sp = (int)saved.speed_pct + dir * 5;
      saved.speed_pct = (uint8_t)(sp < 15 ? 15 : sp > 200 ? 200 : sp);
      break;
    }
    case 6: {
      int fb = (int)saved.fm_bright + dir * 4;
      saved.fm_bright = (uint8_t)(fb < 0 ? 0 : fb > 127 ? 127 : fb);
      v_fm_bright = saved.fm_bright;
      break;
    }
    case 7: {
      int ec = (int)saved.echo_mix + dir * 4;
      saved.echo_mix = (uint8_t)(ec < 0 ? 0 : ec > 127 ? 127 : ec);
      v_echo_mix = saved.echo_mix;
      break;
    }
    case 8:
      saved.metronome = !saved.metronome;
      break;
    case 9: {
      int no = saved.octave + (dir > 0 ? 1 : -1);
      if (no >= -2 && no <= 2) saved.octave = (int8_t)no;
      break;
    }
    default:
      break;
  }
}

static void draw_highway(uint32_t now) {
  if (is_finished) {
    draw_finish_overlay(now);
    if (menu_open) draw_glo_menu(now);
    return;
  }

  /* 1. Themed Background */
  switch (saved.theme % THEME_COUNT) {
    case THEME_SPACE:    draw_bg_space(now); break;
    case THEME_CARTOON:  draw_bg_cartoon(now); break;
    case THEME_CYBER:    draw_bg_cyber(); break;
    case THEME_MEDIEVAL: draw_bg_medieval(now); break;
    case THEME_ARCADE:   draw_bg_arcade(); break;
    case THEME_TIEDYE:   draw_bg_tiedye(now); break;
    case THEME_LAVA:     draw_bg_lava(now); break;
    case THEME_FLOWER:   draw_bg_flower(); break;
    case THEME_MANDALA:  draw_bg_mandala(now); break;
    case THEME_GROOVY:   draw_bg_groovy(); break;
    default:             draw_bg_space(now); break;
  }

  /* 2. A-B Loop edge glow markers when active */
  if (loop_state >= 1) {
    rect(0, HWY_Y0, 2, HWY_H, C_FM1_ORANGE);
    rect(SCREEN_W - 2, HWY_Y0, 2, HWY_H, C_FM1_ORANGE);
  }

  /* 3. Themed Falling Notes */
  int32_t win_end = song_pos_ms + LOOKAHEAD_MS;
  for (int32_t i = 0; i < song_note_count; i++) {
    const active_note_t *n = &song_notes[i];
    if (n->t1_ms < song_pos_ms) continue;
    if (n->t0_ms > win_end) break;

    int32_t dt0 = n->t0_ms - song_pos_ms;
    int32_t dt1 = n->t1_ms - song_pos_ms;
    if (dt0 < 0) dt0 = 0;
    if (dt1 > LOOKAHEAD_MS) dt1 = LOOKAHEAD_MS;

    int y_bot = HWY_Y1 - (int)((dt0 * HWY_H) / LOOKAHEAD_MS);
    int y_top = HWY_Y1 - (int)((dt1 * HWY_H) / LOOKAHEAD_MS);
    if (y_bot - y_top < 8) y_top = y_bot - 8;
    if (y_top < HWY_Y0) y_top = HWY_Y0;
    if (y_bot > HWY_Y1) y_bot = HWY_Y1;
    int h = y_bot - y_top;
    if (h <= 0) continue;

    uint8_t k = n->key;
    int x = key_x[k] + 1;
    int w = key_w[k] - 2;
    draw_themed_note(x, y_top, w, h, (n->flags & NF_LH) != 0, key_black[k], n->flags, now);
  }

  /* 4. Themed Strike-Line Impact Bursts */
  draw_themed_impacts(now);

  /* 5. Rising Particle / Spark Fountain */
  for (int i = 0; i < SPARK_MAX; i++) {
    if (sparks[i].life) {
      int sx = sparks[i].x8 >> 8;
      int sy = sparks[i].y8 >> 8;
      if (sx >= 1 && sx < SCREEN_W - 2 && sy >= HWY_Y0 + 1 && sy < HWY_Y1 - 1) {
        int hot_phase = sparks[i].life * 2 > sparks[i].max_life;
        uint16_t col = hot_phase ? sparks[i].col_hot : sparks[i].col_mid;
        int sz = (sparks[i].life > 5) ? 2 : 1;
        rect(sx, sy, sz, sz, col);
      }
    }
  }

  /* 6. Minimal status pill ONLY when paused / recording / waiting */
  if (!menu_open) {
    if (is_recording) {
      char rbuf[16];
      str_cat_int(str_cat(rbuf, "REC "), custom_count);
      int rw = text_width(rbuf, FONT_SMALL) + 10;
      rect(SCREEN_W - rw - 4, 6, rw, 15, C_MISS_CORE);
      frame(SCREEN_W - rw - 4, 6, rw, 15, C_CRIMSON);
      text(SCREEN_W - rw + 1, 6, rbuf, WHITE, FONT_SMALL);
    } else if (!is_playing && !is_finished) {
      int pw = text_width("PAUSED", FONT_SMALL) + 12;
      int px = (SCREEN_W - pw) >> 1;
      rect(px, 8, pw, 16, C_SURF_RECESS);
      frame(px, 8, pw, 16, C_FM1_ORANGE);
      text(px + 6, 9, "PAUSED", C_FM1_ORANGE, FONT_SMALL);
    } else if (last_judge == JUDGE_WAIT) {
      int pw = text_width("PLAY LIT KEY", FONT_SMALL) + 12;
      int px = (SCREEN_W - pw) >> 1;
      rect(px, 8, pw, 16, C_SURF_RECESS);
      frame(px, 8, pw, 16, C_GOLD);
      text(px + 6, 9, "PLAY LIT KEY", C_GOLD, FONT_SMALL);
    }
  }

  /* 7. Transient Overlays or GLO Menu Window */
  if (menu_open) {
    draw_glo_menu(now);
  } else {
    draw_floating_judge(now);
    draw_song_browser(now);
    draw_knob_popup(now);
  }
}

/* ---- Player Key Input & Scoring */
static void on_player_key_press(int k, int velocity) {
  int midi_note = KEY_FIRST_NOTE + 12 * saved.octave + k;
  synth_note_on(midi_note, velocity, 0);
  midi_send(0x90, (uint8_t)midi_note, (uint8_t)velocity);
  if (k >= 0 && k < KEY_COUNT) {
    lane_flash_ms[k] = 150u;
    lane_flash_lh[k] = (uint8_t)(midi_note < 60 ? 1 : 0);
    spawn_sparks(k, lane_flash_lh[k], 3, 0);
  }

  if (is_recording && custom_count < MAX_CUSTOM_NOTES) {
    uint32_t now = millis();
    uint32_t dt_ms = custom_count == 0 ? 0 : (now - rec_last_ms);
    rec_last_ms = now;
    uint32_t dt_ticks = (dt_ms * 24u * 120u) / 60000u;
    if (dt_ticks > 480u) {
      dt_ticks = 480u;
    }
    uint16_t idx = custom_count++;
    uint8_t raw_n = (uint8_t)(KEY_FIRST_NOTE + k);
    if (raw_n < 60) {
      raw_n |= 0x80u; /* auto-tag below Middle C as Left Hand (Blue) */
    }
    custom_events[idx].dt = (uint16_t)dt_ticks;
    custom_events[idx].note = raw_n;
    custom_events[idx].dur = 18;
    hud_dirty = 1;
    return;
  }

  if (!is_playing || is_finished) {
    return;
  }

  int32_t window = saved.mode == MODE_WAIT ? 230 : 185;
  for (int32_t i = 0; i < song_note_count; i++) {
    active_note_t *n = &song_notes[i];
    if (n->flags & (NF_DONE | NF_HIT | NF_MISSED)) {
      continue;
    }
    if (!note_is_player_hand(n->flags)) {
      continue;
    }
    if (n->t0_ms > song_pos_ms + window) {
      break;
    }
    if (n->key == (uint8_t)k && n->t0_ms >= song_pos_ms - window) {
      n->flags |= NF_HIT | NF_DONE;
      int32_t err = n->t0_ms - song_pos_ms;
      if (err < 0) {
        err = -err;
      }
      combo++;
      if (combo > max_combo) {
        max_combo = combo;
      }
      uint32_t mult = combo >= 30 ? 4u : combo >= 20 ? 3u : combo >= 10 ? 2u : 1u;
      if (saved.mode == MODE_WAIT || err <= 60) {
        last_judge = JUDGE_PERFECT;
        hits_perfect++;
        score += 100u * mult;
      } else if (err <= 115) {
        last_judge = JUDGE_GREAT;
        hits_perfect++;
        score += 75u * mult;
      } else {
        last_judge = JUDGE_GOOD;
        hits_good++;
        score += 50u * mult;
      }
      uint32_t hit_now = millis();
      judge_start_ms = hit_now;
      judge_until_ms = hit_now + 650u;
      if (combo >= 10) {
        combo_pop_until_ms = hit_now + 180u;
      }
      trigger_hit_fx(k, (n->flags & NF_LH) != 0, last_judge);
      hud_dirty = 1;
      break;
    }
  }
}

static void on_player_key_release(int k, int midi_note) {
  (void)k;
  synth_note_off(midi_note, 0);
  midi_send(0x80, (uint8_t)midi_note, 0);
}

/* ---- MIDI Channel 16 (.mid file upload protocol, with LH/RH hand flag in CC 125) */
static uint8_t upload_hand_lh;
static void handle_midi_upload(uint8_t st, uint8_t d1, uint8_t d2) {
  if (st == 0xBF) {
    if (d1 == 120) {
      synth_all_off();
      is_playing = 0;
      is_recording = 0;
      uploading_mid = 1;
      saved.custom_bpm = (uint16_t)(d2 < 15 ? 120 : d2 * 2);
      custom_count = 0;
      upload_dt_acc = 0;
      upload_hand_lh = 0;
      upload_title_len = 0;
      for (int i = 0; i < 16; i++) {
        saved.custom_title[i] = 0;
      }
    } else if (d1 == 121) {
      upload_dt_acc = (upload_dt_acc & ~0x7Fu) | (d2 & 0x7Fu);
    } else if (d1 == 122) {
      upload_dt_acc = (uint16_t)((upload_dt_acc & 0x7Fu) | ((uint16_t)(d2 & 0x7Fu) << 7));
    } else if (d1 == 123) {
      if (upload_title_len < 15 && d2 >= 32 && d2 < 127) {
        char c = (char)d2;
        if (c >= 'a' && c <= 'z') {
          c = (char)(c - 32);
        }
        saved.custom_title[upload_title_len++] = c;
        saved.custom_title[upload_title_len] = 0;
      }
    } else if (d1 == 125) {
      upload_hand_lh = d2 ? 0x80u : 0u;
    } else if (d1 == 124) {
      uploading_mid = 0;
      if (!saved.custom_title[0]) {
        str_cat(saved.custom_title, "CUSTOM .MID");
      }
      load_song(CUSTOM_SLOT);
      is_playing = 1;
      print("Loaded .MID song into Custom Slot!");
    }
  } else if (st == 0x9F && uploading_mid && d2 > 0) {
    if (custom_count < MAX_CUSTOM_NOTES) {
      uint16_t idx = custom_count++;
      uint8_t n = fold_note_to_fm1(d1);
      uint8_t lh = upload_hand_lh ? 0x80u : (d1 < 60 ? 0x80u : 0u);
      custom_events[idx].dt = upload_dt_acc;
      custom_events[idx].note = (uint8_t)(n | lh);
      custom_events[idx].dur = d2;
      upload_dt_acc = 0;
      upload_hand_lh = 0;
    }
  }
}

static int accel(int e, int32_t detents) {
  static uint32_t last[ENC_COUNT];
  uint32_t now = millis(), dt = now - last[e];
  if (!detents) {
    return 0;
  }
  last[e] = now;
  return detents * (dt < 25 ? 4 : dt < 60 ? 2 : 1);
}

static void init_default_custom_song(void) {
  static const uint8_t N[14] = {60, 60, 67, 67, 69, 69, 67, 65, 65, 64, 64, 62, 62, 60};
  str_cat(saved.custom_title, "TWINKLE STAR");
  saved.custom_bpm = 116;
  saved.custom_count = 14;
  custom_count = 14;
  for (int i = 0; i < 14; i++) {
    saved.custom_events[i].dt = i == 0 ? 0 : (i == 7 ? 48 : 24);
    saved.custom_events[i].note = N[i];
    saved.custom_events[i].dur = (i == 6 || i == 13) ? 44 : 20;
    custom_events[i] = saved.custom_events[i];
  }
}

void setup(void) {
  init_key_geometry();
  if (settings_load(&saved, sizeof saved) != 0 || saved.magic != SAVED_MAGIC) {
    saved.magic = SAVED_MAGIC;
    saved.octave = 0;
    saved.song_idx = 0;
    saved.mode = MODE_WAIT;
    saved.hand = HAND_BOTH;
    saved.patch = 0;
    saved.metronome = 0;
    saved.speed_pct = 100;
    saved.fm_bright = 68;
    saved.echo_mix = 32;
    saved.theme = THEME_SPACE;
    init_default_custom_song();
  } else {
    custom_count = saved.custom_count > FLASH_CUSTOM_NOTES ? FLASH_CUSTOM_NOTES : saved.custom_count;
    for (uint16_t i = 0; i < custom_count; i++) {
      custom_events[i] = saved.custom_events[i];
    }
  }
  if (saved.speed_pct < 15 || saved.speed_pct > 200) {
    saved.speed_pct = 100;
  }
  v_patch = saved.patch %= PATCH_COUNT;
  saved.theme %= THEME_COUNT;
  v_fm_bright = saved.fm_bright;
  v_echo_mix = saved.echo_mix;

  clear(C_BG_VOID);
  load_song(saved.song_idx);
  is_playing = 1;
  last_frame_ms = millis();
  print("FM1-Hero Ready! 16 built-in songs + 163-song web library.");
}

void loop(void) {
  static uint8_t held_note[KEY_COUNT];
  static uint32_t last_tick_ms;
  static uint32_t prev_held = ~0u, prev_rh = ~0u, prev_lh = ~0u;

  uint32_t now = millis();
  uint32_t dt_real = last_tick_ms ? (now - last_tick_ms) : 0u;
  if (dt_real > 50u) {
    dt_real = 50u;
  }
  last_tick_ms = now;

  /* 1. Read 27 physical/virtual keys */
  int base = KEY_FIRST_NOTE + 12 * saved.octave;
  uint32_t held_mask = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_held(k)) {
      held_mask |= 1u << k;
    }
    if (key_pressed(k)) {
      held_note[k] = (uint8_t)(base + k);
      on_player_key_press(k, 105);
    }
    if (key_released(k)) {
      on_player_key_release(k, held_note[k]);
    }
  }

  /* 2. Read MIDI In & Browser .MID Upload */
  uint8_t st, d1, d2;
  while (midi_read(&st, &d1, &d2)) {
    if ((st & 0x0Fu) == 0x0Fu) {
      handle_midi_upload(st, d1, d2);
      continue;
    }
    if ((st & 0xF0u) == 0x90u && d2 > 0) {
      uint8_t fn = fold_note_to_fm1(d1);
      int k = (int)fn - KEY_FIRST_NOTE;
      if (k >= 0 && k < KEY_COUNT) {
        on_player_key_press(k, d2);
      }
    } else if ((st & 0xF0u) == 0x80u || ((st & 0xF0u) == 0x90u && d2 == 0)) {
      uint8_t fn = fold_note_to_fm1(d1);
      on_player_key_release((int)fn - KEY_FIRST_NOTE, fn);
    }
  }

  /* 3. Buttons & Encoders */
  if (button_pressed(BTN_PLAY)) {
    if (is_recording) {
      is_recording = 0;
      load_song(CUSTOM_SLOT);
      is_playing = 1;
    } else if (is_finished) {
      restart_song();
    } else {
      is_playing = !is_playing;
      if (!is_playing) {
        synth_all_off();
      }
    }
    browser_until_ms = 0;
    hud_dirty = 1;
  }
  if (button_pressed(BTN_HOME)) {
    restart_song();
  }
  if (button_pressed(BTN_SEL) || button_pressed(BTN_ARP)) {
    saved.mode = (uint8_t)((saved.mode + 1) % MODE_COUNT);
    synth_all_off();
    last_judge = JUDGE_NONE;
    hud_dirty = 1;
  }
  if (button_pressed(BTN_ENV) || button_pressed(BTN_LFO)) {
    saved.hand = (uint8_t)((saved.hand + 1) % HAND_COUNT);
    synth_all_off();
    last_judge = JUDGE_NONE;
    hud_dirty = 1;
  }
  if (button_pressed(BTN_EDIT)) {
    /* A-B Bookmark Loop */
    if (loop_state == 0) {
      loop_a_ms = song_pos_ms > 0 ? song_pos_ms : 0;
      loop_state = 1;
    } else if (loop_state == 1 && song_pos_ms > loop_a_ms + 400) {
      loop_b_ms = song_pos_ms;
      loop_state = 2;
    } else {
      loop_state = 0;
    }
    hud_dirty = 1;
    bot_dirty = 1;
  }
  if (button_pressed(BTN_GLO)) {
    menu_open = !menu_open;
    if (menu_open) {
      menu_open_ms = now;
      browser_until_ms = 0;
      popup_end_ms = 0;
    }
  }
  if (button_pressed(BTN_SEQ)) {
    saved.theme = (uint8_t)((saved.theme + 1) % THEME_COUNT);
    trigger_popup("THEME", THEME_SHORT[saved.theme], (uint8_t)(((saved.theme + 1) * 100) / THEME_COUNT), theme_accent_col());
  }
  if (button_pressed(BTN_FX)) {
    saved.patch = (uint8_t)((saved.patch + 1) % PATCH_COUNT);
    v_patch = saved.patch;
    trigger_popup("PATCH", PATCHES[saved.patch].name, (uint8_t)(((saved.patch + 1) * 100) / PATCH_COUNT), C_LH_EDGE);
    bot_dirty = 1;
  }
  if (button_pressed(BTN_REC)) {
    is_recording = !is_recording;
    if (is_recording) {
      is_playing = 0;
      synth_all_off();
      saved.song_idx = CUSTOM_SLOT;
      custom_count = 0;
      saved.custom_bpm = 120;
      for (int i = 0; i < 16; i++) {
        saved.custom_title[i] = 0;
      }
      str_cat(saved.custom_title, "USER REC");
      rec_last_ms = now;
    } else {
      load_song(CUSTOM_SLOT);
      is_playing = 1;
    }
    hud_dirty = 1;
  }
  if (button_pressed(BTN_SAVE)) {
    synth_all_off();
    saved.custom_count = custom_count > FLASH_CUSTOM_NOTES ? FLASH_CUSTOM_NOTES : custom_count;
    for (uint16_t i = 0; i < saved.custom_count; i++) {
      saved.custom_events[i] = custom_events[i];
    }
    settings_save(&saved, sizeof saved);
    print("Saved scores, settings & custom .MID to flash.");
  }

  int oct_d = button_pressed(BTN_OCT_UP) - button_pressed(BTN_OCT_DOWN);
  if (oct_d) {
    int no = saved.octave + oct_d;
    if (no >= -2 && no <= 2) {
      saved.octave = (int8_t)no;
      char obuf[8];
      char *op = obuf;
      if (saved.octave > 0) {
        *op++ = '+';
      }
      format_int(op, saved.octave);
      trigger_popup("OCTAVE", obuf, (uint8_t)(((saved.octave + 2) * 100) / 4), C_FM1_ORANGE);
    }
  }

  int32_t song_step = encoder(ENC_SELECT);
  if (song_step) {
    if (menu_open) {
      menu_cursor = (uint8_t)(((int)menu_cursor + (song_step > 0 ? 1 : MENU_ITEMS - 1)) % MENU_ITEMS);
    } else {
      load_song((int)saved.song_idx + (song_step > 0 ? 1 : -1));
      is_playing = 1;
      if (now >= browser_until_ms) {
        browser_start_ms = now;
      } else if (now > browser_start_ms + 120u) {
        browser_start_ms = now - 120u;
      }
      browser_until_ms = now + 1500u;
    }
  }

  int32_t theme_step = encoder(ENC_PRESETS);
  if (theme_step) {
    saved.theme = (uint8_t)(((int)saved.theme + (theme_step > 0 ? 1 : THEME_COUNT - 1)) % THEME_COUNT);
    if (!menu_open) {
      trigger_popup("THEME", THEME_SHORT[saved.theme], (uint8_t)(((saved.theme + 1) * 100) / THEME_COUNT), theme_accent_col());
    }
  }

  int32_t alg_step = encoder(ENC_ALGORITHM);
  if (alg_step) {
    saved.mode = (uint8_t)(((int)saved.mode + (alg_step > 0 ? 1 : MODE_COUNT - 1)) % MODE_COUNT);
    synth_all_off();
    last_judge = JUDGE_NONE;
    hud_dirty = 1;
  }

  int32_t d_k1 = accel(ENC_KNOB1, encoder(ENC_KNOB1));
  if (d_k1) {
    if (menu_open) {
      adjust_menu_item(d_k1 > 0 ? 1 : -1);
    } else {
      int sp = (int)saved.speed_pct + d_k1 * 5;
      saved.speed_pct = (uint8_t)(sp < 15 ? 15 : sp > 200 ? 200 : sp);
      char vbuf[10];
      str_cat(str_cat_int(vbuf, saved.speed_pct), "%");
      trigger_popup("TEMPO", vbuf, (uint8_t)(((saved.speed_pct - 15) * 100) / 185), C_RH_EDGE);
    }
  }
  int32_t d_k2 = encoder(ENC_KNOB2);
  if (d_k2) {
    saved.patch = (uint8_t)(((int)saved.patch + (d_k2 > 0 ? 1 : PATCH_COUNT - 1)) % PATCH_COUNT);
    v_patch = saved.patch;
    trigger_popup("PATCH", PATCHES[saved.patch].name, (uint8_t)(((saved.patch + 1) * 100) / PATCH_COUNT), C_LH_EDGE);
    bot_dirty = 1;
  }
  int32_t d_k3 = accel(ENC_KNOB3, encoder(ENC_KNOB3));
  if (d_k3) {
    int fb = (int)saved.fm_bright + d_k3;
    saved.fm_bright = (uint8_t)(fb < 0 ? 0 : fb > 127 ? 127 : fb);
    v_fm_bright = saved.fm_bright;
    char vbuf[10];
    format_int(vbuf, saved.fm_bright);
    trigger_popup("FM BRIGHT", vbuf, (uint8_t)((saved.fm_bright * 100) / 127), C_TEXT_PRI);
    bot_dirty = 1;
  }
  int32_t d_k4 = accel(ENC_KNOB4, encoder(ENC_KNOB4));
  if (d_k4) {
    int ec = (int)saved.echo_mix + d_k4;
    saved.echo_mix = (uint8_t)(ec < 0 ? 0 : ec > 127 ? 127 : ec);
    v_echo_mix = saved.echo_mix;
    char vbuf[10];
    format_int(vbuf, saved.echo_mix);
    trigger_popup("ECHO MIX", vbuf, (uint8_t)((saved.echo_mix * 100) / 127), C_FM1_ORANGE);
    bot_dirty = 1;
  }

  /* 4. Advance Note Waterfall & Evaluate Note Events */
  uint32_t rh_cue = 0, lh_cue = 0;
  int32_t beat_ms = ticks_to_ms(24, song_base_bpm);

  if (is_playing && !is_finished && !is_recording) {
    song_frac_acc += dt_real * (uint32_t)saved.speed_pct;
    int32_t step_ms = (int32_t)(song_frac_acc / 100u);
    song_frac_acc %= 100u;

    int blocked_by_wait = 0;
    for (int32_t i = 0; i < song_note_count; i++) {
      active_note_t *n = &song_notes[i];

      if ((n->flags & NF_AUTO_ON) && song_pos_ms >= n->t1_ms) {
        n->flags &= (uint8_t)~NF_AUTO_ON;
        synth_note_off(n->note, 1);
      }

      if (n->flags & NF_DONE) {
        continue;
      }

      int is_player = note_is_player_hand(n->flags);

      /* Light up key cues when note is within 250 ms of strike line */
      if (n->t0_ms <= song_pos_ms + 250 && n->t1_ms >= song_pos_ms - 100) {
        if (n->flags & NF_LH) {
          lh_cue |= 1u << n->key;
        } else {
          rh_cue |= 1u << n->key;
        }
      }

      if (!is_player) {
        /* Accompaniment hand or Watch mode: play automatically at t0_ms */
        if (song_pos_ms >= n->t0_ms) {
          n->flags |= NF_HIT | NF_DONE | NF_AUTO_ON;
          synth_note_on(n->note, 88, 1);
          midi_send(0x90, n->note, 88);
          if (saved.mode == MODE_AUTO) {
            score += 50u;
            combo++;
            hud_dirty = 1;
          }
        }
      } else if (saved.mode == MODE_WAIT) {
        if (n->t0_ms <= song_pos_ms) {
          if (held_mask & (1u << n->key)) {
            n->flags |= NF_HIT | NF_DONE;
            combo++;
            if (combo > max_combo) {
              max_combo = combo;
            }
            hits_perfect++;
            score += 100u;
            last_judge = JUDGE_PERFECT;
            judge_start_ms = now;
            judge_until_ms = now + 550u;
            if (combo >= 10) {
              combo_pop_until_ms = now + 180u;
            }
            trigger_hit_fx(n->key, (n->flags & NF_LH) != 0, JUDGE_PERFECT);
            hud_dirty = 1;
          } else {
            song_pos_ms = n->t0_ms;
            blocked_by_wait = 1;
            if (n->flags & NF_LH) {
              lh_cue |= 1u << n->key;
            } else {
              rh_cue |= 1u << n->key;
            }
            if (last_judge != JUDGE_WAIT) {
              last_judge = JUDGE_WAIT;
              hud_dirty = 1;
            }
          }
        }
      } else if (saved.mode == MODE_GAME) {
        if (song_pos_ms > n->t0_ms + 175) {
          n->flags |= NF_MISSED | NF_DONE;
          combo = 0;
          hits_missed++;
          last_judge = JUDGE_MISS;
          judge_start_ms = now;
          judge_until_ms = now + 600u;
          hud_dirty = 1;
        }
      }
    }

    if (!blocked_by_wait) {
      if (last_judge == JUDGE_WAIT) {
        last_judge = JUDGE_NONE;
        hud_dirty = 1;
      }
      song_pos_ms += step_ms;

      /* Metronome click on beat boundaries */
      if (saved.metronome && beat_ms > 50 && song_pos_ms >= 0) {
        int32_t cur_beat = song_pos_ms / beat_ms;
        if (cur_beat != last_metro_beat) {
          last_metro_beat = cur_beat;
          metro_inc = note_inc((cur_beat & 3) == 0 ? 84 : 77);
          metro_amp = 10000;
        }
      }

      /* A-B Bookmark Loop wrap-around */
      if (loop_state == 2 && song_pos_ms >= loop_b_ms) {
        synth_all_off();
        song_pos_ms = loop_a_ms;
        reset_notes_range(loop_a_ms);
        hud_dirty = 1;
      } else if (song_pos_ms >= song_total_ms) {
        is_finished = 1;
        is_playing = 0;
        finish_start_ms = now;
        synth_all_off();
        if (score > saved.high_score[saved.song_idx]) {
          saved.high_score[saved.song_idx] = score;
        }
        hud_dirty = 1;
      }
    }
  }

  /* 5. Update Physical Key & Button LEDs */
  uint32_t led_mask = held_mask | rh_cue | lh_cue;
  for (int k = 0; k < KEY_COUNT; k++) {
    led_key(k, (led_mask >> k) & 1u);
  }
  int beat_on = is_playing && beat_ms > 0 && song_pos_ms >= 0 && ((song_pos_ms % beat_ms) < (beat_ms / 4));
  led_button(BTN_PLAY, beat_on || (!is_playing && ((now / 400u) & 1u)));
  led_button(BTN_REC, is_recording);
  led_button(BTN_OCT_DOWN, saved.octave < 0);
  led_button(BTN_OCT_UP, saved.octave > 0);
  led_button(BTN_SEL, saved.mode == MODE_WAIT);
  led_button(BTN_ARP, saved.mode == MODE_GAME);
  led_button(BTN_ENV, saved.hand == HAND_RIGHT);
  led_button(BTN_LFO, saved.hand == HAND_LEFT);
  led_button(BTN_EDIT, loop_state > 0);
  led_button(BTN_GLO, menu_open);

  /* 6. Render Screen at ~30 FPS */
  cur_held_mask = held_mask;
  if (held_mask != prev_held || rh_cue != prev_rh || lh_cue != prev_lh) {
    kb_dirty = 1;
    prev_held = held_mask;
    prev_rh = rh_cue;
    prev_lh = lh_cue;
  }
  if (now - last_frame_ms >= 33u) {
    uint32_t frame_dt = now - last_frame_ms;
    if (frame_dt > 60u) {
      frame_dt = 60u;
    }
    last_frame_ms = now;
    update_fx(frame_dt);

    draw_timeline();
    draw_highway(now);
    draw_keyboard(held_mask, rh_cue, lh_cue);
    hud_dirty = 0;
    kb_dirty = 0;
    bot_dirty = 0;
  }
}

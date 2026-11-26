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

/* Screen Layout (240 x 240) */
#define TIMELINE_H 4
#define HUD_Y 4
#define HUD_H 30
#define HWY_Y0 34
#define HWY_Y1 177
#define HWY_H (HWY_Y1 - HWY_Y0) /* 143 px falling-note highway */
#define FELT_Y 177              /* 3 px crimson keyboard felt strip above keys */
#define KB_Y 180
#define KB_WHITE_H 40
#define KB_BLACK_H 24
#define BOT_Y 221
#define LOOKAHEAD_MS 2200

/* Practice & Game Modes */
enum {
  MODE_WAIT = 0, /* Melody Practice ("Wait for me") */
  MODE_GAME = 1, /* Rhythm Practice / Recital */
  MODE_AUTO = 2, /* Watch and Listen */
  MODE_COUNT = 3
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
static uint32_t judge_until_ms;
static int32_t last_metro_beat = -1;

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

/* ---- Highway & Keyboard Color Palette */
#define C_SYN_BG       RGB(48, 48, 48)    /* screenBackground: rgb(48, 48, 48) */
#define C_SYN_HEADER   RGB(16, 122, 181)  /* header: hsv(200, 91, 71) */
#define C_SYN_HDR_DK   RGB(10, 82, 125)
#define C_SYN_TIMELINE RGB(0, 165, 0)     /* timelineFilled: rgb(0, 145, 0) */
#define C_SYN_TIME_BG  RGB(32, 32, 32)    /* timelineEmpty */
#define C_SYN_LOOP     RGB(240, 130, 15)  /* timelineLoop: hsv(30, 100, 94) */
#define C_SYN_GUIDE_C  RGB(92, 92, 92)    /* fallingNoteGuide: rgb(90, 90, 90) */
#define C_SYN_GUIDE_F  RGB(64, 64, 64)    /* fallingNoteGuideAlt: rgb(60, 60, 60) */
#define C_SYN_MEASURE  RGB(68, 68, 68)    /* fallingMeasureLine */
#define C_SYN_FELT     RGB(165, 32, 32)   /* keyboardFelt: hsv(0, 80, 63) */
#define C_SYN_WHITE_K  RGB(252, 250, 238) /* keyboardWhiteKeys: hsv(54, 6, 100) */

/* Right Hand (Green) & Left Hand (Blue) Note Bars */
#define C_RH_WHITE     RGB(110, 228, 42)
#define C_RH_WHITE_HI  RGB(200, 255, 155)
#define C_RH_BLACK     RGB(66, 164, 18)
#define C_RH_BLACK_HI  RGB(140, 220, 85)

#define C_LH_WHITE     RGB(52, 168, 250)
#define C_LH_WHITE_HI  RGB(160, 220, 255)
#define C_LH_BLACK     RGB(26, 110, 198)
#define C_LH_BLACK_HI  RGB(100, 175, 240)

#define C_WAIT_PULSE   RGB(255, 215, 40)
#define C_HIT_GLOW     RGB(160, 255, 110)
#define C_MISS_BAR     RGB(130, 50, 55)
#define C_BOT_BG       RGB(28, 32, 38)
#define C_DIM          RGB(165, 175, 185)

/* ---- Drawing Functions */
static void draw_timeline(void) {
  rect(0, 0, SCREEN_W, TIMELINE_H, C_SYN_TIME_BG);
  if (song_total_ms > 0 && song_pos_ms > 0) {
    int pw = (int)((song_pos_ms * SCREEN_W) / song_total_ms);
    if (pw > SCREEN_W) {
      pw = SCREEN_W;
    }
    if (pw > 0) {
      rect(0, 0, pw, TIMELINE_H, C_SYN_TIMELINE);
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
      rect(xa, 0, xb - xa, TIMELINE_H, C_SYN_LOOP);
    }
  }
}

static void draw_hud(void) {
  static const char *const MODE_NAMES[MODE_COUNT] = {"WAIT", "GAME", "AUTO"};
  static const char *const HAND_NAMES[HAND_COUNT] = {"BOTH", "RH", "LH"};

  draw_timeline();
  rect(0, HUD_Y, SCREEN_W, HUD_H, C_SYN_HEADER);
  rect(0, HUD_Y + HUD_H - 1, SCREEN_W, 1, C_SYN_HDR_DK);

  /* Row 1: Song # + Title + Hand & Mode badges */
  char line1[28];
  char *p = line1;
  p = str_cat_int(p, saved.song_idx + 1);
  p = str_cat(p, ".");
  p = str_cat(p, song_title);
  text(4, HUD_Y + 1, line1, WHITE, FONT_SMALL);

  char badge[16];
  if (is_recording) {
    str_cat(badge, "REC");
  } else {
    p = str_cat(badge, HAND_NAMES[saved.hand % HAND_COUNT]);
    p = str_cat(p, " ");
    str_cat(p, MODE_NAMES[saved.mode % MODE_COUNT]);
  }
  int bw = text_width(badge, FONT_SMALL) + 6;
  rect(SCREEN_W - bw - 3, HUD_Y + 1, bw, 14, C_SYN_HDR_DK);
  text(SCREEN_W - bw, HUD_Y + 1, badge, is_recording ? RGB(255, 90, 90) : C_WAIT_PULSE, FONT_SMALL);

  /* Row 2: Score + Combo + Live Judgement */
  char line2[28];
  p = str_cat(line2, "PTS ");
  p = str_cat_int(p, (int32_t)score);
  if (combo > 1) {
    p = str_cat(p, " x");
    p = str_cat_int(p, combo);
  }
  text(4, HUD_Y + 15, line2, WHITE, FONT_SMALL);

  const char *jstr = "";
  uint16_t jcol = WHITE;
  if (is_recording) {
    char rbuf[20];
    str_cat_int(str_cat(rbuf, "NOTES:"), saved.custom_count);
    text(SCREEN_W - text_width(rbuf, FONT_SMALL) - 4, HUD_Y + 15, rbuf, C_WAIT_PULSE, FONT_SMALL);
    return;
  }
  if (!is_playing && !is_finished) {
    jstr = "PAUSED";
    jcol = C_WAIT_PULSE;
  } else if (last_judge == JUDGE_WAIT) {
    jstr = "PLAY LIT KEY";
    jcol = C_WAIT_PULSE;
  } else if (millis() < judge_until_ms) {
    switch (last_judge) {
      case JUDGE_PERFECT: jstr = "PERFECT!"; jcol = C_RH_WHITE_HI; break;
      case JUDGE_GREAT:   jstr = "GREAT!";   jcol = C_LH_WHITE_HI; break;
      case JUDGE_GOOD:    jstr = "GOOD";     jcol = C_WAIT_PULSE; break;
      case JUDGE_MISS:    jstr = "MISS";     jcol = RGB(255, 120, 120); break;
      default: break;
    }
  }
  if (jstr[0]) {
    text(SCREEN_W - text_width(jstr, FONT_SMALL) - 4, HUD_Y + 15, jstr, jcol, FONT_SMALL);
  }
}

static void draw_bottom_bar(void) {
  static const char *const SHORT_PATCH[PATCH_COUNT] = {"GRND", "EPNO", "CHIP", "BELL", "BRAS", "PAD"};
  rect(0, BOT_Y, SCREEN_W, SCREEN_H - BOT_Y, C_BOT_BG);
  rect(0, BOT_Y, SCREEN_W, 1, RGB(65, 70, 80));

  char b0[16], b1[16], b2[16], b3[16];
  str_cat(str_cat_int(str_cat(b0, "SPD:"), saved.speed_pct), "%");
  str_cat(str_cat(b1, "SND:"), SHORT_PATCH[saved.patch % PATCH_COUNT]);
  str_cat_int(str_cat(b2, "FM:"), saved.fm_bright);
  if (loop_state == 2) {
    str_cat(b3, "LOOP:AB");
  } else if (saved.metronome) {
    str_cat(b3, "MET:ON");
  } else {
    str_cat_int(str_cat(b3, "ECH:"), saved.echo_mix);
  }

  text(4,   BOT_Y + 2, b0, C_RH_WHITE, FONT_SMALL);
  text(68,  BOT_Y + 2, b1, C_LH_WHITE, FONT_SMALL);
  text(134, BOT_Y + 2, b2, WHITE,      FONT_SMALL);
  text(184, BOT_Y + 2, b3, C_WAIT_PULSE, FONT_SMALL);
}

static void draw_keyboard(uint32_t held_mask, uint32_t rh_cue, uint32_t lh_cue) {
  /* Red felt strip right above the keys (y = 177..179) */
  rect(0, FELT_Y, SCREEN_W, 3, C_SYN_FELT);
  rect(0, KB_Y, SCREEN_W, KB_WHITE_H + 1, BLACK);

  /* 16 White keys */
  for (int k = 0; k < KEY_COUNT; k++) {
    if (!key_black[k]) {
      int held = (held_mask >> k) & 1u;
      int rc = (rh_cue >> k) & 1u;
      int lc = (lh_cue >> k) & 1u;
      uint16_t col = held ? C_HIT_GLOW : rc ? C_RH_WHITE : lc ? C_LH_WHITE : C_SYN_WHITE_K;
      uint16_t lip = held ? C_RH_BLACK : rc ? C_RH_BLACK : lc ? C_LH_BLACK : RGB(180, 178, 168);
      rect(key_x[k], KB_Y, key_w[k], KB_WHITE_H, col);
      rect(key_x[k], KB_Y + KB_WHITE_H - 3, key_w[k], 3, lip);
    }
  }
  /* 11 Black keys */
  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_black[k]) {
      int held = (held_mask >> k) & 1u;
      int rc = (rh_cue >> k) & 1u;
      int lc = (lh_cue >> k) & 1u;
      uint16_t col = held ? C_HIT_GLOW : rc ? C_RH_BLACK : lc ? C_LH_BLACK : RGB(24, 24, 28);
      uint16_t hi  = held ? WHITE      : rc ? C_RH_WHITE : lc ? C_LH_WHITE : RGB(70, 70, 78);
      rect(key_x[k] - 1, KB_Y, key_w[k] + 2, KB_BLACK_H + 1, BLACK);
      rect(key_x[k], KB_Y, key_w[k], KB_BLACK_H, col);
      rect(key_x[k] + 1, KB_Y + KB_BLACK_H - 3, key_w[k] - 2, 2, hi);
    }
  }
}

static void draw_finish_overlay(void) {
  rect(16, 44, 208, 126, RGB(36, 40, 46));
  frame(16, 44, 208, 126, C_SYN_HEADER);
  rect(16, 44, 208, 20, C_SYN_HEADER);

  const char *hdr = "RECITAL COMPLETE!";
  text((SCREEN_W - text_width(hdr, FONT_SMALL)) / 2, 46, hdr, WHITE, FONT_SMALL);

  uint32_t total = (uint32_t)hits_perfect + hits_good + hits_missed;
  uint32_t pct = total ? ((uint32_t)(hits_perfect * 100u + hits_good * 70u) / total) : 100u;
  const char *grade = pct >= 95 ? "S" : pct >= 85 ? "A" : pct >= 70 ? "B" : "C";
  uint16_t gcol = pct >= 95 ? C_WAIT_PULSE : pct >= 85 ? C_RH_WHITE : C_LH_WHITE;

  text(28, 72, "RANK", C_DIM, FONT_SMALL);
  text(34, 90, grade, gcol, FONT_BIG);

  char buf[28];
  str_cat_int(str_cat(buf, "SCORE: "), (int32_t)score);
  text(80, 70, buf, WHITE, FONT_SMALL);

  str_cat_int(str_cat(buf, "STREAK: "), max_combo);
  text(80, 88, buf, C_RH_WHITE, FONT_SMALL);

  str_cat(str_cat_int(str_cat(buf, "HIT: "), (int32_t)pct), "%");
  text(80, 106, buf, C_LH_WHITE, FONT_SMALL);

  str_cat_int(str_cat(buf, "BEST: "), (int32_t)saved.high_score[saved.song_idx]);
  text(80, 124, buf, C_WAIT_PULSE, FONT_SMALL);

  const char *foot = "PLAY:RETRY  SELECT:NEXT";
  text((SCREEN_W - text_width(foot, FONT_SMALL)) / 2, 148, foot, C_DIM, FONT_SMALL);
}

static void draw_highway(void) {
  if (is_finished) {
    draw_finish_overlay();
    return;
  }

  /* 1. Clear falling-note area to dark gray rgb(48, 48, 48) */
  rect(0, HWY_Y0, SCREEN_W, HWY_H, C_SYN_BG);

  /* 2. Draw vertical octave guide lines before every C (brighter) and F (subtle) */
  for (int k = 0; k < KEY_COUNT; k++) {
    int semi = k % 12;
    if (semi == 7) { /* C key */
      rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, C_SYN_GUIDE_C);
    } else if (semi == 0) { /* F key */
      rect(key_x[k] - 1, HWY_Y0, 1, HWY_H, C_SYN_GUIDE_F);
    }
  }

  /* 3. Draw horizontal Measure lines (every 4 beats = 96 ticks) with measure numbers */
  int32_t bar_ms = ticks_to_ms(96, song_base_bpm);
  if (bar_ms > 150) {
    int32_t m_idx = song_pos_ms > 0 ? (song_pos_ms / bar_ms) : 0;
    for (; m_idx * bar_ms < song_pos_ms + LOOKAHEAD_MS; m_idx++) {
      int32_t mt = m_idx * bar_ms;
      int32_t dt = mt - song_pos_ms;
      if (dt >= 0 && dt < LOOKAHEAD_MS) {
        int y = HWY_Y1 - (int)((dt * HWY_H) / LOOKAHEAD_MS);
        if (y >= HWY_Y0 && y < HWY_Y1) {
          rect(8, y, 224, 1, C_SYN_MEASURE);
          if (y >= HWY_Y0 + 14) {
            char mnum[12];
            format_int(mnum, m_idx + 1);
            text(9, y - 14, mnum, C_DIM, FONT_SMALL);
          }
        }
      }
    }
  }

  /* 4. Draw Falling Note Bars (Green = Right Hand, Blue = Left Hand) */
  int32_t win_end = song_pos_ms + LOOKAHEAD_MS;
  for (int32_t i = 0; i < song_note_count; i++) {
    const active_note_t *n = &song_notes[i];
    if (n->t1_ms < song_pos_ms) {
      continue;
    }
    if (n->t0_ms > win_end) {
      break;
    }
    int32_t dt0 = n->t0_ms - song_pos_ms;
    int32_t dt1 = n->t1_ms - song_pos_ms;
    if (dt0 < 0) {
      dt0 = 0;
    }
    if (dt1 > LOOKAHEAD_MS) {
      dt1 = LOOKAHEAD_MS;
    }
    int y_bot = HWY_Y1 - (int)((dt0 * HWY_H) / LOOKAHEAD_MS);
    int y_top = HWY_Y1 - (int)((dt1 * HWY_H) / LOOKAHEAD_MS);
    if (y_bot - y_top < 5) {
      y_top = y_bot - 5;
    }
    if (y_top < HWY_Y0) {
      y_top = HWY_Y0;
    }
    if (y_bot > HWY_Y1) {
      y_bot = HWY_Y1;
    }
    int h = y_bot - y_top;
    if (h <= 0) {
      continue;
    }

    uint8_t k = n->key;
    int blk = key_black[k];
    int is_lh = (n->flags & NF_LH) != 0;
    int x = key_x[k] + 1;
    int w = key_w[k] - 2;
    uint16_t body_col, hi_col;

    if (n->flags & NF_MISSED) {
      body_col = C_MISS_BAR;
      hi_col = RGB(180, 80, 80);
    } else if (last_judge == JUDGE_WAIT && n->t0_ms <= song_pos_ms + 25 && !(n->flags & NF_DONE) && note_is_player_hand(n->flags)) {
      body_col = C_WAIT_PULSE;
      hi_col = WHITE;
    } else if (is_lh) {
      body_col = blk ? C_LH_BLACK : C_LH_WHITE;
      hi_col   = (n->flags & NF_HIT) ? WHITE : (blk ? C_LH_BLACK_HI : C_LH_WHITE_HI);
    } else {
      body_col = blk ? C_RH_BLACK : C_RH_WHITE;
      hi_col   = (n->flags & NF_HIT) ? WHITE : (blk ? C_RH_BLACK_HI : C_RH_WHITE_HI);
    }

    /* Rounded-look note bar with 3D left/bottom bevel */
    rect(x, y_top, w, h, body_col);
    rect(x, y_bot - 2, w, 2, hi_col);
    if (h >= 5) {
      rect(x, y_top, 1, h, hi_col);
    }
  }
}

/* ---- Player Key Input & Scoring */
static void on_player_key_press(int k, int velocity) {
  int midi_note = KEY_FIRST_NOTE + 12 * saved.octave + k;
  synth_note_on(midi_note, velocity, 0);
  midi_send(0x90, (uint8_t)midi_note, (uint8_t)velocity);

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
      judge_until_ms = millis() + 700u;
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
    init_default_custom_song();
  } else {
    custom_count = saved.custom_count > FLASH_CUSTOM_NOTES ? FLASH_CUSTOM_NOTES : saved.custom_count;
    for (uint16_t i = 0; i < custom_count; i++) {
      custom_events[i] = saved.custom_events[i];
    }
  }
  if (saved.speed_pct < 25 || saved.speed_pct > 200) {
    saved.speed_pct = 100;
  }
  v_patch = saved.patch %= PATCH_COUNT;
  v_fm_bright = saved.fm_bright;
  v_echo_mix = saved.echo_mix;

  clear(C_SYN_BG);
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
    saved.metronome = !saved.metronome;
    bot_dirty = 1;
  }
  if (button_pressed(BTN_FX)) {
    saved.patch = (uint8_t)((saved.patch + 1) % PATCH_COUNT);
    v_patch = saved.patch;
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
    }
  }

  int32_t song_step = encoder(ENC_SELECT) + encoder(ENC_PRESETS);
  if (song_step) {
    load_song((int)saved.song_idx + (song_step > 0 ? 1 : -1));
    is_playing = 1;
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
    int sp = (int)saved.speed_pct + d_k1 * 5;
    saved.speed_pct = (uint8_t)(sp < 25 ? 25 : sp > 200 ? 200 : sp);
    bot_dirty = 1;
  }
  int32_t d_k2 = encoder(ENC_KNOB2);
  if (d_k2) {
    saved.patch = (uint8_t)(((int)saved.patch + (d_k2 > 0 ? 1 : PATCH_COUNT - 1)) % PATCH_COUNT);
    v_patch = saved.patch;
    bot_dirty = 1;
  }
  int32_t d_k3 = accel(ENC_KNOB3, encoder(ENC_KNOB3));
  if (d_k3) {
    int fb = (int)saved.fm_bright + d_k3;
    saved.fm_bright = (uint8_t)(fb < 0 ? 0 : fb > 127 ? 127 : fb);
    v_fm_bright = saved.fm_bright;
    bot_dirty = 1;
  }
  int32_t d_k4 = accel(ENC_KNOB4, encoder(ENC_KNOB4));
  if (d_k4) {
    int ec = (int)saved.echo_mix + d_k4;
    saved.echo_mix = (uint8_t)(ec < 0 ? 0 : ec > 127 ? 127 : ec);
    v_echo_mix = saved.echo_mix;
    bot_dirty = 1;
  }

  /* 4. Advance Note Waterfall & Evaluate Note Events */
  uint32_t rh_cue = 0, lh_cue = 0;
  int32_t beat_ms = ticks_to_ms(24, song_base_bpm);

  if (is_playing && !is_finished && !is_recording) {
    int32_t step_ms = (int32_t)((dt_real * saved.speed_pct) / 100u);
    if (step_ms < 1 && dt_real > 0) {
      step_ms = 1;
    }

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
            judge_until_ms = now + 500u;
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
  led_button(BTN_GLO, saved.metronome);

  /* 6. Render Screen at ~30 FPS */
  if (held_mask != prev_held || rh_cue != prev_rh || lh_cue != prev_lh) {
    kb_dirty = 1;
    prev_held = held_mask;
    prev_rh = rh_cue;
    prev_lh = lh_cue;
  }
  if (now - last_frame_ms >= 33u) {
    last_frame_ms = now;
    if (hud_dirty) {
      draw_hud();
      hud_dirty = 0;
    } else if (is_playing) {
      draw_timeline();
    }
    draw_highway();
    if (kb_dirty) {
      draw_keyboard(held_mask, rh_cue, lh_cue);
      kb_dirty = 0;
    }
    if (bot_dirty) {
      draw_bottom_bar();
      bot_dirty = 0;
    }
  }
}

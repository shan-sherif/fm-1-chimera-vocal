/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal
 * Copyright (C) 2026 Shan-Sherif */
/* The template app: every control on the screen as it arrives, by the name the app knows it by (plat.h B_*, EN_*,
 * the keys), and a few of them doing something: the keys play a voice and go out over USB-MIDI (MIDI in plays it
 * too), OCT- / OCT+ set the octave, PRESET the wave, MASTER the volume, SAVE stores the settings; the keys are
 * played with velocity and pressure (lib/touch.h), shown on them. One of each thing the platform offers, kept short. Part of the unity build (device/unity.c, host/plat_host.c), after the platform:
 * gfx.c and announce.c are in scope. */
#include "fastmath.h"

#define SET_MAGIC 0x43484D32u            /* "CHM2": the settings record; another magic loads defaults */
typedef struct {
    uint32_t magic;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t wave;                        /* 0 sine, 1 triangle */
    uint8_t ble_off;                     /* Bluetooth switched off (REC held); 0, the default: on */
    uint8_t slot;                        /* the word slot the keys sing (0..WORDS_SLOTS-1) */
    uint8_t preset;                      /* the voice preset (0..NPRESETS-1) */
    uint8_t rsv[3];
} settings_t;
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

static const char *const WAVE_NAME[] = {"SINE", "TRIANGLE"};
/* the controls by their names in plat.h, in its order */
static const char *const BTN_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                         "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_NAME[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};

/* ------------------------------------------------------------------ voice --- */
/* CHIMERA's voice (engine/, MIT, Jameson Hodge) on both cores, with the waveguide tract (ch_wave.c: CHIMERA's full
 * airway physics is about twice what the FM-1 can run, docs/full-physics-on-fm1.md). Each audio block is APP_BLOCK
 * samples, VM_CTRL to a control step: the second core renders the waveguide for this block from what the first core
 * computed during the block before (the anatomy's shape, the glottis's parameters, the breath), while the first core
 * computes the next: CHIMERA's control (ch_control.c) and its anatomy, every SEQ-selectable 1, 2 or 4 control steps
 * (default 4), the tract gliding between. Each core's time is shown.
 *
 * The words: CHIMERA's front end (ch_speech.c) and its live Sing mode (ch_live.c) run here in the main loop; what they
 * plan goes to the audio interrupt through a mailbox and into the control's score there. The keys (and MIDI in) sing
 * the lyrics a syllable a key; PLAY speaks them. */
#include "ch_tract.h"
#include "ch_wave.h"
#include "ch_control.h"
#include "ch_live.h"
#include "voice_male.h"

/* the words: the slots the Studio sends (below, "words"); CHIMERA's presets until it has (lab/tools/gen_words_default.py) */
#include "words_default.h"
#define SPEECH_F0 112.0f                     /* the adult male's speaking pitch (lab record: CHIMERA's render) */

#define NSUB ((int)(APP_BLOCK / VM_CTRL))           /* control steps a block */
/* a block on plat_ticks24's clock (24 per microsecond). (256 * 24000000 overflows 32 bits: v0.0.1 showed every
 * share 3.32 times too high) */
#define BLOCK_TICKS (APP_BLOCK * (24000000u / 100u) / (VM_FS / 100u))
typedef struct {                             /* one control step, from the first core to the second */
    ch_shape_t shape;                        /* the anatomy's, when it ran at this step */
    uint8_t has_shape, steps;                /* (steps: how many control steps to glide to it over) */
    ch_glottis_params_t g;
    float p_lung;
} step_t;
static ch_tract_t tract PLAT_POOL;
static ch_control_t ctl PLAT_POOL;
static ch_wave_t wave PLAT_POOL;
static step_t steps[2][NSUB] PLAT_POOL;     /* [the block being rendered / the next one][its control steps] */
static float vbuf[APP_BLOCK] PLAT_POOL;
static int cur;                              /* steps[cur] is the block the waveguide renders now */
#define QUIET_BLOCKS 8u                      /* ~46 ms of silence after the voice before it rests */
static uint32_t quiet_blocks;
static int resting = 1;                      /* nothing simulated until the first note */
static int two_cores;
/* the anatomy every ANAT_EVERY control steps, the tract gliding between. Fixed: every 2nd overran core 1 while it
 * speaks (0.0.5: 210 %), every 4th fits (0.0.7 onwards: under 70 % at peak). A faster anatomy could try 2 here (the lab
 * measures any: replay --anat-every=N) */
#define ANAT_EVERY 4
static const uint8_t anat_every = ANAT_EVERY;
/* the timings, on plat_ticks24's clock (24 MHz): the last block's, the sum and the worst since the screen read them */
static volatile uint32_t c1_sum, c1_peak, c0_sum, c0_peak, n_blk, c1_last;
static float gain_now;                       /* glides to MASTER: no step at a block's edge */

/* The mailbox, main loop -> audio interrupt: gestures (times from now) and, in order among them, tags to release
 * (release[k] goes before gesture at[k]), or the whole score cleared first. The main loop collects its commands in
 * `pend` (never waiting: in the host simulator the audio runs on the main loop's thread) and moves them into the
 * mailbox whenever the audio interrupt has emptied it (each block, 5.8 ms). */
#define MB_G 224
#define MB_R 32
typedef struct {
    ch_gesture_t g[MB_G];
    int n;
    ch_real release[MB_R];
    int16_t at[MB_R];
    int n_release;
    uint8_t clear;
} mail_t;
static mail_t mb PLAT_POOL, pend PLAT_POOL;
static volatile uint8_t mb_full;

/* Notes, audio interrupt (MIDI in) -> main loop: a small ring */
#define NQ 32u
static volatile uint32_t nq_head, nq_tail;
static volatile uint8_t nq_note[NQ], nq_vel[NQ];   /* velocity 0: off */

static ch_live_t live PLAT_POOL;            /* the main loop's: the lyrics' syllables and the cursor */
static ch_live_cmd_t live_cmd PLAT_POOL;
static volatile int32_t voice_note = -1;     /* the note sounding (shown), -1 none */
static volatile uint8_t speaking;            /* PLAY's speech is in the score */

/* the knobs' section (below): VOWEL mode routes the keys there; the speech rate PLAY speaks at */
static uint8_t vowel_mode;
static float speech_rate = 1.0f;
static void vowel_on(int32_t note, uint32_t vel);
static void vowel_off(int32_t note);
static void vp_take(void);
static void int_text(int32_t v, char *b);
static void fmt1(float v, char *b);
static void text_cat(char *d, const char *s);
#define NPRESETS 8
static void presets_load(void);
static void preset_apply(int i);

/* the words (below): the slot sung, the Studio's pronunciations */
static char lyric[160];
static const char *words_lookup(const char *w, void *user);
static void words_load(void);
static void words_select(int i);

/* the first core: one control step (HumanVoice::control: ch_control.c) */
static void control_step(step_t *d)
{
    ctl.anat_every = anat_every;
    d->has_shape = (uint8_t)ch_control_step(&ctl, &tract);
    if (d->has_shape) {
        ch_tract_shape(&tract, &d->shape);
        d->steps = (uint8_t)ctl.anat_every;
    }
    d->g = ctl.glottis;
    d->p_lung = ctl.p_lung;
}

/* the second core (or the first, alone): the waveguide for a block. In RAM (.c1_text, as the engine's audio rate),
 * so what it calls in the flash it calls through PLAT_FAR */
__attribute__((section(".c1_text"))) static void tract_job(uint32_t which)
{
    uint32_t t0 = PLAT_FAR(plat_ticks24)();
    int s;
    for (s = 0; s < NSUB; s++) {
        step_t *d = &steps[which][s];
        ch_wave_block(&wave, d->has_shape ? &d->shape : 0, d->steps, &d->g, d->p_lung);
        ch_wave_render(&wave, vbuf + (uint32_t)s * VM_CTRL, VM_CTRL);
    }
    c1_last = PLAT_FAR(plat_ticks24)() - t0;
}

static void voice_init(void)
{
    int s;
    ch_source_t src;
    vm_tract_setup(&tract);
    ch_control_init(&ctl, (float)VM_FS, VM_CTRL, 1.0f);   /* HumanVoice::new: the neutral posture, at rest */
    src = ctl.source;
    src.f0 = SPEECH_F0;
    ch_control_set_source(&ctl, &src);
    ctl.defer_contacts = 1;                  /* no second anatomy run after a contact correction (lab: same sound) */
    ch_tract_apply(&tract, ctl.x, 0.0f);     /* (unslewed, as HumanVoice::new) */
    ch_wave_init(&wave, (float)VM_FS, VM_CTRL, tract.k, tract.lip_s);
    for (s = 0; s < NSUB; s++)
        control_step(&steps[0][s]);
    cur = 0;
    ch_live_init(&live);
    ch_lts_device = 1;                       /* the FM-1's letter-to-sound additions (no room for the dictionary) */
}

/* main loop: the pending commands into the mailbox, when it is free */
static void mb_post(void)
{
    if (mb_full || (!pend.n && !pend.n_release && !pend.clear))
        return;
    mb = pend;
    pend.n = pend.n_release = 0;
    pend.clear = 0;
    mb_full = 1;
}

/* main loop: a live note's command after what is pending */
static void post_live(void)
{
    int i;
    for (i = 0; i < live_cmd.n_release && pend.n_release < MB_R; i++) {
        pend.release[pend.n_release] = live_cmd.release[i];
        pend.at[pend.n_release++] = (int16_t)pend.n;
    }
    for (i = 0; i < live_cmd.n && pend.n < MB_G; i++)
        pend.g[pend.n++] = live_cmd.g[i];
    mb_post();
}

static void live_on(int32_t note, uint32_t vel)
{
    if (vowel_mode) {
        vowel_on(note, vel);
        return;
    }
    live_cmd.n = live_cmd.n_release = 0;
    ch_live_note_on(&live, (float)note, (float)vel, &live_cmd);
    voice_note = note;
    post_live();
}

static void live_off(int32_t note)
{
    if (vowel_mode) {
        vowel_off(note);
        return;
    }
    live_cmd.n = live_cmd.n_release = 0;
    ch_live_note_off(&live, (float)note, &live_cmd);
    if (!live.sounding)
        voice_note = -1;
    else
        voice_note = (int32_t)live.s_midi;
    post_live();
}

/* main loop: PLAY, the lyrics spoken (EnglishG2P::to_phrases, plan_phrases), from now; the keys' notes stop */
static void speak_text(const char *text)
{
    static ch_seg_t segs[CH_SEG_MAX];
    static ch_phrase_t ph[CH_PHRASE_MAX];
    ch_plan_opts_t o;
    ch_plan_t plan;
    int n_segs, np;
    np = ch_text_to_phrases(text, words_lookup, 0, segs, CH_SEG_MAX, &n_segs, ph, CH_PHRASE_MAX);
    ch_plan_defaults(&o);
    o.t0 = 0.05f;                            /* as CHIMERA's `render text` */
    o.f0 = SPEECH_F0;
    o.rate = speech_rate;
    plan.g = pend.g;                         /* (what was pending is dropped: the score is cleared anyway) */
    plan.cap = MB_G;
    plan.n = 0;
    plan.dropped = 0;
    ch_plan_phrases(segs, ph, np, &o, &plan);
    pend.n = plan.n;
    pend.n_release = 0;
    pend.clear = 1;
    live.sounding = 0;                       /* (the keys' notes are cleared with the score) */
    live.n_keys = 0;
    voice_note = -1;
    speaking = 1;
    mb_post();
}

/* audio interrupt: the mailbox into the score, now */
static void mb_take(void)
{
    int i, r = 0;
    if (!mb_full)
        return;
    if (mb.clear)
        ch_score_clear(&ctl.score);
    for (i = 0; i <= mb.n; i++) {
        while (r < mb.n_release && mb.at[r] <= i)
            ch_score_release(&ctl.score, mb.release[r++], ctl.clock);
        if (i < mb.n) {
            ch_gesture_t g = mb.g[i];
            g.t0 += ctl.clock;
            g.t1 += ctl.clock;
            ch_score_add(&ctl.score, &g);
        }
    }
    mb_full = 0;
}

static void nq_push(uint32_t note, uint32_t vel)   /* audio interrupt */
{
    uint32_t h = nq_head;
    if (h - nq_tail >= NQ)
        return;
    nq_note[h % NQ] = (uint8_t)note;
    nq_vel[h % NQ] = (uint8_t)vel;
    nq_head = h + 1;
}

static void nq_run(void)                     /* main loop: MIDI in's notes */
{
    while (nq_tail != nq_head) {
        uint32_t t = nq_tail;
        if (nq_vel[t % NQ])
            live_on(nq_note[t % NQ], nq_vel[t % NQ]);
        else
            live_off(nq_note[t % NQ]);
        nq_tail = t + 1;
    }
}

static volatile uint32_t midi_last, midi_last_port, midi_count;   /* the last message in (shown), and how many */
static void midi_in(void)                    /* audio interrupt: the packets that came in */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if (st != 0xF8u && st != 0xFEu) {    /* (clock and active sensing would flood the screen) */
            midi_last = pkt;
            midi_last_port = (uint32_t)port;
            midi_count++;
        }
        if ((st & 0xF0u) == 0x90u && d2)
            nq_push(d1, d2);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            nq_push(d1, 0);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, t0, c0;
    int s;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;   /* MASTER, read once a block */
    int on2;
    midi_in();
    mb_take();
    vp_take();
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    /* At rest (the score empty, the breath gone, the sound died away for QUIET_BLOCKS blocks) nothing is simulated:
     * the output is silence and the voice keeps its state as it was. A note wakes it: the first block renders from
     * the targets of the block before the rest (one block, 5.8 ms, before the new ones arrive). v0.0.1 / v0.0.2 ran
     * the whole voice in silence too: on the FM-1 that kept the audio overloaded (a constant sound). */
    if (resting && ctl.score.n == 0 && ctl.gate_target <= 0.0f) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        n_blk++;                                 /* (a resting block costs nothing: it counts as 0 %) */
        return;
    }
    resting = 0;
    /* the waveguide for this block on the second core; the next block's control steps here, meanwhile */
    on2 = two_cores && plat_cpu2_on() && plat_cpu2_run(tract_job, (uint32_t)cur);
    t0 = plat_ticks24();
    for (s = 0; s < NSUB; s++)
        control_step(&steps[cur ^ 1][s]);
    c0 = plat_ticks24() - t0;
    if (!on2 || plat_cpu2_wait() != 0)
        PLAT_FAR(tract_job)((uint32_t)cur);   /* one core: the waveguide here too */
    cur ^= 1;
    c0_sum += c0;
    c1_sum += c1_last;
    if (c0 > c0_peak)
        c0_peak = c0;
    if (c1_last > c1_peak)
        c1_peak = c1_last;
    n_blk++;
    if (speaking && ctl.score.n == 0)
        speaking = 0;
    {
        float peak = 0.0f;
        for (i = 0; i < n; i++) {
            float x;
            gain_now += (gain - gain_now) * 0.004f;              /* ~5 ms: turning MASTER does not click */
            x = vbuf[i] * gain_now;
            lr[2 * i] = (int32_t)x;
            lr[2 * i + 1] = (int32_t)x;
            if (fm_fabsf(vbuf[i]) > peak)
                peak = fm_fabsf(vbuf[i]);
        }
#ifdef PLAT_HOST
        {   /* (the simulator: say when the voice stops being a number, and what the control was doing) */
            static int told;
            int bad = 0;
            for (i = 0; i < n; i++)
                bad |= !(vbuf[i] == vbuf[i] && vbuf[i] < 1e30f && vbuf[i] > -1e30f);
            if (!told && bad) {
                told = 1;
                int k;
                fprintf(stderr, "voice not finite at %.3f s: p_lung %g f0 %g glottis f0 %g gate %g score %d gestures (%u dropped)\n  x",
                        (double)ctl.clock, (double)ctl.p_lung, (double)ctl.f0, (double)ctl.glottis.f0, (double)ctl.gate,
                        ctl.score.n, ctl.score.dropped);
                for (k = 0; k < CH_NARTIC; k++)
                    fprintf(stderr, " %g", (double)ctl.x[k]);
                fprintf(stderr, "\n  glottis");
                for (k = 0; k < 17; k++)
                    fprintf(stderr, " %g", (double)(&ctl.glottis.f0)[k]);
                fprintf(stderr, "\n  wave: u_g %g p_supra %g a_g %g vol_u %g wall_v %g jets %d shut %d\n", (double)wave.u_g,
                        (double)wave.p_supra, (double)wave.a_g, (double)wave.vol_u, (double)wave.wall_v, wave.n_jet, wave.n_shut);
            }
        }
#endif
        /* rest once the score has played out, the breath gone and the output below 16-bit silence for a while */
        if (ctl.score.n == 0 && !mb_full && ctl.gate_target <= 0.0f && ctl.gate < 1e-4f && peak < 2e-6f) {
            if (++quiet_blocks >= QUIET_BLOCKS)
                resting = 1;
        } else {
            quiet_blocks = 0;
        }
    }
}

void app_panic(void)
{
    voice_note = -1;
    ch_score_clear(&ctl.score);
    ctl.gate = 0.0f;
}

/* --------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.wave = 0;
    set.slot = 0;
    set.preset = 0;
    set.ble_off = 0;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2 || set.wave > 1u || set.ble_off > 1u)
        settings_defaults();
    if (set.slot >= 8u)
        set.slot = 0;
    if (set.preset >= NPRESETS)
        set.preset = 0;
    voice_init();
    words_load();
    words_select(set.slot);
    presets_load();
    preset_apply(set.preset);
    two_cores = plat_cpu2_set(1) == 0;       /* else one core: the waveguide runs here too */
}

/* ---------------------------------------------------------------------- words --- */
/* The word slots, as the Studio sends them (app/web/studio.html) and the store keeps them (OBJ_WORDS): a text
 * document, ASCII, at most PLAT_STORE_MAX bytes:
 *   VOCALWORDS 1                   the header
 *   1 Harder, better, faster       a slot (1..8) and its words
 *   @stronger S T R AO1 NG ER0     a word's pronunciation (CMUdict's ARPAbet), asked before the built-in table
 * The SysEx, F0 7D 56 4F cmd ... F7, 7-bit (numbers 7 bits a byte, most significant first):
 *   01            hello      -> 01 proto(1) max(3) length(3) version...
 *   02 len(3)     begin a document of len bytes        -> 7F 02 status
 *   03 off(3) b.. its bytes at off (in order, <= 512)  -> 7F 03 status
 *   04 sum(3)     the sum of its bytes (21 bits): check, save, sing slot 1  -> 7F 04 status
 *   05 off(3) n(2) read the document saved             -> 05 off(3) bytes
 *   06 slot       sing slot (0-based)                   -> 7F 06 status
 *   07 slot       sing slot and speak it now (PLAY)     -> 7F 07 status
 * status 0 ok, 1 out of range / order, 2 bad sum, 3 not a words document, 4 the flash refused it */
#define WORDS_SLOTS 8
#define WORDS_MAX PLAT_STORE_MAX
#define WORDS_PROTO 1
static char words_doc[WORDS_MAX + 1] PLAT_POOL, words_rx[WORDS_MAX + 1] PLAT_POOL;
static uint32_t words_len, rx_len, rx_got;
static const char *slot_text[WORDS_SLOTS];
static uint16_t slot_len[WORDS_SLOTS];
static const char *pron[256];             /* the "@word ARPA" lines */
static int n_pron, n_slots;

/* a document: its header and that it is printable ASCII lines */
static int words_valid(const char *d, uint32_t n)
{
    uint32_t i;
    if (n < 12 || memcmp(d, "VOCALWORDS 1", 12))
        return 0;
    for (i = 0; i < n; i++)
        if (!((d[i] >= 32 && d[i] < 127) || d[i] == '\n'))
            return 0;
    return 1;
}

/* index the document in words_doc: its slots and its pronunciations */
static void words_index(void)
{
    const char *p = words_doc, *end = words_doc + words_len;
    int i;
    for (i = 0; i < WORDS_SLOTS; i++) {
        slot_text[i] = 0;
        slot_len[i] = 0;
    }
    n_pron = n_slots = 0;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n')
            e++;
        if (*p == '@' && n_pron < 256) {
            pron[n_pron++] = p + 1;
        } else if (*p >= '1' && *p <= '0' + WORDS_SLOTS && p + 1 < e && p[1] == ' ') {
            i = *p - '1';
            slot_text[i] = p + 2;
            slot_len[i] = (uint16_t)(e - p - 2);
            if (i + 1 > n_slots)
                n_slots = i + 1;
        }
        p = e + 1;
    }
}

/* ch_speech's lookup: a pronunciation the document gives (copied out, the ARPAbet up to the line's end) */
static const char *words_lookup(const char *w, void *user)
{
    static char out[96];
    uint32_t n = 0;
    int i;
    (void)user;
    while (w[n])
        n++;
    for (i = 0; i < n_pron; i++) {
        const char *q = pron[i], *e;
        uint32_t k, j;
        for (j = 0; j < n && q[j] && q[j] == w[j]; j++)
            ;
        if (j < n || q[n] != ' ')
            continue;
        q += n + 1;
        for (e = q; *e && *e != '\n'; e++)
            ;
        k = (uint32_t)(e - q) < sizeof out - 1 ? (uint32_t)(e - q) : sizeof out - 1;
        memcpy(out, q, k);
        out[k] = 0;
        return out;
    }
    return 0;
}

/* the keys sing slot i (an empty one: the first that has words) */
static void words_select(int i)
{
    uint32_t n;
    if (i < 0 || i >= WORDS_SLOTS || !slot_text[i])
        for (i = 0; i < WORDS_SLOTS && !slot_text[i]; i++)
            ;
    if (i >= WORDS_SLOTS) {
        lyric[0] = 0;
        i = 0;
    } else {
        n = slot_len[i] < sizeof lyric - 1 ? slot_len[i] : sizeof lyric - 1;
        memcpy(lyric, slot_text[i], n);
        lyric[n] = 0;
    }
    set.slot = (uint8_t)i;
    ch_live_lyrics(&live, lyric, words_lookup, 0);
}

static void words_load(void)
{
    int n = plat_store_load(OBJ_WORDS, words_doc, WORDS_MAX);
    if (n > 0 && words_valid(words_doc, (uint32_t)n)) {
        words_len = (uint32_t)n;
    } else {
        words_len = sizeof WORDS_DEFAULT - 1;
        memcpy(words_doc, WORDS_DEFAULT, words_len);
    }
    words_doc[words_len] = 0;
    words_index();
}

static uint32_t sx_num(const uint8_t *p, int n)    /* 7 bits a byte, most significant first */
{
    uint32_t v = 0;
    while (n--)
        v = v << 7 | (*p++ & 0x7Fu);
    return v;
}
static void sx_put(uint8_t *p, uint32_t v, int n)
{
    while (n--) {
        p[n] = (uint8_t)(v & 0x7Fu);
        v >>= 7;
    }
}
static void sx_status(uint8_t cmd, uint8_t st)
{
    uint8_t f[8] = {0xF0, 0x7D, 0x56, 0x4F, 0x7F, cmd, st, 0xF7};
    plat_sysex_send(f, 8);
}

int app_sysex(const uint8_t *p, uint32_t n)
{
    static uint8_t f[600];
    uint8_t cmd;
    if (n < 4 || p[0] != 0x7D || p[1] != 0x56 || p[2] != 0x4F)
        return 0;
    cmd = p[3];
    p += 4;
    n -= 4;
    switch (cmd) {
    case 0x01: {                             /* hello */
        static const char VER[] = APP_VERSION;
        uint32_t k = 0, i;
        f[k++] = 0xF0; f[k++] = 0x7D; f[k++] = 0x56; f[k++] = 0x4F; f[k++] = 0x01; f[k++] = WORDS_PROTO;
        sx_put(f + k, WORDS_MAX, 3);
        k += 3;
        sx_put(f + k, words_len, 3);
        k += 3;
        for (i = 0; VER[i] && i < 24; i++)
            f[k++] = (uint8_t)(VER[i] & 0x7F);
        f[k++] = 0xF7;
        plat_sysex_send(f, k);
        break;
    }
    case 0x02:                               /* begin */
        if (n < 3 || (rx_len = sx_num(p, 3)) > WORDS_MAX) {
            rx_len = 0;
            sx_status(cmd, 1);
            break;
        }
        rx_got = 0;
        sx_status(cmd, 0);
        break;
    case 0x03: {                             /* data, in order */
        uint32_t off, k;
        if (n < 3 || (off = sx_num(p, 3)) != rx_got || off + (n - 3) > rx_len) {
            sx_status(cmd, 1);
            break;
        }
        for (k = 0; k < n - 3; k++)
            words_rx[off + k] = (char)(p[3 + k] & 0x7F);
        rx_got += n - 3;
        sx_status(cmd, 0);
        break;
    }
    case 0x04: {                             /* commit: check, save, sing */
        uint32_t sum = 0, k;
        if (n < 3 || rx_got != rx_len || !rx_len) {
            sx_status(cmd, 1);
            break;
        }
        for (k = 0; k < rx_len; k++)
            sum += (uint8_t)words_rx[k];
        if ((sum & 0x1FFFFFu) != sx_num(p, 3)) {
            sx_status(cmd, 2);
            break;
        }
        if (!words_valid(words_rx, rx_len)) {
            sx_status(cmd, 3);
            break;
        }
        if (plat_store_save(OBJ_WORDS, words_rx, rx_len) != 0) {
            sx_status(cmd, 4);
            break;
        }
        memcpy(words_doc, words_rx, rx_len);
        words_len = rx_len;
        words_doc[words_len] = 0;
        rx_len = rx_got = 0;
        words_index();
        words_select(0);
        set_dirty = 1;
        app_say("WORDS", "RECEIVED");
        sx_status(cmd, 0);
        break;
    }
    case 0x05: {                             /* read */
        uint32_t off, len, k = 0, i;
        if (n < 5 || (off = sx_num(p, 3)) > words_len) {
            sx_status(cmd, 1);
            break;
        }
        len = sx_num(p + 3, 2);
        if (len > 512)
            len = 512;
        if (off + len > words_len)
            len = words_len - off;
        f[k++] = 0xF0; f[k++] = 0x7D; f[k++] = 0x56; f[k++] = 0x4F; f[k++] = 0x05;
        sx_put(f + k, off, 3);
        k += 3;
        for (i = 0; i < len; i++)
            f[k++] = (uint8_t)(words_doc[off + i] & 0x7F);
        f[k++] = 0xF7;
        plat_sysex_send(f, k);
        break;
    }
    case 0x06:                               /* sing slot */
    case 0x07:                               /* sing slot, and speak it */
        if (n < 1 || p[0] >= WORDS_SLOTS || !slot_text[p[0]]) {
            sx_status(cmd, 1);
            break;
        }
        words_select(p[0]);
        set_dirty = 1;
        if (cmd == 0x07)
            speak_text(lyric);
        sx_status(cmd, 0);
        break;
    default:
        sx_status(cmd, 1);
        break;
    }
    return 1;
}

/* ------------------------------------------------------- knobs, pages, presets --- */
/* K1..K4 are the page's four values (CHIMERA's controls, its ranges: src/ui/components/VoiceControls.tsx, SingPanel.tsx);
 * SELECT turns the page, HOME is WORDS and page 1, EDIT steps pages 2-5, FX is VOWEL (the keys play pitch, the knobs
 * move through CHIMERA's vowel pad). ALGORITHM is Bright. PRESET picks a voice preset, loaded when the knob rests; SAVE
 * keeps the knobs in it (and the vowel snapshots). HOME held + PRESET picks the word slot. */
enum {
    K_VIB, K_RATE, K_BREATHY, K_RING,         /* 1 SING */
    K_JAW, K_LIPS, K_LARYNX, K_VELUM,         /* 2 SHAPE */
    K_ROUND, K_PROT, K_ROOT, K_CURL,          /* 3 MOUTH */
    K_BREATH, K_PRESSED, K_ROUGH, K_FRY,      /* 4 SOURCE */
    K_GLIDE, K_SPEED, K_FALSE, K_BODY,        /* 5 PLAY */
    K_BRIGHT,                                 /* ALGORITHM */
    K_FRONT, K_OPEN,                          /* VOWEL (with K_VELUM as Nasal, K_ROUND as Rounding) */
    NK
};
enum { F_PCT, F_CM, F_SCM, F_CENTS, F_HZ, F_MS, F_X, F_VELUM, F_BODY, F_F2, F_F1 };
typedef struct {
    const char *name;
    float min, max, def, step;                /* step: a detent's change (log knobs: its ratio) */
    uint8_t fmt, log;
} knob_t;
static const knob_t KNOB[NK] = {
    {"VIBRATO", 0, 80, 35, 1, F_CENTS, 0},   {"RATE", 3.5f, 7.5f, 6, 0.1f, F_HZ, 0},
    {"BREATHY", 0, 1, 0.15f, 0.02f, F_PCT, 0}, {"RING", 0, 1, 0, 0.02f, F_PCT, 0},
    {"JAW", 0, 2.4f, 0.6f, 0.05f, F_CM, 0},  {"LIPS", -0.3f, 2.2f, 0.7f, 0.05f, F_CM, 0},
    {"LARYNX", -1.5f, 1.5f, 0, 0.05f, F_SCM, 0}, {"PALATE", -0.25f, 1.2f, -0.15f, 0.025f, F_VELUM, 0},
    {"ROUND", 0, 1, 0.5f, 0.02f, F_PCT, 0},  {"PROTRUDE", -0.1f, 1.4f, 0.2f, 0.05f, F_CM, 0},
    {"ROOT", -0.9f, 0.9f, 0, 0.05f, F_SCM, 0}, {"CURL", 0, 1, 0, 0.02f, F_PCT, 0},
    {"BREATH", 2, 22, 8, 0.25f, F_X, 0},      {"PRESSED", 0, 1, 0.5f, 0.02f, F_PCT, 0},
    {"ROUGH", 0, 1, 0.1f, 0.02f, F_PCT, 0},  {"FRY", 0, 1, 0, 0.02f, F_PCT, 0},
    {"GLIDE", 0.005f, 0.6f, 0.06f, 1.08f, F_MS, 1}, {"SPEED", 0.5f, 1.8f, 1, 0.05f, F_X, 0},
    {"F.FOLDS", 0, 1, 0, 0.02f, F_PCT, 0},   {"BODY", 0, 0, 0, 0, F_BODY, 0},
    {"BRIGHT", 0, 1, 0.25f, 0.02f, F_PCT, 0},
    {"FRONT", 0, 1, 0.45f, 0.02f, F_F2, 0},  {"OPEN", 0, 1, 0.35f, 0.02f, F_F1, 0},
};
#define NPAGES 6
#define PAGE_VOWEL 5
static const uint8_t PAGE[NPAGES][4] = {
    {K_VIB, K_RATE, K_BREATHY, K_RING}, {K_JAW, K_LIPS, K_LARYNX, K_VELUM}, {K_ROUND, K_PROT, K_ROOT, K_CURL},
    {K_BREATH, K_PRESSED, K_ROUGH, K_FRY}, {K_GLIDE, K_SPEED, K_FALSE, K_BODY}, {K_FRONT, K_OPEN, K_VELUM, K_ROUND},
};
static const char *const PAGE_NAME[NPAGES] = {"SING", "SHAPE", "MOUTH", "SOURCE", "PLAY", "VOWEL"};
/* the vowel pad's formant ranges (the adult male's: CHIMERA's VowelMap), log scales; FRONT 0 is the front (F2 high) */
#define PAD_F1_LO 230.0f
#define PAD_F1_HI 950.0f
#define PAD_F2_LO 700.0f
#define PAD_F2_HI 2700.0f
static float pad_f1(float open) { return PAD_F1_LO * fm_exp2f(open * fm_log2f(PAD_F1_HI / PAD_F1_LO)); }
static float pad_f2(float front) { return PAD_F2_HI * fm_exp2f(-front * fm_log2f(PAD_F2_HI / PAD_F2_LO)); }

/* the voice presets (this firmware's own: CHIMERA has none) and the vowel snapshots, as the store keeps them */
#define NSNAP 12
#define PRESETS_MAGIC 0x43485031u             /* "CHP1" */
typedef struct {
    uint32_t magic;
    float v[NPRESETS][NK];
    float snap[NSNAP][4];                     /* FRONT, OPEN, PALATE (nasal), ROUND */
} presets_t;
static presets_t presets PLAT_POOL;
static const char *const PRESET_NAME[NPRESETS] = {"NATURAL", "BREATHY", "BELT", "OPERA", "AIRY", "ROUGH", "FRY", "TWANG"};
static float kv[NK];                          /* the knobs now */
static uint8_t page, vowel_mode;
static int8_t preset_pending = -1;            /* PRESET turned: the preset that loads when it rests */
static uint32_t preset_turned_at;

static void preset_factory(int i, float *v)
{
    int k;
    for (k = 0; k < NK; k++)
        v[k] = KNOB[k].def;
    switch (i) {
    case 1: v[K_BREATHY] = 0.7f; v[K_BREATH] = 6; v[K_PRESSED] = 0.3f; v[K_BRIGHT] = 0.15f; break;            /* BREATHY */
    case 2: v[K_BREATH] = 14; v[K_PRESSED] = 0.75f; v[K_RING] = 0.6f; v[K_BRIGHT] = 0.55f; v[K_JAW] = 0.9f; break;   /* BELT */
    case 3: v[K_VIB] = 60; v[K_RATE] = 5.5f; v[K_RING] = 0.8f; v[K_LARYNX] = -0.6f; v[K_PROT] = 0.5f;               /* OPERA */
            v[K_ROUND] = 0.65f; v[K_BREATH] = 12; v[K_BRIGHT] = 0.4f; break;
    case 4: v[K_BREATHY] = 1; v[K_BREATH] = 5; v[K_PRESSED] = 0.1f; v[K_BRIGHT] = 0.1f; v[K_ROUGH] = 0; break;     /* AIRY */
    case 5: v[K_ROUGH] = 0.65f; v[K_FRY] = 0.25f; v[K_BREATH] = 10; v[K_PRESSED] = 0.6f; break;                    /* ROUGH */
    case 6: v[K_FRY] = 0.8f; v[K_BREATH] = 4; v[K_PRESSED] = 0.65f; v[K_VIB] = 0; break;                          /* FRY */
    case 7: v[K_RING] = 0.95f; v[K_BRIGHT] = 0.8f; v[K_LARYNX] = 0.5f; v[K_PRESSED] = 0.6f; v[K_JAW] = 0.4f; break; /* TWANG */
    default: break;                                                                                            /* NATURAL */
    }
}

/* the snapshots before any is stored: twelve English vowels, where the pad has them (their calibrated formants), their
 * own rounding */
static void snap_factory(void)
{
    static const char *const V[NSNAP] = {"i", "ɪ", "e", "ɛ", "æ", "ɑ", "ɔ", "o", "ʊ", "u", "ʌ", "ə"};
    int i;
    for (i = 0; i < NSNAP; i++) {
        float f1 = 500, f2 = 1500, x[CH_NARTIC];
        ch_vowel_formants(V[i], &f1, &f2);
        presets.snap[i][0] = fm_clampf(fm_log2f(PAD_F2_HI / f2) / fm_log2f(PAD_F2_HI / PAD_F2_LO), 0, 1);
        presets.snap[i][1] = fm_clampf(fm_log2f(f1 / PAD_F1_LO) / fm_log2f(PAD_F1_HI / PAD_F1_LO), 0, 1);
        presets.snap[i][2] = KNOB[K_VELUM].def;
        x[CH_LIP_WIDTH] = 0.5f;
        ch_vowel_pad(f1, f2, x);
        presets.snap[i][3] = fm_clampf(1.0f - x[CH_LIP_WIDTH], 0, 1);
    }
}

static void presets_load(void)
{
    int i;
    for (i = 0; i < NK; i++)                  /* the knobs a preset leaves (the vowel pad's place) start at theirs */
        kv[i] = KNOB[i].def;
    if (plat_store_load(OBJ_PRESETS, &presets, sizeof presets) == (int)sizeof presets && presets.magic == PRESETS_MAGIC)
        return;
    presets.magic = PRESETS_MAGIC;
    for (i = 0; i < NPRESETS; i++)
        preset_factory(i, presets.v[i]);
    snap_factory();
}

/* the voice's parameters from the knobs, the mode and the keys, into the audio interrupt (it takes them at its next
 * block: vp_take). The mailbox is one struct and a flag: the main loop fills it, the interrupt copies it out */
typedef struct {
    ch_source_t src;
    float target[CH_NARTIC];
    float gate;
} vparams_t;
static vparams_t vp_out;
static volatile uint8_t vp_dirty;
static int32_t vkeys[16];                     /* VOWEL: the keys down, oldest first (MIDI notes) */
static int n_vkeys;
static float vowel_breath = 1.0f;             /* the last VOWEL key's breath (its velocity) */

static void vp_send(void)
{
    vparams_t p;
    int k;
    p.src = ctl.source;                       /* (the fields the knobs do not set: as they are) */
    p.src.glide = kv[K_GLIDE];
    p.src.pressure = kv[K_BREATH];
    p.src.breathiness = kv[K_BREATHY];
    p.src.tension = kv[K_PRESSED];
    p.src.fry = kv[K_FRY];
    p.src.roughness = kv[K_ROUGH];
    p.src.vibrato_rate = kv[K_RATE];
    p.src.brightness = kv[K_BRIGHT];
    p.src.vibrato_depth = vowel_mode ? kv[K_VIB] : 0.0f;   /* sung notes carry their own vibrato (ch_live) */
    p.src.voiced = 1;
    p.src.f0 = vowel_mode && n_vkeys ? 440.0f * fm_exp2f((float)(vkeys[n_vkeys - 1] - 69) / 12.0f)
                                     : vowel_mode ? ctl.source.f0 : SPEECH_F0;
    for (k = 0; k < CH_NARTIC; k++)
        p.target[k] = CH_NEUTRAL[k];
    if (vowel_mode)                           /* the vowel pad: the tongue, the jaw, the lips, the root, the larynx */
        ch_vowel_pad(pad_f1(kv[K_OPEN]), pad_f2(kv[K_FRONT]), p.target);
    else {
        p.target[CH_JAW] = kv[K_JAW];
        p.target[CH_LIP_AP] = kv[K_LIPS];
        p.target[CH_LARYNX] = kv[K_LARYNX];
        p.target[CH_LIP_PROT] = kv[K_PROT];
        p.target[CH_ROOT] = kv[K_ROOT];
        p.target[CH_TT_CURL] = kv[K_CURL];
    }
    p.target[CH_LIP_WIDTH] = 1.0f - kv[K_ROUND];
    p.target[CH_VELUM] = kv[K_VELUM];
    p.target[CH_EPILARYNX] = kv[K_RING];
    p.target[CH_VENTRICULAR] = kv[K_FALSE];
    p.gate = vowel_mode && n_vkeys ? vowel_breath : 0.0f;
    live.vibrato = kv[K_VIB];
    speech_rate = kv[K_SPEED];
    vp_dirty = 0;                             /* (the interrupt does not take a half-written one) */
    vp_out = p;
    vp_dirty = 1;
}

/* audio interrupt: the parameters, now */
static void vp_take(void)
{
    int k;
    if (!vp_dirty)
        return;
    ch_control_set_source(&ctl, &vp_out.src);
    for (k = 0; k < CH_NARTIC; k++)
        if (k != CH_TT_X && k != CH_TT_Y)     /* (the tip rests behind the teeth: rest_tip sets those) */
            ctl.target[k] = vp_out.target[k];
    ch_control_set_gate(&ctl, vp_out.gate);
    vp_dirty = 0;
}

static void mode_set(uint8_t v)
{
    if (v == vowel_mode)
        return;
    vowel_mode = v;
    n_vkeys = 0;
    voice_note = -1;
    if (v) {                                  /* the words' notes stop: their gestures go */
        pend.n = pend.n_release = 0;
        pend.clear = 1;
        live.sounding = 0;
        live.n_keys = 0;
        mb_post();
        page = PAGE_VOWEL;
    } else if (page == PAGE_VOWEL) {
        page = 0;
    }
    vp_send();
    app_say(v ? "VOWEL" : "WORDS", v ? "KEYS PLAY PITCH" : "KEYS SING");
}

/* VOWEL: the keys play pitch (the last key held), the breath on while one is down, at its velocity's level (CHIMERA's
 * breath for a velocity) */
static void vowel_on(int32_t note, uint32_t vel)
{
    int i, j = 0;
    for (i = 0; i < n_vkeys; i++)
        if (vkeys[i] != note)
            vkeys[j++] = vkeys[i];
    n_vkeys = j;
    if (n_vkeys < 16)
        vkeys[n_vkeys++] = note;
    vowel_breath = 0.62f + 0.4f * fm_clampf((float)vel / 96.0f, 0, 1) + 0.12f * fm_maxf(0, ((float)vel - 96.0f) / 31.0f);
    voice_note = note;
    vp_send();
}
static void vowel_off(int32_t note)
{
    int i, j = 0;
    for (i = 0; i < n_vkeys; i++)
        if (vkeys[i] != note)
            vkeys[j++] = vkeys[i];
    n_vkeys = j;
    voice_note = n_vkeys ? vkeys[n_vkeys - 1] : -1;
    vp_send();
}

static void knob_turn(int id, int32_t e)
{
    const knob_t *d = &KNOB[id];
    float v = kv[id];
    if (d->fmt == F_BODY) {
        app_say("BODY", "MALE: MORE TO COME");
        return;
    }
    v = d->log ? v * fm_exp2f((float)e * fm_log2f(d->step)) : v + (float)e * d->step;
    kv[id] = fm_clampf(v, d->min, d->max);
    set_dirty = 1;
    vp_send();
}

/* a knob's value as the screen shows it */
static void knob_text(int id, char *b)
{
    float v = kv[id];
    int32_t t;
    char d[12];
    b[0] = 0;
    switch (KNOB[id].fmt) {
    case F_PCT: int_text((int32_t)(v * 100.0f + 0.5f), b); text_cat(b, "%"); break;
    case F_CM: fmt1(v, b); break;
    case F_SCM: if (v > 0.04f) text_cat(b, "+"); fmt1(v, d); text_cat(b, d); break;
    case F_CENTS: int_text((int32_t)(v + 0.5f), b); break;
    case F_HZ: fmt1(v, b); break;
    case F_MS: int_text((int32_t)(v * 1000.0f + 0.5f), b); break;
    case F_X: fmt1(v, b); break;
    case F_VELUM:
        if (v <= 0.0f) {
            text_cat(b, "SHUT");
        } else {
            t = (int32_t)(v * 100.0f + 0.5f);
            int_text(t / 100, b);
            text_cat(b, ".");
            d[0] = (char)('0' + t / 10 % 10);
            d[1] = (char)('0' + t % 10);
            d[2] = 0;
            text_cat(b, d);
        }
        break;
    case F_BODY: text_cat(b, "MALE"); break;
    case F_F2: int_text((int32_t)(pad_f2(v) + 0.5f), b); break;
    case F_F1: int_text((int32_t)(pad_f1(v) + 0.5f), b); break;
    }
}

static void preset_apply(int i)
{
    int k;
    for (k = 0; k < NK; k++)                  /* (the vowel pad's place is played, not the voice's: it stays) */
        if (k != K_FRONT && k != K_OPEN)
            kv[k] = fm_clampf(presets.v[i][k], KNOB[k].min, KNOB[k].max);
    set.preset = (uint8_t)i;
    vp_send();
}

/* VOWEL: a snapshot recalled (FX + a key), or the vowel now stored in it (FX + a key held a second) */
static void snap_recall(int i)
{
    kv[K_FRONT] = presets.snap[i][0];
    kv[K_OPEN] = presets.snap[i][1];
    kv[K_VELUM] = presets.snap[i][2];
    kv[K_ROUND] = presets.snap[i][3];
    vp_send();
}
static void snap_store(int i)
{
    presets.snap[i][0] = kv[K_FRONT];
    presets.snap[i][1] = kv[K_OPEN];
    presets.snap[i][2] = kv[K_VELUM];
    presets.snap[i][3] = kv[K_ROUND];
    set_dirty = 1;
}

/* ---------------------------------------------------------------- boot screen --- */
/* The head side on, at rest, as CHIMERA's flat view draws it (ch_profile.c, from the anatomy's frame), with the name,
 * the version and CHIMERA's credit. Drawn in two strips (the canvas holds 240 x 144). */
#include "ch_profile.h"
static ch_profile_t prof PLAT_POOL;
static void text_cat(char *d, const char *s);
/* where the head goes on the screen: px a cm, the view's left and top edges (screen px) */
static float head_s = 8.0f, head_x0 = 49.0f, head_y0 = 6.0f;
#define HEAD_X(x) (head_x0 + ((x) - CH_VIEW_X0) * head_s)
#define HEAD_Y(y) (head_y0 + (CH_VIEW_Y1 - (y)) * head_s)

static void poly_line(const ch_v2 *p, int n, int closed, int32_t y0, uint16_t c)
{
    int i;
    for (i = 0; i + 1 < n + closed; i++) {
        const ch_v2 *a = &p[i], *b = &p[(i + 1) % n];
        cv_line((int32_t)HEAD_X(a->x), (int32_t)HEAD_Y(a->y) - y0, (int32_t)HEAD_X(b->x), (int32_t)HEAD_Y(b->y) - y0, c);
    }
}

/* CHIMERA's vector-display palette (src/ui/colors.ts): blue the body, the articulators in their control colours */
#define V_BLUE RGB(0x4f, 0xb6, 0xf2)
#define V_BLUE_MID RGB(0x34, 0x87, 0xc4)
#define V_BLUE_DIM RGB(0x1f, 0x5f, 0x8c)
#define V_GREEN RGB(0x5b, 0xe3, 0x81)
#define V_RED RGB(0xff, 0x5d, 0x40)
#define V_PINK RGB(0xff, 0x73, 0xcf)
#define V_WHITE RGB(0xee, 0xf6, 0xf8)
#define CORE(r, g, b) RGB((r) + (255 - (r)) * 55 / 100, (g) + (255 - (g)) * 55 / 100, (b) + (255 - (b)) * 55 / 100)

/* a "hot" beam (CHIMERA's): the colour a pixel either side, its whitish core along the middle */
static void beam(const ch_v2 *p, int n, int closed, int32_t y0, uint16_t c, uint16_t core)
{
    int i;
    for (i = 0; i + 1 < n + closed; i++) {
        const ch_v2 *a = &p[i], *b = &p[(i + 1) % n];
        int32_t x0 = (int32_t)HEAD_X(a->x), ya = (int32_t)HEAD_Y(a->y) - y0, x1 = (int32_t)HEAD_X(b->x), yb = (int32_t)HEAD_Y(b->y) - y0;
        cv_line(x0 + 1, ya, x1 + 1, yb, c);
        cv_line(x0, ya + 1, x1, yb + 1, c);
    }
    poly_line(p, n, closed, y0, core);
}

/* the head (prof) on the canvas, whose top is screen row y0 */
static void draw_head(int32_t y0)
{
    ch_v2 e[24];
    int i;
    /* the body's structure, dim to mid blue */
    poly_line(prof.skull, prof.n_skull, 1, y0, V_BLUE_DIM);
    poly_line(prof.brain, prof.n_brain, 1, y0, V_BLUE_MID);
    poly_line(prof.sulcus, prof.n_sulcus, 0, y0, V_BLUE_DIM);
    for (i = 0; i < prof.n_vertebrae; i++) {
        const ch_v2 *v = prof.vertebrae[i];
        ch_v2 r[4];
        r[0] = v[0];
        r[1].x = v[1].x;
        r[1].y = v[0].y;
        r[2] = v[1];
        r[3].x = v[0].x;
        r[3].y = v[1].y;
        poly_line(r, 4, 1, y0, V_BLUE_DIM);
    }
    poly_line(prof.nasal, prof.n_nasal, 1, y0, V_BLUE_MID);
    poly_line(prof.palate, 10, 0, y0, V_BLUE_MID);
    poly_line(prof.trachea[0], 2, 0, y0, V_BLUE_MID);
    poly_line(prof.trachea[1], 2, 0, y0, V_BLUE_MID);
    poly_line(prof.upper_teeth, 5, 1, y0, V_BLUE_MID);
    poly_line(prof.lower_teeth, 7, 0, y0, V_BLUE_MID);
    for (i = 0; i < 24; i++) {              /* the eye */
        float an = 6.2831853f * (float)i / 24.0f;
        e[i].x = prof.eye_c.x + prof.eye_rx * fm_cosf(an);
        e[i].y = prof.eye_c.y + prof.eye_ry * fm_sinf(an);
    }
    poly_line(e, 24, 1, y0, V_BLUE_MID);
    for (i = 0; i < 12; i++) {              /* its iris, looking ahead */
        float an = 6.2831853f * (float)i / 12.0f;
        e[i].x = prof.eye_c.x + 0.6f * prof.eye_rx + 0.28f * prof.eye_rx * fm_cosf(an);
        e[i].y = prof.eye_c.y + 0.36f * prof.eye_ry * fm_sinf(an);
    }
    poly_line(e, 12, 1, y0, V_BLUE);
    /* the skin and the airway's walls, bright blue */
    beam(prof.skin, prof.n_skin, 0, y0, V_BLUE, CORE(0x4f, 0xb6, 0xf2));
    poly_line(prof.airway, prof.n_airway, 0, y0, V_BLUE);
    /* the articulators: tongue green, jaw red, lips pink, soft palate white */
    beam(prof.tongue, prof.n_tongue, 1, y0, V_GREEN, CORE(0x5b, 0xe3, 0x81));
    beam(prof.jaw, prof.n_jaw, 0, y0, V_RED, CORE(0xff, 0x5d, 0x40));
    beam(prof.upper_lip, 12, 0, y0, V_PINK, CORE(0xff, 0x73, 0xcf));
    beam(prof.lower_lip, 12, 0, y0, V_PINK, CORE(0xff, 0x73, 0xcf));
    poly_line(prof.velum, 11, 0, y0, V_WHITE);
}

/* the boot screen: the name, and at the bottom the version and the credits (the head boot screen, kept aside while its
 * use as the image is asked about, is in private/boot-head/, not published) */
static void boot_text(int32_t y0, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text((240 - text_w(f, s)) / 2, y - y0, f, s, c);
}

static void boot_strip(int32_t y0, int32_t h)
{
    static const char VER[] = APP_VERSION;
    char v[24] = "";
    cv_begin(240, (uint32_t)h, C_BLACK);
    boot_text(y0, 78, &FONT_L, "FM-1 Chimera", V_BLUE);
    boot_text(y0, 112, &FONT_L, "Vocal", V_BLUE);
    cv_rect(60, 158 - y0, 120, 1, RGB(0x13, 0x39, 0x54));
    if (VER[0] >= '0' && VER[0] <= '9')
        text_cat(v, "V");
    text_cat(v, VER);
    boot_text(y0, 170, &FONT_B, v, V_WHITE);
    boot_text(y0, 196, &FONT_XS, "Created by Shan-Sherif", V_BLUE_MID);
    boot_text(y0, 212, &FONT_XS, "Ported from the work of Jameson Hodge", V_BLUE_MID);
    cv_blit(0, (uint32_t)y0);
}

void app_splash(void)
{
    boot_strip(0, 120);
    boot_strip(120, 120);
}

/* --------------------------------------------------------------------- UI --- */
static uint8_t page_debug;                   /* GLO: the debug page (the timings) instead of the flat view */
static struct {
    char msg[2][24];                     /* a short message over the header */
    uint32_t msg_until;
    uint32_t keys, btn;                  /* held at the last input */
    uint8_t vel[NKEYS], prs[NKEYS];      /* each key's last velocity, and its pressure while held */
    int32_t last_key;                    /* the key pressed last: its numbers in the header */
    uint8_t looking;                     /* Bluetooth MIDI devices: REC pressed, the scan's list up (lib/btdev) */
    uint32_t rec_t;                      /* REC went down (0: not held, or its hold already acted) */
    int32_t turns[NE];                   /* detents since boot, per encoder */
    uint32_t turned_at[NE];              /* when each was last turned: it shows lit for a moment */
    uint8_t redraw;
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)     /* signed, no padding */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {                          /* and to a screen reader (announce.c, fm1-reader) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void octave_text(char *t)
{
    t[0] = set.octave < 0 ? '-' : '+';
    t[1] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave));
    t[2] = 0;
}

/* a screen reader that has just started listening (or asks) hears the whole screen */
static void speak(void)
{
    char t[96], o[4];
    if (ann_on() && ann_where()) {
        octave_text(o);
        t[0] = 0;
        text_cat(t, APP_NAME ". OCTAVE ");
        text_cat(t, o);
        text_cat(t, ". WORDS ");
        text_cat(t, lyric);
        if (midi_count) {                    /* the last MIDI message in, as the screen shows it */
            static const char *const PORT[NPORTS] = {"USB", "BLE", "TRS"};
            static const char HEX[] = "0123456789ABCDEF";
            uint32_t m = midi_last, b;
            char h[10];
            for (b = 0; b < 3u; b++) {
                uint32_t v = (m >> (8u + 8u * b)) & 0xFFu;
                h[3 * b] = HEX[v >> 4];
                h[3 * b + 1] = HEX[v & 15u];
                h[3 * b + 2] = b < 2u ? ' ' : 0;
            }
            text_cat(t, ". MIDI IN ");
            text_cat(t, midi_last_port < NPORTS ? PORT[midi_last_port] : "");
            text_cat(t, " ");
            text_cat(t, h);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void note_name(int32_t note, char *b)
{
    static const char *const N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char o[12];
    b[0] = 0;
    text_cat(b, N[note % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave)
        return;
    {   /* a key held across the change would not find its note off: its note ends now */
        uint32_t k;
        for (k = 0; k < NKEYS; k++)
            if (ui.keys >> k & 1u)
                live_off(53 + (int32_t)k + 12 * set.octave);
    }
    set.octave = (int8_t)o;
    set_dirty = 1;
    octave_text(t);
    app_say("OCTAVE", t);
}

int template_ble_off(void) { return set.ble_off; }

/* REC held a second: Bluetooth off or on (kept with the settings, so it stays so at the next power-on) */
static void ble_toggle(void)
{
    set.ble_off = (uint8_t)!set.ble_off;
    set_dirty = 1;
    plat_ble_enable(!set.ble_off);
    app_say("BLUETOOTH", set.ble_off ? "OFF" : "ON");
}

/* REC: a Bluetooth MIDI device (FM1B_BLE_CENTRAL, lib/btdev): look for devices, then connect to the one ALGORITHM
 * picks, then let it go */
static void ble_device_say(void)
{
    char name[21];
    if (btdev_found(btdev_pick(), name, 0))
        app_say("DEVICE", name);
    else
        app_say("BLE", "SCANNING");
}
static void ble_device_button(void)
{
    int ev;
    if (!btdev_busy() && !ui.looking) {
        ui.looking = 1;
        app_say("BLE", "SCANNING");
        return;
    }
    ev = btdev_save();
    ui.looking = 0;
    app_say(ev == BTDEV_CONNECTING ? btdev_word(ev) : "BLE", ev == BTDEV_CONNECTING ? btdev_name() : btdev_word(ev));
}

static void input(void)
{
    static uint32_t snap_mask;               /* keys pressed with FX held: snapshots, not notes */
    static uint8_t fx_used;                  /* FX held for a snapshot: its release does not switch the mode */
    static int8_t snap_key = -1;             /* the snapshot key held (stored when held a second) */
    static uint32_t snap_at;
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms();
    int32_t e;
    const int fx_held = (int)(btn >> B_FX & 1u), home_held = (int)(btn >> B_HOME & 1u);
    nq_run();                                /* MIDI in's notes, planned here */
    mb_post();                               /* what is pending, if the mailbox has been emptied */
    touch_poll();                            /* the sensed keys' presses, velocities and pressure */
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {            /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k, v;
        int32_t note = 53 + (int32_t)k + 12 * set.octave;
        if ((keys & bit) && !(ui.keys & bit)) {
            v = touch_vel(k);
            ui.vel[k] = (uint8_t)v;
            ui.prs[k] = 0;
            ui.last_key = (int32_t)k;
            if (fx_held && k < NSNAP) {      /* FX + key: a vowel snapshot (VOWEL mode) */
                char nm[12] = "SNAPSHOT ";
                int_text((int32_t)k + 1, nm + 9);
                fx_used = 1;
                snap_mask |= bit;
                mode_set(1);
                snap_recall((int)k);
                snap_key = (int8_t)k;
                snap_at = now;
                app_say(nm, "");
                continue;
            }
            live_on(note, v);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | v << 24, TO_USB | TO_BLE);   /* BLE: if built with it */
        } else if (!(keys & bit) && (ui.keys & bit)) {
            ui.prs[k] = 0;
            if (snap_mask & bit) {
                snap_mask &= ~bit;
                if (snap_key == (int8_t)k)
                    snap_key = -1;
                continue;
            }
            live_off(note);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB | TO_BLE);
        } else if ((keys & bit) && touch_pressure(k) != ui.prs[k]) {   /* pressure: poly aftertouch */
            ui.prs[k] = touch_pressure(k);
            ui.redraw = 1;
            plat_midi_send(0x0Au | 0xA0u << 8 | (uint32_t)note << 16 | (uint32_t)ui.prs[k] << 24, TO_USB | TO_BLE);
        }
    }
    if (snap_key >= 0 && fx_held && now - snap_at >= 1000u) {   /* FX + key held a second: the vowel stored there */
        char nm[12] = "SNAPSHOT ";
        int_text((int32_t)snap_key + 1, nm + 9);
        snap_store(snap_key);
        app_say(nm, "STORED");
        snap_key = -1;
    }
    if ((ui.btn >> B_FX & 1u) && !fx_held) { /* FX released: alone, it switches WORDS <-> VOWEL */
        if (!fx_used)
            mode_set((uint8_t)!vowel_mode);
        fx_used = 0;
        snap_key = -1;
    }
    for (k = 0; k < NB; k++) {               /* a button pressed: what it does here */
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_OCTDN)
            octave_step(-1);
        else if (k == B_OCTUP)
            octave_step(1);
        else if (k == B_FX)
            fx_used = 0;                         /* (it acts on release) */
        else if (k == B_HOME) {                  /* WORDS, page 1 */
            mode_set(0);
            page = 0;
        } else if (k == B_EDIT) {                /* pages 2-5 */
            page = (uint8_t)(page >= 1 && page < 4 ? page + 1 : 1);
            app_say(PAGE_NAME[page], "");
        } else if (k == B_REC && FM1B_BLE)
            ui.rec_t = plat_ms();                /* a press: on release (a hold switches Bluetooth) */
        else if (k == B_GLO) {                 /* the debug page (the timings) / the flat view */
            page_debug = (uint8_t)!page_debug;
            lcd_fill(0, 0, 240, 240, C_BLACK);
            ui.redraw = 1;
        } else if (k == B_PLAY) {                /* speak the slot's words (in WORDS) */
            mode_set(0);
            speak_text(lyric);
            app_say("SPEAKING", "");
        } else if (k == B_SAVE) {                /* the settings, the knobs into the preset, the snapshots */
            int i;
            for (i = 0; i < NK; i++)
                presets.v[set.preset][i] = kv[i];
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0
                && plat_store_save(OBJ_PRESETS, &presets, sizeof presets) == 0) {
                set_dirty = 0;
                app_say("SAVED", PRESET_NAME[set.preset]);
            } else {
                app_say("SAVE", "FAILED");
            }
        }
    }
    if (FM1B_BLE && ui.rec_t) {             /* REC: held a second, Bluetooth on / off; a press, a device */
        if (btn >> B_REC & 1u) {
            if (plat_ms() - ui.rec_t >= 1000u) {
                ui.rec_t = 0;
                ble_toggle();
            }
        } else {
            ui.rec_t = 0;
            if (FM1B_BLE_CENTRAL && !set.ble_off)
                ble_device_button();
            else
                app_say("BUTTON", "REC");
        }
    }
    for (k = 0; k < NE; k++) {               /* an encoder turned */
        char t[16];
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = now;
        ui.redraw = 1;
        if (k == EN_ALGO && FM1B_BLE_CENTRAL && ui.looking) {
            btdev_choose(btdev_pick() + e);
            ble_device_say();
        } else if (k == EN_SELECT) {             /* the page (VOWEL's in VOWEL mode) */
            const int n = vowel_mode ? NPAGES : NPAGES - 1;
            page = (uint8_t)((((int)page + e) % n + n) % n);
            app_say(PAGE_NAME[page], "");
        } else if (k == EN_ALGO) {               /* Bright, always */
            knob_turn(K_BRIGHT, e);
            knob_text(K_BRIGHT, t);
            app_say("BRIGHT", t);
        } else if (k == EN_PRESET && home_held) {   /* HOME + PRESET: the word slot */
            int i = set.slot, step = e > 0 ? 1 : -1, tries;
            char nm[8] = "WORDS ";
            for (tries = 0; tries < 8; tries++) {
                i = (i + step + 8) % 8;
                if (slot_text[i])
                    break;
            }
            words_select(i);
            set_dirty = 1;
            nm[6] = (char)('1' + set.slot);
            nm[7] = 0;
            app_say(nm, "");
        } else if (k == EN_PRESET) {             /* a voice preset: loaded when the knob rests */
            int i = preset_pending >= 0 ? preset_pending : set.preset;
            i = ((i + e) % NPRESETS + NPRESETS) % NPRESETS;
            preset_pending = (int8_t)i;
            preset_turned_at = now;
            t[0] = 'P';
            t[1] = (char)('1' + i);
            t[2] = 0;
            app_say(t, PRESET_NAME[i]);
        } else if (k >= EN_K1 && k <= EN_K4) {   /* the page's four values */
            knob_turn(PAGE[page][k - EN_K1], e);
        }
    }
    if (preset_pending >= 0 && now - preset_turned_at >= 400u) {
        preset_apply(preset_pending);
        preset_pending = -1;
        ui.redraw = 1;
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);                            /* the unlit buttons glow, readable in the dark */
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u) | (vowel_mode ? 1u << B_FX : 0u), keys);
}

void app_input_only(void) { input(); }

static void cell(int32_t x, int32_t y, int32_t w, const char *s, int on)   /* a labelled box, lit when on */
{
    cv_rect(x, y, w, 18, on ? C_HI : C_LINE);
    cv_text(x + (w - text_w(&FONT_XS, s)) / 2, y + 2, &FONT_XS, s, on ? C_BLACK : C_GRAY);
}

/* the timing test's readout, read from the audio interrupt's sums every half second */
static struct {
    uint32_t avg[2], peak[2], load, xruns, at;
} stat;
static int stat_update(void)
{
    uint32_t now = plat_ms(), n = n_blk;
    if (now - stat.at < 500u || !n)
        return 0;
    stat.at = now;
    /* (in float: 64-bit integer arithmetic beside loads is a known hazard of this CPU, felucca-bsp docs/CPU.md) */
    stat.avg[0] = (uint32_t)((float)c1_sum * 100.0f / ((float)n * (float)BLOCK_TICKS));
    stat.avg[1] = (uint32_t)((float)c0_sum * 100.0f / ((float)n * (float)BLOCK_TICKS));
    stat.peak[0] = (uint32_t)((float)c1_peak * 100.0f / (float)BLOCK_TICKS);
    stat.peak[1] = (uint32_t)((float)c0_peak * 100.0f / (float)BLOCK_TICKS);
    stat.load = plat_cpu_pct();
    stat.xruns = plat_xruns();
    c1_sum = c0_sum = c1_peak = c0_peak = 0;   /* (the interrupt may add one block between: harmless) */
    n_blk = 0;
    return 1;
}

static void key_cell(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t k, uint16_t off)
{
    int32_t v, p;
    cv_rect(x, y, w, h, off);
    if (!(ui.keys >> k & 1u))
        return;
    v = (ui.vel[k] * h + 126) / 127;
    p = (ui.prs[k] * h + 126) / 127;
    cv_rect(x, y + h - v, w, v, C_HI);
    if (p)
        cv_rect(x + 2, y + h - p, w - 4, p, C_WHITE);
}

static void draw_debug(void)
{
    char t[24];
    int32_t note = voice_note;
    uint32_t i, now = plat_ms(), pct = plat_master() * 100u / 4096u;
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    /* the header: the name and the note, or a message for a moment */
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE);
        if (FM1B_BLE)                        /* Bluetooth: grey while it waits, lit while a computer is connected */
            x = cv_text(x + 8, 10, &FONT_XS, "BLE", plat_ble_connected() ? C_HI : C_DIM);
        if (FM1B_BLE_CENTRAL)                /* a Bluetooth MIDI device: lit while connected */
            cv_text(x + 6, 10, &FONT_XS, "DEV", plat_ble_device() == BLE_DEV_READY ? C_HI : C_DIM);
        if (note >= 0) {                     /* the note, and the last key's velocity and pressure */
            int32_t x;
            note_name(note, t);
            x = 230 - text_w(&FONT_B, t);
            cv_text(x, 6, &FONT_B, t, C_HI);
            if (ui.keys >> ui.last_key & 1u) {
                char n[24] = "V ";
                int_text(ui.vel[ui.last_key], t);
                text_cat(n, t);
                text_cat(n, "  P ");
                int_text(ui.prs[ui.last_key], t);
                text_cat(n, t);
                cv_text(x - 12 - text_w(&FONT_XS, n), 10, &FONT_XS, n, C_GRAY);
            }
        }
    }
    cv_blit(0, 0);
    /* the timing test: each core's share of the block, on average and at worst, over the last half second */
    cv_begin(240, 144, C_BLACK);
    {
        static const char *const ROW[4] = {"CORE 2 TRACT", "CORE 1 CONTROL", "AUDIO LOAD", "XRUNS"};
        for (i = 0; i < 4u; i++) {
            int32_t y = (int32_t)i * 26;
            cv_text(4, y + 4, &FONT_XS, ROW[i], C_GRAY);
            t[0] = 0;
            if (i < 2u) {
                char p[12];
                int_text((int32_t)stat.avg[i], p);
                text_cat(t, p);
                text_cat(t, "%  PEAK ");
                int_text((int32_t)stat.peak[i], p);
                text_cat(t, p);
                text_cat(t, "%");
            } else {
                int_text((int32_t)(i == 2u ? stat.load : stat.xruns), t);
                if (i == 2u)
                    text_cat(t, "%");
            }
            cv_text(116, y, &FONT_B, t, C_WHITE);
        }
        cv_text(4, 108, &FONT_XS, two_cores ? "TWO CORES" : "ONE CORE", two_cores ? C_HI : C_GRAY);
        {   /* the words: the syllable sounding (a hyphen when its word goes on) and the one the next key sings */
            char w[40] = "";
            const ch_syl_t *so = ch_live_sounding(&live), *nx = ch_live_next(&live);
            if (speaking) {
                text_cat(w, "SPEAKING (PLAY)");
            } else {
                if (so) {
                    text_cat(w, so->text);
                    if (!so->word_end)
                        text_cat(w, "-");
                    text_cat(w, "  ");
                }
                text_cat(w, "NEXT ");
                text_cat(w, nx ? nx->text : "AH");
            }
            for (i = 0; w[i]; i++)
                if (w[i] >= 'a' && w[i] <= 'z')
                    w[i] = (char)(w[i] - 'a' + 'A');
            cv_text(100, 108, &FONT_XS, w, speaking || so ? V_WHITE : C_GRAY);
        }
        {
            char e[28] = "ANATOMY EVERY ";
            char nb[4];
            int_text((int32_t)anat_every, nb);
            text_cat(e, nb);
            text_cat(e, anat_every == 1 ? " STEP" : " STEPS");
            cv_text(4, 124, &FONT_XS, e, C_GRAY);
            cv_text(150, 124, &FONT_XS, "% OF 5.8 MS", C_DIM);
        }
    }
    cv_blit(0, 36);
    /* the keys, black above white: a held key fills to its velocity (amber), its pressure over that (white) */
    cv_begin(240, 60, C_BLACK);
    for (i = 0; i < 16u; i++)
        key_cell(4 + (int32_t)i * 14, 14, 13, 14, WK[i], C_DIM);
    for (i = 0; i < 11u; i++) {
        static const uint8_t AT[11] = {0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14};   /* the white key each one follows */
        key_cell(4 + (int32_t)AT[i] * 14 + 8, 0, 11, 12, BK[i], C_LINE);
    }
    octave_text(t);
    cv_text(4, 36, &FONT_XS, "OCT", C_GRAY);
    cv_text(30, 36, &FONT_XS, t, C_HI);
    {   /* the word slot (PRESET) and the start of its words */
        char sl[2] = {(char)('1' + set.slot), 0}, wd[24];
        int i;
        cv_text(64, 36, &FONT_XS, "WORDS", C_GRAY);
        cv_text(100, 36, &FONT_XS, sl, C_HI);
        for (i = 0; i < 10 && lyric[i]; i++)
            wd[i] = (char)(lyric[i] >= 'a' && lyric[i] <= 'z' ? lyric[i] - 'a' + 'A' : lyric[i]);
        wd[i] = 0;
        if (lyric[i])
            text_cat(wd, "..");
        cv_text(110, 36, &FONT_XS, wd, V_WHITE);
    }
    if (midi_count) {                        /* the last MIDI message in, and where from */
        static const char *const PORT[NPORTS] = {"USB", "BLE", "TRS"};
        static const char HEX[] = "0123456789ABCDEF";
        uint32_t m = midi_last, b;
        char h[12];
        for (b = 0; b < 3u; b++) {
            uint32_t v = (m >> (8u + 8u * b)) & 0xFFu;
            h[3 * b] = HEX[v >> 4];
            h[3 * b + 1] = HEX[v & 15u];
            h[3 * b + 2] = b < 2u ? ' ' : 0;
        }
        cv_text(4, 48, &FONT_XS, "MIDI IN", C_GRAY);
        cv_text(52, 48, &FONT_XS, midi_last_port < NPORTS ? PORT[midi_last_port] : "", C_HI);
        cv_text(82, 48, &FONT_XS, h, C_WHITE);
    }
    cv_text(178, 36, &FONT_XS, "VOL", C_GRAY);
    int_text((int32_t)pct, t);
    text_cat(t, "%");
    cv_text(204, 36, &FONT_XS, t, C_HI);
    cv_blit(0, 180);
}


/* ------------------------------------------------------------------- home page --- */
/* CHIMERA's flat view, live: the head as the voice holds it now (its own anatomy, computed here from the control's
 * articulation when it moves, at most every HOME_MS), the words beside it, and the four articulators' readouts in
 * their colours (tongue green, jaw red, lips pink, soft palate white). GLO switches to the debug page (the timings). */
static ch_tract_t dtract PLAT_POOL;         /* the drawing's anatomy (the audio's is the voice's own) */
static float drawn_x[CH_NARTIC];
static uint32_t head_at;
static uint8_t head_ready;
#define HOME_MS 60u
#define HOME_TOP 24
#define HOME_HEAD_H 152

static void home_init(void)
{
    vm_tract_setup(&dtract);
    dtract.draw = 1;
    head_ready = 0;
}

/* the head again if the voice's articulation moved (1: it was) */
static int home_head_update(void)
{
    float a[CH_NARTIC], d = 0.0f;
    uint32_t now = plat_ms();
    int k;
    if (head_ready && now - head_at < HOME_MS)
        return 0;
    for (k = 0; k < CH_NARTIC; k++) {
        a[k] = ctl.x[k];
        d = fm_fabsf(a[k] - drawn_x[k]) > d ? fm_fabsf(a[k] - drawn_x[k]) : d;
    }
    if (head_ready && d < 0.004f)
        return 0;
    ch_tract_apply(&dtract, a, 0.0f);
    ch_profile_build(&dtract, &prof);
    for (k = 0; k < CH_NARTIC; k++)
        drawn_x[k] = a[k];
    head_at = now;
    head_ready = 1;
    return 1;
}

static void fmt1(float v, char *b)          /* one decimal */
{
    int32_t t = (int32_t)(v * 10.0f + (v < 0 ? -0.5f : 0.5f));
    char d[12];
    if (t < 0) {
        *b++ = '-';
        t = -t;
    }
    int_text(t / 10, d);
    *b = 0;
    text_cat(b, d);
    text_cat(b, ".");
    d[0] = (char)('0' + t % 10);
    d[1] = 0;
    text_cat(b, d);
}

static void upper(char *d, const char *s, int n)
{
    int i;
    for (i = 0; i < n && s[i]; i++)
        d[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 'a' + 'A' : s[i]);
    d[i] = 0;
}

static void draw_home(void)
{
    char t[40];
    int32_t note = voice_note, y0, x;
    uint32_t now = plat_ms();
    const ch_syl_t *so = ch_live_sounding(&live), *nx = ch_live_next(&live);
    /* the header: what the view is, and the note; or a message for a moment */
    cv_begin(240, HOME_TOP, C_BLACK);
    if (now < ui.msg_until) {
        x = cv_text(8, 4, &FONT_B, ui.msg[0], V_WHITE);
        cv_text(x + 8, 4, &FONT_B, ui.msg[1], V_BLUE);
    } else {                                 /* the page (or VOWEL), the preset; the note while one sounds */
        char pg[16] = "";
        text_cat(pg, PAGE_NAME[page]);
        if (page != PAGE_VOWEL) {
            char d[4] = {' ', (char)('1' + page), '/', 0};
            text_cat(pg, d);
            text_cat(pg, "5");
        }
        x = cv_text(8, 7, &FONT_XS, pg, V_BLUE);
        cv_text(x + 10, 7, &FONT_XS, vowel_mode ? "VOWEL MODE" : "WORDS", V_BLUE_DIM);
        if (note >= 0) {
            note_name(note, t);
            cv_text(232 - text_w(&FONT_B, t), 4, &FONT_B, t, V_BLUE);
        } else {
            char pn[16] = {'P', (char)('1' + set.preset), ' ', 0};
            text_cat(pn, PRESET_NAME[set.preset]);
            cv_text(232 - text_w(&FONT_XS, pn), 7, &FONT_XS, pn, V_BLUE_MID);
        }
    }
    cv_rect(8, HOME_TOP - 1, 224, 1, RGB(0x13, 0x39, 0x54));
    cv_blit(0, 0);
    /* the head, and the words beside it: the syllable sounding, the next, the slot */
    head_s = (float)HOME_HEAD_H / (CH_VIEW_Y1 - CH_VIEW_Y0);
    head_x0 = 2.0f;
    head_y0 = (float)HOME_TOP;
    for (y0 = HOME_TOP; y0 < HOME_TOP + HOME_HEAD_H; y0 += HOME_HEAD_H / 2) {
        cv_begin(240, HOME_HEAD_H / 2, C_BLACK);
        if (head_ready)
            draw_head(y0);
        if (vowel_mode) {                    /* the vowel made now, its formants, the snapshots' hint */
            char f[24] = "F1 ", d[8];
            cv_text(126, HOME_TOP + 8 - y0, &FONT_XS, "VOWEL", V_BLUE_MID);
            cv_text(126, HOME_TOP + 22 - y0, &FONT_L, ch_vowel_name(drawn_x), note >= 0 ? V_WHITE : V_BLUE);
            int_text((int32_t)(pad_f1(kv[K_OPEN]) + 0.5f), d);
            text_cat(f, d);
            text_cat(f, "  F2 ");
            int_text((int32_t)(pad_f2(kv[K_FRONT]) + 0.5f), d);
            text_cat(f, d);
            cv_text(126, HOME_TOP + 66 - y0, &FONT_XS, f, V_BLUE);
            cv_text(126, HOME_TOP + 112 - y0, &FONT_XS, "FX + KEY: SNAPSHOT", V_BLUE_MID);
            cv_text(126, HOME_TOP + 126 - y0, &FONT_XS, "HOLD 1 S: STORE", V_BLUE_DIM);
            cv_blit(0, (uint32_t)y0);
            continue;
        }
        cv_text(126, HOME_TOP + 8 - y0, &FONT_XS, speaking ? "SPEAKING" : "NOW", V_BLUE_MID);
        if (so && !speaking) {
            upper(t, so->text, 10);
            if (!so->word_end)
                text_cat(t, "-");
        } else {
            t[0] = 0;
        }
        cv_text(126, HOME_TOP + 22 - y0, &FONT_L, t[0] ? t : "-", so && !speaking ? V_WHITE : V_BLUE_DIM);
        cv_text(126, HOME_TOP + 66 - y0, &FONT_XS, "NEXT", V_BLUE_MID);
        upper(t, nx ? nx->text : "ah", 10);
        cv_text(126, HOME_TOP + 80 - y0, &FONT_B, t, V_BLUE);
        {
            char sl[12] = "WORDS ";
            sl[6] = (char)('1' + set.slot);
            sl[7] = 0;
            cv_text(126, HOME_TOP + 112 - y0, &FONT_XS, sl, V_BLUE_MID);
            upper(t, lyric, 14);
            if (lyric[14])
                text_cat(t, "..");
            cv_text(126, HOME_TOP + 126 - y0, &FONT_XS, t, V_BLUE_DIM);
        }
        cv_blit(0, (uint32_t)y0);
    }
    /* the readouts: the four articulators in their colours (CHIMERA's flat view's) */
    cv_begin(240, 240 - HOME_TOP - HOME_HEAD_H, C_BLACK);
    cv_rect(8, 0, 224, 1, RGB(0x13, 0x39, 0x54));
    {   /* the page's four knobs, K1..K4 in the control slots' colours */
        const uint16_t col[4] = {V_GREEN, V_RED, V_PINK, V_WHITE};
        int i;
        for (i = 0; i < 4; i++) {
            const int id = PAGE[page][i];
            int32_t cx = 8 + i * 58;
            const char *nm = page == PAGE_VOWEL && id == K_VELUM ? "NASAL" : KNOB[id].name;
            cv_rect(cx, 13, 4, 4, col[i]);
            cv_text(cx + 8, 9, &FONT_XS, nm, col[i]);
            knob_text(id, t);
            cv_text(cx, 28, &FONT_M, t, col[i]);
        }
    }
    cv_blit(0, (uint32_t)(HOME_TOP + HOME_HEAD_H));
}

static void draw(void)
{
    if (page_debug)
        draw_debug();
    else
        draw_home();
}

#ifdef PLAT_HOST
static int splash_hold;                      /* (the simulator: VOCAL_SPLASH=1 shows the boot screen and stays on it) */
#endif
void app_ui_init(void)
{
#ifdef PLAT_HOST
    if (getenv("VOCAL_SPLASH")) {
        app_splash();
        splash_hold = 1;
        return;
    }
#endif
    touch_on(1);                             /* velocity and pressure: the keys' readings on */
    /* CHIMERA's vector-display palette for fm1-base's five roles: rules, inactive, labels, secondary, values */
    pal[0] = RGB(0x13, 0x39, 0x54);
    pal[1] = V_BLUE_DIM;
    pal[2] = V_BLUE_MID;
    pal[3] = RGB(0xff, 0xb3, 0x40);
    pal[4] = V_BLUE;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    home_init();
    ui.redraw = 1;
}

void app_frame(void)
{
#ifdef PLAT_HOST
    if (splash_hold)
        return;
#endif
    static uint32_t last_pct, lit_until, last_ble, last_midi;
    uint32_t i, now = plat_ms(), pct = plat_master() * 100u / 4096u;
    input();
    speak();
    for (i = 0; i < NE; i++)                 /* an encoder's light goes out */
        if (ui.turned_at[i] && now - ui.turned_at[i] < 400u && now > lit_until)
            lit_until = ui.turned_at[i] + 300u;
    if (lit_until && now >= lit_until) {
        lit_until = 0;
        ui.redraw = 1;
    }
    if (FM1B_BLE_CENTRAL) {                  /* looking: a device found is named at once (REC connects to it) */
        static int found;
        int n = ui.looking ? btdev_count() : 0, ev;
        btdev_page(ui.looking && !set.ble_off);
        if (n > found && btdev_pick() < n)
            ble_device_say();
        found = n;
        while ((ev = btdev_event()) != BTDEV_NONE)   /* a device connected, given up or lost */
            if (ev == BTDEV_CONNECTED || ev == BTDEV_NO_ANSWER || ev == BTDEV_LOST)
                app_say("DEVICE", btdev_word(ev));
    }
    if (midi_count != last_midi) {
        last_midi = midi_count;
        ui.redraw = 1;
    }
    if (stat_update() && page_debug)
        ui.redraw = 1;
    if (!page_debug && home_head_update())   /* the voice moved: the head again */
        ui.redraw = 1;
    {
        static uint8_t was_speaking;
        if (speaking != was_speaking) {
            was_speaking = speaking;
            ui.redraw = 1;
        }
    }
    if (pct != last_pct || (uint32_t)(plat_ble_connected() | plat_ble_device() << 1) != last_ble ||
        (ui.msg_until && now >= ui.msg_until)) {
        last_pct = pct;
        last_ble = (uint32_t)(plat_ble_connected() | plat_ble_device() << 1);
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "wave"))
        *got = set.wave;
    else if (!strcmp(what, "note"))
        *got = voice_note;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "vel"))           /* the last key's velocity and pressure */
        *got = ui.vel[ui.last_key];
    else if (!strcmp(what, "prs"))
        *got = ui.prs[ui.last_key];
    else if (!strncmp(what, "turns", 5))    /* turnsN: encoder N's count (plat.h EN_*) */
        *got = ui.turns[atoi(what + 5) % NE];
    else
        return 0;
    return 1;
}
#endif

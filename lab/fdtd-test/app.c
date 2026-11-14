/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* The template app: every control on the screen as it arrives, by the name the app knows it by (plat.h B_*, EN_*,
 * the keys), and a few of them doing something: the keys play a voice and go out over USB-MIDI (MIDI in plays it
 * too), OCT- / OCT+ set the octave, PRESET the wave, MASTER the volume, SAVE stores the settings; the keys are
 * played with velocity and pressure (lib/touch.h), shown on them. One of each thing the platform offers, kept short. Part of the unity build (device/unity.c, host/plat_host.c), after the platform:
 * gfx.c and announce.c are in scope. */
#include "fastmath.h"

#define SET_MAGIC 0x54504C31u            /* "TPL1": the settings record; another magic loads defaults */
typedef struct {
    uint32_t magic;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t wave;                        /* 0 sine, 1 triangle */
    uint8_t ble_off;                     /* Bluetooth switched off (REC held); 0, the default: on */
    uint8_t rsv[1];
} settings_t;
static settings_t set;
static uint8_t set_dirty;                /* changed since the last save: SAVE is lit */

static const char *const WAVE_NAME[] = {"SINE", "TRIANGLE"};
/* the controls by their names in plat.h, in its order */
static const char *const BTN_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                         "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_NAME[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};

/* ------------------------------------------------------------------ voice --- */
/* The timing test: CHIMERA's voice (engine/, MIT, Jameson Hodge) holding one vowel at the key's pitch, on both
 * cores. Each audio block is APP_BLOCK samples, VM_CTRL to a control step: the second core renders the airway for
 * this block (ch_fast) from the targets the first core computed during the block before, while the first core runs
 * the anatomy (ch_tract) for the next. Each core's time is measured and shown. PLAY switches the posture between
 * the held vowel and a vowel / closure swing every 150 ms (closures and releases: the airway's worst case). */
#include "ch_voice.h"
#include "ch_tract.h"
#include "voice_male.h"

#define NSUB ((int)(APP_BLOCK / VM_CTRL))           /* control steps a block */
/* a block on plat_ticks24's clock (24 per microsecond). (256 * 24000000 overflows 32 bits: v0.0.1 showed every
 * share 3.32 times too high) */
#define BLOCK_TICKS (APP_BLOCK * (24000000u / 100u) / (VM_FS / 100u))
typedef struct {
    ch_real area[CH_MAXN], amin[CH_MAXN], dx[CH_MAXN], port[CH_MAXB];
    ch_noise_t noise[4];
    ch_block_t b;
} step_t;
static ch_voice_t voice PLAT_POOL;
static ch_tract_t tract PLAT_POOL;
static step_t steps[2][NSUB] PLAT_POOL;     /* [the block being rendered / the next one][its control steps] */
static float vbuf[APP_BLOCK] PLAT_POOL;
static int cur;                              /* steps[cur] is the block the airway renders now */
#define QUIET_BLOCKS 8u                      /* ~46 ms of silence after the key before the voice rests */
static uint32_t quiet_blocks;
static int resting = 1;                      /* nothing simulated until the first key */
static int two_cores;
static float gate, f0_now = 110.0f, artic_now[CH_NARTIC], swing_t;
static volatile uint8_t swing;               /* PLAY: the vowel / closure swing */
/* the timings, on plat_ticks24's clock (24 MHz): the last block's, the sum and the worst since the screen read them */
static volatile uint32_t c1_sum, c1_peak, c0_sum, c0_peak, n_blk, c1_last;

/* one voice, the last note wins; the main loop and MIDI in set it, the audio interrupt plays it */
static volatile int32_t voice_note = -1;     /* MIDI note, -1 = released */
static volatile float voice_vel = 1.0f;      /* the note's velocity, 0..1 */
static float gain_now;                       /* glides to MASTER: no step at a block's edge */

/* the first core: one control step's targets for the airway (HumanVoice::control's part this test has) */
static void control_step(step_t *d)
{
    const float dt = (float)VM_CTRL / (float)VM_FS;
    int32_t note = voice_note;
    float gate_goal = note >= 0 ? 1.0f : 0.0f, f0_goal, k;
    int i;
    /* the posture: held, or swinging between the vowel and the closure (articulators ~15 ms behind) */
    const float *goal = vm_artic;
    if (swing) {
        swing_t += dt;
        if (swing_t >= 0.3f)
            swing_t -= 0.3f;
        if (swing_t >= 0.15f)
            goal = vm_artic2;
    }
    k = 1.0f - fm_expf(-dt / 0.015f);
    for (i = 0; i < CH_NARTIC; i++)
        artic_now[i] += (goal[i] - artic_now[i]) * k;
    /* breath: fast attack, slower release (HumanVoice's gate); pitch: a 30 ms glide to the key */
    gate += (gate_goal - gate) * (1.0f - fm_expf(-dt / (gate_goal > gate ? 0.025f : 0.07f)));
    f0_goal = note >= 0 ? 440.0f * fm_exp2f((float)(note - 69) / 12.0f) : f0_now;
    f0_now *= fm_powf(f0_goal / f0_now, 1.0f - fm_expf(-dt / 0.03f));
    ch_tract_apply(&tract, artic_now, dt);
    ch_tract_targets(&tract, d->area, d->amin, d->dx, d->port);
    for (i = 0; i < tract.n_noise; i++)
        d->noise[i] = tract.noise[i];
    d->b.n_noise = (uint8_t)tract.n_noise;
    for (i = 0; i < 17; i++)
        (&d->b.glottis.f0)[i] = vm_glottis[i];
    d->b.glottis.f0 = f0_now;
    d->b.glottis.effort = fm_clampf((vm_p_lung / 980.665f * gate - 3.0f) / 10.0f, 0.0f, 1.0f);
    d->b.p_lung = vm_p_lung * gate;
}

/* the second core (or the first, alone): the airway for a block. In RAM (.c1_text: the engine's audio rate is
 * there too), so what it calls in the flash it calls through PLAT_FAR */
__attribute__((section(".c1_text"))) static void airway_job(uint32_t which)
{
    uint32_t t0 = PLAT_FAR(plat_ticks24)();
    int s;
    for (s = 0; s < NSUB; s++) {
        ch_fast_block(&voice, &steps[which][s].b);
        ch_fast_render(&voice, vbuf + s * VM_CTRL, VM_CTRL);
    }
    c1_last = PLAT_FAR(plat_ticks24)() - t0;
}

static void voice_init(void)
{
    int w, s, k;
    vm_setup(&voice, &tract);
    ch_fast_init_state(&voice, 16);
    for (k = 0; k < CH_NARTIC; k++)
        artic_now[k] = vm_artic[k];
    ch_tract_apply(&tract, artic_now, 0.0f);   /* (unslewed, as HumanVoice::new) */
    for (w = 0; w < 2; w++)
        for (s = 0; s < NSUB; s++) {             /* the nodes the anatomy never sets keep the airway's own */
            step_t *d = &steps[w][s];
            for (k = 0; k < voice.n; k++) {
                d->area[k] = voice.area[k];
                d->amin[k] = voice.amin[k];
                d->dx[k] = voice.dx[k];
            }
            for (k = 0; k < voice.nb; k++)
                d->port[k] = voice.port_area[k];
            d->b.area_target = d->area;
            d->b.amin_target = d->amin;
            d->b.dx_target = d->dx;
            d->b.port_area_target = d->port;
            d->b.noise = d->noise;
        }
    for (s = 0; s < NSUB; s++)
        control_step(&steps[0][s]);
    cur = 0;
}

static void note_on(int32_t note, uint32_t vel)
{
    voice_vel = (float)vel / 127.0f;
    voice_note = note;
}
static void note_off(int32_t note)
{
    if (voice_note == note)
        voice_note = -1;
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
            note_on((int32_t)d1, d2);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            note_off((int32_t)d1);
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, t0, c0;
    int s;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;   /* MASTER, read once a block */
    int on2;
    midi_in();
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    /* At rest (no key, the breath gone, the sound died away for QUIET_BLOCKS blocks) nothing is simulated: the
     * output is silence and the voice keeps its state as it was. A key wakes it: the first block renders from the
     * targets of the block before the rest (one block, 5.8 ms, before the new ones arrive). v0.0.1 / v0.0.2 ran the
     * whole voice in silence too: on the FM-1 that kept the audio overloaded with no key held (a constant sound). */
    if (resting && voice_note < 0) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        n_blk++;                                 /* (a resting block costs nothing: it counts as 0 %) */
        return;
    }
    resting = 0;
    /* the airway for this block on the second core; the anatomy for the next one here, meanwhile */
    on2 = two_cores && plat_cpu2_on() && plat_cpu2_run(airway_job, (uint32_t)cur);
    t0 = plat_ticks24();
    for (s = 0; s < NSUB; s++)
        control_step(&steps[cur ^ 1][s]);
    c0 = plat_ticks24() - t0;
    if (!on2 || plat_cpu2_wait() != 0)
        PLAT_FAR(airway_job)((uint32_t)cur);  /* one core: the airway here too */
    cur ^= 1;
    c0_sum += c0;
    c1_sum += c1_last;
    if (c0 > c0_peak)
        c0_peak = c0;
    if (c1_last > c1_peak)
        c1_peak = c1_last;
    n_blk++;
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
        /* rest once the key is up, the breath gone and the output below 16-bit silence for a while */
        if (voice_note < 0 && gate < 1e-4f && peak < 2e-6f) {
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
    gate = 0.0f;
}

/* --------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.octave = 0;
    set.wave = 0;
    set.ble_off = 0;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.octave < -2 || set.octave > 2 || set.wave > 1u || set.ble_off > 1u)
        settings_defaults();
    voice_init();
    two_cores = plat_cpu2_set(1) == 0;       /* else one core: the airway runs here too */
}

/* --------------------------------------------------------------------- UI --- */
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
        text_cat(t, ". WAVE ");
        text_cat(t, WAVE_NAME[set.wave]);
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
    voice_note = -1;                         /* a key held across the change would not find its note off */
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
    uint32_t keys, btn = plat_buttons(), k;
    int32_t e;
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
            note_on(note, v);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | v << 24, TO_USB | TO_BLE);   /* BLE: if built with it */
        } else if (!(keys & bit) && (ui.keys & bit)) {
            ui.prs[k] = 0;
            note_off(note);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB | TO_BLE);
        } else if ((keys & bit) && touch_pressure(k) != ui.prs[k]) {   /* pressure: poly aftertouch */
            ui.prs[k] = touch_pressure(k);
            ui.redraw = 1;
            plat_midi_send(0x0Au | 0xA0u << 8 | (uint32_t)note << 16 | (uint32_t)ui.prs[k] << 24, TO_USB | TO_BLE);
        }
    }
    for (k = 0; k < NB; k++) {               /* a button pressed: its name, and what it does here */
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_OCTDN)
            octave_step(-1);
        else if (k == B_OCTUP)
            octave_step(1);
        else if (k == B_REC && FM1B_BLE)
            ui.rec_t = plat_ms();                /* a press: on release (a hold switches Bluetooth) */
        else if (k == B_PLAY) {                  /* the timing test's worst case on / off */
            swing = (uint8_t)!swing;
            app_say("POSTURE", swing ? "SWING" : "HELD VOWEL");
        }
        else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
        } else {
            app_say("BUTTON", BTN_NAME[k]);
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
    for (k = 0; k < NE; k++) {               /* an encoder turned: its count; PRESET also changes the wave */
        char t[12];
        if ((e = plat_enc((int)k)) == 0)
            continue;
        ui.turns[k] += e;
        ui.turned_at[k] = plat_ms();
        ui.redraw = 1;
        if (k == EN_ALGO && FM1B_BLE_CENTRAL && ui.looking) {
            btdev_choose(btdev_pick() + e);
            ble_device_say();
        } else if (k == EN_PRESET) {
            set.wave ^= 1u;
            set_dirty = 1;
            app_say("WAVE", WAVE_NAME[set.wave]);
        } else {
            int_text(ui.turns[k], t);
            app_say(ENC_NAME[k], t);
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);                            /* the unlit buttons glow, readable in the dark */
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u), keys);
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

static void draw(void)
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
        static const char *const ROW[4] = {"CORE 2 AIRWAY", "CORE 1 ANATOMY", "AUDIO LOAD", "XRUNS"};
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
        cv_text(100, 108, &FONT_XS, swing ? "POSTURE SWING (PLAY)" : "HELD VOWEL (PLAY: SWING)", C_GRAY);
        cv_text(4, 124, &FONT_XS, "% OF A 5.8 MS AUDIO BLOCK", C_DIM);
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
    cv_text(64, 36, &FONT_XS, "PRESET", C_GRAY);
    cv_text(112, 36, &FONT_XS, WAVE_NAME[set.wave], C_HI);
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

void app_ui_init(void)
{
    touch_on(1);                             /* velocity and pressure: the keys' readings on */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
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
    if (stat_update())
        ui.redraw = 1;
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

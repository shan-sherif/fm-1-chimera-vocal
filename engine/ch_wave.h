/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jameson Hodge (CHIMERA, https://github.com/jamesondh/chimera-vocal-synth)
 * C port for the M-VAVE FM-1: fm-1-chimera-vocal contributors.
 *
 * The FM-1's tract: a Kelly-Lochbaum waveguide driven by CHIMERA's anatomy (ch_tract.c), its glottis and its noise
 * sources, in place of CHIMERA's FDTD airway (ch_voice.c, ch_fast.c), which is about twice what the FM-1 can run
 * (docs/full-physics-on-fm1.md).
 *
 *   tract    pressure waves, one section a sample (c/fs: 0.79 cm at 44.1 kHz), the section count fixed per body;
 *            each section's area from the anatomy's pharynx and oral tubes (as CHIMERA resamples them, slew limits
 *            included) spread over the tract's current length
 *   nose     a second waveguide from the velopharyngeal junction, joined by a three-port junction weighted by the
 *            velum port's area (closed: no coupling)
 *   glottis  CHIMERA's parametric glottis gives the area; the flow through it solves CHIMERA's glottal port (inertance,
 *            slit-viscous resistance, entrance and exit losses) against the lungs' pressure and the pressure just
 *            above the glottis, so the source still feels the tract: a closure chokes the flow
 *   noise    CHIMERA's turbulence sources (aspiration, the constriction's jet, the teeth), at the same places, with
 *            the same Reynolds-number law and filters, injected as series pressure sources at the junctions
 *   output   the radiated flow's time derivative at the lips and the nostrils, then CHIMERA's DC blocker and limiter
 *
 * Lumped against the FDTD: the yielding walls (one wall for the whole tract) and the moving-wall volume sources (one
 * source, at the glottis's end). Lost: the tract's length changing the section count (the length is spread over a fixed count), the sinus and the
 * piriform fossae. */
#ifndef CH_WAVE_H
#define CH_WAVE_H
#include "ch_voice.h"
#include "ch_tract.h"

#define CH_WMAX 40      /* tract sections (the adult male: ~22) */
#define CH_WJET_AREA 1.0f   /* cm²: a junction narrower than this gets the jet's loss */
#define CH_WNMAX 24     /* nose sections */

/* a tract section: its waves in two buffers (the sample reads one and writes the other: ch_wave_t.p), its area and
 * the reflection of the junction at its left end, both ramped over a control step. One struct a section: on the FM-1
 * a field of a separate array cost an address computation of its own (docs/full-physics-on-fm1.md) */
typedef struct {
    ch_real R[2], L[2];         /* the right-going wave at the section's right end, the left-going at its left end */
    ch_real k, dk;              /* the junction at its left end (to section i - 1) */
    ch_real area, darea;
} ch_wsec_t;

typedef struct {
    ch_real R[2], L[2], k, area;
} ch_wnsec_t;

typedef struct {
    ch_real fs, rho_c, closed_area;
    int n, nn, jv;                          /* tract and nose sections; the nose joins at junction jv */
    ch_real dx;                             /* a section's length, c / fs */
    uint8_t var_len;                        /* 1: the section count follows the tract's length each control step */
    uint8_t len_changed;                    /* the count changed at this step: its first sample skips the output's jump */
    int ctrl_every;
    /* the tract and the nose; p: the wave buffer this sample reads */
    ch_wsec_t sec[CH_WMAX];
    ch_wnsec_t nsec[CH_WNMAX];
    int p;
    ch_real fade, fade_n;
    /* the junction where the nose joins: its three weights 2A/sum(A), ramped */
    ch_real w1, w2, w3, dw1, dw2, dw3;
    ch_real r_lip, r_nose, r_glottis;      /* (r_lip, r_nose: the radiation load's reflection when rad_load is 0) */
    /* the radiation load at the lips [0] and the nostrils [1]: CHIMERA's (Flanagan's) parallel R-L against the end
     * section's characteristic impedance, as a reflection filter refl = (b0 + b1 z^-1) / (1 + a1 z^-1) (bilinear),
     * its coefficients from the end's area each control step; its last input and output */
    uint8_t rad_load;
    ch_real rb0[2], rb1[2], ra1[2], r_in[2], r_out[2];
    ch_real noise_gain;                    /* on CHIMERA's noise sources (calibrated against its renders) */
    /* the jets: at the junctions narrower than CH_WJET_AREA a series resistance, CHIMERA's kinetic (Borda-Carnot)
     * loss times the junction's flow a sample before: the flow out of a constriction (a release, a fricative) is
     * limited as in CHIMERA. Per jet: its junction, the loss coefficients each way, the flow */
    uint8_t n_jet;
    int16_t jet_m[8];
    ch_real jet_kf[8], jet_kb[8], jet_u[8];
    /* the yielding walls, lumped: CHIMERA's soft-tissue mass, resistance and stiffness per cm² over the tract's wall
     * area, moved by the pressure above the glottis; their motion draws air (the voice bar through a closure) and a
     * quarter of it radiates (CHIMERA's wall_radiation) */
    /* the sections closed during this control step (shut now or by its end): a sealed cell holds no air, so its waves
     * are emptied every sample (CHIMERA: a sealed cell has no pressure of its own). Without it the waves trapped in a
     * shut section doubled at each reflection and came out as a burst at the release */
    uint8_t n_shut;
    int16_t shut[CH_WMAX];
    uint8_t walls;
    ch_real wall_s, wall_v, wall_y, wall_u;
    /* the moving walls' volume source (CHIMERA's vol_src), lumped: the volume the tract behind its first closure (the
     * anatomy's cells) loses per second as the tongue, jaw and lips move, pushed in at the glottis's end. Behind a
     * closure it is what builds the pressure that stops the voicing in a stop (without it a /g/ stayed voiced ~80 ms,
     * CHIMERA's devoices in ~15) */
    uint8_t vol_src;                        /* 0 off, 1 behind a closure, 2 behind a closure or a constriction under vol_narrow */
    ch_real vol_narrow, vol_smooth;
    uint8_t narrowest;                      /* a section's area: its narrowest anatomy cell's (1), its middle's (0) */
    ch_real vol_u, vol_goal;
    ch_real vcell[2 * CH_MAXTUBE];          /* each anatomy cell's volume at the last shape (pharynx, then mouth) */
    uint8_t vol_ok;
    int vol_cells;
    /* the glottis: CHIMERA's area model and its port's constants (HumanAirway's glottis_port) */
    ch_glottis_t glottis;
    ch_real port_len, port_slit, port_kin, port_kex;
    ch_real u_g, l_prev, mom, p_supra, a_g;
    ch_real p_lung, p_lung_now, p_lung_step;
    /* the noise sources: CHIMERA's (ch_noise_t), and the junctions they read and go in at */
    ch_noise_t noise[4];
    int16_t n_src[4], n_fwd[4], n_bwd[4];
    uint8_t n_noise;
    uint32_t rng;
    /* the output: CHIMERA's gain, DC blocker and limiter */
    ch_real rad_prev, output_gain, dc_x, dc_y, dc_coef, lim_env, lim_release;
    int32_t lim_hold, lim_hold_n;
} ch_wave_t;

/* a body's waveguide: section counts from its neutral tract length (cm), the nose's from its length */
void ch_wave_init(ch_wave_t *w, ch_real fs, int ctrl_every, ch_real body_scale, ch_real tract_len);
/* a control step: the glottis's parameters and the lungs' pressure, and when the anatomy has a new shape
 * (ch_tract_shape of an apply) that shape, glided to over `steps` control steps (the anatomy runs every `steps`
 * steps); with t NULL the tract keeps gliding to the last one */
void ch_wave_block(ch_wave_t *w, const ch_shape_t *t, int steps, const ch_glottis_params_t *g, ch_real p_lung);
void ch_wave_render(ch_wave_t *w, float *out, uint32_t n);

#endif

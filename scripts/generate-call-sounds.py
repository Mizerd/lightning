#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rokas Smetonis
#
# Renders Lightning's call sound effects into data/sounds/*.wav.
#
# Every sound is synthesised here; nothing is sampled, so this file is the
# complete source of the WAVs, which are licensed like the rest of Lightning
# (GPL-3.0-or-later). Needs numpy and scipy:
#
#     nix-shell -p "python3.withPackages (p: [p.numpy p.scipy])" --run \
#         "python3 scripts/generate-call-sounds.py"              # data/sounds
#     ... generate-call-sounds.py --direction E2 --out /tmp/e2   # a variant
#     ... generate-call-sounds.py --check                        # levels only
#     ... generate-call-sounds.py --preview /tmp/preview.wav     # all 17
#
# Output is deterministic: every random choice (noise, humanised timing and
# velocity, reverb) comes from a generator seeded by the direction and the
# sound's name, so the same numpy/scipy give the same bytes.
#
# The set is "Clean UI" (E): short triangle/sine plucks with a quick filter
# envelope, soft triangle pads, a small dry room, in E-flat major. Four
# variants play the same score through a few named dials (see E_STYLE):
#
#   E1 "warmer"            rounder and lower: gentler filter, more sine,
#                          longer decays, softer taps
#   E2 "brighter"          crisper and shorter: the filter opens further and
#                          closes faster, shorter decays, drier
#   E3 "fuller"            add9/maj9 voicings, a soft pad under the session
#                          cues, a wider stereo picture and a longer room,
#                          and a gentler "disconnected" (a falling F minor
#                          figure instead of a drifting diminished chord)
#   E4 "new key and ring"  F major, and a different ringer tune and rhythm
#
# Rules every variant keeps:
#   * Western diatonic harmony: major and minor triads, add9, maj7 and maj9
#     voicings, perfect fourths and fifths, chord movement (I-IV-V, I-vi-IV)
#     so the 4th and 7th degrees are heard. No pentatonic runs.
#   * Harmonic partials only (triangle, sine). No bells, glass or bowls, no
#     plucked-string models, no pitch bends.
#   * Tamed tops: every partial passes a 4-pole roll-off at 3.2 kHz
#     (warm_guard), filters open no higher than ~3 kHz, noise is band-passed
#     below 1.5 kHz, the master is low-passed at 8 kHz. Rounded attacks:
#     8-11 ms on tones, 4.5 ms on the four tap controls; no clicks.
#   * Meaning: rising = arriving/on, falling = leaving/off; your own buttons
#     are short and dry; other people are melodic; sharing is a soft air
#     swell; a raised hand repeats one note; losing the connection falls and
#     stays unresolved (E3: through F minor; the others: a tritone into a
#     diminished chord that drifts out of tune).
#   * Levels matched by BS.1770 max momentary loudness (M-max, 400 ms) per
#     category, the ringer loudest and the in-call loops quietest; true peak
#     never above -1 dBTP.
#   * Loops (ring, ringback, call-waiting) are rendered as a periodic signal
#     (four periods, the third kept), so the end of the phrase continues into
#     its start: the seam is continuous by construction.
#   * Every file starts and ends at exact digital zero behind a 3-5 ms fade.
#
# Output: 48 kHz, 16-bit, stereo (or mono with --channels 1). QSoundEffect
# takes the format from the file.

import argparse
import os
import sys
import wave
import zlib

import numpy as np
from scipy import signal

SR = 48000
LEAD = 0.004  # seconds of silence before the first note, under the fade-in

# The direction data/sounds is rendered from: the one the app ships.
DEFAULT_DIRECTION = 'E3'

ORDER = ['ring', 'ringback', 'call-waiting', 'connected', 'ended',
         'disconnected', 'join', 'leave', 'mute', 'unmute', 'deafen',
         'undeafen', 'share-start', 'share-stop', 'hand-raised',
         'message', 'mention']

# Max momentary loudness (LUFS, as played on stereo speakers) per sound.
# Ringer loudest; session cues next; social, share and hand a step below;
# controls short and quieter; the loops that play DURING a call quietest.
TARGET = {
    'ring': -11.0, 'ringback': -23.5, 'call-waiting': -23.5,
    'connected': -17.0, 'ended': -17.5, 'disconnected': -17.0,
    'join': -18.0, 'leave': -18.5,
    'mute': -21.0, 'unmute': -21.0, 'deafen': -21.0, 'undeafen': -21.0,
    'share-start': -18.0, 'share-stop': -18.5, 'hand-raised': -18.0,
    # Desktop notifications: not a call cue, but the same voice. A message
    # sits with the controls (it can repeat all day); a mention one step up.
    'message': -21.0, 'mention': -18.5,
}
PEAK_CAP_DBTP = -1.0
# Controls are taps: end them once the tail is 50 dB down, not 60.
TRIM_DB = {'mute': -50.0, 'unmute': -50.0, 'deafen': -50.0, 'undeafen': -50.0,
           # Notification chimes end once the tail is 35 dB down, so they
           # stay under half a second however long the room rings.
           'message': -22.0, 'mention': -22.0}


# ── tuning and time ────────────────────────────────────────────────────────

_SEMI = {'C': -9, 'D': -7, 'E': -5, 'F': -4, 'G': -2, 'A': 0, 'B': 2}


def hz(name):
    """'F#4', 'Eb3', 'C5' -> Hz, equal temperament, A4 = 440."""
    letter, acc, octave = name[0], name[1:-1], int(name[-1])
    s = _SEMI[letter] + {'': 0, '#': 1, 'b': -1}[acc] + 12 * (octave - 4)
    return 440.0 * 2.0 ** (s / 12.0)


def secs(d):
    return int(round(d * SR))


def taxis(n):
    return np.arange(n) / SR


def decay(t, t60):
    return np.exp(-6.907755 * t / t60)


def ramp(n, seconds):
    """Raised-cosine rise from exactly 0 over `seconds`, then 1."""
    k = min(n, max(1, secs(seconds)))
    r = np.ones(n)
    r[:k] = 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    return r


def damper(n, at, release=0.15):
    """1 until `at`, then a raised-cosine fall to 0 over `release`."""
    d = np.ones(n)
    i0, k = secs(at), max(1, secs(release))
    if i0 < n:
        seg = 0.5 + 0.5 * np.cos(np.pi * np.arange(min(k, n - i0)) / k)
        d[i0:i0 + len(seg)] = seg
        d[i0 + len(seg):] = 0.0
    return d


def lp4(x):
    """Magnitude of a 4-pole (24 dB/octave) Butterworth low-pass at f/fc=x."""
    return 1.0 / np.sqrt(1.0 + x ** 8)


def warm_guard(f):
    """Every partial passes this: flat to ~2 kHz, -3 dB at 3.2 kHz, then
    24 dB/octave. Keeps every voice out of the 2-6 kHz band that reads as
    harsh."""
    return lp4(f / 3200.0)


def stereo(x):
    """A mono signal (1-D or one row) as two identical channels: how a mono
    file plays on stereo speakers."""
    if x.ndim == 1:
        return np.vstack([x, x])
    return np.vstack([x[0], x[0]]) if x.shape[0] == 1 else x


def peak_norm(x):
    p = np.abs(x).max()
    return x / p if p > 0 else x


def phase_of(inst):
    return 2 * np.pi * np.concatenate([[0.0], np.cumsum(inst)[:-1]]) / SR


def envelope(n, attack, decay_tau, sustain=0.0, release=0.06):
    """Raised-cosine attack, exponential decay towards `sustain`, and a
    raised-cosine release that ends the buffer at zero."""
    t = taxis(n)
    env = sustain + (1.0 - sustain) * np.exp(-np.maximum(t - attack, 0.0)
                                             / decay_tau)
    return env * ramp(n, attack) * damper(n, n / SR - release, release)


# ── oscillators and effects ────────────────────────────────────────────────

def osc_bank(phase, finst, fc, shape='saw', res=0.0, fmax=6000.0):
    """Band-limited saw, square or triangle by additive synthesis, through a
    4-pole low-pass whose cutoff may move (fc: array). Harmonic partials
    only; no aliasing; `res` adds a small resonant bump at the cutoff."""
    y = np.zeros_like(phase)
    kmax = int(fmax / max(float(np.min(finst)), 20.0))
    for k in range(1, kmax + 1):
        if shape == 'saw':
            a = 1.0 / k
        elif k % 2 == 0:
            continue
        elif shape == 'square':
            a = 1.0 / k
        else:  # triangle
            a = (-1.0) ** ((k - 1) // 2) / k ** 2
        x = k * finst / fc
        h = lp4(x) * warm_guard(k * finst)
        if res:
            h = h * (1.0 + res * np.exp(-np.log2(np.maximum(x, 1e-6)) ** 2
                                        / 0.08))
        if h.max() * abs(a) < 1e-4:
            continue
        y += a * h * np.sin(k * phase)
    return y


def noise_shape(rng, dur, gain_fn, channels=2):
    """White noise shaped in the STFT domain by gain_fn(f[:,None],
    t[None,:]) -> gain."""
    n = secs(dur)
    x = rng.standard_normal((channels, n + 2048))
    f, tt, z = signal.stft(x, SR, nperseg=1024, noverlap=768)
    z = z * gain_fn(np.maximum(f, 1.0)[:, None], tt[None, :])[None]
    _, y = signal.istft(z, SR, nperseg=1024, noverlap=768)
    return peak_norm(y[:, :n])


def air(rng, dur, f0, f1, bw_oct=1.2, rise=0.7, top=1500.0):
    """Soft band-passed noise whose centre moves from f0 to f1 (log): the air
    of a window sliding into or out of view. Low-passed at `top` with 24
    dB/octave and tilted pink, so it never hisses."""
    def g(f, t):
        frac = np.clip(t / dur, 0.0, 1.0)
        fc = f0 * (f1 / f0) ** frac
        o = np.log2(f / fc)
        return (np.exp(-0.5 * (o / (bw_oct / 2)) ** 2) * lp4(f / top)
                * (500.0 / f) ** 0.5)
    y = noise_shape(rng, dur, g)
    t = taxis(y.shape[1]) / dur
    env = np.where(t < rise, np.sin(0.5 * np.pi * t / rise) ** 2,
                   np.cos(0.5 * np.pi * np.clip((t - rise) / (1 - rise), 0, 1))
                   ** 2)
    return y * env


# ── voices ─────────────────────────────────────────────────────────────────

def analog(f, dur, vel=0.8, shape='saw', detune=7.0, sub=0.2, fc_peak=1600.0,
           fc_end=450.0, f_attack=0.012, ftau=0.13, res=0.1, attack=0.010,
           decay_tau=0.32, sustain=0.0, release=0.08, fc_fn=None, drift=None):
    """Two detuned oscillators (one leaning each way) plus a sine sub an
    octave down, through a 4-pole low-pass with its own envelope: the analog
    pluck, bass and stab. `fc_fn(t)` replaces the filter envelope (for
    sweeps); `drift(t)` in cents pushes the two oscillators apart."""
    n = secs(dur)
    t = taxis(n)
    if fc_fn is not None:
        fc = fc_fn(t)
    else:
        fenv = ramp(n, f_attack) * np.exp(-np.maximum(t - f_attack, 0.0)
                                          / ftau)
        fc = fc_end + (fc_peak - fc_end) * fenv
    outs = []
    for sign in (-1.0, 1.0):
        cents = sign * detune + (sign * drift(t) if drift is not None else 0.0)
        inst = f * 2.0 ** (cents / 1200.0) * np.ones(n)
        outs.append(osc_bank(phase_of(inst), inst, fc, shape, res))
    lo, hi = outs
    left, right = lo + 0.6 * hi, 0.6 * lo + hi
    if sub:
        s = sub * 1.4 * np.sin(phase_of(0.5 * f * np.ones(n)))
        left, right = left + s, right + s
    env = envelope(n, attack, decay_tau, sustain, release)
    return vel * peak_norm(np.vstack([left * env, right * env]))


def pad(rng, freqs, dur, vel=0.5, shape='saw', fc=900.0, attack=0.25,
        release=0.45, detune=8.0, breath=None, fc_fn=None, drift=None):
    """Three detuned oscillators per note, low-passed, swelling in and out.
    `breath` = (rate, depth) is a slow amplitude pulse."""
    n = secs(dur)
    t = taxis(n)
    cut = fc_fn(t) if fc_fn is not None else fc * np.ones(n)
    left = np.zeros(n)
    right = np.zeros(n)
    for f in freqs:
        for j, c in enumerate((-detune, 0.0, detune)):
            cents = c + ((j - 1) * drift(t) if drift is not None else 0.0)
            inst = f * 2.0 ** (cents / 1200.0) * np.ones(n)
            v = osc_bank(phase_of(inst) + rng.uniform(0, 2 * np.pi), inst, cut,
                         shape, 0.05, fmax=4000.0)
            left += (0.7, 0.5, 0.3)[j] * v
            right += (0.3, 0.5, 0.7)[j] * v
    env = ramp(n, attack) * damper(n, dur - release, release)
    if breath:
        rate, depth = breath
        env = env * (1.0 - depth * 0.5 * (1.0 - np.cos(2 * np.pi * rate * t)))
    return vel * peak_norm(np.vstack([left, right]) * env)


def ui_tone(f, dur=None, vel=0.8, tau=0.16, attack=0.009, fc_peak=2200.0,
            fc_end=800.0, ftau=0.05, body=0.5, detune=2.0, sub=0.0,
            edge=0.0):
    """E's voice: a triangle and a sine together, the triangle's harmonics
    opening briefly and closing within 50 ms. Clean, short, no click.
    `sub` adds a sine an octave down, for weight; `edge` blends in a
    sawtooth through the same fast-closing filter, for a crisper attack."""
    dur = dur or max(0.25, tau * 6.0)
    n = secs(dur)
    t = taxis(n)
    fc = fc_end + (fc_peak - fc_end) * ramp(n, 0.008) * np.exp(-t / ftau)
    outs = []
    for cents in (-detune, detune):
        inst = f * 2.0 ** (cents / 1200.0) * np.ones(n)
        ph = phase_of(inst)
        voice = osc_bank(ph, inst, fc, 'tri') + body * np.sin(ph)
        if sub:
            voice = voice + sub * np.sin(0.5 * ph)
        if edge:
            voice = voice + edge * osc_bank(ph, inst, fc, 'saw')
        outs.append(voice)
    a, b = outs
    env = envelope(n, attack, tau, 0.0, min(0.06, dur / 4))
    return vel * np.vstack([env * (0.6 * a + 0.4 * b),
                            env * (0.4 * a + 0.6 * b)])


def tap(f, vel=0.8, shape='tri', fc=1300.0, tau=0.035, attack=0.0045):
    """A button: a short, filtered blip with a 4.5 ms rounded attack."""
    n = secs(max(0.12, tau * 7.0))
    inst = f * np.ones(n)
    y = osc_bank(phase_of(inst), inst, fc * np.ones(n), shape) \
        + 0.4 * np.sin(phase_of(inst))
    return vel * y * envelope(n, attack, tau, 0.0, 0.02)


def sub(f, dur, vel=0.5, attack=0.012, tau=0.4, release=0.1):
    n = secs(dur)
    t = taxis(n)
    return (vel * np.sin(phase_of(f * np.ones(n))) * np.exp(-t / tau)
            * ramp(n, attack) * damper(n, dur - release, release))


def opening(f0, f1, over):
    """A filter cutoff moving exponentially from f0 to f1 over `over` s."""
    return lambda t: f0 * (f1 / f0) ** np.clip(t / over, 0.0, 1.0)


# ── room and mixer ─────────────────────────────────────────────────────────

def make_ir(seed, rt_lo, rt_hi, length, predelay, er_count=6):
    """A synthetic stereo room: decorrelated noise per side, decaying faster
    in the treble than the bass (rt_lo at 250 Hz, rt_hi at 6 kHz), a few
    early reflections, a pre-delay; each side unit energy."""
    rng = np.random.default_rng(seed)
    n = secs(length)
    x = rng.standard_normal((2, n + 1024))
    f, tt, z = signal.stft(x, SR, nperseg=512, noverlap=384)
    lf = np.log(np.clip(f, 250.0, 6000.0))
    rt = rt_lo + (rt_hi - rt_lo) * (lf - np.log(250.0)) \
        / (np.log(6000.0) - np.log(250.0))
    z = z * np.exp(-6.9078 * tt[None, None, :] / rt[None, :, None])
    z = z * lp4(f / 5000.0)[None, :, None]
    _, ir = signal.istft(z, SR, nperseg=512, noverlap=384)
    ir = ir[:, :n] * ramp(n, 0.012)
    for c in range(2):
        for k in range(er_count):
            at = secs(rng.uniform(0.004, 0.032))
            ir[c, at] += rng.choice([-1.0, 1.0]) * 0.9 * (0.8 ** k) \
                * ir[c].std() * 12
    ir = np.concatenate([np.zeros((2, secs(predelay))), ir], axis=1)
    return ir / np.sqrt((ir ** 2).sum(axis=1, keepdims=True))


class Mix:
    """A stereo buffer with a reverb send."""

    def __init__(self, seconds):
        self.n = secs(seconds)
        self.dry = np.zeros((2, self.n))
        self.wet = np.zeros((2, self.n))

    def add(self, at, sig, pan=0.0, gain=1.0, send=0.25):
        sig = stereo(sig)
        th = (pan + 1.0) * np.pi / 4.0
        gl, gr = np.cos(th) * np.sqrt(2.0), np.sin(th) * np.sqrt(2.0)
        i0 = secs(at + LEAD)
        i1 = min(self.n, i0 + sig.shape[1])
        if i1 <= i0:
            return
        seg = sig[:, :i1 - i0] * gain
        # Whatever made it, a segment ends with a 5 ms fade: a voice cut
        # off above silence would click.
        k = min(secs(0.005), seg.shape[1])
        seg[:, -k:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(1, k + 1) / k)
        seg = np.vstack([seg[0] * gl, seg[1] * gr])
        self.dry[:, i0:i1] += seg
        self.wet[:, i0:i1] += seg * send

    def render(self, ir):
        out = self.dry.copy()
        if np.any(self.wet):
            for c in range(2):
                out[c] += signal.fftconvolve(self.wet[c], ir[c])[:self.n]
        return out


def pitch_pan(f, centre=440.0, width=0.3):
    """Low notes a little left, high a little right."""
    return float(np.clip(np.log2(f / centre) * width, -0.35, 0.35))


class Human:
    """Small deterministic timing and velocity variation."""

    def __init__(self, rng, t_ms=5.0, v=0.05):
        self.rng, self.t, self.v = rng, t_ms / 1000.0, v

    def at(self, x):
        # Only ever late: a note never starts before LEAD, under the fade-in.
        return x + self.rng.uniform(0.0, self.t)

    def vel(self, x):
        return x * (1.0 + self.rng.uniform(-self.v, self.v))


# ── the E family: "Clean UI" and its variants ─────────────────────────────
#
# Short triangle/sine plucks with a quick filter envelope, soft triangle
# pads, a small dry room. E-flat major; the ringer walks I (Ebmaj7) - IV
# (Abmaj7) - V (Bb). Every variant plays the same score through a STYLE: a
# few named dials, all at E's values for E itself, so each variant differs
# from E only in the dials it names (and uses E's random seeds, so even the
# humanised timing is the same).

E_ROOM = dict(seed=606, rt_lo=0.5, rt_hi=0.28, length=1.0, predelay=0.008)
# E3's room: a little longer. A synthetic room, like a real one, answers
# some exact pitches more than others; this seed is the one (of 600-615)
# that treats the set's notes most evenly: every sound 1.5 dB wetter than in
# E on average, none more than 3.9 dB (seed 606 put C5 on a resonance, 8 dB).
E3_ROOM = dict(seed=602, rt_lo=0.75, rt_hi=0.4, length=1.4, predelay=0.012)

E_STYLE = dict(
    tr=0,            # transposition, semitones (the whole set but the ring)
    bright=1.0,      # scales every filter's peak
    bright_end=1.0,  # scales where the filters settle
    snap=1.0,        # scales how fast the pluck's brightness closes
    decay=1.0,       # scales note decays
    body=0.5,        # sine blended into the triangle (more = rounder)
    attack=0.009,    # pluck attack, seconds
    sub=0.0,         # octave-down sine under each pluck (weight)
    edge=0.0,        # sawtooth blended into the pluck's attack (crispness)
    tap_bright=1.0,  # scales the button taps' filter
    tap_decay=1.0,   # scales the button taps' decay
    send=1.0,        # scales every reverb send
    width=1.0,       # scales every pan position
    rich=False,      # add9/maj9 voicings, a soft pad under the session cues
    ring='E',        # which ringer melody
    lost='tense',    # disconnected: 'tense' (tritone, drifting diminished
                     # chord) or 'soft' (a falling minor figure, no drift)
)


def e_style(**changes):
    s = dict(E_STYLE)
    s.update(changes)
    return s


# E1 "warmer": rounder and lower; a gentler filter, more sine, a little
# weight an octave down, longer decays, softer taps.
E1_STYLE = e_style(bright=0.55, bright_end=0.7, snap=1.3, decay=1.3,
                   body=1.0, attack=0.011, sub=0.18, tap_bright=0.7,
                   tap_decay=1.2)
# E2 "brighter": crisper and shorter; a little sawtooth edge on each
# attack, the filter opening further and closing faster, less sine, shorter
# decays, drier.
E2_STYLE = e_style(bright=1.4, bright_end=1.25, snap=0.5, decay=0.65,
                   body=0.3, attack=0.009, edge=0.35, tap_bright=1.25,
                   tap_decay=0.8, send=0.7)
# E3 "fuller": richer voicings and a soft pad under the session cues, a wider
# stereo picture and a slightly longer room.
E3_STYLE = e_style(rich=True, width=1.6, send=1.25, lost='soft')
# E4 "new key and ring": F major, and a different ringer melody and rhythm.
E4_STYLE = e_style(tr=2, ring='E4')


def e_family(S):
    """The seventeen sounds of E (fifteen for calls, two notification
    chimes), played in style S."""

    def F(name):
        return hz(name) * 2.0 ** (S['tr'] / 12.0)

    def tone(f, v, tau=0.16, fc_peak=2200.0, attack=None):
        attack = S['attack'] if attack is None else attack
        return ui_tone(f, None, v, tau=tau * S['decay'], attack=attack,
                       fc_peak=fc_peak * S['bright'],
                       fc_end=800.0 * S['bright_end'], ftau=0.05 * S['snap'],
                       body=S['body'], sub=S['sub'], edge=S['edge'])

    def btn(f, v, fc, tau=0.035):
        return tap(f, v, 'tri', fc=fc * S['tap_bright'],
                   tau=tau * S['tap_decay'])

    def send(x):
        return x * S['send']

    def pan(x):
        return x * S['width']

    def soft_pad(rng, freqs, dur, vel, attack=0.08, release=0.6, fc=1200.0,
                 **kw):
        return pad(rng, freqs, dur, vel, shape='tri', fc=fc * S['bright'],
                   attack=attack, release=release, **kw)

    def play(m, h, notes, snd):
        """notes: (time, Hz, velocity, tone kwargs), humanised."""
        for when, f, vel, kw in notes:
            m.add(h.at(when), tone(f, h.vel(vel), **kw),
                  pitch_pan(f, 440.0, 0.3 * S['width']), send=send(snd))

    def extra_rng():
        # E3's added pads draw from their own generator, so they never
        # shift the humanised timing E's own draws produce.
        return np.random.default_rng(3)

    def ring_e(m, rng, t0):
        # Two crisp four-note runs per bar (up an arpeggio, then a held
        # step), Ebmaj7 then Abmaj7 - Bb, ending on the leading tone.
        h = Human(rng, t_ms=3.0)
        step = 0.11
        rich = S['rich']
        prng = extra_rng() if rich else rng

        def run(t, names, hold_vel=0.85):
            notes = [(t + i * step, F(nm), 0.62 + 0.06 * i, {})
                     for i, nm in enumerate(names[:3])]
            notes.append((t + 3 * step, F(names[3]), hold_vel,
                          dict(tau=0.4)))
            play(m, h, notes, 0.2)

        chords = ((('Eb3', 'G3', 'Bb3', 'D4', 'F4'), ('Ab3', 'C4', 'Eb4', 'G4',
                                                      'Bb4'),
                   ('Bb3', 'D4', 'F4', 'C5')) if rich else
                  (('Eb3', 'G3', 'Bb3', 'D4'), ('Ab3', 'C4', 'Eb4', 'G4'),
                   ('Bb3', 'D4', 'F4')))
        m.add(t0, soft_pad(prng, [F(x) for x in chords[0]], 2.6, 0.2), 0.0,
              send=send(0.25))
        m.add(t0, sub(F('Eb3'), 1.2, 0.35, tau=0.5), 0.0, send=send(0.1))
        run(t0, ['G4', 'Bb4', 'Eb5', 'D5'])
        run(t0 + 0.66, ['G4', 'Bb4', 'Eb5', 'F5'])
        b = t0 + 3.0
        m.add(b, soft_pad(prng, [F(x) for x in chords[1]], 1.0, 0.2,
                          release=0.35), 0.0, send=send(0.25))
        m.add(b + 0.66, soft_pad(prng, [F(x) for x in chords[2]], 1.9, 0.2),
              0.0, send=send(0.25))
        m.add(b, sub(F('Ab3'), 0.66, 0.3, tau=0.4), 0.0, send=send(0.1))
        m.add(b + 0.66, sub(F('Bb3'), 1.2, 0.3, tau=0.5), 0.0,
              send=send(0.1))
        run(b, ['Ab4', 'C5', 'Eb5', 'C5'])
        run(b + 0.66, ['Bb4', 'D5', 'F5', 'D5'], hold_vel=0.8)

    def ring_e4(m, rng, t0):
        # A different tune in F major: a dotted rising arpeggio, then a
        # four-note scale run as its answer. Bar one falls (Fmaj7 - Dm7),
        # bar two rises to the leading tone (Bbmaj7 - C), so the loop asks.
        # Written in absolute pitches; not transposed by S['tr'].
        h = Human(rng, t_ms=3.0)
        dotted = (0.0, 0.165, 0.33)
        run4 = (0.0, 0.11, 0.22, 0.33)

        def motif(t, names, times, hold_vel=0.85):
            notes = []
            for i, (dt, nm) in enumerate(zip(times, names)):
                last = i == len(names) - 1
                notes.append((t + dt, hz(nm),
                              hold_vel if last else 0.62 + 0.06 * i,
                              dict(tau=0.4) if last else {}))
            play(m, h, notes, 0.2)

        m.add(t0, soft_pad(rng, [hz(x) for x in ('F3', 'A3', 'C4', 'E4')],
                           0.9, 0.2, release=0.3), 0.0, send=send(0.25))
        m.add(t0 + 0.66, soft_pad(rng, [hz(x) for x in ('D3', 'F3', 'A3',
                                                        'C4')], 1.9, 0.2),
              0.0, send=send(0.25))
        m.add(t0, sub(hz('F3'), 0.66, 0.35, tau=0.4), 0.0, send=send(0.1))
        m.add(t0 + 0.66, sub(hz('D3'), 1.2, 0.3, tau=0.5), 0.0,
              send=send(0.1))
        motif(t0, ['A4', 'C5', 'F5'], dotted)
        motif(t0 + 0.66, ['E5', 'D5', 'C5', 'A4'], run4)
        b = t0 + 3.0
        m.add(b, soft_pad(rng, [hz(x) for x in ('Bb3', 'D4', 'F4', 'A4')],
                          0.9, 0.2, release=0.3), 0.0, send=send(0.25))
        m.add(b + 0.66, soft_pad(rng, [hz(x) for x in ('C4', 'E4', 'G4')],
                                 1.9, 0.2), 0.0, send=send(0.25))
        m.add(b, sub(hz('Bb3'), 0.66, 0.3, tau=0.4), 0.0, send=send(0.1))
        m.add(b + 0.66, sub(hz('C4'), 1.2, 0.3, tau=0.5), 0.0,
              send=send(0.1))
        motif(b, ['Bb4', 'D5', 'F5'], dotted)
        motif(b + 0.66, ['G4', 'C5', 'D5', 'E5'], run4, hold_vel=0.8)

    def ringback(m, rng, t0):
        names = ('Eb4', 'G4', 'Bb4', 'D5') if S['rich'] else \
            ('Eb4', 'G4', 'Bb4')
        m.add(t0, pad(rng, [F(x) for x in names], 1.4, 0.5, shape='tri',
                      fc=1300.0 * S['bright'], attack=0.2, release=0.45,
                      breath=(3.0, 0.2)), 0.0, send=send(0.25))
        m.add(t0, sub(F('Eb3'), 1.3, 0.25, attack=0.06, tau=0.8), 0.0,
              send=send(0.1))

    def call_waiting(m, rng, t0):
        # The ringer's first run, soft and quick.
        h = Human(rng, t_ms=2.0)
        if S['ring'] == 'E4':
            head = [(0.00, hz('A4'), 0.5), (0.12, hz('C5'), 0.55),
                    (0.24, hz('F5'), 0.62)]
        else:
            head = [(0.00, F('G4'), 0.5), (0.09, F('Bb4'), 0.55),
                    (0.18, F('Eb5'), 0.6), (0.27, F('D5'), 0.65)]
        play(m, h, [(t0 + t, f, v, dict(tau=0.1, fc_peak=1500.0))
                    for t, f, v in head], 0.18)

    def connected(m, rng):
        h = Human(rng, t_ms=2.0)
        play(m, h, [(0.00, F('Eb4'), 0.6, {}), (0.05, F('G4'), 0.66, {}),
                    (0.10, F('Bb4'), 0.72, {}),
                    (0.15, F('Eb5'), 0.85, dict(tau=0.35))], 0.22)
        bloom = ('Eb4', 'G4', 'Bb4', 'D5', 'F5') if S['rich'] else \
            ('Eb4', 'G4', 'Bb4')
        m.add(0.15, soft_pad(rng, [F(x) for x in bloom], 0.8, 0.25,
                             attack=0.04, release=0.5, fc=1300.0), 0.0,
              send=send(0.22))
        if S['rich']:
            m.add(0.1, soft_pad(extra_rng(), [F(x) for x in ('Eb3', 'Bb3',
                                                               'F4')],
                                1.4, 0.15, attack=0.12, release=0.7), 0.0,
                  send=send(0.25))

    def ended(m, rng):
        h = Human(rng, t_ms=2.0)
        dark = dict(fc_peak=1500.0)  # ended's tones are a little darker
        play(m, h, [(0.00, F('Bb4'), 0.75, dark), (0.08, F('G4'), 0.7, dark),
                    (0.16, F('Eb4'), 0.75, dict(tau=0.35, **dark))], 0.22)
        m.add(0.16, sub(F('Eb3'), 0.9, 0.35, tau=0.35), 0.0, send=send(0.1))
        if S['rich']:
            m.add(0.12, soft_pad(extra_rng(), [F(x) for x in ('Eb3', 'Bb3',
                                                                'F4', 'G4')],
                                 1.3, 0.18, attack=0.1, release=0.7), 0.0,
                  send=send(0.25))

    def disconnected(m, rng):
        if S['lost'] == 'soft':
            return disconnected_soft(m, rng)
        h = Human(rng, t_ms=2.0)
        play(m, h, [(0.00, F('D5'), 0.7, dict(tau=0.2)),
                    (0.11, F('Ab4'), 0.7, dict(tau=0.2))], 0.2)
        names = ('D4', 'F4', 'Ab4', 'C5') if S['rich'] else \
            ('D4', 'F4', 'Ab4')
        m.add(0.11, pad(rng, [F(x) for x in names], 0.9, 0.55, shape='tri',
                        attack=0.02, release=0.45,
                        fc_fn=opening(1600.0 * S['bright'], 300.0, 0.7),
                        drift=lambda t: 30.0 * np.clip(t / 0.7, 0, 1)),
              0.0, send=send(0.2))

    def disconnected_soft(m, rng):
        # Connection lost, kindly: three soft notes falling through F minor
        # (the ii chord, which wants to go somewhere and doesn't), landing on
        # an F minor 9 pad that slowly darkens. No tritone, no drift, a
        # 14 ms attack, darker plucks than the other cues.
        h = Human(rng, t_ms=2.0)
        soft = dict(fc_peak=1300.0, attack=0.014)
        play(m, h, [(0.00, F('C5'), 0.62, dict(tau=0.22, **soft)),
                    (0.15, F('Ab4'), 0.58, dict(tau=0.22, **soft)),
                    (0.30, F('F4'), 0.64, dict(tau=0.3, **soft))], 0.24)
        names = ('F3', 'Ab3', 'C4', 'Eb4', 'G4') if S['rich'] else \
            ('F3', 'Ab3', 'C4', 'Eb4')
        m.add(0.26, pad(extra_rng(), [F(x) for x in names], 1.0, 0.3,
                        shape='tri', attack=0.14, release=0.55,
                        fc_fn=opening(1300.0 * S['bright'], 450.0, 0.9)),
              0.0, send=send(0.25))

    def join(m, rng):
        m.add(0.0, tone(F('Bb4'), 0.75, tau=0.12), pan(-0.1), send=send(0.18))
        m.add(0.08, tone(F('Eb5'), 0.85, tau=0.16), pan(0.1), send=send(0.18))

    def leave(m, rng):
        m.add(0.0, tone(F('Eb5'), 0.8, tau=0.12), pan(0.1), send=send(0.18))
        m.add(0.09, tone(F('Bb4'), 0.65, tau=0.09), pan(-0.1),
              send=send(0.18))

    def mute(m, rng):
        m.add(0.0, btn(F('Bb4'), 0.8, 1600.0), 0.0, send=send(0.03))
        m.add(0.045, btn(F('Eb4'), 0.75, 1300.0, tau=0.03), 0.0,
              send=send(0.03))

    def unmute(m, rng):
        m.add(0.0, btn(F('Eb4'), 0.75, 1300.0, tau=0.03), 0.0,
              send=send(0.04))
        m.add(0.045, btn(F('Bb4'), 0.85, 1800.0, tau=0.05), 0.0,
              send=send(0.04))

    def deafen(m, rng):
        close = opening(1600.0 * S['tap_bright'], 250.0, 0.22)
        for when, notes, v in ((0.0, ('Eb4', 'Bb4'), 0.75),
                               (0.08, ('Bb3', 'F4'), 0.8)):
            for name in notes:
                m.add(when, analog(F(name), 0.3, v, shape='tri', sub=0.0,
                                   fc_fn=lambda t, w=when: close(t + w),
                                   attack=0.005,
                                   decay_tau=0.09 * S['tap_decay']),
                      0.0, send=send(0.06))

    def undeafen(m, rng):
        op = opening(250.0, 1800.0 * S['tap_bright'], 0.2)
        for when, notes, v in ((0.0, ('Bb3', 'F4'), 0.7),
                               (0.08, ('Eb4', 'Bb4'), 0.85)):
            for name in notes:
                m.add(when, analog(F(name), 0.35, v, shape='tri', sub=0.0,
                                   fc_fn=lambda t, w=when: op(t + w),
                                   attack=0.005,
                                   decay_tau=0.12 * S['tap_decay']),
                      0.0, send=send(0.08))

    def chord(rich_names, plain_names):
        return rich_names if S['rich'] else plain_names

    def share_start(m, rng):
        m.add(0.0, air(rng, 0.3, 400.0, 1100.0, rise=0.85), 0.0, gain=1.1,
              send=send(0.2))
        names = chord(('Eb4', 'G4', 'Bb4', 'D5', 'F5'), ('Eb4', 'Bb4', 'F5'))
        k = len(names)
        for i, name in enumerate(names):
            p = -0.2 + 0.4 * i / (k - 1)
            m.add(0.26 + 0.02 * i,
                  tone(F(name), 0.65 if k == 3 else 0.5, tau=0.25), pan(p),
                  send=send(0.22))

    def share_stop(m, rng):
        names = chord(('F5', 'D5', 'Bb4', 'G4', 'Eb4'), ('F5', 'Bb4', 'Eb4'))
        k = len(names)
        for i, name in enumerate(names):
            p = 0.2 - 0.4 * i / (k - 1)
            m.add(0.02 * i, tone(F(name), 0.6 if k == 3 else 0.46, tau=0.15),
                  pan(p), send=send(0.22))
        m.add(0.06, air(rng, 0.34, 1100.0, 400.0, rise=0.25), 0.0, gain=1.0,
              send=send(0.2))

    def hand_raised(m, rng):
        m.add(0.0, tone(F('C5'), 0.65, tau=0.18), 0.0, send=send(0.22))
        m.add(0.2, tone(F('C5'), 0.85, tau=0.4), 0.0, send=send(0.22))

    def message(m, rng):
        # A new message: two soft notes rising a fourth through the tonic's
        # major seventh and third (D5 - G5 over Ebmaj7). Short, high and
        # dry, so it can repeat all day without wearing.
        h = Human(rng, t_ms=2.0)
        play(m, h, [(0.00, F('D5'), 0.55, dict(tau=0.07, fc_peak=1800.0)),
                    (0.075, F('G5'), 0.7, dict(tau=0.11, fc_peak=1800.0))],
             0.14)

    def mention(m, rng):
        # Someone wants you: the same two notes, then Bb5 on top of them
        # (the Ebmaj7 arpeggio's last step) held a little longer, with a
        # small pad under it, so it is plainly the same family and plainly
        # more.
        h = Human(rng, t_ms=2.0)
        play(m, h, [(0.00, F('D5'), 0.58, dict(tau=0.07, fc_peak=1900.0)),
                    (0.075, F('G5'), 0.72, dict(tau=0.09, fc_peak=1900.0)),
                    (0.15, F('Bb5'), 0.85, dict(tau=0.12, fc_peak=2000.0))],
             0.16)
        m.add(0.15, soft_pad(extra_rng(), [F(x) for x in ('G4', 'Bb4',
                                                           'D5')],
                             0.4, 0.12, attack=0.03, release=0.25), 0.0,
              send=send(0.15))

    return {
        'message': message, 'mention': mention,
        'ring': (ring_e4 if S['ring'] == 'E4' else ring_e, 6.0),
        'ringback': (ringback, 4.0), 'call-waiting': (call_waiting, 4.0),
        'connected': connected, 'ended': ended, 'disconnected': disconnected,
        'join': join, 'leave': leave, 'mute': mute, 'unmute': unmute,
        'deafen': deafen, 'undeafen': undeafen, 'share-start': share_start,
        'share-stop': share_stop, 'hand-raised': hand_raised,
    }


# ── directions ─────────────────────────────────────────────────────────────

DIRECTIONS = {
    'E': dict(name='clean-ui', room=E_ROOM, sounds=e_family(E_STYLE)),
    'E1': dict(name='warmer', room=E_ROOM, seed_as='E',
               sounds=e_family(E1_STYLE)),
    'E2': dict(name='brighter', room=E_ROOM, seed_as='E',
               sounds=e_family(E2_STYLE)),
    'E3': dict(name='fuller', room=E3_ROOM, seed_as='E',
               sounds=e_family(E3_STYLE)),
    'E4': dict(name='new-key-and-ring', room=E_ROOM, seed_as='E',
               sounds=e_family(E4_STYLE)),
}


# ── loudness, mastering, output ────────────────────────────────────────────

def _kweight(x):
    b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]
    a1 = [1.0, -1.69065929318241, 0.73248077421585]
    b2 = [1.0, -2.0, 1.0]
    a2 = [1.0, -1.99004745483398, 0.99007225036621]
    return signal.lfilter(b2, a2, signal.lfilter(b1, a1, x, axis=-1), axis=-1)


def loudness(x):
    """(integrated LUFS, max momentary LUFS), ITU-R BS.1770-4, stereo."""
    z = _kweight(stereo(x)) ** 2
    blk, hop = secs(0.4), secs(0.1)
    if z.shape[1] < blk:
        z = np.pad(z, ((0, 0), (0, blk - z.shape[1])))
    c = np.cumsum(np.pad(z.sum(axis=0), (1, 0)))
    starts = np.arange(0, z.shape[1] - blk + 1, hop)
    pw = (c[starts + blk] - c[starts]) / blk
    lk = -0.691 + 10 * np.log10(np.maximum(pw, 1e-20))
    g = pw[lk > -70]
    if len(g) == 0:
        return -np.inf, float(lk.max())
    rel = -0.691 + 10 * np.log10(g.mean()) - 10
    g2 = pw[(lk > -70) & (lk > rel)]
    return float(-0.691 + 10 * np.log10(g2.mean())), float(lk.max())


def true_peak_db(x):
    up = signal.resample_poly(x, 4, 1, axis=-1)
    p = max(np.abs(up).max(), np.abs(x).max())
    return float(20 * np.log10(p)) if p > 0 else -np.inf


_HP = signal.butter(2, 45, 'highpass', fs=SR, output='sos')
_LP = signal.butter(2, 8000, 'lowpass', fs=SR, output='sos')


def seal(x, fade_in=0.003, fade_out=0.005):
    x = x.copy()
    n = x.shape[1]
    a, b = secs(fade_in), secs(fade_out)
    x[:, :a] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(a) / a)
    x[:, n - b:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(1, b + 1) / b)
    x[:, 0] = 0.0
    x[:, -1] = 0.0
    return x


def trim(x, floor_db=-60.0):
    """End a one-shot once its tail is `floor_db` below its peak, with a
    short fade over the last 15 ms."""
    env = np.abs(x).max(axis=0)
    floor = env.max() * 10 ** (floor_db / 20)
    last = int(np.nonzero(env >= floor)[0][-1])
    end = min(x.shape[1], last + secs(0.010))
    x = x[:, :end].copy()
    k = min(secs(0.015), end // 4)
    x[:, end - k:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(k) / k)
    return x


_IR_CACHE = {}


def _ir(direction):
    if direction not in _IR_CACHE:
        _IR_CACHE[direction] = make_ir(**DIRECTIONS[direction]['room'])
    return _IR_CACHE[direction]


def render(direction, name, channels=2):
    """Returns (float buffer, one row per channel, and info). channels=1
    folds the stereo mix to mono BEFORE levelling, so a mono set meets the
    same loudness targets (the files are embedded in the app binary, and
    mono halves their size)."""
    ir = _ir(direction)
    spec = DIRECTIONS[direction]['sounds'][name]
    # A variant uses its parent's seeds, so it differs only in its dials.
    base = DIRECTIONS[direction].get('seed_as', direction)
    seed = zlib.crc32(f'{base}/{name}'.encode())
    if isinstance(spec, tuple):
        fn, period = spec
        m = Mix(4 * period)
        for k in range(4):
            # Same seed for every period: the signal is exactly periodic.
            fn(m, np.random.default_rng(seed), k * period)
        x = m.render(ir)
        x = signal.sosfilt(_LP, signal.sosfilt(_HP, x, axis=-1), axis=-1)
        x = x[:, secs(2 * period):secs(3 * period)]
        loop = True
    else:
        m = Mix(4.0)
        spec(m, np.random.default_rng(seed))
        x = m.render(ir)
        x = signal.sosfilt(_LP, signal.sosfilt(_HP, x, axis=-1), axis=-1)
        loop = False
    if channels == 1:
        x = x.mean(axis=0, keepdims=True)
    _, mmax = loudness(x)
    gain = 10 ** ((TARGET[name] - mmax) / 20)
    tp = true_peak_db(x * gain)
    capped = tp > PEAK_CAP_DBTP
    if capped:
        gain *= 10 ** ((PEAK_CAP_DBTP - tp) / 20)
    x = x * gain
    if not loop:
        x = trim(x, TRIM_DB.get(name, -60.0))
        x = seal(x)
    else:
        x = seal(x, 0.003, 0.003)
    li, mm = loudness(x)
    return x, dict(seconds=x.shape[1] / SR, lufs_i=li, lufs_m=mm,
                   dbtp=true_peak_db(x), capped=capped)


def to_pcm16(x):
    q = np.clip(np.round(x * 32767.0), -32768, 32767).astype('<i2')
    return q.T.reshape(-1).tobytes()


def write_wav(path, x):
    with wave.open(path, 'wb') as w:
        w.setnchannels(x.shape[0])
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(to_pcm16(x))


def preview(rendered, gap=0.7):
    """All sounds in ORDER with `gap` seconds between; the ring twice, back
    to back, so its loop seam is heard."""
    parts = []
    for name in ORDER:
        x = rendered[name]
        parts.append(np.concatenate([x, x], axis=1) if name == 'ring' else x)
        parts.append(np.zeros((x.shape[0], secs(gap))))
    return np.concatenate(parts, axis=1)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description='Render Lightning call sounds.')
    ap.add_argument('--direction', default=DEFAULT_DIRECTION,
                    choices=sorted(DIRECTIONS))
    ap.add_argument('--out', default=os.path.join(os.path.dirname(here),
                                                  'data', 'sounds'))
    ap.add_argument('--channels', type=int, default=2, choices=(1, 2),
                    help='1 folds every sound to mono (half the size)')
    ap.add_argument('--check', action='store_true',
                    help='print levels without writing files')
    ap.add_argument('--preview', metavar='WAV',
                    help='also write every sound in order to one file')
    args = ap.parse_args()
    if not args.check:
        os.makedirs(args.out, exist_ok=True)
    rendered = {}
    print(f"direction {args.direction} ({DIRECTIONS[args.direction]['name']})")
    print(f"{'sound':<14}{'seconds':>8}{'LUFS-I':>8}{'M-max':>8}{'dBTP':>7}")
    for name in ORDER:
        x, info = render(args.direction, name, args.channels)
        rendered[name] = x
        print(f"{name:<14}{info['seconds']:>8.3f}{info['lufs_i']:>8.1f}"
              f"{info['lufs_m']:>8.1f}{info['dbtp']:>7.1f}"
              f"{'  (peak-capped)' if info['capped'] else ''}")
        if not args.check:
            write_wav(os.path.join(args.out, name + '.wav'), x)
    if args.preview:
        write_wav(args.preview, preview(rendered))
    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rokas Smetonis
#
# Renders Lightning's call sound effects into data/sounds/*.wav.
#
# EVERY SOUND HERE IS SYNTHESISED FROM ARITHMETIC. Nothing is sampled, traced
# or derived from another product's audio: each cue is a handful of decaying
# sine partials placed on a score written out below, so the score and this
# file are the whole of the "source" and anyone can regenerate the set
# (bit-for-bit on the same platform; libm may differ by a rounding step
# elsewhere). Standard library only (no numpy, no sox), so it runs anywhere
# a python3 does:
#
#     python3 scripts/generate-call-sounds.py            # writes data/sounds
#     python3 scripts/generate-call-sounds.py --check    # report levels only
#
# The rendered WAVs are licensed like the rest of Lightning,
# GPL-3.0-or-later; this script is their preferred form for modification.
#
# DESIGN, so a later edit keeps the family coherent:
#
#   * ONE KEY. Every pitch is in D major pentatonic (D E F# A B). Any two cues
#     that overlap — a join landing on a ring tail — are consonant.
#   * THREE TIMBRES, one per meaning. "glass" (a soft struck-glass tone: sine
#     fundamental, a weak octave and a faint inharmonic shimmer) is presence —
#     people arriving and leaving, the ringer. "wood" (heavily damped, almost
#     no overtones) is YOUR OWN controls — mute and deafen — so a control
#     never sounds like somebody arriving. "bell" (inharmonic partials, long
#     tail) is attention — a raised hand.
#   * DIRECTION CARRIES THE MEANING, and every pair is the same material
#     mirrored: up = on / arriving / live, down = off / leaving / silenced.
#     Pairs are separated by more than direction too: join/leave step by a
#     third on glass, mute/unmute by a fourth on wood and three times faster,
#     deafen/undeafen are three wood notes an octave lower, share start/stop
#     GLIDE instead of stepping.
#   * SHORT AND SOFT. Effects sit around -24 dBFS RMS with peaks capped at
#     -9 dBFS. Every one-shot is over within 0.25 s of its first note; the
#     files run on only until the decaying tail is 60 dB down. Attack is a
#     4-6 ms raised cosine, so no cue clicks, and every file starts and ends
#     on an exact digital zero.
#   * THE RINGER LOOPS SEAMLESSLY. It is one 3.2 s bar that ends in silence,
#     so QSoundEffect's infinite loop repeats it with no join to hear.
#
# Levels are plain RMS over the audible span (from the first to the last
# sample above -40 dB of the peak), not K-weighted LUFS; every partial here
# sits between 290 Hz and 5 kHz, where K-weighting is within about 1 dB of
# flat, so the two agree closely enough for cue-to-cue balance.

import argparse
import math
import os
import struct
import sys
import wave

SR = 48000

# D major pentatonic, equal temperament, A4 = 440 Hz.
def hz(semitones_from_a4):
    return 440.0 * (2.0 ** (semitones_from_a4 / 12.0))

D4, E4, FS4, A4, B4 = hz(-7), hz(-5), hz(-3), hz(0), hz(2)
D5, E5, FS5, A5, B5 = hz(5), hz(7), hz(9), hz(12), hz(14)
D6, E6, FS6, A6 = hz(17), hz(19), hz(21), hz(24)

# (frequency ratio, relative amplitude, decay speed relative to the
# fundamental). Higher partials die faster, which is what makes a tone read as
# struck rather than as a beep.
GLASS = [(1.0, 1.00, 1.0), (2.0, 0.16, 1.9), (3.0, 0.05, 2.8),
         (4.23, 0.025, 4.5)]
WOOD = [(1.0, 1.00, 1.0), (2.0, 0.08, 2.4), (3.93, 0.03, 5.0)]
BELL = [(1.0, 1.00, 1.0), (2.76, 0.30, 1.7), (5.40, 0.10, 2.8),
        (8.93, 0.03, 4.2)]


def smoothstep(x):
    x = 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)
    return x * x * (3.0 - 2.0 * x)


class Score:
    """A mono float buffer that notes are added into."""

    def __init__(self, seconds):
        self.buf = [0.0] * int(round(seconds * SR))

    def note(self, at, freq, *, timbre=GLASS, amp=1.0, tau=0.30, length=None,
             attack=0.005, glide_to=None, glide_time=0.0, beat_hz=0.0,
             sustain=False):
        """Strike one note.

        tau        decay time constant of the fundamental, seconds.
        length     how long to render; defaults to 5.5 tau, where the
                   fundamental has fallen by about 48 dB — the release fade
                   below takes it the rest of the way inside the span.
        glide_to   end frequency of a pitch glide over `glide_time`.
        beat_hz    adds a second, detuned copy of every partial, so the note
                   wavers — used only for the "connection lost" cue.
        sustain    organ-like: flat after the attack instead of decaying, for
                   the ringback, which must read as a TONE and not a strike.
        """
        if length is None:
            length = 5.5 * tau
        start = int(round(at * SR))
        n = int(round(length * SR))
        release = min(int(0.020 * SR), n // 4)
        phases = [0.0] * len(timbre)
        beat_phases = [0.0] * len(timbre)
        two_pi = 2.0 * math.pi
        for i in range(n):
            idx = start + i
            if idx >= len(self.buf):
                break
            t = i / SR
            if glide_to is not None and glide_time > 0.0:
                f = freq * (glide_to / freq) ** smoothstep(t / glide_time)
            else:
                f = freq
            # Attack: raised cosine, so the waveform starts from zero slope.
            if t < attack:
                env = 0.5 - 0.5 * math.cos(math.pi * t / attack)
            else:
                env = 1.0
            # Release: the last 20 ms of the rendered span always fades to
            # zero, so a note cut short never clicks.
            if i >= n - release:
                env *= 0.5 + 0.5 * math.cos(math.pi * (i - (n - release))
                                            / release)
            sample = 0.0
            for p, (ratio, rel, speed) in enumerate(timbre):
                if sustain:
                    decay = 1.0 if speed == 1.0 else math.exp(-t * speed * 2.0)
                else:
                    decay = math.exp(-t * speed / tau)
                phases[p] += two_pi * f * ratio / SR
                sample += rel * decay * math.sin(phases[p])
                if beat_hz:
                    beat_phases[p] += two_pi * (f * ratio + beat_hz) / SR
                    sample += rel * decay * math.sin(beat_phases[p])
            if beat_hz:
                sample *= 0.5
            self.buf[idx] += amp * env * sample


def audible_span(buf, floor_db=-40.0):
    peak = max((abs(s) for s in buf), default=0.0)
    if peak <= 0.0:
        return 0, 0, 0.0
    floor = peak * (10.0 ** (floor_db / 20.0))
    first = next(i for i, s in enumerate(buf) if abs(s) >= floor)
    last = len(buf) - 1 - next(i for i, s in enumerate(reversed(buf))
                               if abs(s) >= floor)
    return first, last + 1, peak


def rms_db(buf):
    first, last, _ = audible_span(buf)
    if last <= first:
        return -math.inf
    acc = sum(s * s for s in buf[first:last]) / (last - first)
    return 10.0 * math.log10(acc) if acc > 0 else -math.inf


def peak_db(buf):
    p = max((abs(s) for s in buf), default=0.0)
    return 20.0 * math.log10(p) if p > 0 else -math.inf


def normalise(buf, rms_target_db, peak_cap_db):
    """Scale to the RMS target, then pull down if the peak would pass the
    cap. The cap wins: a cue may come out quieter than its target, never
    louder than its ceiling."""
    gain = 10.0 ** ((rms_target_db - rms_db(buf)) / 20.0)
    p = max(abs(s) for s in buf) * gain
    cap = 10.0 ** (peak_cap_db / 20.0)
    if p > cap:
        gain *= cap / p
    return [s * gain for s in buf]


def trim_tail(buf, keep_seconds=None):
    """Drop trailing silence (below -60 dB of peak) unless a fixed length is
    required — a loop must keep its full bar."""
    if keep_seconds is not None:
        return buf[:int(round(keep_seconds * SR))]
    peak = max(abs(s) for s in buf)
    floor = peak * (10.0 ** (-60.0 / 20.0))
    end = len(buf)
    while end > 0 and abs(buf[end - 1]) < floor:
        end -= 1
    end = min(len(buf), end + int(0.010 * SR))
    return buf[:end]


def seal(buf):
    """Exact digital zero at both ends plus a 3 ms guard fade, so a file can
    be started, stopped or looped at any boundary without a click."""
    g = int(0.003 * SR)
    out = list(buf)
    for i in range(min(g, len(out))):
        w = i / g
        out[i] *= w
        out[-1 - i] *= w
    out[0] = 0.0
    out[-1] = 0.0
    return out


def to_pcm16(buf):
    frames = bytearray()
    for s in buf:
        v = int(round(s * 32767.0))
        v = 32767 if v > 32767 else (-32768 if v < -32768 else v)
        frames += struct.pack('<h', v)
    return bytes(frames)


# ── The score ──────────────────────────────────────────────────────────────
#
# Each entry: name -> (render function, RMS target dBFS, peak cap dBFS,
# fixed length or None, one-line rationale). Levels: effects -24/-9, the
# ringer -17/-3 (it must be heard across a room), call-waiting and ringback
# deliberately quiet, since they play while you are already listening.

def join():
    # Somebody arrived: a rising minor third on glass.
    s = Score(2.5)
    s.note(0.000, FS5, tau=0.16)
    s.note(0.085, A5, tau=0.22)
    return s.buf


def leave():
    # Somebody left: the same two notes, falling.
    s = Score(2.5)
    s.note(0.000, A5, tau=0.16)
    s.note(0.085, FS5, tau=0.22)
    return s.buf


def connected():
    # You are in: the full rising triad, one step fuller than a join.
    s = Score(2.5)
    s.note(0.000, D5, tau=0.14)
    s.note(0.075, FS5, tau=0.14)
    s.note(0.150, A5, tau=0.28)
    return s.buf


def ended():
    # You are out: the triad falling, landing on the tonic.
    s = Score(2.5)
    s.note(0.000, A5, tau=0.14)
    s.note(0.075, FS5, tau=0.14)
    s.note(0.150, D5, tau=0.30)
    return s.buf


def disconnected():
    # Connection lost: a falling fifth whose second note WAVERS (two detuned
    # copies beating at 6 Hz) — the one cue in the set that sounds unsteady,
    # because it is the one that reports trouble.
    s = Score(2.5)
    s.note(0.000, D5, tau=0.12)
    s.note(0.110, A4, tau=0.32, beat_hz=6.0)
    return s.buf


def mute():
    # Your microphone went off: a quick falling fourth, damped wood.
    s = Score(2.5)
    s.note(0.000, B5, timbre=WOOD, tau=0.045, attack=0.004)
    s.note(0.045, FS5, timbre=WOOD, tau=0.060, attack=0.004)
    return s.buf


def unmute():
    # Your microphone is live: the same fourth, rising.
    s = Score(2.5)
    s.note(0.000, FS5, timbre=WOOD, tau=0.045, attack=0.004)
    s.note(0.045, B5, timbre=WOOD, tau=0.060, attack=0.004)
    return s.buf


def deafen():
    # You hear nothing now: three wood notes an octave below mute, falling —
    # heavier than mute because it silences more.
    s = Score(2.5)
    s.note(0.000, B4, timbre=WOOD, tau=0.050, attack=0.005)
    s.note(0.050, FS4, timbre=WOOD, tau=0.050, attack=0.005)
    s.note(0.100, D4, timbre=WOOD, tau=0.080, attack=0.005)
    return s.buf


def undeafen():
    s = Score(2.5)
    s.note(0.000, D4, timbre=WOOD, tau=0.050, attack=0.005)
    s.note(0.050, FS4, timbre=WOOD, tau=0.050, attack=0.005)
    s.note(0.100, B4, timbre=WOOD, tau=0.080, attack=0.005)
    return s.buf


def share_start():
    # A screen share went live: one note GLIDING up a fourth, with a faint
    # octave sparkle — a glide, so it is never mistaken for a join.
    s = Score(2.5)
    s.note(0.000, A5, tau=0.24, glide_to=D6, glide_time=0.090, attack=0.006)
    s.note(0.080, D6 * 2.0, tau=0.10, amp=0.10)
    return s.buf


def share_stop():
    # The share ended: the same glide, falling, without the sparkle.
    s = Score(2.5)
    s.note(0.000, D6, tau=0.22, glide_to=A5, glide_time=0.090, attack=0.006)
    return s.buf


def hand_raised():
    # Someone wants the floor: one soft bell strike with a long tail.
    s = Score(2.5)
    s.note(0.000, B5, timbre=BELL, tau=0.26, attack=0.004)
    return s.buf


def ring():
    # The ringer, one 3.2 s bar ending in silence so it loops cleanly: a
    # rising arpeggio, then an answering pair from the top. Musical rather
    # than an alarm — it is meant to be pleasant on the twentieth repeat —
    # but the arpeggio's steady rhythm still says "call" from across a room.
    s = Score(3.2)
    s.note(0.00, D5, tau=0.20)
    s.note(0.14, FS5, tau=0.20)
    s.note(0.28, A5, tau=0.20)
    s.note(0.42, D6, tau=0.26)
    s.note(0.98, A5, tau=0.16)
    s.note(1.12, D6, tau=0.24)
    return s.buf


def call_waiting():
    # A second call while you are already in one: two quiet taps every four
    # seconds, so it is noticed without talking over the call you are in.
    s = Score(4.0)
    s.note(0.00, A5, tau=0.07)
    s.note(0.20, A5, tau=0.07)
    return s.buf


def ringback():
    # You are calling someone: a soft sustained dyad for one second in every
    # four — the cadence people already read as "it is ringing at the other
    # end", in the key of the rest of the set.
    s = Score(4.0)
    tone = [(1.0, 1.0, 1.0), (2.0, 0.10, 3.0)]
    s.note(0.00, FS4, timbre=tone, sustain=True, length=1.0, attack=0.030,
           amp=0.6)
    s.note(0.00, A4, timbre=tone, sustain=True, length=1.0, attack=0.030,
           amp=0.6)
    return s.buf


SOUNDS = {
    'join': (join, -24.0, -9.0, None),
    'leave': (leave, -24.0, -9.0, None),
    'connected': (connected, -23.0, -9.0, None),
    'ended': (ended, -23.0, -9.0, None),
    'disconnected': (disconnected, -23.0, -9.0, None),
    'mute': (mute, -24.0, -9.0, None),
    'unmute': (unmute, -24.0, -9.0, None),
    'deafen': (deafen, -24.0, -9.0, None),
    'undeafen': (undeafen, -24.0, -9.0, None),
    'share-start': (share_start, -24.0, -9.0, None),
    'share-stop': (share_stop, -24.0, -9.0, None),
    'hand-raised': (hand_raised, -24.0, -9.0, None),
    'ring': (ring, -17.0, -3.0, 3.2),
    'call-waiting': (call_waiting, -27.0, -12.0, 4.0),
    'ringback': (ringback, -26.0, -12.0, 4.0),
}


def render(name):
    fn, target, cap, fixed = SOUNDS[name]
    buf = fn()
    buf = normalise(buf, target, cap)
    buf = trim_tail(buf, fixed)
    return seal(buf)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.join(os.path.dirname(here), 'data', 'sounds')
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', default=default_out)
    ap.add_argument('--check', action='store_true',
                    help='print levels without writing files')
    args = ap.parse_args()
    if not args.check:
        os.makedirs(args.out, exist_ok=True)
    print(f"{'sound':<14}{'seconds':>8}{'rms dBFS':>10}{'peak dBFS':>11}")
    for name in SOUNDS:
        buf = render(name)
        print(f"{name:<14}{len(buf) / SR:>8.3f}{rms_db(buf):>10.1f}"
              f"{peak_db(buf):>11.1f}")
        if args.check:
            continue
        path = os.path.join(args.out, name + '.wav')
        with wave.open(path, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(to_pcm16(buf))
    return 0


if __name__ == '__main__':
    sys.exit(main())

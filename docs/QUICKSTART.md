<!--
SPDX-FileCopyrightText: 2026 MontroneDSP
SPDX-License-Identifier: GPL-3.0-or-later
-->

# SWARA XT Quick Start

SWARA XT is a monophonic synthesizer: two digital oscillators, a resonant
low-pass filter, envelopes and LFOs, a 12-slot modulation matrix, a
sequencer/arpeggiator, and onboard effects.

This booklet is for making sound, not for building the plugin. Load it, play a
note, and change one control at a time.

## 1. Getting sound

1. Open **SWARA XT** in a VST3 host, or launch the Standalone app.
2. Choose a factory preset from the selector at the top (the arrows step through
   the list). **Saw Bass** is a reliable first stop.
3. Send MIDI notes to the track, and make sure that track is routed to an
   audible output.
4. In **OSC 1**, try another **MODEL**. Raise or lower **PITCH**.
5. In **MIXER**, set **MIX** (oscillator balance), **SUB**, and **NOISE**.
6. In **FILTER**, open **CUTOFF** and add a little **RES**.
7. Use filter **ENV** and **LFO** for movement, then **ENV 2** for the loudness
   envelope.

If nothing is heard, skip to [Troubleshooting](#troubleshooting).

## 2. Signal flow at a glance

```text
OSC 1 + OSC 2 + SUB + NOISE
        → MIXER (balance, operator, sub shape)
        → FILTER
        → VCA (ENV 2)
        → FX (optional)
        → MASTER
```

Play a note, then follow the path with your ear: source, mix, filter, loudness,
effect.

## 3. Oscillators and mixer

**OSC 1** and **OSC 2** each have:

- **MODEL** — the oscillator type (analog-style waves, CZ models, wavetables,
  noise/vowel colours, and others)
- **PITCH** — coarse tuning in semitones
- **TIMBRE** — the model’s main colour control
- **DETUNE** — OSC 2 only; fine offset against OSC 1

You do not need every model on day one. **Analog Saw**, **Analog Square**, and
**Linear FM** already cover a lot of ground.

**MIXER** sits in the centre of the panel:

- **MIX** — balance between OSC 1 and OSC 2
- **SUB** — extra low layer
- **NOISE** — noise level
- **OPERATOR** — how the two oscillators are combined (for example **Sum**,
  **Sync**, **Ring**, **Fold**)
- **SHAPE** — the sub/transient flavour (**Square -1**, **Click**, **Metallic**,
  and so on)

The lower half of the same module is the **MOD MATRIX**. Press **SEQ/ARP** to
swap this centre module into the sequencer. Press **SYNTH** to return.

**GLOBAL** (right column) holds **MASTER** level, **GLIDE**, and **VOICE**
(**Retrigger** or **Legato**). Press **FX** there to open the effects page.

## 4. Filter

The filter is a resonant four-pole low-pass.

- **CUTOFF** — brightness
- **RES** — resonance; high settings can self-oscillate
- **ENV** — how much **ENV 1** opens (or closes) cutoff
- **LFO** — how much **LFO 2** moves cutoff
- **KEY TRACK** — how cutoff follows the played note

Those two depth knobs are dedicated:

- filter **ENV** = **ENV 1 → cutoff**
- filter **LFO** = **LFO 2 → cutoff**

The **MOD MATRIX** is separate. You can still route other sources to
**Filter Cutoff** or **Filter Res** without replacing those two knobs.

## 5. Envelopes, LFOs and modulation matrix

**ENV 1** is labelled **VCF**. It is the envelope that the filter **ENV** knob
listens to. **ENV 2** is labelled **VCA** and shapes loudness. Both use **A D S
R**.

**LFO 1** and **LFO 2** each have **WAVE**, **RETRIGGER**, **RATE**, **FADE**,
plus **TIMING** (**Free** or **Sync**) and **DIVISION** when synced to the host.

Useful **RETRIGGER** modes:

- **Free phase** — keeps running
- **Retrigger** — restarts with each note
- **One-shot** — plays once per note

The **MOD MATRIX** has 12 slots (three visible at a time; use the arrows). Each
slot is source → depth → destination.

Short examples that exist in the matrix:

- **LFO 1 → VCO 1** — vibrato
- **Env 1 → Timbre 1** — the tone blooms with the note
- **LFO 2 → Filter Cutoff** — extra sweep on top of the dedicated filter **LFO**
  knob
- **Velocity → VCA** or **Filter Cutoff** — touch-sensitive lines

Sources include both LFOs, both envelopes, **Note**, **Gate**, **Mod Wheel**,
**Seq** / **Seq 1** / **Seq 2** / **Step** (pattern values), and **Random**.
Destinations include cutoff, resonance, VCA, oscillator pitch and timbre, mix
levels, LFO rates, and envelope times.

## 6. Sequencer and arpeggiator

Press **SEQ/ARP**. This is a full pattern workshop, not only a running
16-step box.

**MODE**

- **Step** — one pattern step per played note
- **Arp** — arpeggiates held notes
- **Sequence** — the pattern runs from the clock

**CLOCK** is **Free** (uses **TEMPO**) or **Host sync** (follows the DAW;
**TEMPO** is dimmed). **SWING** and **GATE** apply to the running pattern.
**LENGTH** is the shared cycle length (1–16). **START** rotates where the cycle
begins. **GROOVE** adds a feel: **Swing**, **Shuffle**, **Push**, **Lag**,
**Human**, **Monkey**.

When **MODE** is **Arp**, use:

- **DIRECTION** — **Up**, **Down**, **Up/Down**, **Random**, **Played**
- **PATTERN** — **Pattern 1**–**15**, or **Sequence** (the arp then uses the
  step grid)
- **OCTAVES** — **Octave 1**–**4**
- **DIVISION** — arp rate (**1/1** down to **1/64**, including triplets)

The 16 step buttons select a step. Edit that step with:

- **NOTE**
- **EVENT** — **Rest**, **Note**, or **Tie**
- **VELOCITY** (0–7)
- **VALUE** (0–15) — the per-step controller that can feed **Seq** sources in
  the matrix, in every mode

### Generating sequences

The five buttons rewrite **dimensions** of the pattern. They do not reset
**MODE**, **CLOCK**, **TEMPO**, **LENGTH**, or similar playback settings.

- **RANDOM NOTES** — new pitches only. Events, velocities, and values stay.
- **RANDOM VELOCITY** — new velocities only.
- **RANDOM EVENT** — new **Note** / **Rest** / **Tie** structure. Ties are only
  placed after a sounding step, so the pattern stays playable. Pitches,
  velocities, and values stay.
- **RANDOM VALUE** — new per-step **VALUE** (0–15) only.
- **RANDOM SEQ** — new notes, velocities, events, and values together.

Use them like layers: generate a skeleton with **RANDOM SEQ**, then rebuild only
the rhythm with **RANDOM EVENT**, or only the melody with **RANDOM NOTES**.

**Type 1** is the current generator. Generated pitches stay on a pentatonic set
inside the range you set below.

### Random note range

The bipolar **RANGE** slider sets how far generated notes may move **relative to
step 1**.

The lower handle is **0 to −48** semitones. The upper handle is **0 to +48**.
The label reads like **RANGE -12 / +12**.

Examples:

- **−12 … +12** — about one octave around step 1
- **−24 … 0** — only below step 1
- **0 … +24** — only above step 1
- **−48 … +48** — the widest span the generator allows

Only pitches that also sit in the Type 1 pentatonic set inside that window are
chosen.

### LOCK — keep the pattern while changing sounds

**LOCK** preserves the sequencer/arpeggiator **workflow**, not the synth voice.

With **LOCK** on, loading another factory or user preset changes oscillators,
filter, envelopes, matrix, and effects — while the pattern, step data, and
Seq/Arp settings (**MODE**, **CLOCK**, **TEMPO**, **SWING**, **GATE**, arp
**DIRECTION** / **PATTERN** / **OCTAVES** / **DIVISION**) stay where you left
them.

Typical use:

1. Build or generate a pattern.
2. Press **LOCK**.
3. Browse presets.
4. Hear the same riff through different timbres.

Turn **LOCK** off to restore normal preset behaviour: a preset may load its own
sequence state again.

**LOCK** does not freeze cutoff, mix, or FX. Those still follow the preset (and
your hands).

### Try this: generate a sequence in 60 seconds

1. Load **Saw Bass** or **1996 Sub Bass**.
2. Open **SEQ/ARP**.
3. Set **MODE** to **Sequence**.
4. Set **LENGTH** to 16 (or 8 for a tighter loop).
5. Set **CLOCK** to **Host sync** if you are in a DAW.
6. Set **RANGE** to about **−12 / +12**.
7. Press **RANDOM SEQ**.
8. If the rhythm is busy, press **RANDOM EVENT**. If the pitches clash, press
   **RANDOM NOTES**.
9. Add a little **SWING**, or try **GROOVE: Shuffle**.
10. Press **LOCK**, then step through presets with the arrows.
11. Shape **CUTOFF**, filter **ENV**, and filter **LFO**.
12. Open **FX** and try **Delay** or **Crush**.
13. Press **SAVE** if you want to keep the combination.

## 7. Presets

The selector lists factory programs, then any user presets you have saved.

Factory material includes:

- SWARA-designed starting points (including **Saw Bass** and related inits)
- a bank derived from the official Shruthi factory patches
- additional SWARA factory sounds such as **1996 Sub Bass** and **Ibiza Bass**

**SAVE** writes a **user preset**: the complete instrument, including the
current sequence. You will be asked for a name. User files live beside the
plugin as `.swaraxtpreset` documents.

With **LOCK** on, browsing factory or user presets keeps the active pattern.
With **LOCK** off, the incoming preset’s sequence is loaded.

Host sessions remember **LOCK**. User preset files do not turn **LOCK** on by
themselves.

## DSP effects

In **GLOBAL**, press **FX**.

**FX** is **Off** plus 16 programs:

**Distortion**, **Crush**, **Comb +**, **Comb −**, **Ring Mod**, **Delay**,
**Delay FB**, **Delay Dub**, **Crush Delay FB**, **Crush Delay Dub**,
**Delay 1/16**, **Delay 1/12**, **Delay 1/8**, **Delay 3/16**, **Looper**,
**Pitch**.

Effects sit after the filter. **POST MIXER** is the level into that stage. The
two contextual knobs change with the program (for example **FOLD** / **FUZZ** on
Distortion, **TIME** / **FEEDBACK** on delays, **PITCH** / **MIX** on Pitch).

**Looper** shows a **Replay** switch: unchecked records, checked replays. The
loop is temporary and is cleared when you load a preset or host state.

## Interface

Right-click the panel (or use the host’s plugin menu where it forwards to the
editor) for appearance and MIDI.

These choices do not change the sound (except **Filter Quality**, which is a
CPU/quality trade-off, and **MIDI Channel**):

- **Skin** — **Pastel**, **Midnight Gold**, **Neon Cobalt**, **Jungle**,
  **Rossocorsa**
- **GUI Size** — **Small**, **Medium**, **Large**
- **Decoration** — **Legacy**, **PCB Trace**, **Square**, **Eurocrack**,
  **Plait**, **Intertwined**, **Guilloche**, **Interlace**, **Wicker**,
  **Meander**, **Key Border**, **Banner**, **Acanthus**, **Lion**
- **Manufacturer Mark** — **MontroneDSP Wordmark** or **Stones**

## A few useful starting points

- **Bass** — one saw oscillator, some **SUB**, moderate **CUTOFF**, filter
  **ENV** up, short **ENV 2** decay.
- **Animated line** — **RES** up, **Sequence** mode, filter **LFO** or matrix
  **LFO 2 → Filter Cutoff**.
- **Evolving riff** — **RANDOM SEQ**, then **RANDOM EVENT** or **RANDOM NOTES**,
  **LOCK**, browse sounds.
- **More motion** — do not stop at the dedicated filter knobs; add a matrix
  slot.
- **Keep it** — **SAVE**.

## Troubleshooting

**No sound**

- The host track must receive MIDI and feed an output.
- Right-click → **MIDI Channel**: **Omni** hears all channels.
- Try **Saw Bass** and play a mid-range note.
- If **MODE** is **Sequence** or **Arp**, hold notes (or start the host
  transport when **CLOCK** is **Host sync**).

**Sequence ignores the DAW**

- Set **CLOCK** to **Host sync**, and start the host transport.

**A new preset wipes the pattern**

- Enable **LOCK** before changing presets.

**CPU is high**

- Use a larger audio buffer and a moderate sample rate. Right-click →
  **Filter Quality** → **Normal** or **Eco** if the host is struggling.

---

SWARA XT 1.2.3 — MontroneDSP

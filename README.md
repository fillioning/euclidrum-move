# Euclidrum -- 8-Lane Generative Euclidean Drum Sequencer

A MIDI FX module for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. Purpose-built companion for [Weird Dreams](https://github.com/fillioning/weird-dreams-move), but works with any sound generator that responds to MIDI notes 36--43.

## What It Does

Euclidrum generates autonomous drum patterns using the Euclidean algorithm -- the same mathematical principle behind many traditional rhythms worldwide. It distributes a number of pulses as evenly as possible across a number of steps, producing patterns like son clave (3 pulses in 8 steps) or a standard 4/4 kick (4 pulses in 16 steps).

Each of the 8 lanes controls one drum voice. The patterns run on their own -- press Play and the drums start. Layer manual hits on top using the pads (pass-through is on by default).

## Features

- **8 autonomous lanes** hardwired to MIDI notes 36--43 (Kick, Snare, HH Closed, HH Open, Tom Lo, Tom Hi, Perc, FX)
- **Transport-triggered** -- fires on Play, no held notes required
- **32 built-in presets** covering house, hip-hop, trap, techno, DnB, afrobeat, bossa, glitch, and more
- **Random preset generator** -- turn a knob for instant inspiration
- **Per-lane rate multiplier** for polymetric patterns (x1, x2, x3, x4, /2, /3, /4)
- **Accent, fill, and drop probabilities** per lane
- **Pattern mutation/drift** -- pulses and rotation evolve automatically over time
- **Per-lane frequency and decay CC output** with randomization (CC 70--87)
- **Velocity humanization** -- global random velocity deviation
- **Internal or MIDI clock sync** with swing
- **Live MIDI pass-through** for manual layering

## How to Use

### Quick Start

1. In Schwung's Signal Chain, insert **Euclidrum** as MIDI FX
2. Add **Weird Dreams** (or any drum synth) as the sound generator
3. Press **Play** on the Move -- the sequencer starts immediately
4. Turn the **Preset** knob on the Global page to browse 32 rhythm patterns
5. Turn **Rnd Preset** for a random pattern each click

### The Root Page

The root page gives you 8 knobs -- one per lane. Each knob toggles a lane on or off:

| Knob | Lane | MIDI Note |
|------|------|-----------|
| 1 | Kick | 36 (C2) |
| 2 | Snare | 37 (C#2) |
| 3 | HH Closed | 38 (D2) |
| 4 | HH Open | 39 (D#2) |
| 5 | Tom Lo | 40 (E2) |
| 6 | Tom Hi | 41 (F2) |
| 7 | Perc | 42 (F#2) |
| 8 | FX | 43 (G2) |

### The Global Page

Navigate into the Global sub-page for master controls:

| Knob | Parameter | Description |
|------|-----------|-------------|
| 1 | **Preset** | Browse 32 built-in rhythm patterns by name |
| 2 | **Rnd Preset** | Turn for a new random pattern each click |
| 3 | **Rate** | Master clock division (1/32 to 1/1) |
| 4 | **Swing** | Shuffle amount (0--100%) |
| 5 | **Gate** | Note duration as % of step (1--1600%) |
| 6 | **Mutation** | Pattern drift probability per cycle (0--100%) |
| 7 | **Rnd Cyc** | Steps between mutation checks (1--128) |
| 8 | **Vel Rnd** | Velocity humanization amount (0--64) |

**Menu-only parameters** (scroll past the knobs):

| Parameter | Description |
|-----------|-------------|
| **Velocity** | Base MIDI velocity for all notes (1--127, default 100) |
| **Sync** | `clock` = follow Move transport, `internal` = free-running |
| **BPM** | Tempo when Sync is internal (10--500, default 120) |
| **Voices** | Max simultaneous note-ons (1--64, default 16) |
| **Rnd Seed** | Master seed for drop/fill/accent randomness (0--65535) |
| **Mut Seed** | Seed for mutation decisions (0--65535) |
| **Passthru** | Forward incoming pad MIDI alongside generated patterns |

### Lane Pages (L1--L8)

Each lane has its own page with 8 knobs:

| Knob | Parameter | Description |
|------|-----------|-------------|
| 1 | **Enabled** | Lane on/off |
| 2 | **Steps** | Pattern length (1--64) |
| 3 | **Pulses** | Number of hits distributed in the pattern (0--steps) |
| 4 | **Rotation** | Shift the pattern start point (0--63) |
| 5 | **Rate** | Per-lane speed multiplier (x1, x2, x3, x4, /2, /3, /4) |
| 6 | **Accent** | Probability of velocity boost on each hit (0--100%) |
| 7 | **Freq CC** | Base frequency CC value sent to downstream synth (0--127, 64 = no change) |
| 8 | **Decay CC** | Base decay CC value sent to downstream synth (0--127, 64 = no change) |

**Menu-only lane parameters:**

| Parameter | Description |
|-----------|-------------|
| **Drop** | Probability of skipping a hit (0--100%) |
| **Velocity** | Per-lane velocity override (0 = use global) |
| **Accent Amt** | How much accent boosts velocity (0--64) |
| **Fill** | Probability of adding extra hits between Euclidean pulses (0--100%) |
| **Gate** | Per-lane gate override (0 = use global) |
| **Freq Rnd** | Random deviation on freq CC per hit (0--64) |
| **Decay Rnd** | Random deviation on decay CC per hit (0--64) |

## CC Mapping

Euclidrum sends per-voice CC messages before each note-on, allowing the downstream synth to change pitch and decay per hit:

| CC Range | Target | Description |
|----------|--------|-------------|
| CC 70--77 | Voice 1--8 frequency | Per-hit pitch control |
| CC 80--87 | Voice 1--8 decay | Per-hit decay control |

CC value 64 = center / no change. Values above or below 64 shift from the voice's default. Works with Weird Dreams' CC patch out of the box.

## Understanding the Euclidean Algorithm

The core idea: distribute `pulses` as evenly as possible across `steps`. Examples:

- **4 pulses in 16 steps** = `X . . . X . . . X . . . X . . .` (standard 4/4 kick)
- **3 pulses in 8 steps** = `X . . X . . X .` (Cuban tresillo)
- **5 pulses in 16 steps** = `X . . X . . X . . X . . X . . .` (bossa nova kick)
- **7 pulses in 12 steps** = `X . X X . X X . X X . X` (West African bell)

**Rotation** shifts the pattern's start point, creating variations of the same rhythm. **Rate** makes a lane run faster or slower than the master clock, enabling polymetric patterns (e.g., a 7-step lane at x1 against a 16-step lane at x2).

## Pattern Evolution

Set **Mutation** above 0% and patterns will slowly evolve. At each **Rnd Cyc** boundary, each lane has a Mutation% chance of shifting its pulses or rotation by +/-1. This creates organic, gradually changing rhythms without manual intervention.

The mutations are deterministic (seeded by **Mut Seed**), so the same seed always produces the same evolution path.

## Presets

32 built-in presets covering a wide range of styles:

| # | Name | Style |
|---|------|-------|
| 0 | Init | Blank slate (4 lanes active) |
| 1 | 4 Floor | Classic house/techno four-on-the-floor |
| 2 | BoomBap | Hip-hop with heavy kick and shuffle |
| 3 | Trap808 | 808-style with hi-hat rolls |
| 4 | MinTech | Minimal techno with polymetric tom |
| 5 | Break | Syncopated breakbeat |
| 6 | DubTech | Spacious dub techno with long decays |
| 7 | Afro | Afrobeat with 12/8 feel and bells |
| 8 | Indstrl | Industrial, all 8 lanes, harsh |
| 9 | Reggton | Reggaeton dembow |
| 10 | Poly | Polyrhythm (7, 5, 9, 11, 13, 3-step lanes) |
| 11 | Sparse | Generative ambient, high drop rates |
| 12 | DnB | Drum & bass with double-time hats |
| 13 | Samba | Brazilian surdo/caixa/ganza/agogo |
| 14 | Electro | 808 electro with cowbell |
| 15 | Waltz | 3/4 time signature |
| 16 | Gabber | Relentless kicks and hats |
| 17 | Bossa | Bossa nova with brushes |
| 18 | UKGarage | Skippy two-step garage |
| 19 | Acid | 909-style acid house |
| 20 | HalfTm | Half-time beat |
| 21 | Funk | Syncopated funk with ghost notes |
| 22 | Shuffle | Triplet shuffle |
| 23 | Glitch | Chaotic fills and odd lengths |
| 24 | Dub | Deep dub with rimshot delay |
| 25 | Lo-Fi | Dusty, relaxed beats |
| 26 | Machine | Relentless 8th-note grid, all lanes |
| 27 | Ritual | Polymetric world percussion |
| 28 | Pulse | Minimal 4/4 pulse |
| 29 | Chaos | Maximum generative, high fills/drops |
| 30 | Tresllo | 3+3+2 tresillo pattern |
| 31 | Zen | Meditative, very sparse |

## Prerequisites

- [Schwung](https://github.com/charlesvestal/schwung) installed on your Ableton Move
- SSH access enabled: `http://move.local/development/ssh`
- A drum synth (recommended: [Weird Dreams](https://github.com/fillioning/weird-dreams-move))

## Installation

### From Release

Download the latest `.tar.gz` from [Releases](https://github.com/fillioning/euclidrum-move/releases), then:

```bash
scp -r euclidrum root@move.local:/data/UserData/schwung/modules/midi_fx/euclidrum
ssh root@move.local "chown -R ableton:users /data/UserData/schwung/modules/midi_fx/euclidrum"
```

Remove and re-add the module from an FX slot to reload.

### From Source

```bash
git clone https://github.com/fillioning/euclidrum-move.git
cd euclidrum-move
./build-module.sh    # Requires Docker (ARM64 cross-compile)
./install.sh         # Deploys to move.local via SSH
```

## Building

Requires Docker Desktop. The build script cross-compiles for ARM64 (Move's architecture):

```bash
./build-module.sh
```

Output: `dist/euclidrum/dsp.so` and `dist/euclidrum/module.json`

## Credits

- Euclidean engine based on [Eucalypso](https://github.com/handcraftedcc/move-everything-eucalypso) by handcraftedcc
- [Schwung](https://github.com/charlesvestal/schwung) framework by Charles Vestal and contributors
- [Weird Dreams](https://github.com/fillioning/weird-dreams-move) drum synth by Vincent Fillion (DSP: Daniele Filaretti)

## License

MIT

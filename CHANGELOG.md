# Changelog

## 0.2.0

- ACID: a TB-303 style bass machine (saw / square, a resonant four-pole filter, the accent and its
  sweep). The default kit's track 4 and the demo's bass line use it.
- STEP REC (KEYS + REC, stopped): a note a step and on to the next; rest, slide, accent, tie and back on
  the five buttons above the keys; a key pressed while another is held slides (also when recording live).
- A slide keeps the step before it open into it (a tie).
- Scales: root, scale (15) and SNAP on the HOME page's second row; the keys' lights show the scale; held
  steps move by scale degrees.
- The screen: a colour per track and per page, live graphs (envelope, filter, delay, LFO scope,
  arpeggio), a piano roll in the steps, the touched knob and the step playing light up; Chakra Petch for
  the headers and values.
- Songs of 0.1 still load (without a scale).

## 0.1.0

The first version (not yet tried on the hardware).

- Six mono tracks, eleven machines: SAW, PULS, ENS, SID, FM, FMDY, WAVE, DRUM, NOIS, PLUK, SINE.
- The same strip on every track: high-pass and low-pass filters with their envelope, the amp
  envelope, distortion, EQ, sample-rate reduction, volume, pan and a tempo-synced delay.
- Three LFOs per track, aimed at any value of any track.
- A 64-step sequencer: notes, accent, slide, 32 lock rows a pattern, trigless locks, swing, speed,
  probability, an arpeggiator per track, MIDI clock in and out.
- GRID and KEYS modes, live and step recording, copy / paste / clear of a track, mutes.
- 32 slots of a pattern and its kit, autosaved; save as; a demo in A01.
- MIDI: the selected track on the auto channel, each track on its own channel, CCs for every value,
  program change for the patterns.

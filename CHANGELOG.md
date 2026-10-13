# Changelog

## 0.3.0

- The KIT: track 1 is a drum kit of eleven voices, one per black key, each with its own sixteen values
  (type, tune, decay, tone, pitch envelope, noise, click; level, pan, delay send, probability, accent,
  choke group, ratchet, mute), its own steps with accents, and locks; through the track's strip in
  stereo. At most six voices sound at once.
- The black keys pick a voice (GRID) or play it as a pad (KEYS); the white keys are then its steps.
  The tracks and the tools (MUT ACC SLD CPY PST) moved to SEQ held + a black key.
- MIDI: GM drum notes on track 1's channel play the kit's voices.
- The default kit: KIT, ACID, FM, ENS, WAVE, PLUK; the demo uses the kit.
- 16 pattern slots (A01-A16): 148 KB of flash kept for the sampler to come.
- Songs of 0.1 and 0.2 still load: a DRUM on track 1 goes into the kit's voice of its type.

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

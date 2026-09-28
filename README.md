My fork of TUS's Gearmulator project, where I add emulations of Elektron's
Machinedrum and Monomachine.

I'm not affiliated with TUS or Elektron. Don't bug them for support :)

There is a Discord channel [here](https://discord.gg/BnkTKpmp8) at #gearmulator-development.
**Do NOT discuss firmware or ROMs in Discord.**
**DO NOT ask us for the .bin files / firmware! They're under Elektron's copyright. This emulator is for people who own the original hardware.**

[Downloads](https://github.com/joelanders/gearmulator-md-mm/releases) ·
[Report a bug](https://github.com/joelanders/gearmulator-md-mm/issues)

Link to a short demo on Youtube:

<a href="https://www.youtube.com/watch?v=NmfE5xljYRU"><img width="800" alt="youtube" src="https://i3.ytimg.com/vi/NmfE5xljYRU/maxresdefault.jpg" /></a>


## Features

- **Key chording / p-locks:** shift-click one or more buttons to hold them
  down until you release the shift key.
- **Secondary functions:** rather than shift-click Function and another button,
  you can just click the secondary function text label.
- **Encoder clicking:** Alt/Option-click a DATA ENTRY encoder to press it, or
  Alt/Option-drag to press and turn. With a trig held, pressing its parameter's
  encoder toggles that parameter lock. This applies to encoders A–H, not LEVEL
  or SOUND SELECTION.
- **Send SysEx File** under the right click menu to send a `.syx` file to the
  machine. The menu shows transfer progress and lets you cancel. Follow the
  machine's normal receive procedure.
- **Panel look and feel:** adjust encoder-drag and mouse-wheel sensitivity in settings.
  An experimental crisp LCD/panel rendering option is also available.
- **Audio inputs and outputs:** route host audio to the machine's input effects or sampling
  functions. Additional output pairs are available in a multi-output VST3 host;
  the standalone apps use stereo output.

## Ableton Push 3 control

Play the Machinedrum's front panel from an Ableton Push 3 in **User Mode**. The plugin
connects to Push's "User Port" by itself, so pressing Push's **User** button switches
between controlling Live and controlling the Machinedrum. There's nothing to re-route.

**Setup**

1. Connect Push 3 over USB (standalone models: Control mode) and load Gearmulator MD.
   Use only one instance.
2. In Live > Settings > Link, Tempo & MIDI, turn **Track** and **Remote** off for the
   *Ableton Push 3 User Port* input.
3. Press **User** on Push. Press it again to return to Live.

📄 [Printable cheat sheet (PDF)](doc/push3_cheatsheet.pdf)

**Pads** (top to bottom, with an empty row between blocks)

| Rows | Function |
|---|---|
| 8 / 7 | Mutes 1–8 / 9–16. Tap to mute or unmute (red = playing) |
| 5 / 4 | Tracks 1–8 / 9–16. Tap to select a track |
| 2 / 1 | Trigs 1–8 / 9–16. Lit in grid record, while holding a bank, or in Accent/Swing mode |

To select a pattern, hold a bank button (lower row 1–4) and tap a trig pad. The pad
blinks until the pattern switches.

**Buttons and knobs**

| Push 3 | Machinedrum |
|---|---|
| Encoders 1–8 | Data entry A–H |
| Left jog wheel / touch strip | Level (fine / fast slide) |
| Right jog wheel | Track select |
| Shift | Function |
| D-pad arrows / centre | Arrow buttons / Enter (Yes) |
| Master | Exit (No) |
| Select | Synthesis / Effects / Routing |
| Note | Kit |
| Play / Stop / Tap Tempo | Play / Stop / Tempo |
| Record | Grid record |
| Capture | Live record (Record + Play) |
| Double Loop | Next trig page (Scale) |
| Lower row 1–4 (below display) | Bank A–D |
| Upper row 1–4 (above display) | Page lights 1–4 |
| Hot Swap | Bank group A–D / E–H |
| Mute / Accent / Quantize / Gear (Setup) | Mute mode / Accent / Swing / Global |

Mapped buttons glow and brighten while held. Live keeps the Push display while it runs,
so the Machinedrum screen stays in the plugin window. Function shortcuts are Machinedrum only.

## Implementation references

- [TurboMIDI negotiation](doc/turbomidi.md): a worked exchange, firmware observations,
  and Gearmulator sender policy.

Thanks to the upstream Gearmulator contributors whose work makes this fork
possible. See [the upstream README](README.upstream.md) for the original project
overview.

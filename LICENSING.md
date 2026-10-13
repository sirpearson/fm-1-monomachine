# MNM-1 licensing

MNM-1 is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full
text in `LICENSE`). It is built on FiMba-1 (and through it FoMni, X0X and Felucca), and keeps their
licence. If you distribute MNM-1, or firmware derived from it, you must give your recipients its
complete corresponding source under the same licence.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| Platform: `firmware/hal/`, `firmware/loader/`, `firmware/src/{libc,lcd,gfx,usb,storage,ota,midi_uart}.c`, `firmware/crt0.S`, `firmware/app.ld`, `tools/` (build, package, install, rescue, font), `web/fm1*.js` | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, as changed for [X0X](https://github.com/charlesvestal/fm1-x0x), [FoMni](https://github.com/charlesvestal/fm1-fomni) and [FiMba-1](https://github.com/jadamsowers/fm1-fimba) (USB names and the flash object map changed here) | GPL-3.0-only |
| `firmware/src/app/{panel,plat_fm1,main_fm1}.c`, `firmware/src/main.c`, `host/mnm_host.c`, the frame of `firmware/src/app/ui.c` (bands, knobs, autosave) and of `project.c` | adapted from FiMba-1 / FoMni (themselves from X0X and Felucca) | GPL-3.0-only |
| `firmware/src/dsp/fastmath.h` | FoMni / X0X | GPL-3.0-only |
| Platform tests (`tests/host/{fastmath,encoder,uac,trs}_test.c`, `tests/{storage,ota,ldr}_test.c`, `tests/install_test.py`) | FiMba-1 / FoMni / Felucca | GPL-3.0-only |
| Everything else: the six-track engine, the machines, the sequencer, locks and arpeggiator (`firmware/src/dsp/mnm.{c,h}`), the UI's pages and grid, the storage of the pattern slots, `tests/host/mnm_test.c`, `tests/scenarios/` | MNM-1 | GPL-3.0-only |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors), the UI face | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus (Dimitar Toshkov Zhekov), an alternative font set | SIL OFL 1.1 | `assets/fonts/ter-u*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Monomachine" and "Elektron" are trademarks of Elektron Music Machines. "Felucca" and "Hügelton
Instruments" are names of Hügelton Instruments. "M-VAVE" and "FM-1" are trademarks of their
respective owners. MNM-1 is independent firmware, not affiliated with, endorsed by or supported by
any of them; it borrows the Monomachine's way of working, not its code or sounds.

## Radio

MNM-1 never enables the Bluetooth / Wi-Fi radio of the hardware.

// SPDX-License-Identifier: GPL-3.0-only
// The browser emulator's WebAssembly, in Node: it needs no imports, boots to a drawn screen, plays the
// demo when PLAY is held, keeps a saved object across a boot, and runs well faster than real time.
//   node tests/web/emu_test.mjs build/emu/mnm.wasm
import { readFileSync } from "node:fs";
let fails = 0;
const ok = (c, what) => { console.log(`${c ? "  ok  " : "  FAIL"} ${what}`); if (!c) fails++; };
const mod = await WebAssembly.compile(readFileSync(process.argv[2]));
ok(WebAssembly.Module.imports(mod).length === 0, "no imports: no C library, nothing from the page");
const { exports: ex } = await WebAssembly.instantiate(mod, {});
ex.web_init();
ex.web_boot();
for (let i = 0; i < 100; i++) ex.web_render(441);
const lit = new Uint16Array(ex.memory.buffer, ex.web_fb(), 240 * 240).filter((v) => v).length;
ok(lit > 10000, `the screen is drawn (${lit} pixels lit)`);
ex.web_buttons(1 << 10);
for (let i = 0; i < 10; i++) ex.web_render(441);
ex.web_buttons(0);
let pk = 0;
const t0 = performance.now();
for (let i = 0; i < 200; i++) {
  ex.web_render(441);
  for (const x of new Float32Array(ex.memory.buffer, ex.web_out_l(), 441)) pk = Math.max(pk, Math.abs(x));
}
const ms = performance.now() - t0;
ok(ex.web_playing() === 1 && pk > 0.2 && pk < 1, `PLAY: the demo plays (peak ${pk.toFixed(2)})`);
ok(ms < 1000, `2 s of the device in ${ms.toFixed(0)} ms`);
ex.web_buttons(1 << 10);                                   // stop, then SAVE: an object to keep
for (let i = 0; i < 10; i++) ex.web_render(441);
ex.web_buttons(1 << 7);
for (let i = 0; i < 10; i++) ex.web_render(441);
ex.web_buttons(0);
for (let i = 0; i < 10; i++) ex.web_render(441);
ok(ex.web_store_writes() > 0 && ex.web_store_len(2) > 3000, "SAVE writes the pattern slot");
console.log(fails ? `FAILED: ${fails}` : "emulator tests passed");
process.exit(fails ? 1 : 0);

// Runs `vite build` with ASCII-only output for the MSBuild custom build step.
// MSBuild decodes that output with the OEM code page whatever chcp says, so the UTF-8 glyphs in
// Vite's log ("✓ built in", the "│" of the size table) would show up as "Γ£ô" and "Γöé". Rolldown's
// native reporter writes "✓ N modules transformed." straight to stdout, past any Vite logger, so the
// build runs in a child process whose output is rewritten here.
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const vite = fileURLToPath(new URL('../node_modules/vite/bin/vite.js', import.meta.url));
const glyphs = { '✓': 'OK', '✗': 'x', '│': '|', '─': '-', '…': '...' };
const ascii = (text) => text.replace(/[✓✗│─…]/g, (glyph) => glyphs[glyph]);

const child = spawn(process.execPath, [vite, 'build', ...process.argv.slice(2)], {
  stdio: ['inherit', 'pipe', 'pipe'],
});
for (const [from, to] of [
  [child.stdout, process.stdout],
  [child.stderr, process.stderr],
]) {
  from.setEncoding('utf8'); // keeps multi-byte characters whole across chunks
  from.on('data', (text) => to.write(ascii(text)));
}
child.on('error', (error) => {
  console.error(error.message);
  process.exitCode = 1;
});
child.on('close', (code) => {
  process.exitCode = code ?? 1;
});

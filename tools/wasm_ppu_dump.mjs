#!/usr/bin/env node
// Capture GBA memory snapshots + reference canvas RGBA from the WASM build, for
// validating the C PPU (rp2350/ppu.c) against the app.js reference rasteriser.
//
// Same event-file format as wasm_replay.mjs. On each `screenshot` event it dumps,
// into <output>/<NNNNNN-name>/:  reg.bin pal.bin vram.bin oam.bin ref_rgba.bin
// The host test (rp2350/ppu_host_test.c) renders the snapshot and diffs ref_rgba.
//
// usage: node tools/wasm_ppu_dump.mjs <events.txt> [output-dir] [--no-build] [--keep-browser]

import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import { spawn } from 'node:child_process';

const buttons = new Set(['a', 'b', 'select', 'start', 'right', 'left', 'up', 'down', 'r', 'l']);

// GBA region (wasm absolute addr, byte length). Registers: 0x60 used, grab 0x100.
const REGIONS = {
  reg: [0x04000000, 0x100],
  pal: [0x05000000, 0x400],
  vram: [0x06000000, 0x18000],
  oam: [0x07000000, 0x400],
};

function usage() {
  console.error('usage: node tools/wasm_ppu_dump.mjs <events.txt> [output-dir] [--no-build] [--keep-browser]');
  process.exit(2);
}

function parseArgs(argv) {
  const options = { build: true, keepBrowser: false };
  const paths = [];
  for (const arg of argv) {
    if (arg === '--no-build') options.build = false;
    else if (arg === '--keep-browser') options.keepBrowser = true;
    else paths.push(arg);
  }
  if (paths.length < 1 || paths.length > 2) usage();
  return { inputPath: resolve(paths[0]), outputDir: resolve(paths[1] || 'wasm-ppu-dump'), options };
}

function parseEvents(text) {
  const events = [];
  const lines = text.split(/\r?\n/);
  for (let index = 0; index < lines.length; index++) {
    const line = lines[index].replace(/#.*/, '').trim();
    if (!line) continue;
    const fields = line.split(/\s+/);
    const frame = Number(fields[0]);
    if (!Number.isInteger(frame) || frame < 0) throw new Error(`${index + 1}: frame must be a non-negative integer`);
    if (fields[1] === 'screenshot') {
      events.push({ frame, type: 'screenshot', name: fields[2] || `frame-${frame}` });
      continue;
    }
    if (fields[1] !== 'button' || fields.length !== 4) {
      throw new Error(`${index + 1}: expected "<frame> button <name> <on|off>" or "<frame> screenshot [name]"`);
    }
    if (!buttons.has(fields[2])) throw new Error(`${index + 1}: unknown button "${fields[2]}"`);
    if (fields[3] !== 'on' && fields[3] !== 'off') throw new Error(`${index + 1}: button state must be on or off`);
    events.push({ frame, type: 'button', name: fields[2], pressed: fields[3] === 'on' });
  }
  return events.sort((a, b) => a.frame - b.frame || (a.type === 'button' ? -1 : 1));
}

function run(command, args, log) {
  return new Promise((res, rej) => {
    const child = spawn(command, args, { cwd: resolve('.'), env: process.env });
    child.stdout.on('data', (c) => log(`${command} stdout: ${c}`));
    child.stderr.on('data', (c) => log(`${command} stderr: ${c}`));
    child.on('error', rej);
    child.on('exit', (code) => (code === 0 ? res() : rej(new Error(`${command} exited ${code}`))));
  });
}

function browserPath() {
  const candidates = [
    process.env.CHROME_BIN,
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
    '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser', '/snap/bin/chromium',
  ].filter(Boolean);
  const found = candidates.find((p) => existsSync(p));
  if (!found) throw new Error('Chrome/Chromium not found; set CHROME_BIN');
  return found;
}

async function startServer(log) {
  const child = spawn('node', ['web/server.mjs'], { cwd: resolve('.'), env: { ...process.env, PORT: '0' } });
  return await new Promise((res, rej) => {
    const timeout = setTimeout(() => rej(new Error('timed out waiting for wasm web server')), 10000);
    child.stdout.on('data', (chunk) => {
      const text = String(chunk);
      log(`server stdout: ${text}`);
      const match = text.match(/http:\/\/localhost:(\d+)/);
      if (match) { clearTimeout(timeout); res({ child, url: `http://localhost:${match[1]}` }); }
    });
    child.stderr.on('data', (c) => log(`server stderr: ${c}`));
    child.on('error', rej);
    child.on('exit', (code) => rej(new Error(`server exited before ready with ${code}`)));
  });
}

async function startBrowser(userDataDir, log) {
  const child = spawn(browserPath(), [
    '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${userDataDir}`,
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-gpu', 'about:blank',
  ]);
  return await new Promise((res, rej) => {
    const timeout = setTimeout(() => rej(new Error('timed out waiting for Chrome DevTools')), 10000);
    child.stderr.on('data', (chunk) => {
      const text = String(chunk);
      log(`browser stderr: ${text}`);
      const match = text.match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) { clearTimeout(timeout); res({ child, browserWs: match[1] }); }
    });
    child.stdout.on('data', (c) => log(`browser stdout: ${c}`));
    child.on('error', rej);
    child.on('exit', (code) => rej(new Error(`browser exited before ready with ${code}`)));
  });
}

class Cdp {
  constructor(url) {
    this.nextId = 1; this.pending = new Map(); this.handlers = new Map();
    this.socket = new WebSocket(url);
    this.ready = new Promise((res, rej) => {
      this.socket.addEventListener('open', res, { once: true });
      this.socket.addEventListener('error', rej, { once: true });
    });
    this.socket.addEventListener('message', (e) => this.receive(JSON.parse(e.data)));
  }
  receive(message) {
    if (message.id) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(message.error.message));
      else pending.resolve(message.result);
      return;
    }
    const handler = this.handlers.get(message.method);
    if (handler) handler(message.params);
  }
  on(method, handler) { this.handlers.set(method, handler); }
  async send(method, params = {}) {
    await this.ready;
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params }));
    return await new Promise((res, rej) => this.pending.set(id, { resolve: res, reject: rej }));
  }
  close() { this.socket.close(); }
}

async function newPage(browserWs) {
  const endpoint = new URL(browserWs);
  const response = await fetch(`http://${endpoint.host}/json/new`, { method: 'PUT' });
  const target = await response.json();
  return new Cdp(target.webSocketDebuggerUrl);
}

async function evaluate(cdp, expression, awaitPromise = true) {
  const result = await cdp.send('Runtime.evaluate', { expression, awaitPromise, returnByValue: true });
  if (result.exceptionDetails) {
    const d = result.exceptionDetails;
    throw new Error([d.text, d.exception?.description || d.exception?.value].filter(Boolean).join('\n'));
  }
  return result.result.value;
}

function safeName(name) { return name.replace(/[^A-Za-z0-9._-]+/g, '_'); }

// Read a wasm memory range (or the canvas pixels) as base64, chunking the
// String.fromCharCode to avoid blowing the argument stack on large regions.
async function dumpBase64(cdp, expr) {
  const b64 = await evaluate(cdp, `(() => {
    const bytes = ${expr};
    let binary = '';
    for (let i = 0; i < bytes.length; i += 0x8000) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(binary);
  })()`);
  return Buffer.from(b64, 'base64');
}

async function saveSnapshot(cdp, outputDir, event) {
  const dir = resolve(outputDir, `${String(event.frame).padStart(6, '0')}-${safeName(event.name)}`);
  await mkdir(dir, { recursive: true });
  for (const [name, [addr, len]] of Object.entries(REGIONS)) {
    const buf = await dumpBase64(cdp, `new Uint8Array(window.pokeemerald.memory.buffer, ${addr}, ${len})`);
    await writeFile(resolve(dir, `${name}.bin`), buf);
  }
  const rgba = await dumpBase64(cdp,
    `document.querySelector('#screen').getContext('2d').getImageData(0,0,240,160).data`);
  await writeFile(resolve(dir, 'ref_rgba.bin'), rgba);
  const state = await evaluate(cdp, `window.pokeemerald.automation.state()`);
  await writeFile(resolve(dir, 'state.json'), JSON.stringify(state, null, 2));
  return dir;
}

async function main() {
  const { inputPath, outputDir, options } = parseArgs(process.argv.slice(2));
  await rm(outputDir, { recursive: true, force: true });
  await mkdir(outputDir, { recursive: true });
  const logLines = [];
  const log = (line) => logLines.push(String(line).trimEnd());
  const userDataDir = await mkdtemp(resolve(tmpdir(), 'pokeemerald-ppu-dump-'));
  let server, browser, cdp;

  try {
    const events = parseEvents(await readFile(inputPath, 'utf8'));
    if (options.build) await run('make', ['wasm'], log);
    server = await startServer(log);
    browser = await startBrowser(userDataDir, log);
    cdp = await newPage(browser.browserWs);
    cdp.on('Runtime.consoleAPICalled', (p) => log(`console ${p.type}: ${p.args.map((a) => a.value ?? a.description).join(' ')}`));
    await cdp.send('Runtime.enable');
    await cdp.send('Page.enable');
    await cdp.send('Page.navigate', { url: `${server.url}/?automate=1` });
    await evaluate(cdp, `new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timed out waiting for wasm automation')), 30000);
      const check = () => {
        if (window.pokeemerald?.automation?.ready) {
          window.pokeemerald.automation.ready.then(() => { clearTimeout(timeout); resolve(); });
        } else setTimeout(check, 20);
      };
      check();
    })`, true);

    const dumps = [];
    for (const event of events) {
      await evaluate(cdp, `window.pokeemerald.automation.runToFrame(${event.frame})`);
      if (event.type === 'button') {
        await evaluate(cdp, `window.pokeemerald.automation.setButton(${JSON.stringify(event.name)}, ${event.pressed})`);
      } else {
        const dir = await saveSnapshot(cdp, outputDir, event);
        dumps.push(dir);
        log(`dumped ${dir}`);
      }
    }
    console.log(`Wrote ${dumps.length} snapshot(s) under ${outputDir}`);
  } catch (error) {
    logLines.push(error.stack || String(error));
    console.error(error);
    process.exitCode = 1;
  } finally {
    await writeFile(resolve(outputDir, 'console.log'), `${logLines.join('\n')}\n`).catch(() => {});
    if (cdp) cdp.close();
    if (browser && !options.keepBrowser) browser.child.kill();
    if (server) server.child.kill();
    if (!options.keepBrowser) await rm(userDataDir, { recursive: true, force: true }).catch(() => {});
  }
}

main();

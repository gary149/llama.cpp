// Replay PTY captures in a browser terminal, compare checkpoints, and save screenshots.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const [artifacts, dependencies, executable] = process.argv.slice(2);
if (!artifacts || !dependencies) {
    throw new Error('Usage: node replay_terminal.mjs ARTIFACT_DIRECTORY NPM_PREFIX [CHROMIUM]');
}
const require = createRequire(path.resolve(dependencies, 'package.json'));
const { chromium } = require('playwright');
const browser = await chromium.launch({ executablePath: executable, args: ['--no-sandbox'] });
let failures = 0;
let checkpoints = 0;
const gallery = [];
const escapeHTML = text => text.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('"', '&quot;');
try {
    for (const file of fs.readdirSync(artifacts, { recursive: true }).filter(name => name.endsWith('.json') && fs.existsSync(path.join(artifacts, name.slice(0, -5) + '.ansi')))) {
        const stem = path.join(artifacts, file.slice(0, -5));
        const events = JSON.parse(fs.readFileSync(stem + '.json', 'utf8'));
        const raw = fs.readFileSync(stem + '.ansi');
        gallery.push(`<details><summary>${escapeHTML(file)}</summary><div class="frames">`);
        const page = await browser.newPage({ viewport: { width: 1800, height: 1400 } });
        await page.setContent('<body style="margin:0;background:#101010"><div id="terminal"></div></body>');
        await page.addStyleTag({ path: path.join(dependencies, 'node_modules/@xterm/xterm/css/xterm.css') });
        await page.addScriptTag({ path: require.resolve('@xterm/xterm') });
        await page.evaluate(() => {
            window.term = new Terminal({ cols: 80, rows: 24, fontSize: 14, cursorBlink: false,
                                        theme: { background: '#101010', foreground: '#eeeeee' }, scrollback: 10000 });
            window.term.open(document.querySelector('#terminal'));
            window.term.focus();
        });
        let offset = 0;
        let index = 0;
        for (const event of events) {
            if (event.offset === undefined) {
                throw new Error(`${file}: capture lacks byte offsets; rerun the Python harness`);
            }
            const bytes = [...raw.subarray(offset, event.offset)];
            if (bytes.length) {
                await page.evaluate(bytes => new Promise(resolve => window.term.write(new Uint8Array(bytes), resolve)), bytes);
            }
            offset = event.offset;
            if (event.type === 'resize') {
                await page.evaluate(event => window.term.resize(event.columns, event.rows), event);
                continue;
            }
            const actual = await page.evaluate(() => {
                const buffer = window.term.buffer.active;
                return { screen: Array.from({ length: window.term.rows }, (_, row) => buffer.getLine(buffer.viewportY + row).translateToString(true).trimEnd().normalize('NFC')),
                         cursor: [buffer.cursorX, buffer.cursorY] };
            });
            const staleFooter = await page.evaluate(() => {
                const buffer = window.term.buffer.active;
                for (let row = 0; row < buffer.baseY; row++) {
                    if (buffer.getLine(row).translateToString().includes('fixture-model')) return true;
                }
                return false;
            });
            const expected = { screen: event.screen.map(line => line.trimEnd().normalize('NFC')), cursor: event.cursor };
            if (staleFooter || JSON.stringify(actual) !== JSON.stringify(expected)) {
                failures++;
                fs.writeFileSync(`${stem}-${index}.difference.json`, JSON.stringify({ checkpoint: event.checkpoint, staleFooter, expected, actual }, null, 2));
                console.error(`FAIL ${file}: ${event.checkpoint}`);
            }
            const screenshot = `${stem}-${index++}.png`;
            await page.locator('.xterm-screen').screenshot({ path: screenshot });
            const relative = escapeHTML(path.relative(artifacts, screenshot));
            gallery.push(`<figure><a href="${relative}"><img loading="lazy" src="${relative}"></a><figcaption>${escapeHTML(event.checkpoint)} (${event.columns}x${event.rows})</figcaption></figure>`);
            checkpoints++;
        }
        gallery.push('</div></details>');
        await page.close();
    }
} finally {
    await browser.close();
}
fs.writeFileSync(path.join(artifacts, 'index.html'), `<!doctype html><meta charset="utf-8"><title>Agent terminal verification</title><style>body{font:16px system-ui;margin:2rem;background:#eee;color:#222}summary{cursor:pointer;padding:1rem}.frames{display:flex;flex-wrap:wrap;gap:1rem}figure{margin:0;padding:1rem;background:white}img{max-width:640px;height:auto}figcaption{margin-top:.5rem}</style><h1>Agent terminal verification</h1><p>${checkpoints} checkpoints, ${failures} differences. Expand a test to inspect its screenshots.</p>${gallery.join('')}`);
console.log(`${checkpoints} browser checkpoints, ${failures} differences`);
process.exitCode = failures ? 1 : 0;

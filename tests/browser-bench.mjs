#!/usr/bin/env node
// browser-bench.mjs — measures how long a Chromium browser needs to show a local
// image. This is the comparison baseline required by the plan: "the same file
// dropped into a browser".
//
// It drives the browser over the DevTools protocol, using Node's built-in
// WebSocket (no npm dependencies).
//
//   node tests/browser-bench.mjs <chrome-binary> <image-path> [runs]

import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = process.argv[2];
const IMAGE = process.argv[3];
const RUNS = Number(process.argv[4] ?? '3');
const PORT = 9333;
const WIDTH = 1200;
const HEIGHT = 800;

if (!CHROME || !IMAGE) {
  console.error('usage: browser-bench.mjs <chrome-binary> <image-path> [runs]');
  process.exit(2);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitForTarget() {
  for (let attempt = 0; attempt < 200; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const targets = await response.json();
      const page = targets.find((target) => target.type === 'page');
      if (page?.webSocketDebuggerUrl) return page.webSocketDebuggerUrl;
    } catch {
      // browser not up yet
    }
    await sleep(50);
  }
  throw new Error('browser did not expose a page target');
}

function makeClient(url) {
  const socket = new WebSocket(url);
  const pending = new Map();
  const listeners = new Set();
  let nextId = 1;

  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) reject(new Error(message.error.message));
      else resolve(message.result);
      return;
    }
    for (const listener of listeners) listener(message);
  });

  return {
    ready: new Promise((resolve, reject) => {
      socket.addEventListener('open', resolve, { once: true });
      socket.addEventListener('error', reject, { once: true });
    }),
    send(method, params = {}) {
      const id = nextId++;
      socket.send(JSON.stringify({ id, method, params }));
      return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
    },
    onEvent(listener) {
      listeners.add(listener);
    },
    close() {
      socket.close();
    },
  };
}

async function runOnce(executable) {
  const profile = mkdtempSync(join(tmpdir(), 'browser-bench-'));
  const started = performance.now();
  const child = spawn(
    executable,
    [
      '--headless=new',
      '--no-sandbox',
      '--disable-gpu',
      '--hide-scrollbars',
      '--force-device-scale-factor=1',
      `--window-size=${WIDTH},${HEIGHT}`,
      `--remote-debugging-port=${PORT}`,
      `--user-data-dir=${profile}`,
      'about:blank',
    ],
    { stdio: 'ignore' },
  );

  try {
    const wsUrl = await waitForTarget();
    const client = makeClient(wsUrl);
    await client.ready;

    await client.send('Page.enable');
    await client.send('Runtime.enable');
    await client.send('Page.addScriptToEvaluateOnNewDocument', {
      source: `
        window.__lcp = 0;
        window.__lcpSize = 0;
        new PerformanceObserver((list) => {
          for (const entry of list.getEntries()) {
            window.__lcp = entry.startTime;
            window.__lcpSize = entry.size || 0;
          }
        }).observe({ type: 'largest-contentful-paint', buffered: true });
      `,
    });

    let loadResolve;
    const loadEvent = new Promise((resolve) => {
      loadResolve = resolve;
    });
    client.onEvent((message) => {
      if (message.method === 'Page.loadEventFired') loadResolve();
    });

    const navigateStart = performance.now();
    await client.send('Page.navigate', { url: IMAGE });
    await loadEvent;
    const loadWall = performance.now() - navigateStart;

    // Give the paint that carries the image a moment to be recorded.
    await sleep(150);
    const { result } = await client.send('Runtime.evaluate', {
      expression: `JSON.stringify({
        lcp: window.__lcp,
        lcpSize: window.__lcpSize,
        nav: performance.getEntriesByType('navigation')[0]
          ? {
              domContentLoaded: performance.getEntriesByType('navigation')[0].domContentLoadedEventEnd,
              load: performance.getEntriesByType('navigation')[0].loadEventEnd,
              transfer: performance.getEntriesByType('navigation')[0].transferSize
            }
          : null,
        paints: performance.getEntriesByType('paint').map((entry) => [entry.name, entry.startTime])
      })`,
      returnByValue: true,
    });

    client.close();
    return {
      coldLaunchMs: Math.round(loadWall * 10) / 10,
      ...JSON.parse(result.value),
      processMs: Math.round((performance.now() - started) * 10) / 10,
    };
  } finally {
    child.kill('SIGKILL');
    await sleep(200);
    rmSync(profile, { recursive: true, force: true });
  }
}

const results = [];
for (let index = 0; index < RUNS; index += 1) {
  results.push(await runOnce(CHROME));
}

const median = (values) => {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
};

console.log(JSON.stringify({
  image: IMAGE,
  runs: results,
  medianLcpMs: median(results.map((r) => r.lcp)),
  medianLoadMs: median(results.map((r) => r.coldLaunchMs)),
}, null, 2));

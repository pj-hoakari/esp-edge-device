import { createServer } from 'node:http';
import { readFile, writeFile } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = path.join(HERE, 'public');
const DEFAULT_STUN_URL = 'stun:stun.l.google.com:19302';
const MAX_SDP_BYTES = 64 * 1024;
const DEFAULT_OFFER_TTL_MS = 120000;

const CONTENT_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
};

function send(res, status, body = '', type = 'text/plain; charset=utf-8') {
  const data = Buffer.isBuffer(body) ? body : Buffer.from(body);
  res.writeHead(status, {
    'Content-Type': type,
    'Content-Length': data.length,
    'Cache-Control': 'no-store',
  });
  res.end(data);
}

function sendJson(res, status, value) {
  send(res, status, JSON.stringify(value), 'application/json; charset=utf-8');
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', chunk => {
      size += chunk.length;
      if (size > MAX_SDP_BYTES) {
        reject(Object.assign(new Error('SDP too large'), { statusCode: 413 }));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function parseIntParam(value) {
  if (!value || !/^\d+$/.test(value)) return null;
  const n = Number(value);
  return Number.isSafeInteger(n) ? n : null;
}

function normalizeStunUrls(value) {
  const list = Array.isArray(value) ? value : String(value ?? '').split(',');
  return list.map(url => url.trim()).filter(Boolean);
}

function isIPv4(value) {
  return /^\d{1,3}(\.\d{1,3}){3}$/.test(value);
}

function isUnusableIPv4(value) {
  return !isIPv4(value) || value === '127.0.0.1' || value === '0.0.0.0';
}

// Pick our LAN address on the ESP's subnet. fallback is the first usable
// non-internal IPv4 when the ESP has not polled yet.
function localAddressForPeer(peerAddress) {
  const peer = String(peerAddress ?? '').replace(/^::ffff:/, '');
  let fallback = '';
  for (const list of Object.values(networkInterfaces())) {
    for (const ni of list ?? []) {
      if (ni.family !== 'IPv4' || ni.internal) continue;
      if (!fallback) fallback = ni.address;
      if (isIPv4(peer)) {
        const prefix = peer.split('.').slice(0, 3).join('.') + '.';
        if (ni.address.startsWith(prefix)) return ni.address;
      }
    }
  }
  return fallback;
}

// Browsers hide LAN addresses behind mDNS host candidates. The ESP cannot
// resolve mDNS reliably, so substitute an address it can reach. When the
// viewer is opened via localhost the socket address is 127.0.0.1, so fall
// back to the interface shared with the ESP.
export function expandLocalCandidates(sdp, viewerAddress, espAddress) {
  let ip = String(viewerAddress ?? '').replace(/^::ffff:/, '');
  if (isUnusableIPv4(ip)) ip = localAddressForPeer(espAddress);
  if (isUnusableIPv4(ip)) return sdp;
  return sdp.replace(
    /^(a=candidate:\S+ \d+ \S+ \d+ )(\S+\.local)( \d+ typ host)/gm,
    (_, prefix, _address, suffix) => `${prefix}${ip}${suffix}`,
  );
}

export function createSignalingServer(options = {}) {
  const stunUrls = normalizeStunUrls(options.stunUrls ?? process.env.STUN_URL ?? DEFAULT_STUN_URL);
  const offerTtlMs = Number(options.offerTtlMs ?? DEFAULT_OFFER_TTL_MS);
  // ESP remembers only the last offer ID it handled. Seed above 1 so a server
  // restart cannot accidentally reuse the ID the ESP already saw.
  let nextOfferId = 1 + Math.floor(Math.random() * 0x3fffffff);
  let offer = null;
  let answer = null;
  let espSeenId = 0;
  let espLastPollAt = 0;
  let espAddress = '';

  function currentOffer(now = Date.now()) {
    if (offer && now - offer.at > offerTtlMs) {
      offer = null;
      answer = null;
    }
    return offer;
  }

  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://localhost');
      const route = `${req.method} ${url.pathname}`;

      if (route === 'GET /' || route === 'GET /index.html') {
        return send(res, 200, await readFile(path.join(PUBLIC_DIR, 'index.html')),
          CONTENT_TYPES['.html']);
      }
      if (route === 'GET /receiver.js') {
        return send(res, 200, await readFile(path.join(PUBLIC_DIR, 'receiver.js')),
          CONTENT_TYPES['.js']);
      }
      if (route === 'GET /ice') {
        return sendJson(res, 200, {
          iceServers: stunUrls.map(urls => ({ urls: [urls] })),
          iceTransportPolicy: 'all',
        });
      }
      if (route === 'GET /api/status') {
        const active = currentOffer();
        return sendJson(res, 200, {
          offer_id: active?.id ?? 0,
          has_answer: active != null && answer?.id === active.id,
          esp_seen_id: espSeenId,
          esp_last_poll_ms_ago: espLastPollAt ? Date.now() - espLastPollAt : -1,
          offer_age_ms: active ? Date.now() - active.at : -1,
          stun_urls: stunUrls,
        });
      }
      if (route === 'POST /api/offer') {
        const body = await readBody(req);
        if (!body.startsWith('v=0')) {
          return send(res, 400, 'invalid SDP');
        }
        const sdp = expandLocalCandidates(body, req.socket.remoteAddress, espAddress);
        const id = nextOfferId++;
        offer = { id, sdp, at: Date.now() };
        answer = null;
        console.log(`[signal] viewer offer ${id} (${sdp.length} bytes)`);
        const debug = `remote=${req.socket.remoteAddress}\n` +
          `--- received ---\n${body}\n--- sent to ESP ---\n${sdp}\n`;
        await writeFile(path.join(HERE, 'last-offer.sdp'), debug, 'utf8').catch(() => {});
        for (const line of body.split('\n')) {
          if (line.startsWith('a=candidate:')) console.log(`[signal] offer candidate${line}`);
        }
        return send(res, 200, String(id));
      }
      if (route === 'GET /api/answer') {
        const id = parseIntParam(url.searchParams.get('id'));
        if (id == null) return send(res, 400, 'missing id');
        const active = currentOffer();
        if (!active || active.id !== id) return send(res, 409, 'offer replaced');
        if (answer?.id === id) return send(res, 200, answer.sdp);
        return send(res, 202);
      }
      if (route === 'GET /api/esp/offer') {
        const seen = parseIntParam(url.searchParams.get('seen')) ?? 0;
        espAddress = req.socket.remoteAddress ?? espAddress;
        espLastPollAt = Date.now();
        espSeenId = seen;
        const active = currentOffer();
        if (!active || active.id === seen) return send(res, 204);
        return send(res, 200, `${active.id}\n${active.sdp}`);
      }
      if (route === 'POST /api/esp/answer') {
        const id = parseIntParam(url.searchParams.get('id'));
        const sdp = await readBody(req);
        if (id == null) return send(res, 400, 'missing id');
        const active = currentOffer();
        if (!active || active.id !== id) return send(res, 409, 'offer replaced');
        if (!sdp.startsWith('v=0')) return send(res, 400, 'invalid SDP');
        answer = { id, sdp };
        console.log(`[signal] ESP answer ${id} (${sdp.length} bytes)`);
        return send(res, 200, 'ok');
      }
      return send(res, 404, 'not found');
    } catch (err) {
      const status = err.statusCode ?? 500;
      console.error('[signal]', err);
      if (!res.headersSent) send(res, status, err.message || 'error');
      else res.destroy();
    }
  });

  server.on('listening', () => {
    console.log(`[signal] viewer: http://localhost:${server.address().port}/`);
    console.log(`[signal] ESP polling: http://<this-host>:${server.address().port}/api/esp/offer`);
    console.log(`[signal] STUN: ${stunUrls.join(', ') || '(disabled)'}`);
  });
  return server;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
  const port = Number(process.env.PORT ?? 8080);
  const host = process.env.HOST ?? '0.0.0.0';
  const server = createSignalingServer({
    stunUrls: process.env.STUN_URL ?? DEFAULT_STUN_URL,
    offerTtlMs: Number(process.env.OFFER_TTL_MS ?? DEFAULT_OFFER_TTL_MS),
  });
  server.listen(port, host);
}

export { DEFAULT_STUN_URL };

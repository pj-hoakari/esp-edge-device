import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createSignalingServer, expandLocalCandidates } from '../server.mjs';

function listen(server) {
  return new Promise(resolve => server.listen(0, '127.0.0.1', () => {
    resolve(`http://127.0.0.1:${server.address().port}`);
  }));
}

const offer = 'v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\n';
const answer = 'v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=-\r\n';

test('relays offer to ESP and answer to viewer', async t => {
  const server = createSignalingServer({ stunUrls: ['stun:example.test:3478'] });
  const base = await listen(server);
  t.after(() => server.close());

  let res = await fetch(`${base}/api/esp/offer?seen=0`);
  assert.equal(res.status, 204);

  res = await fetch(`${base}/api/offer`, { method: 'POST', body: offer });
  assert.equal(res.status, 200);
  const id = Number(await res.text());
  assert.ok(id > 0);

  res = await fetch(`${base}/api/esp/offer?seen=0`);
  assert.equal(res.status, 200);
  assert.equal(await res.text(), `${id}\n${offer}`);
  assert.equal((await fetch(`${base}/api/esp/offer?seen=${id}`)).status, 204);

  assert.equal((await fetch(`${base}/api/answer?id=${id}`)).status, 202);
  res = await fetch(`${base}/api/esp/answer?id=${id}`, { method: 'POST', body: answer });
  assert.equal(res.status, 200);
  res = await fetch(`${base}/api/answer?id=${id}`);
  assert.equal(res.status, 200);
  assert.equal(await res.text(), answer);

  res = await fetch(`${base}/ice`);
  assert.deepEqual(await res.json(), {
    iceServers: [{ urls: ['stun:example.test:3478'] }],
    iceTransportPolicy: 'all',
  });
  assert.equal((await fetch(`${base}/`)).status, 200);
  assert.equal((await fetch(`${base}/receiver.js`)).status, 200);
});

test('a new viewer offer invalidates the previous answer', async t => {
  const server = createSignalingServer({ stunUrls: [] });
  const base = await listen(server);
  t.after(() => server.close());

  const first = Number(await (await fetch(`${base}/api/offer`, { method: 'POST', body: offer })).text());
  await fetch(`${base}/api/esp/answer?id=${first}`, { method: 'POST', body: answer });
  const second = Number(await (await fetch(`${base}/api/offer`, { method: 'POST', body: offer })).text());
  assert.equal((await fetch(`${base}/api/answer?id=${first}`)).status, 409);
  assert.equal((await fetch(`${base}/api/answer?id=${second}`)).status, 202);
  assert.equal((await fetch(`${base}/api/esp/answer?id=${first}`, { method: 'POST', body: answer })).status, 409);
});

test('connectivity GET /ice preserves the session and ESP polling state', async t => {
  const server = createSignalingServer({ stunUrls: ['stun:example.test:3478'] });
  const base = await listen(server);
  t.after(() => server.close());
  const id = Number(await (await fetch(`${base}/api/offer`, { method: 'POST', body: offer })).text());
  await fetch(`${base}/api/esp/answer?id=${id}`, { method: 'POST', body: answer });
  for (let i = 0; i < 3; i++) assert.equal((await fetch(`${base}/ice`)).status, 200);
  const status = await (await fetch(`${base}/api/status`)).json();
  assert.equal(status.offer_id, id);
  assert.equal(status.has_answer, true);
  assert.equal(status.esp_seen_id, 0);
  assert.equal(status.esp_last_poll_ms_ago, -1);
  assert.equal(await (await fetch(`${base}/api/answer?id=${id}`)).text(), answer);
});

test('mDNS host candidates keep the a=candidate prefix', () => {
  const sdp = 'a=candidate:1 1 udp 100 abc-123.local 5000 typ host generation 0\n';
  const out = expandLocalCandidates(sdp, '192.168.137.1');
  assert.equal(out, 'a=candidate:1 1 udp 100 192.168.137.1 5000 typ host generation 0\n');
});

test('loopback viewer never corrupts candidate lines', () => {
  const sdp = 'a=candidate:1 1 udp 100 abc-123.local 5000 typ host generation 0\n';
  const out = expandLocalCandidates(sdp, '127.0.0.1', '192.168.137.110');
  assert.ok(out.startsWith('a=candidate:1 1 udp 100 '));
  assert.ok(out.includes(' 5000 typ host generation 0'));
});

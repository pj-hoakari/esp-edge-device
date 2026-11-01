import { test } from 'node:test';
import assert from 'node:assert/strict';

test('decode keeps the latest pending frame and ACKs only drawn frames', async t => {
  const drawn = [], acks = [], decodes = [];
  const elements = {
    '#status': {}, '#connect': {}, '#disconnect': {},
    '#video': { width: 1280, height: 720, getContext: () => ({
      drawImage: bitmap => drawn.push(bitmap.id),
    }) },
  };
  let connection;
  class Peer {
    constructor() { connection = this; this.iceGatheringState = 'complete'; }
    createDataChannel() {}
    async createOffer() { return { type: 'offer', sdp: 'offer' }; }
    async setLocalDescription(sdp) { this.localDescription = sdp; }
    async setRemoteDescription() {}
    close() {}
  }
  t.mock.timers.enable({ apis: ['setTimeout'] });
  t.mock.method(globalThis, 'fetch', async url => ({
    ok: true, status: 200,
    json: async () => ({ iceServers: [] }),
    text: async () => url === '/api/offer' ? '1' : 'answer',
  }));
  const originals = {};
  for (const key of ['document', 'RTCPeerConnection', 'createImageBitmap']) originals[key] = globalThis[key];
  globalThis.document = { querySelector: key => elements[key] };
  globalThis.RTCPeerConnection = Peer;
  globalThis.createImageBitmap = () => new Promise(resolve => decodes.push(resolve));
  t.after(() => {
    for (const [key, value] of Object.entries(originals)) {
      if (value === undefined) delete globalThis[key]; else globalThis[key] = value;
    }
  });
  await import(`../public/receiver.js?render-test`);
  await elements['#connect'].onclick();
  const ch = { label: 'jpeg', readyState: 'open', send: ack => acks.push(ack), close() {} };
  connection.ondatachannel({ channel: ch }); ch.onopen();
  function send(id) {
    const data = new ArrayBuffer(24), v = new DataView(data);
    v.setUint32(0, 0x3147504a, true); v.setUint32(4, id, true); v.setUint32(8, 4, true);
    new Uint8Array(data, 20).set([255,216,255,217]);
    ch.onmessage({ data });
  }
  const flush = () => new Promise(resolve => setImmediate(resolve));
  const bitmap = id => ({ id, width: 1280, height: 720, close() {} });
  send(1); send(2); send(3);
  assert.equal(decodes.length, 1);
  assert.deepEqual(acks, []);
  decodes.shift()(bitmap(1)); await flush();
  assert.deepEqual(drawn, [1]); assert.deepEqual(acks, ['ACK:1']);
  assert.equal(decodes.length, 1);
  decodes.shift()(bitmap(3)); await flush();
  assert.deepEqual(drawn, [1,3]); assert.deepEqual(acks, ['ACK:1','ACK:3']);
  send(4); send(5);
  elements['#disconnect'].onclick();
  decodes.shift()(bitmap(4)); await flush();
  assert.deepEqual(drawn, [1,3]);
  assert.equal(decodes.length, 0);
});

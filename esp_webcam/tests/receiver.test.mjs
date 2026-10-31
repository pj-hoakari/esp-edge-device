import { test } from 'node:test';
import assert from 'node:assert/strict';
import { JpegAssembler, CHUNK, MAX_FRAME, motionScore } from '../main/web/receiver.js';
function packet(id, jpeg, offset) {
  const n = Math.min(CHUNK, jpeg.length - offset), b = new ArrayBuffer(20 + n), v = new DataView(b);
  v.setUint32(0, 0x3147504a, true); v.setUint32(4, id, true);
  v.setUint32(8, jpeg.length, true); v.setUint32(12, offset, true);
  new Uint8Array(b, 20).set(jpeg.subarray(offset, offset + n)); return b;
}
function jpeg(n) { const b = new Uint8Array(n).fill(37); b.set([255,216]); b.set([255,217], n - 2); return b; }
test('HD-size JPEG: unordered, duplicates, exact byte reconstruction', () => {
  const a = new JpegAssembler(), b = jpeg(150001);
  const offsets = Array.from({length: Math.ceil(b.length / CHUNK)}, (_, i) => i * CHUNK).reverse();
  assert.equal(a.push(packet(1,b,offsets[0]),0), null);
  let result;
  for (const o of offsets) result = a.push(packet(1,b,o),10) || result;
  assert.deepEqual(result.bytes, b);
  assert.equal(a.push(packet(1,b,0),20), null);
});
test('loss, expiry and a newer frame do not mix data', () => {
  const a = new JpegAssembler(), b = jpeg(CHUNK + 10);
  a.push(packet(7,b,0),0);
  assert.equal(a.push(packet(7,b,CHUNK),1100),null);
  a.push(packet(8,b,CHUNK),1101);
  assert.equal(a.push(packet(7,b,0),1102),null);
  assert.deepEqual(a.push(packet(8,b,0),1103).bytes,b);
});
test('reject malformed sizes, alignment, magic and broken JPEG', () => {
  const a = new JpegAssembler(), b = jpeg(32);
  for (const [pos,value] of [[0,0],[8,MAX_FRAME+1],[12,1]]) {
    const p = packet(1,b,0); new DataView(p).setUint32(pos,value,true); assert.equal(a.push(p,0),null);
  }
  b[0]=0; assert.equal(a.push(packet(2,b,0),0),null);
  assert.equal(a.push(new ArrayBuffer(20),0),null);
});
test('frame ID wraps at uint32 boundary', () => {
  const a = new JpegAssembler(), b = jpeg(32);
  assert.ok(a.push(packet(0xffffffff,b,0),0)); assert.ok(a.push(packet(0,b,0),1));
  assert.equal(a.push(packet(0xffffffff,b,0),2),null);
});
test('client motion score detects change and rejects unknown frames', () => {
  const a = new Uint8ClampedArray(4 * 4 * 4);
  assert.equal(motionScore(null, a, 4, 4), 255);
  assert.equal(motionScore(a, a.slice(), 4, 4), 0);
  const b = a.slice(); b[0] = 30;
  assert.equal(motionScore(a, b, 4, 4), 1);
  assert.equal(motionScore(a, new Uint8ClampedArray(3), 4, 4), 255);
  const white = new Uint8ClampedArray(4 * 4 * 4).fill(255);
  assert.equal(motionScore(a, white, 4, 4), 255);
});

export const MAX_FRAME = 256 * 1024;
export const CHUNK = 12000;

// One bounded frame in memory. Unordered delivery and duplicate chunks are supported.
export class JpegAssembler {
  constructor() { this.frame = null; this.last = null; }
  push(buffer, now = performance.now()) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength <= 20 || buffer.byteLength > CHUNK + 20) return null;
    const v = new DataView(buffer);
    if (v.getUint32(0, true) !== 0x3147504a) return null; // JPG1
    const id = v.getUint32(4, true), total = v.getUint32(8, true), offset = v.getUint32(12, true);
    const n = buffer.byteLength - 20;
    if (total < 4 || total > MAX_FRAME || offset >= total || offset % CHUNK !== 0 ||
        n !== Math.min(CHUNK, total - offset)) return null;
    if (this.last !== null && ((id - this.last) | 0) <= 0) return null;
    if (this.frame && now - this.frame.start > 1000) this.frame = null;
    if (this.frame && this.frame.id !== id && ((id - this.frame.id) | 0) <= 0) return null;
    if (!this.frame || this.frame.id !== id) {
      this.frame = { id, total, start: now, bytes: new Uint8Array(total),
        seen: new Set(), count: Math.ceil(total / CHUNK) };
    }
    const f = this.frame;
    if (f.total !== total) return null;
    if (!f.seen.has(offset)) {
      f.bytes.set(new Uint8Array(buffer, 20), offset); f.seen.add(offset);
    }
    if (f.seen.size !== f.count) return null;
    this.frame = null; this.last = id;
    const b = f.bytes;
    if (b[0] !== 255 || b[1] !== 216 || b[total - 2] !== 255 || b[total - 1] !== 217) return null;
    return { id, bytes: b, assemblyMs: now - f.start };
  }
}

// Mean absolute RGB difference on a small preview. The ESP uses this to
// decide whether the wire can drop to the idle FPS. 255 means "unknown".
export function motionScore(prev, curr, width, height) {
  if (!prev || !curr || prev.length !== curr.length || !width || !height || curr.length % 4) return 255;
  let sum = 0;
  for (let i = 0; i < curr.length; i += 4) {
    sum += Math.abs(curr[i] - prev[i]) + Math.abs(curr[i + 1] - prev[i + 1]) + Math.abs(curr[i + 2] - prev[i + 2]);
  }
  return Math.min(255, Math.round(sum / (width * height * 3)));
}

if (typeof document !== 'undefined') {
  const status = document.querySelector('#status');
  const canvas = document.querySelector('#video'), ctx = canvas.getContext('2d');
  let pc = null, dc = null, generation = 0, abort = null;
  function disconnect() {
    generation++; abort?.abort(); abort = null;
    dc?.close(); pc?.close(); dc = null; pc = null;
    status.textContent = '切断しました';
    document.querySelector('#connect').disabled = false;
  }
  document.querySelector('#disconnect').onclick = disconnect;
  document.querySelector('#connect').onclick = async () => {
    disconnect(); const run = generation;
    document.querySelector('#connect').disabled = true;
    abort = new AbortController(); const signal = abort.signal;
    const connection = new RTCPeerConnection({ iceServers: [] }); pc = connection;
    let channel = null;
    connection.createDataChannel('control');

    const assembler = new JpegAssembler();
    let shown = 0, bytes = 0, start = performance.now(), decoding = false;
    const motionCanvas = document.createElement('canvas');
    motionCanvas.width = 64; motionCanvas.height = 36;
    const motionCtx = motionCanvas.getContext('2d', { willReadFrequently: true });
    let prevMotion = null;
    function setupChannel(incoming) {
    channel = incoming; dc = incoming; channel.binaryType = 'arraybuffer';
    channel.onopen = () => {
      prevMotion = null;
      if (run === generation) { start = performance.now(); status.textContent = '接続済み・JPEG待機中'; }
    };
    channel.onmessage = async event => {
      if (run !== generation) return;
      const frame = assembler.push(event.data);
      if (!frame) return;
      // A dropped frame is still acknowledged so the ESP can measure liveness;
      // without a motion score the ESP keeps its previous adaptive rate.
      if (decoding) { channel.send(`ACK:${frame.id}`); return; }
      decoding = true;
      let frameMotion = -1;
      try {
        const bitmap = await createImageBitmap(new Blob([frame.bytes], { type: 'image/jpeg' }));
        try {
          if (run !== generation) return;
          motionCtx.drawImage(bitmap, 0, 0, motionCanvas.width, motionCanvas.height);
          const pixels = motionCtx.getImageData(0, 0, motionCanvas.width, motionCanvas.height).data;
          frameMotion = motionScore(prevMotion, pixels, motionCanvas.width, motionCanvas.height);
          prevMotion = pixels;
          ctx.drawImage(bitmap, 0, 0, canvas.width, canvas.height); shown++; bytes += frame.bytes.length;
          const seconds = (performance.now() - start) / 1000;
          status.textContent = `${bitmap.width}×${bitmap.height} カラーJPEG | 平均 ${(shown / seconds).toFixed(2)} fps | ${(bytes * 8 / seconds / 1e6).toFixed(2)} Mbit/s\n` +
            `${Math.round(frame.bytes.length / 1024)} KiB/枚 | 分割受信 ${Math.round(frame.assemblyMs)} ms | 動き ${frameMotion}/255 | ${shown}枚表示`;
        } finally { bitmap.close(); }
      } catch (e) { if (run === generation) status.textContent = `JPEG表示失敗: ${e.message}`; }
      finally {
        decoding = false;
        if (channel.readyState === 'open') {
          channel.send(frameMotion >= 0 ? `ACK:${frame.id}:${frameMotion}` : `ACK:${frame.id}`);
        }
      }
    };
    }
    connection.ondatachannel = event => { if(event.channel.label === 'jpeg') setupChannel(event.channel); };
    connection.oniceconnectionstatechange = () => {
      if(run === generation && channel?.readyState !== 'open') status.textContent = 'ICE: ' + connection.iceConnectionState + ' / DTLS: ' + connection.connectionState;
    };
    connection.onconnectionstatechange = () => {
      if (run === generation && ['failed', 'disconnected', 'closed'].includes(connection.connectionState)) {
        disconnect(); status.textContent = '接続が終了しました。接続ボタンで再接続できます。';
      }
    };
    try {
      status.textContent = 'ICE候補を収集中…';
      await connection.setLocalDescription(await connection.createOffer());
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => finish(new Error('ICE収集タイムアウト')), 10000);
        const finish = error => { clearTimeout(timer); connection.onicegatheringstatechange = null; error ? reject(error) : resolve(); };
        connection.onicegatheringstatechange = () => { if (connection.iceGatheringState === 'complete') finish(); };
        if (connection.iceGatheringState === 'complete') finish();
      });
      if (run !== generation) return;
      let res = await fetch('/offer', { method: 'POST', body: connection.localDescription.sdp, signal });
      if (!res.ok) throw new Error(`Offer HTTP ${res.status}`);
      const id = (await res.text()).trim();
      status.textContent = 'ESP32の応答を待っています…';
      const until = performance.now() + 20000;
      let sdp = null;
      while (performance.now() < until) {
        res = await fetch(`/answer?id=${encodeURIComponent(id)}`, { cache: 'no-store', signal });
        if (res.status === 200) { sdp = await res.text(); break; }
        if (res.status !== 202) throw new Error(`Answer HTTP ${res.status}`);
        await new Promise(resolve => setTimeout(resolve, 250));
      }
      if (!sdp) throw new Error('SDP応答タイムアウト');
      if (run !== generation) return;
      await connection.setRemoteDescription({ type: 'answer', sdp });
      setTimeout(() => {
        if (run === generation && channel?.readyState !== 'open') {
          const phase = 'ICE=' + connection.iceConnectionState + ', DTLS=' + connection.connectionState;
          disconnect(); status.textContent = '接続タイムアウト: ' + phase;
        }
      }, 15000);
    } catch (e) {
      if (run === generation) { disconnect(); status.textContent = `接続失敗: ${e.message}`; }
    }
  };
}

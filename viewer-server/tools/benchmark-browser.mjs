import {spawn} from 'node:child_process';
import {mkdir,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const base=process.env.ESP_URL||'http://192.168.137.145';
const duration=Number(process.env.SECONDS||60);
if(!Number.isFinite(duration)||duration<=0)throw Error('SECONDS must be positive');
const debugPort=Number(process.env.CDP_PORT||9227);
const root=fileURLToPath(new URL('../../', import.meta.url));
const profile=path.join(root,'esp_webcam/.tools/browser-profile-perf');
await mkdir(profile,{recursive:true});
const browser=spawn('C:/Program Files/Google/Chrome/Application/chrome.exe',[
  '--headless=new',`--remote-debugging-port=${debugPort}`,`--user-data-dir=${profile}`,
  '--no-first-run','--no-default-browser-check','--disable-background-timer-throttling',
  '--disable-renderer-backgrounding','http://127.0.0.1:8080/'
],{windowsHide:true,stdio:'ignore'});
let ws,sequence=0;
const pending=new Map();
async function request(route){const r=await fetch(base+route,{signal:AbortSignal.timeout(4000)});if(!r.ok)throw Error(`${route}: ${r.status}`);return r.json();}
async function perf(){try{return await request('/perf');}catch{return null;}}
function cdp(method,params={}){
  const id=++sequence;
  return new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{pending.delete(id);reject(Error('CDP timeout '+method));},10000);
    pending.set(id,{resolve:r=>{clearTimeout(timer);resolve(r);},reject});
    ws.send(JSON.stringify({id,method,params}));
  });
}
async function evaluate(expression){const r=await cdp('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.text);return r.result.value;}
try{
  let target;
  for(let i=0;i<100;i++){
    try{const list=await (await fetch(`http://127.0.0.1:${debugPort}/json/list`)).json();target=list.find(t=>t.type==='page'&&t.url.includes('127.0.0.1:8080'));if(target)break;}catch{}
    await sleep(100);
  }
  if(!target)throw Error('Test browser did not start');
  ws=new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
  ws.onmessage=e=>{const r=JSON.parse(e.data);if(r.id&&pending.has(r.id)){const p=pending.get(r.id);pending.delete(r.id);r.error?p.reject(Error(r.error.message)):p.resolve(r.result);}};
  await cdp('Runtime.enable');
  // DOM elements can exist before the module attaches the connect handler.
  for(let i=0;i<100;i++){if(await evaluate(`document.readyState==='complete'&&!!document.querySelector('#connect')`))break;await sleep(100);}
  await evaluate(`(() => {
    window.__bench={draws:0,drawUs:0,lastDraw:0,frameTimes:[]};
    const draw=CanvasRenderingContext2D.prototype.drawImage;
    CanvasRenderingContext2D.prototype.drawImage=function(...args){
      const start=performance.now();const value=draw.apply(this,args);
      if(this.canvas.id==='video'){window.__bench.draws++;window.__bench.drawUs+=(performance.now()-start)*1000;window.__bench.lastDraw=performance.now();window.__bench.frameTimes.push(window.__bench.lastDraw);}
      return value;
    };
    document.querySelector('#connect').click();
  })()`);
  let ready=false;
  for(let i=0;i<120;i++){
    if(await evaluate('window.__bench.draws > 2')){ready=true;break;}
    await sleep(500);
  }
  if(!ready)throw Error('No canvas frames: '+await evaluate(`document.querySelector('#status').textContent`));
  const snapshot=async()=>({browser:await evaluate('(({draws,drawUs,lastDraw})=>({draws,drawUs,lastDraw,now:performance.now()}))(window.__bench)'),rtc:await request('/rtc-status'),perf:await perf()});
  const initial=await snapshot(),cameraStart=await request('/status');
  const start=performance.now();let previous=initial,prevTime=start;
  const windows=[];
  for(let i=0;i<Math.ceil(duration/5);i++){
    await sleep(Math.max(0,start+(i+1)*5000-performance.now()));
    const current=await snapshot(),now=performance.now(),dt=(now-prevTime)/1000;
    if(current.rtc.sent_frames<previous.rtc.sent_frames||current.browser.draws<previous.browser.draws)throw Error('Measurement session was replaced');
    const canvasSeconds=(current.browser.now-previous.browser.now)/1000;
    const window={seconds:dt,canvas_seconds:canvasSeconds,canvas_fps:(current.browser.draws-previous.browser.draws)/canvasSeconds,
      ack_fps:(current.rtc.acked_frames-previous.rtc.acked_frames)/dt,
      send_fps:(current.rtc.sent_frames-previous.rtc.sent_frames)/dt,
      jpeg_bytes_per_frame:(current.rtc.sent_bytes-previous.rtc.sent_bytes)/Math.max(1,current.rtc.sent_frames-previous.rtc.sent_frames),...current};
    windows.push(window);
    console.log(JSON.stringify(window));previous=current;prevTime=now;
  }
  const elapsed=(previous.browser.now-initial.browser.now)/1000;
  const frameTimes=(await evaluate('window.__bench.frameTimes')).filter(t=>t>=initial.browser.now&&t<=previous.browser.now);
  let maxGap=0,minRolling=Infinity,right=0;
  for(let left=0;left<frameTimes.length;left++){
    if(left)maxGap=Math.max(maxGap,frameTimes[left]-frameTimes[left-1]);
    if(frameTimes[left]+5000>previous.browser.now)break;
    while(right<frameTimes.length&&frameTimes[right]<=frameTimes[left]+5000)right++;
    minRolling=Math.min(minRolling,(right-left-1)/5);
  }
  // Include the final gaps as well, even after the last complete rolling window.
  for(let i=1;i<frameTimes.length;i++)maxGap=Math.max(maxGap,frameTimes[i]-frameTimes[i-1]);
  const result={client:'Chrome headless real JPEG decode + canvas draw',seconds:elapsed,
    canvas_frames:previous.browser.draws-initial.browser.draws,
    canvas_fps:(previous.browser.draws-initial.browser.draws)/elapsed,
    minimum_window_fps:Math.min(...windows.map(w=>w.canvas_fps)),initial,final:previous,
    minimum_rolling_5s_fps:Number.isFinite(minRolling)?minRolling:null,max_frame_gap_ms:maxGap,
    cameraStart,cameraEnd:await request('/status'),windows};
  if(process.env.RESULT_FILE)await writeFile(process.env.RESULT_FILE,JSON.stringify(result,null,2));
  console.log('RESULT',JSON.stringify({seconds:elapsed,canvas_fps:result.canvas_fps,minimum_window_fps:result.minimum_window_fps,frames:result.canvas_frames}));
  await evaluate(`document.querySelector('#disconnect').click()`);
}finally{
  if(ws?.readyState===WebSocket.OPEN){try{await cdp('Browser.close');}catch{}}
  ws?.close();browser.kill();
}

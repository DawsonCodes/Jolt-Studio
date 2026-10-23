import { Input, BlobSource, ALL_FORMATS, CanvasSink } from 'mediabunny';
import { validateResult } from './shared.js';

let input, track, metadata, selectedFile;
let enginePromise;
const status = (id, message, extra = {}) => postMessage({ id, type: 'progress', message, ...extra });
const engine = () => enginePromise ??= import('./jolt-engine.mjs').then(m => m.default());
function readJSON(e, ptr) {
  const value = JSON.parse(e.UTF8ToString(ptr));
  if (value?.error) throw new Error(value.error);
  return value;
}
function withString(e, text, fn) {
  const p = e.stringToNewUTF8(text);
  try { return fn(p); } finally { e._free(p); }
}
async function open(file) {
  if (!('VideoDecoder' in self)) throw new Error('This browser needs WebCodecs video support in a secure context (HTTPS). Open Jolt Studio in a current Chrome or Edge browser.');
  if (file === selectedFile && metadata) return metadata;
  input?.dispose();
  input = new Input({ formats: ALL_FORMATS, source: new BlobSource(file) });
  track = await input.getPrimaryVideoTrack();
  if (!track) throw new Error('No video track was found in this file.');
  if (!await track.canDecode()) throw new Error('This browser cannot decode this video codec. Try Chrome or Edge, or use an H.264 MP4 / VP9 WebM copy.');
  const [width, height, end, origin, stats, codec] = await Promise.all([
    track.getDisplayWidth(), track.getDisplayHeight(), track.computeDuration(), track.getFirstTimestamp(), track.computePacketStats(600), track.getCodec()
  ]);
  if (width * height > 33_200_000 || width < 16 || height < 16) throw new Error('This video resolution is not supported. Use a video up to 8K.');
  metadata = { width, height, duration: end - origin, origin, fps: stats.averagePacketRate || 30, codec };
  selectedFile = file;
  return metadata;
}
async function locate(id, start, end, known = []) {
  status(id, 'Finding the hunger bar…');
  const e = await engine();
  e._js_clear();
  const ratio = Math.min(1, 1280 / metadata.width);
  const sink = new CanvasSink(track, { width: Math.round(metadata.width * ratio), poolSize: 1 });
  let count = 0;
  for (let n = 0; n < 6; n++) {
    const sample = await sink.getCanvas(metadata.origin + start + (end - start) * (.05 + .9 * n / 5));
    if (!sample) continue;
    const canvas = sample.canvas, ctx = canvas.getContext('2d', { willReadFrequently: true });
    // Mask earlier screens when searching for another player's HUD.
    for (const b of known) {
      ctx.fillStyle = '#777';
      ctx.fillRect((b.x - 104 * b.scale) * ratio, (b.y - 4 * b.scale) * ratio, 190 * b.scale * ratio, 18 * b.scale * ratio);
    }
    const rgba = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
    const ptr = e._malloc(rgba.length);
    if (!ptr) throw new Error('Not enough browser memory to locate the HUD.');
    try { e.HEAPU8.set(rgba, ptr); if (!e._js_sample(canvas.width, canvas.height, ptr)) throw new Error('Unable to sample this video resolution.'); }
    finally { e._free(ptr); }
    count++;
  }
  if (count < 3) throw new Error('Too few video frames could be read to locate the hunger bar.');
  const bar = readJSON(e, e._js_locate());
  return bar && { x: bar.x / ratio, y: bar.y / ratio, scale: bar.scale / ratio };
}
async function scan(id, options) {
  const { start, end, maxHours, bars: configured, multi } = options;
  if (!metadata) throw new Error('Select a video first.');
  if (!(start >= 0 && end > start && end <= metadata.duration + .1)) throw new Error('The scan range must fall within the video.');
  const e = await engine();
  let bars = configured;
  if (!bars.length) {
    const primary = await locate(id, start, end);
    if (!primary) throw new Error('No vanilla hunger bar was found. Choose a gameplay range, or set the hunger bar position manually.');
    bars = [primary];
    if (multi) while (bars.length < 4) {
      status(id, `Checking for another player’s HUD (${bars.length} found)…`);
      const next = await locate(id, start, end, bars);
      if (!next) break;
      bars.push(next);
    }
  }
  e._js_begin(metadata.width, metadata.height, metadata.fps);
  for (const b of bars) readJSON(e, e._js_add_bar(b.x, b.y, b.scale));
  const rects = readJSON(e, e._js_rects());
  const bytes = rects.reduce((n, r) => n + r.width * r.height * 4, 0);
  const ptr = e._malloc(bytes);
  if (!ptr) throw new Error('Not enough browser memory for the selected HUD region.');
  const sink = new CanvasSink(track, { poolSize: 2, decoderOptions: { hardwareAcceleration: 'prefer-software' } });
  let frames = 0, previous = -Infinity, lastUpdate = 0;
  const started = performance.now();
  try {
    for await (const frame of sink.canvases(metadata.origin + start, metadata.origin + end)) {
      const time = frame.timestamp - metadata.origin;
      if (time < start - 1e-6 || time >= end) continue;
      if (time <= previous) throw new Error('The video has duplicate or out-of-order timestamps. Use a copy with corrected timestamps.');
      previous = time;
      const ctx = frame.canvas.getContext('2d', { willReadFrequently: true });
      let offset = ptr;
      for (const rect of rects) {
        const data = ctx.getImageData(rect.x, rect.y, rect.width, rect.height).data;
        e.HEAPU8.set(data, offset); offset += data.length;
      }
      if (!e._js_frame(time, ptr)) readJSON(e, e._js_last());
      frames++;
      if (performance.now() - lastUpdate > 200) {
        lastUpdate = performance.now();
        status(id, 'Reading hunger and heart jolts…', { percent: Math.min(90, 90 * (time - start) / (end - start)), frames, time, elapsed: (performance.now() - started) / 1000, bars });
      }
    }
  } finally { e._free(ptr); }
  status(id, 'Matching jolts to game ticks…', { percent: 94, frames, bars });
  const result = withString(e, selectedFile.name, p => readJSON(e, e._js_analyze(maxHours, p)));
  validateResult(result);
  status(id, 'Capturing the clock-event frames…', { percent: 98, frames, bars });
  const witnesses = new CanvasSink(track, { width: Math.min(640, metadata.width), poolSize: 1 });
  for (const event of result.breaks) {
    if (event.kind !== 'removed') continue;
    event.frames = [];
    for (const time of [event.from, event.to]) {
      const frame = await witnesses.getCanvas(time + metadata.origin);
      if (!frame) break;
      const blob = await frame.canvas.convertToBlob({ type: 'image/jpeg', quality: .78 });
      const bytes = new Uint8Array(await blob.arrayBuffer());
      let binary = '';
      for (let i = 0; i < bytes.length; i += 16384) binary += String.fromCharCode(...bytes.subarray(i, i + 16384));
      event.frames.push(`data:image/jpeg;base64,${btoa(binary)}`);
    }
    if (event.frames.length !== 2) delete event.frames;
  }
  result.studio = { ...result.studio, engine: 'Splosh Jolt 2.5', browserPort: 'Jolt Studio 0.1.0', elapsed: (performance.now() - started) / 1000, codec: metadata.codec, width: metadata.width, height: metadata.height };
  return result;
}
onmessage = async ({ data: { id, command, args } }) => {
  try {
    let value;
    if (command === 'open') value = await open(args.file);
    else if (command === 'locate') { if (!metadata) throw new Error('Select a video first.'); value = await locate(id, args.start, args.end); }
    else if (command === 'scan') value = await scan(id, args);
    else if (command === 'manual') {
      status(id, 'Matching your jolt codes…');
      const e = await engine();
      value = withString(e, args.codes, p => readJSON(e, e._js_manual(args.row === 'hearts' ? 1 : 0, args.maxHours, p)));
    } else if (command === 'html') {
      const e = await engine();
      value = withString(e, JSON.stringify(validateResult(args.result)).replace(/</g, '\\u003c'), p => e.UTF8ToString(e._js_html(p)));
      if (!value.startsWith('<')) throw new Error('The report could not be rendered.');
    } else throw new Error('Unknown scanner command.');
    postMessage({ id, type: 'result', value });
  } catch (error) { postMessage({ id, type: 'error', message: error?.message || String(error) }); }
};

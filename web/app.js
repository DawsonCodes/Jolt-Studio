import { scanRange, videoBounds, parseBars, clock, overview, validateResult } from './shared.js';

const $ = id => document.getElementById(id);
let worker, nextID = 0, pending = new Map(), file, meta, result, busy = false, picking = false, url;
let activeTab = 'scan', selectedBars = [], tableFilter = 'all', busyTask = '', importedReport = false;
const emptyManual = 'Your matching ticks will appear here.';
function notice(message, error = false) {
  $('notice').textContent = message; $('notice').classList.toggle('error', error); $('notice').hidden = !message;
  $('notice').setAttribute('role', error ? 'alert' : 'status');
}
function createWorker() {
  worker = new Worker(new URL('./scanner-worker.js', import.meta.url), { type: 'module' });
  worker.onmessage = ({ data }) => {
    const item = pending.get(data.id);
    if (!item) return;
    if (data.type === 'progress') { item.progress?.(data); return; }
    pending.delete(data.id);
    if (data.type === 'error') item.reject(new Error(data.message)); else item.resolve(data.value);
  };
  worker.onerror = error => {
    error.preventDefault();
    for (const item of pending.values()) item.reject(new Error('The scanner worker stopped. Your browser may have run out of memory. Try a shorter range or lower uptime.'));
    pending.clear();
    worker.terminate(); worker = undefined;
  };
}
function request(command, args, progress) {
  if (!worker) createWorker();
  return new Promise((resolve, reject) => {
    const id = ++nextID; pending.set(id, { resolve, reject, progress }); worker.postMessage({ id, command, args });
  });
}
function hasSession() {
  return !!(file || result || $('manual-output').textContent !== emptyManual || [...document.querySelectorAll('main input,main textarea')].some(el => el.type === 'checkbox' ? el.checked !== el.defaultChecked : el.value !== el.defaultValue) || [...document.querySelectorAll('main select')].some(el => el.selectedIndex !== 0));
}
function setBusy(value, task = '') {
  busy = value; busyTask = value ? task : '';
  if (value) setPicking(false);
  for (const id of ['browse','replace','demo','scan','crack','code-example','sample-report','open-report','clear','pick-bar','export-html','export-json']) $(id).disabled = value || (id === 'scan' && !meta) || (id === 'clear' && !hasSession()) || (id === 'pick-bar' && !meta);
  for (const el of document.querySelectorAll('.settings input,.settings select,.settings textarea,#manual-row,#manual-hours,#codes')) el.disabled = value;
  $('manual-cancel').hidden = !value || task !== 'manual';
  $('scan').firstChild.textContent = value && task === 'scan' ? 'Analyzing… ' : 'Start analysis ';
}
async function action(fn, task = '') {
  if (busy) return;
  notice(''); setBusy(true, task);
  try { await fn(); } catch (error) {
    if (error.message !== 'Cancelled') {
      notice(error.message, true);
      if (task === 'scan' && !$('progress-panel').hidden) {
        $('scan-status').textContent = 'Analysis stopped'; $('percent').textContent = 'Stopped';
        $('progress').value = 0; $('progress-detail').textContent = error.message;
        $('progress-panel').dataset.state = 'error';
      }
      if (task === 'manual') $('manual-output').textContent = 'No result. Check the message above and try again.';
    }
  }
  finally { setBusy(false); }
}
function switchTab(tab) {
  activeTab = tab;
  document.querySelectorAll('.tab-panel').forEach(p => p.hidden = p.id !== `${tab}-tab`);
  document.querySelectorAll('.nav').forEach(b => {
    const active = b.dataset.tab === tab;
    b.classList.toggle('active', active);
    if (active) b.setAttribute('aria-current', 'page'); else b.removeAttribute('aria-current');
  });
  const names = { scan: ['Follow the ticks.','Find the story between the frames.','VIDEO SCANNER'], manual: ['Read a jolt. Recover a tick.','Match patterns you’ve read by hand.','MANUAL CRACKER'], report: ['Back to the evidence.','Open and explore a saved investigation.','OPEN REPORT'], guide: ['The clock in the HUD.','A little randomness leaves a useful trail.','HOW IT WORKS'] };
  const [title, subtitle, crumb] = names[tab];
  $('page-title').textContent = title; $('page-subtitle').textContent = subtitle;
  $('breadcrumb').textContent = `WORKSPACE / ${crumb}`;
  $('results').hidden = !result || !['scan','report'].includes(tab);
}
document.querySelectorAll('[data-tab]').forEach(b => b.onclick = () => { setPicking(false); notice(''); switchTab(b.dataset.tab); });
function progress(data) {
  $('scan-status').textContent = data.message;
  if (data.percent != null) { $('progress').value = data.percent; $('percent').textContent = `${Math.floor(data.percent)}%`; }
  else { $('progress').removeAttribute('value'); $('percent').textContent = '…'; }
  if (data.frames != null) $('progress-detail').textContent = `${data.frames.toLocaleString()} frames${data.time != null ? ` · ${clock(data.time)}` : ''}${data.elapsed != null ? ` · ${Math.round(data.elapsed)}s elapsed` : ''}`;
  if (data.bars) { selectedBars = data.bars; drawOverlay(); }
}
function cancel() {
  const manual = busyTask === 'manual';
  worker?.terminate(); worker = undefined;
  for (const item of pending.values()) item.reject(new Error('Cancelled'));
  pending.clear(); $('progress-panel').hidden = true;
  if (manual) $('manual-output').textContent = 'Pattern matching cancelled. Your codes are still available.';
  notice(manual ? 'Pattern matching cancelled.' : 'Scan cancelled. The selected video is still available.');
  // A fresh worker will need to reopen the local File on the next operation.
}
$('cancel').onclick = cancel; $('manual-cancel').onclick = cancel;
async function ensureOpen() { if (!file) throw new Error('Choose a video first.'); meta = await request('open', { file }); }
async function loadFile(next) {
  if (!next.size) throw new Error('This file is empty. Choose a video with content.');
  worker?.terminate(); worker = undefined;
  pending.clear();
  // Commit the new file only after decoding succeeds, so a bad replacement cannot destroy the current session.
  const nextMeta = await request('open', { file: next });
  const preserveReport = importedReport && !file && !!result;
  file = next; meta = nextMeta; selectedBars = []; setPicking(false);
  if (!preserveReport) { result = undefined; importedReport = false; $('results').hidden = true; }
  resetControls('.settings');
  $('manual-bar').hidden = true;
  if (url) URL.revokeObjectURL(url);
  url = URL.createObjectURL(file); $('video').src = url;
  $('file-name').textContent = file.name;
  $('preview-panel').hidden = false; $('dropzone').hidden = true;
  $('progress-panel').hidden = true;
  $('file-meta').textContent = `${meta.width} × ${meta.height} · ${meta.fps.toFixed(2)} fps · ${clock(meta.duration)} · ${(file.size / 1048576).toFixed(1)} MB · ${String(meta.codec).toUpperCase()}`;
  $('start').value = '0:00'; $('end').value = clock(meta.duration, 3);
  $('video').currentTime = Math.min(1, meta.duration / 4);
  drawOverlay();
  if (preserveReport) notice('Source video attached. Check that this is the footage used to create the imported report.');
}
$('browse').onclick = () => $('video-file').click(); $('replace').onclick = () => $('video-file').click();
$('video-file').onchange = () => { const f = $('video-file').files[0]; if (f) action(() => loadFile(f)); $('video-file').value = ''; };
for (const name of ['dragenter','dragover']) document.addEventListener(name, event => {
  if (!event.dataTransfer?.types.includes('Files')) return;
  event.preventDefault(); if (!busy) document.body.classList.add('file-drag');
});
document.addEventListener('dragleave', event => { if (!event.relatedTarget) document.body.classList.remove('file-drag'); });
document.addEventListener('drop', event => {
  if (!event.dataTransfer?.files.length) return;
  event.preventDefault(); document.body.classList.remove('file-drag');
  if (busy) return;
  const f = event.dataTransfer.files[0];
  if (/\.json$/i.test(f.name)) { switchTab('report'); action(() => openReportFile(f)); }
  else { switchTab('scan'); action(() => loadFile(f)); }
});
$('demo').onclick = () => action(async () => {
  const response = await fetch(new URL('./demo.webm', import.meta.url));
  if (!response.ok) throw new Error('The sample clip is unavailable. Choose a local gameplay video.');
  await loadFile(new File([await response.blob()], 'Jolt Studio — synthetic known-cut sample.webm', { type: 'video/webm' }));
  $('bar-mode').value = 'manual'; $('bar-mode').dispatchEvent(new Event('change'));
  $('bar-x').value = 330; $('bar-y').value = 290; $('bar-scale').value = 2; $('max-hours').value = 1;
  notice('Synthetic test clip: the game clock jumps ahead by 15.25 seconds at video time 0:15. This is a detector test, not real gameplay.');
});
$('bar-mode').onchange = () => { setPicking(false); selectedBars = []; $('manual-bar').hidden = $('bar-mode').value !== 'manual'; drawOverlay(); };
for (const id of ['bar-x','bar-y','bar-scale']) $(id).oninput = drawOverlay;
function configuredBars() {
  const extras = parseBars($('extra-bars').value);
  if ($('bar-mode').value === 'manual') {
    const own = parseBars(`${$('bar-x').value},${$('bar-y').value},${$('bar-scale').value}`);
    return [...own, ...extras];
  }
  return [];
}
function drawOverlay() {
  const canvas = $('overlay'), video = $('video');
  if (!meta) { canvas.width = 0; canvas.height = 0; return; }
  canvas.width = meta.width; canvas.height = meta.height;
  const bounds = videoBounds(meta.width, meta.height, video.clientWidth, video.clientHeight);
  canvas.style.width = `${bounds.width}px`; canvas.style.height = `${bounds.height}px`;
  canvas.style.left = `${video.offsetLeft + bounds.x}px`; canvas.style.top = `${video.offsetTop + bounds.y}px`;
  const ctx = canvas.getContext('2d'); ctx.clearRect(0, 0, canvas.width, canvas.height);
  let bars = selectedBars;
  try { if ($('bar-mode').value === 'manual') bars = configuredBars(); } catch {}
  ctx.strokeStyle = '#d8f367'; ctx.lineWidth = Math.max(1, meta.width / 650);
  for (const b of bars) ctx.strokeRect(b.x - b.scale, b.y - 2 * b.scale, 83 * b.scale, 13 * b.scale);
}
new ResizeObserver(drawOverlay).observe($('video'));
$('video').ontimeupdate = () => $('preview-time').textContent = clock($('video').currentTime);
$('video').onloadedmetadata = drawOverlay;
$('frame-back').onclick = () => { $('video').pause(); $('video').currentTime = Math.max(0, $('video').currentTime - 1 / (meta?.fps || 30)); };
$('frame-next').onclick = () => { $('video').pause(); $('video').currentTime = Math.min(meta?.duration || 0, $('video').currentTime + 1 / (meta?.fps || 30)); };
function setPicking(value) {
  picking = value; $('overlay').classList.toggle('picking', value); $('pick-help').hidden = !value;
  $('pick-bar').classList.toggle('selected', value); $('pick-bar').setAttribute('aria-pressed', String(value));
  $('pick-bar').textContent = value ? 'Cancel picking · Esc' : 'Pick hunger bar position';
  if (value) { $('video').pause(); drawOverlay(); }
}
$('pick-bar').onclick = () => setPicking(!picking);
document.addEventListener('pointerdown', event => {
  if (picking && !event.target.closest('.video-stage,#pick-bar')) setPicking(false);
});
$('overlay').onclick = event => {
  if (!picking || !meta) return;
  const r = $('overlay').getBoundingClientRect();
  $('bar-x').value = Math.round((event.clientX - r.left) / r.width * meta.width);
  $('bar-y').value = Math.round((event.clientY - r.top) / r.height * meta.height);
  $('bar-mode').value = 'manual'; $('bar-mode').dispatchEvent(new Event('change'));
  setPicking(false);
};
$('scan').onclick = () => action(async () => {
  $('progress-panel').hidden = true;
  const { start, end } = scanRange($('start').value, $('end').value, meta?.duration);
  const maxHours = Number($('max-hours').value);
  if (!Number.isFinite(maxHours) || maxHours < .01 || maxHours > 72) throw new Error('Choose a game uptime between 0.01 and 72 hours.');
  let bars = configuredBars();
  if (bars.length > 4) throw new Error('Use up to four HUD positions.');
  await ensureOpen();
  result = undefined; importedReport = false; $('results').hidden = true; $('progress-panel').hidden = false; $('cancel').hidden = false;
  $('progress-panel').dataset.state = 'running';
  $('progress').value = 0; $('percent').textContent = '0%'; $('scan-status').textContent = 'Preparing scanner…';
  $('progress-detail').textContent = 'Your video stays on this device.'; $('video').pause();
  try {
    if ($('bar-mode').value === 'auto' && $('extra-bars').value.trim()) {
      const primary = await request('locate', { start, end }, progress);
      if (!primary) throw new Error('No primary hunger bar was found. Set its position manually.');
      bars = [primary, ...parseBars($('extra-bars').value)];
    }
    if (bars.length > 4) throw new Error('Use up to four HUD positions.');
    result = await request('scan', { start, end, maxHours, bars, multi: $('multi').checked }, progress);
    selectedBars = (result.screens ?? [result.bar]).filter(Boolean); drawOverlay();
    $('progress-panel').dataset.state = 'complete';
    $('progress').value = 100; $('percent').textContent = '100%'; $('scan-status').textContent = 'Analysis complete';
    $('progress-detail').textContent = `${result.studio.frames.toLocaleString()} frames · ${Math.round(result.studio.elapsed)}s processing time`;
    renderResults();
  } finally { $('cancel').hidden = true; }
}, 'scan');
$('code-example').onclick = () => { $('manual-row').value = 'food'; $('manual-hours').value = 1; $('codes').value = '@0.1 20 +--+000-++\n@3.1667 20 -0+++0+--+'; setBusy(false); };
$('crack').onclick = () => action(async () => {
  if (!$('codes').value.trim()) throw new Error('Enter a jolt pattern or load the example first.');
  $('manual-output').classList.remove('empty-output'); $('manual-output').textContent = 'Matching patterns…';
  const answer = await request('manual', { row: $('manual-row').value, maxHours: Number($('manual-hours').value), codes: $('codes').value });
  $('manual-output').textContent = answer.text;
}, 'manual');
$('open-report').onclick = () => $('report-file').click();
$('sample-report').onclick = () => action(async () => {
  const response = await fetch(new URL('./demo-report.json', import.meta.url));
  if (!response.ok) throw new Error('The example report could not be loaded.');
  result = validateResult(await response.json()); importedReport = true; $('progress-panel').hidden = true; renderResults();
  notice('Example report from the synthetic known-cut fixture. This loads saved test results; use the scanner to analyze your own video.');
});
$('report-file').onchange = () => {
  const f = $('report-file').files[0]; $('report-file').value = '';
  if (f) action(() => openReportFile(f));
};
async function openReportFile(f) {
  if (f.size > 64 * 1024 * 1024) throw new Error('Choose a results JSON file smaller than 64 MB.');
  let data;
  try { data = JSON.parse(await f.text()); } catch { throw new Error('This file is not valid JSON. Choose a downloaded Jolt results report.'); }
  result = validateResult(data); importedReport = true; $('progress-panel').hidden = true; renderResults();
  notice('Report loaded. Attach its source video in the scanner to inspect individual frames.');
}
function svgNode(name, attributes) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', name);
  for (const [key, value] of Object.entries(attributes)) el.setAttribute(key, value);
  return el;
}
function renderChart() {
  const svg = $('timeline'); svg.replaceChildren();
  let pointCount = 0, min = Infinity, max = -Infinity;
  for (const run of result.runs) for (const [time, tick] of run.points) {
    const offset = tick / 20 - time; min = Math.min(min, offset); max = Math.max(max, offset); pointCount++;
  }
  const [start, end] = result.span;
  if (!pointCount) { min = 0; max = 1; }
  const padding = Math.max(1, (max - min) * .15); min -= padding; max += padding;
  const x = t => 70 + (t - start) / Math.max(.001, end - start) * 910;
  const y = value => 195 - (value - min) / Math.max(.001, max - min) * 170;
  for (let i = 0; i < 4; i++) {
    const v = min + (max - min) * i / 3;
    svg.append(svgNode('line', { x1: 70, x2: 980, y1: y(v), y2: y(v), stroke: '#30382b', 'stroke-width': 1 }));
    const label = svgNode('text', { x: 58, y: y(v) + 3, fill: '#8f9e87', 'font-size': 10, 'text-anchor': 'end', 'font-family': 'monospace' }); label.textContent = v.toFixed(1); svg.append(label);
  }
  for (const b of result.breaks.filter(b => b.kind === 'removed')) svg.append(svgNode('rect', { x: x(b.from), y: 22, width: Math.max(2, x(b.to) - x(b.from)), height: 178, fill: '#f2bd80', opacity: .09 }));
  for (const r of result.runs) {
    const sampled = r.points.filter((_, i) => i % Math.max(1, Math.ceil(pointCount / 2000)) === 0 || i === r.points.length - 1);
    svg.append(svgNode('polyline', { points: sampled.map(p => `${x(p[0])},${y(p[1] / 20 - p[0])}`).join(' '), fill: 'none', stroke: '#d8f367', 'stroke-width': 1.5 }));
    for (const p of sampled.slice(0, 500)) {
      const dot = svgNode('circle', { cx: x(p[0]), cy: y(p[1] / 20 - p[0]), r: 2.5, fill: '#d8f367', tabindex: 0, role: 'button', 'aria-label': `Seek to ${clock(p[0])}, tick ${p[1]}` });
      dot.onclick = () => seek(p[0]); dot.onkeydown = event => { if (['Enter',' '].includes(event.key)) { event.preventDefault(); seek(p[0]); } }; svg.append(dot);
    }
  }
  if (!pointCount) { const t = svgNode('text', { x: 520, y: 113, fill: '#9aab94', 'font-size': 13, 'text-anchor': 'middle' }); t.textContent = 'No confirmed tick readings to plot'; svg.append(t); }
  $('chart-start').textContent = clock(start); $('chart-end').textContent = clock(end);
}
function seek(time) {
  if (!file) return notice('Attach the source video in the scanner to inspect this moment.');
  switchTab('scan'); $('video').currentTime = Math.max(0, Math.min(meta?.duration ?? time, time)); $('video').pause(); $('preview-panel').scrollIntoView({ behavior: 'smooth', block: 'center' });
}
function renderTable() {
  const filtered = result.jolts.filter(j => tableFilter === 'all' || (j.tick != null) === (tableFilter === 'matched'));
  const tbody = $('jolt-table'); tbody.replaceChildren();
  for (const j of filtered.slice(0, 500)) {
    const tr = document.createElement('tr'); tr.tabIndex = 0; tr.setAttribute('aria-label', `Inspect jolt at ${clock(j.time)}`);
    const pattern = [...j.offsets].reverse().map(n => n == null || n === 9 ? '?' : n === -1 ? '+' : n === 0 ? '0' : '-').join('');
    for (const value of [clock(j.time), 'health' in j ? 'Hearts' : 'Hunger', pattern, j.tick ?? '—']) { const td = document.createElement('td'); td.textContent = value; tr.append(td); }
    tr.onclick = () => seek(j.time); tr.onkeydown = event => { if (['Enter',' '].includes(event.key)) { event.preventDefault(); seek(j.time); } }; tbody.append(tr);
  }
  if (!filtered.length) { const tr = document.createElement('tr'), td = document.createElement('td'); td.colSpan = 4; td.className = 'table-empty'; td.textContent = 'No jolts match this filter.'; tr.append(td); tbody.append(tr); }
  $('table-note').textContent = `Showing ${Math.min(filtered.length, 500).toLocaleString()} of ${filtered.length.toLocaleString()} jolts. Download JSON for all readings.`;
}
function renderResults() {
  const info = overview(result);
  $('result-origin').textContent = importedReport ? 'SAVED REPORT' : 'ANALYSIS COMPLETE';
  $('result-title').textContent = info.title; $('result-note').textContent = info.note;
  $('jolts-count').textContent = result.jolts.length.toLocaleString(); $('matched-count').textContent = info.matched.toLocaleString(); $('cuts-count').textContent = info.cuts.length;
  $('coverage').textContent = `${(100 * (Number(result.readable) || 0)).toFixed(1)}%`;
  $('events-count').textContent = result.breaks.length;
  const list = $('events'); list.replaceChildren();
  const names = { removed: 'Possible missing time', stalled: 'Lag or pause', restart: 'Game clock restarted', back: 'Earlier game clock returns', screen: 'Different player’s HUD' };
  for (const b of result.breaks) {
    const button = document.createElement('button'); button.className = `event ${b.kind === 'removed' ? 'removed' : ''}`;
    const title = document.createElement('strong'); title.textContent = names[b.kind] ?? 'Clock change';
    const text = document.createElement('p'); text.textContent = b.jump_seconds == null ? `Game ${b.game}` : `${Math.abs(b.jump_seconds).toFixed(2)} seconds ${b.jump_seconds > 0 ? 'ahead' : 'behind'}`;
    const time = document.createElement('span'); time.className = 'event-time'; time.textContent = `${clock(b.from)} → ${clock(b.to)}`;
    button.append(title, text, time); button.onclick = () => seek(b.from); list.append(button);
  }
  if (!result.breaks.length) { const p = document.createElement('p'); p.className = 'no-events'; p.textContent = info.matched ? 'No clock discontinuities found in the confirmed readings.' : 'No confirmed ticks, so there are no clock events to classify.'; list.append(p); }
  $('description').textContent = result.studio?.description ?? 'Imported desktop Jolt report. Explore the events and matched ticks above.';
  renderChart(); renderTable(); $('results').hidden = !['scan','report'].includes(activeTab);
}
$('jolt-filter').onchange = () => { tableFilter = $('jolt-filter').value; if (result) renderTable(); };
function download(content, name, type) {
  const blobURL = URL.createObjectURL(new Blob([content], { type }));
  const a = document.createElement('a'); a.href = blobURL; a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(blobURL), 30000);
}
const basename = () => String(result?.title ?? file?.name ?? 'jolt-studio').replace(/\.[^.]+$/, '').replace(/[^\p{L}\p{N}_. -]/gu, '_');
$('export-json').onclick = () => { if (result) download(JSON.stringify(result, null, 2), `${basename()}.jolt.json`, 'application/json'); };
$('export-html').onclick = () => action(async () => { const html = await request('html', { result }); download(html, `${basename()}.jolt.html`, 'text/html'); });
function resetControls(root) {
  for (const el of document.querySelectorAll(`${root} input,${root} textarea`)) { if (el.type === 'checkbox') el.checked = el.defaultChecked; else el.value = el.defaultValue; }
  for (const el of document.querySelectorAll(`${root} select`)) el.selectedIndex = 0;
  document.querySelectorAll(`${root} details`).forEach(el => el.open = false);
}
$('clear').onclick = () => {
  worker?.terminate(); worker = undefined; pending.clear(); file = undefined; meta = undefined; result = undefined; selectedBars = []; importedReport = false; tableFilter = 'all';
  if (url) URL.revokeObjectURL(url); url = undefined;
  resetControls('main'); setPicking(false); $('manual-bar').hidden = true;
  $('manual-output').textContent = emptyManual; $('manual-output').classList.add('empty-output'); $('preview-time').textContent = '0:00.00'; drawOverlay();
  $('video').removeAttribute('src'); $('video').load(); $('preview-panel').hidden = true; $('dropzone').hidden = false; $('results').hidden = true; $('progress-panel').hidden = true; notice(''); setBusy(false);
};
document.querySelector('main').addEventListener('input', () => { if (!busy) $('clear').disabled = !hasSession(); });
document.querySelector('main').addEventListener('change', () => { if (!busy) $('clear').disabled = !hasSession(); });
window.addEventListener('pagehide', () => { worker?.terminate(); if (url) URL.revokeObjectURL(url); });
window.addEventListener('pageshow', event => { if (event.persisted) location.reload(); });
document.addEventListener('keydown', event => {
  if (event.key === 'Escape') setPicking(false);
  if (!event.ctrlKey && !event.metaKey && !event.altKey && !['INPUT','TEXTAREA','SELECT'].includes(document.activeElement.tagName)) {
    const tab = { 1:'scan',2:'manual',3:'report',4:'guide' }[event.key]; if (tab) { setPicking(false); notice(''); switchTab(tab); }
  }
});
switchTab('scan'); setBusy(false);

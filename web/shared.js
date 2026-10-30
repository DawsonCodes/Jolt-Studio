export function parseTime(value, fallback = 0) {
  if (String(value).trim() === '') return fallback;
  const parts = String(value).trim().split(':');
  if (parts.length > 3 || parts.some(p => !/^\d+(?:\.\d+)?$/.test(p))) throw new Error('Use seconds, mm:ss, or hh:mm:ss for the time range.');
  const result = parts.reduce((sum, part) => sum * 60 + Number(part), 0);
  if (!Number.isFinite(result)) throw new Error('Enter a valid time.');
  return result;
}
export function clock(seconds, decimals = 2) {
  if (!Number.isFinite(seconds)) return '—';
  const sign = seconds < 0 ? '−' : '';
  seconds = Math.round(Math.abs(seconds) * 10 ** decimals) / 10 ** decimals;
  const minutes = Math.floor(seconds / 60);
  return `${sign}${minutes}:${(seconds % 60).toFixed(decimals).padStart(decimals ? 3 + decimals : 2, '0')}`;
}
export function scanRange(startValue, endValue, duration) {
  const start = parseTime(startValue), end = parseTime(endValue, duration);
  if (!Number.isFinite(duration) || start < 0 || end <= start || end > duration + .001) {
    throw new Error('Choose a start before the end, within the video’s duration.');
  }
  return { start, end: Math.min(end, duration) };
}
export function videoBounds(sourceWidth, sourceHeight, width, height) {
  const scale = Math.min(width / sourceWidth, height / sourceHeight);
  const fittedWidth = sourceWidth * scale, fittedHeight = sourceHeight * scale;
  return { x: (width - fittedWidth) / 2, y: (height - fittedHeight) / 2, width: fittedWidth, height: fittedHeight };
}
export function parseBars(text) {
  if (!text.trim()) return [];
  return text.trim().split(/\n+/).map(line => {
    const p = line.trim().split(/[\s,]+/).map(Number);
    if (p.length !== 3 || p.some(n => !Number.isFinite(n)) || p[0] < 0 || p[1] < 0 || p[2] < .5 || p[2] > 24) throw new Error('Each bar needs x, y, scale; for example: 970, 990, 3.');
    return { x: p[0], y: p[1], scale: p[2] };
  });
}
export function validateResult(data) {
  if (!data || typeof data !== 'object' || !Array.isArray(data.jolts) || !Array.isArray(data.runs) || !Array.isArray(data.breaks) || !Array.isArray(data.span) || data.span.length !== 2 || !data.span.every(Number.isFinite) || data.span[0] < 0 || data.span[1] < data.span[0]) throw new Error('This file is not a supported Jolt JSON report.');
  if (data.jolts.length > 2_000_000 || data.runs.length > 100_000 || data.breaks.length > 100_000) throw new Error('This report is too large to display.');
  if (data.fps != null && (!Number.isFinite(data.fps) || data.fps <= 0)) throw new Error('The report contains an invalid frame rate.');
  if (data.version != null && (typeof data.version !== 'string' || !/^[\w .-]{1,64}$/.test(data.version))) throw new Error('The report contains an invalid version.');
  for (const j of data.jolts) if (!j || !Number.isFinite(j.time) || !Array.isArray(j.offsets) || j.offsets.length !== 10 || j.offsets.some(n => ![-1,0,1,9,null].includes(n)) || (j.tick != null && !Number.isFinite(j.tick))) throw new Error('The report contains an invalid jolt.');
  for (const r of data.runs) if (!r || !Array.isArray(r.points) || (r.game != null && !Number.isInteger(r.game)) || r.points.some(p => !Array.isArray(p) || !Number.isFinite(p[0]) || !Number.isFinite(p[1]) || (p[2] != null && !Number.isInteger(p[2])))) throw new Error('The report contains an invalid tick run.');
  for (const b of data.breaks) {
    if (!b || !Number.isFinite(b.from) || !Number.isFinite(b.to) || !['removed','stalled','restart','back','screen'].includes(b.kind) || !Number.isInteger(b.game) || (b.jump_seconds != null && !Number.isFinite(b.jump_seconds)) || (b.since != null && !Number.isFinite(b.since))) throw new Error('The report contains an invalid event.');
    if (b.frames != null && (!Array.isArray(b.frames) || b.frames.length !== 2 || b.frames.some(f => typeof f !== 'string' || !/^data:image\/jpeg;base64,[A-Za-z0-9+/=]+$/.test(f)))) throw new Error('The report contains an invalid witness image.');
    if (b.frame_positions != null && (!Array.isArray(b.frame_positions) || b.frame_positions.some(p => !p || !['number','second','within_second','time'].every(k => Number.isFinite(p[k]))))) throw new Error('The report contains invalid frame positions.');
  }
  if (data.games != null && !Array.isArray(data.games)) throw new Error('The report contains invalid game clocks.');
  for (const g of data.games ?? []) if (!g || ![g.number,g.uptime_at_video_start,g.screen].every(Number.isFinite)) throw new Error('The report contains an invalid game clock.');
  return data;
}
export function overview(data) {
  const matched = data.jolts.filter(j => j.tick != null).length;
  const cuts = data.breaks.filter(b => b.kind === 'removed');
  return {
    matched, cuts,
    title: !matched ? 'Not enough tick evidence' : cuts.length ? `${cuts.length} possible cut${cuts.length === 1 ? '' : 's'} found` : 'No forward clock jumps found',
    note: !matched ? 'The scanner could not confirm game ticks. This result cannot establish whether footage was edited.' : cuts.length ? 'The game clock advanced further than the video. Review the marked intervals; speed changes or a different game instance can also explain a jump.' : 'No cut evidence was found in the matched readings. Unreadable footage and gaps between readings remain inconclusive.'
  };
}

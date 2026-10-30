import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseTime, parseBars, scanRange, videoBounds, clock, validateResult, overview } from '../web/shared.js';
test('time ranges accept clock timestamps and reject invalid numbers', () => {
  assert.equal(parseTime('1:02:03.5'),3723.5);
  assert.equal(parseTime('',90),90);
  assert.throws(()=>parseTime('-1'));
  assert.throws(()=>parseTime('1:wat'));
});
test('manual HUD coordinates validate complete triples', () => {
  assert.deepEqual(parseBars('330,290,2\n100 200 1'),[{x:330,y:290,scale:2},{x:100,y:200,scale:1}]);
  assert.throws(()=>parseBars('10,20'));
  assert.throws(()=>parseBars('10,20,0'));
});
test('a scan without matched ticks is inconclusive', () => {
  const data={span:[0,10],runs:[],jolts:[],breaks:[]};
  assert.equal(validateResult(data),data);
  assert.equal(overview(data).title,'Not enough tick evidence');
  assert.throws(()=>validateResult({}));
  assert.throws(()=>validateResult({...data,runs:[{points:[[null,5]]}]}));
});
test('a lack of detected cuts is never presented as proof of an unedited video', () => {
  const info=overview({jolts:[{tick:6222}],breaks:[]});
  assert.equal(info.title,'No forward clock jumps found');
  assert.match(info.note,/inconclusive/);
});
test('a scan rejects reversed and out-of-video ranges instead of silently clamping them', () => {
  assert.deepEqual(scanRange('0:05', '', 30), { start: 5, end: 30 });
  assert.throws(() => scanRange('0:05', '0:02', 30), /start before the end/);
  assert.throws(() => scanRange('0:00', '0:31', 30), /duration/);
  assert.throws(() => scanRange('0:30', '0:30', 30));
});
test('displayed timestamps carry rounded seconds into the next minute', () => {
  assert.equal(clock(59.999), '1:00.00');
  assert.equal(clock(3599.999), '60:00.00');
  assert.equal(clock(-59.999), '−1:00.00');
});
test('HUD overlays fit the actual video content inside a letterboxed player', () => {
  assert.deepEqual(videoBounds(1920, 1080, 800, 600), { x: 0, y: 75, width: 800, height: 450 });
  assert.deepEqual(videoBounds(1080, 1920, 800, 480), { x: 265, y: 0, width: 270, height: 480 });
});
test('malformed imported rows produce readable validation errors', () => {
  const report = { span: [0, 30], jolts: [], runs: [], breaks: [] };
  assert.throws(() => validateResult({ ...report, jolts: [null] }), /invalid jolt/);
  assert.throws(() => validateResult({ ...report, runs: [null] }), /invalid tick run/);
  assert.throws(() => validateResult({ ...report, breaks: [null] }), /invalid event/);
  assert.throws(() => validateResult({ ...report, games: {} }), /invalid game clocks/);
  assert.throws(() => validateResult({ ...report, span: [30, 0] }), /not a supported/);
});

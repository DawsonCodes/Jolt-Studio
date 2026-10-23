import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseTime, parseBars, validateResult, overview } from '../web/shared.js';
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

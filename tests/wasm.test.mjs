import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import createEngine from '../build/wasm/jolt-engine.mjs';

const e = await createEngine();
const read = ptr => { const out = JSON.parse(e.UTF8ToString(ptr)); if(out?.error)throw new Error(out.error);return out; };
function stringArg(text,fn){const p=e.stringToNewUTF8(text);try{return fn(p);}finally{e._free(p);}}
test('WebAssembly reproduces known manual hunger and heart matches',()=>{
  const food=stringArg('@0.1 20 +--+000-++ @3.1667 20 -0+++0+--+',p=>read(e._js_manual(0,1,p)));
  assert.equal(food.count,1);assert.equal(food.candidates[0].tick,6222);
  const hearts=stringArg('0--------0 -00-0--0-0 0-0000-0-0',p=>read(e._js_manual(1,1,p)));
  assert.equal(hearts.count,1);assert.equal(hearts.candidates[0].tick,25926);
  assert.throws(()=>stringArg('21 x',p=>read(e._js_manual(0,1,p))));
});

const spriteSource=await readFile(new URL('../vendor/jolt/sprites.h',import.meta.url),'utf8');
const sprites=Object.fromEntries([...spriteSource.matchAll(/inline constexpr Pixels (\w+) = \{([^}]+)\}/g)].map(m=>[m[1],m[2].split(',').map(Number)]));
function offsets(tick){
  let seed=(BigInt((Math.imul(tick,312871))|0)^0x5deece66dn)&((1n<<48n)-1n);const out=[];
  while(out.length<10){seed=(seed*0x5deece66dn+11n)&((1n<<48n)-1n);const bits=Number(seed>>17n);if(bits-bits%3+2<=2147483647)out.push(bits%3-1);}
  return out;
}
function draw(out,width,height,sprite,x,y){
  for(let cy=0;cy<9;cy++)for(let cx=0;cx<9;cx++){
    const at=(cy*9+cx)*4;if(!sprite[at+3])continue;
    for(let dy=0;dy<2;dy++)for(let dx=0;dx<2;dx++){
      const px=x+cx*2+dx,py=y+cy*2+dy;if(px<0||py<0||px>=width||py>=height)continue;
      const dest=(py*width+px)*4;for(let c=0;c<3;c++)out[dest+c]=sprite[at+2-c];
    }
  }
}
function base(pixels){const out=new Uint8Array(pixels*4);for(let i=0;i<pixels;i++)out.set([65,95,75,255],i*4);return out;}
function strips(rects,tick){
  const out=new Uint8Array(rects.reduce((n,r)=>n+r.width*r.height*4,0));let offset=0;
  for(let row=0;row<rects.length;row++){
    const r=rects[row],data=base(r.width*r.height),pat=tick%61===0?offsets(tick):Array(10).fill(0);
    for(let slot=0;slot<10;slot++){
      const x=330-(row?202:0)+(9-slot)*16-r.x,y=290+(row?0:pat[slot]*2)-r.y;
      draw(data,r.width,r.height,sprites[row?'HEART_CONTAINER':'FOOD_EMPTY'],x,y);
      draw(data,r.width,r.height,sprites[row?'HEART_FULL':'FOOD_FULL'],x,y);
    }
    out.set(data,offset);offset+=data.length;
  }
  return out;
}
test('WebAssembly pixel scanning and timeline exactly match the native engine on continuous and known-cut fixtures',async()=>{
  for(const cut of [false,true]){
    e._js_begin(640,360,60);assert.equal(read(e._js_add_bar(330,290,2)),true);
    const rects=read(e._js_rects());const data=strips(rects,6100),ptr=e._malloc(data.length);
    try{
      let lastTick=-1,cached;
      for(let n=0;n<1800;n++){
        const tick=6100+Math.floor(n/3)+(cut&&n>=900?305:0);
        if(tick!==lastTick){cached=strips(rects,tick);lastTick=tick;}
        e.HEAPU8.set(cached,ptr);assert.equal(e._js_frame(n/60,ptr),1);
      }
      const title=cut?'known-cut':'continuous';
      const got=stringArg(title,p=>read(e._js_analyze(1,p)));
      const native=JSON.parse(await readFile(new URL(`../build/${title}.json`,import.meta.url),'utf8'));
      assert.deepEqual(got,native);
      assert.equal(got.breaks.some(b=>b.kind==='removed'),cut);
      assert.ok(got.jolts.some(j=>j.tick!==null));
      const html=stringArg(JSON.stringify(got),p=>e.UTF8ToString(e._js_html(p)));
      assert.ok(html.startsWith('<!--'));assert.match(html,/known-cut|continuous/);
    }finally{e._free(ptr);}
  }
});

from pathlib import Path
import os, shutil, subprocess
from prepare_engine import ROOT, prepare

core = prepare()
compiler = os.environ.get('EMXX') or shutil.which('em++')
if not compiler:
    raise SystemExit('em++ is required. Install Emscripten 4.0.18 and activate its environment; see README.md.')
dest = ROOT / 'build/wasm'
dest.mkdir(parents=True, exist_ok=True)
exports = ['malloc','free','js_clear','js_sample','js_locate','js_begin','js_add_bar',
           'js_rects','js_frame','js_analyze','js_manual','js_html','js_last']
subprocess.run([compiler, '-std=c++20', '-O3', '-fexceptions', '-I'+str(core),
    str(ROOT/'engine/browser.cpp'), *[str(core/(n+'.cpp')) for n in ['hud','wobble','timeline','manual','report']],
    '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=createJoltEngine',
    '-sENVIRONMENT=worker,node', '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=2147483648',
    '-sSTACK_SIZE=4194304', '-sFILESYSTEM=0',
    '-sEXPORTED_FUNCTIONS='+','.join('_'+e for e in exports),
    '-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToNewUTF8,HEAPU8',
    '-o', str(dest/'jolt-engine.mjs')], check=True)

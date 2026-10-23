from pathlib import Path
import os, subprocess
from prepare_engine import ROOT, prepare

core=prepare()
out=ROOT/'build/native'
out.mkdir(parents=True,exist_ok=True)
compiler=os.environ.get('CXX','g++')
sources=[core/(n+'.cpp') for n in ['hud','wobble','timeline','manual','report']]
objects=[]
for source in sources:
    obj=out/(source.stem+'.o')
    subprocess.run([compiler,'-std=c++20','-O2','-I'+str(core),'-c',str(source),'-o',str(obj)],check=True)
    objects.append(str(obj))
for test in ['manual_tests','frame_position_tests','browser_tests']:
    extra=[str(ROOT/'engine/browser.cpp')] if test=='browser_tests' else []
    subprocess.run([compiler,'-std=c++20','-O2','-I'+str(core),str(ROOT/'tests'/(test+'.cpp')),*extra,*objects,'-o',str(out/test)],check=True)
    subprocess.run([str(out/test)],check=True)

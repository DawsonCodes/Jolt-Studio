"""Generate a small, clearly labelled synthetic known-cut test video."""
from pathlib import Path
import re, subprocess
import numpy as np
ROOT=Path(__file__).resolve().parent.parent
source=(ROOT/'vendor/jolt/sprites.h').read_text()
sprites={}
for name,values in re.findall(r'inline constexpr Pixels (\w+) = \{([^}]+)\}',source):
    pixels=np.array([int(v) for v in values.split(',')],dtype=np.uint8).reshape(9,9,4)
    sprites[name]=np.repeat(np.repeat(pixels,2,0),2,1)

def offsets(tick):
    seed=(tick*312871)&0xffffffff
    if seed>=0x80000000:seed-=0x100000000
    state=(seed^0x5deece66d)&((1<<48)-1)
    out=[]
    while len(out)<10:
        state=(state*0x5deece66d+11)&((1<<48)-1)
        bits=state>>17
        if bits-bits%3+2<=2147483647:out.append(bits%3-1)
    return out

def paint(frame,name,x,y):
    src=sprites[name]
    region=frame[y:y+18,x:x+18]
    opaque=src[:,:,3]!=0
    region[opaque]=src[:,:,:3][:,:,::-1][opaque]

cmd=['ffmpeg','-hide_banner','-loglevel','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s','640x360','-r','60','-i','-',
     '-an','-c:v','libvpx-vp9','-lossless','1','-threads','2','-pix_fmt','yuv420p',str(ROOT/'web/demo.webm')]
process=subprocess.Popen(cmd,stdin=subprocess.PIPE)
base=np.zeros((360,640,3),dtype=np.uint8);base[:]=[65,95,75]
# Background, coordinates, timing and seed are independent of the scanner.
for n in range(1800):
    frame=base.copy();tick=6100+n//3+(305 if n>=900 else 0)
    pattern=offsets(tick) if tick%61==0 else [0]*10
    for slot in range(10):
        x=330+(9-slot)*16;y=290+pattern[slot]*2
        paint(frame,'FOOD_EMPTY',x,y);paint(frame,'FOOD_FULL',x,y)
        paint(frame,'HEART_CONTAINER',330-202+(9-slot)*16,290)
        paint(frame,'HEART_FULL',330-202+(9-slot)*16,290)
    process.stdin.write(frame.tobytes())
process.stdin.close()
if process.wait():raise SystemExit('ffmpeg failed')
print('Generated 30-second synthetic clip: +15.25s clock jump at 0:15.')

"""Prepare a browser adapter without editing the pinned upstream files."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build/core'

def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    for source in (ROOT / 'vendor/jolt').glob('*'):
        if source.suffix in {'.cpp', '.h'}:
            shutil.copy2(source, OUT / source.name)
    # File decoding is supplied by the browser. Keep all pixel-reading and
    # locator functions; omit only the native process-driven video orchestration.
    hud = (OUT / 'hud.cpp').read_text()
    hud = hud[:hud.index('Bar find_bar(')] + '\n}  // namespace jolt\n'
    (OUT / 'hud.cpp').write_text(hud)
    (OUT / 'video.h').write_text('''#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace jolt {
struct Rect { int left, top, width, height; };
struct Frame { int width = 0, height = 0; std::vector<std::uint8_t> pixels; };
struct VideoInfo { int width = 0, height = 0; };
}
''')
    # A dedicated browser Worker supplies isolation and cancellation. No shared
    # memory/pthreads or custom HTTP headers are needed on GitHub Pages.
    (OUT / 'parallel.h').write_text('''#pragma once
namespace jolt {
inline int all_cores() { return 1; }
inline int default_jobs() { return 1; }
template <typename Work> void for_each_index(int count, int, Work work) {
    for (int index = 0; index < count; ++index) work(index);
}
}
''')
    return OUT

if __name__ == '__main__':
    print(prepare())

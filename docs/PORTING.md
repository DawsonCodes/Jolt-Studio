# Browser port and validation

Jolt Studio uses Splosh Jolt 2.5 at commit
`19958fa2c349cb1fd15cc2b05545d0c880732b52` from
https://gitlab.com/skippyznjiff/jolt. Unmodified source files and their SHA-256
hashes are preserved in `vendor/jolt/PROVENANCE.json`, along with the MIT license.

`scripts/prepare_engine.py` creates a disposable build tree. Its only changes to
the upstream source are:

* Replace native video/process declarations with the three data types the pixel
  detector needs (`Rect`, `Frame`, `VideoInfo`).
* Omit `find_bar` and `find_bars`, which launch native FFmpeg processes. Keep
  `locate` and every pixel-reading function unchanged.
* Execute the upstream parallel loops sequentially inside a dedicated Worker.
  No SharedArrayBuffer, pthreads, service worker, custom response headers, or
  cross-origin isolation is required on GitHub Pages.

`engine/browser.cpp` adapts video frames to the original `BarModel`, keeps the
same 1.5-second rest windows and neighboring-window measurement, and calls the
upstream jolt extraction, faint-heart chains, RNG tables, timeline matching,
game grouping, JSON serialization, manual cracker, and interactive report.

Mediabunny reads selected local files lazily through BlobSource and supplies
frames in presentation order. Original frame timestamps are preserved relative
to the first video timestamp; the frame rate is used for window size and
matching tolerance, rather than manufacturing frame timestamps. Duplicate or
non-increasing timestamps stop the scan with an error. Decode errors stop the
scan rather than silently returning a clean result.

Automatic HUD finding samples six images across the selected range. Additional
screens can be searched by masking already located HUDs, or supplied manually.
Different screen positions are analyzed separately by the upstream timeline
engine. Moving HUDs, resource packs, overlays and rapid perspective switches
still need manual review and may be unreadable.

## Validation

Run `npm run build:engine` followed by `npm test`.

* Original upstream manual-code tests check known hunger and heart ticks,
  timed and untimed patterns, low-health behavior, food levels and bad inputs.
* Upstream source-frame tests cover constant, fractional and variable timestamps
  and second boundaries.
* Browser-adapter tests feed synthetic vanilla HUD pixels generated with an
  independent Java RNG implementation into the native adapter. They check a
  continuous 30-second run and a 30-second run with a +305-tick jump at 15s.
* WebAssembly tests check known hunger and heart answers, then compare the
  complete detector JSON against the native results for both video fixtures.
* JavaScript tests cover time/HUD settings, imported reports and inconclusive
  outcomes.

The bundled synthetic WebM is generated from the same documented scenario;
it is explicitly presented as test footage. Recreate it with
`python3 scripts/make_demo.py` (NumPy and FFmpeg required).

This is not a full real-world accuracy certification. Native FFmpeg and browser
decoders can produce different pixels. The adapter has a different decode and
HUD-discovery path, scans every frame rather than using the desktop fast skim,
and limits game uptime to 72 hours and screen positions to four. Run the same
real videos through both implementations before relying on difficult cases.

## Privacy and storage

There is no backend, telemetry, video upload endpoint, persistent browser file
storage, or saved FileSystem handle. A selected File and temporary detector data
exist only in the current page and its Worker. Clearing or refreshing discards
the session, terminates the Worker and revokes the video object URL. The
original disk file remains intact. Browser/OS memory reclamation is controlled
by the platform; this is not a secure-erasure mechanism.

## Licenses

The browser UI and adapter are MIT licensed. Preserve the complete Splosh MIT
notice when distributing the core or compiled detector. Mediabunny 1.62.1 is
unmodified MPL-2.0 software. The static build ships its license, and the source
map embeds the corresponding TypeScript sources. Source is also distributed
in the exact `mediabunny@1.62.1` npm package. No code from the unlicensed Python
script or Tick Studio project is used in this implementation.

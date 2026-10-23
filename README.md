# Jolt Studio

A local, browser-based workspace for investigating Minecraft gameplay videos.
Select a video, recover game ticks from vanilla hunger/heart jolts, inspect
clock changes, and export an interactive HTML report or results JSON.

Videos are processed on the visitor's device. Refreshing or clearing the session
discards the selected file reference and analysis. There is no upload server.

## Features

- Local video selection and drag-and-drop, preview, seeking and frame stepping.
- Automatic or manual hunger-bar location; up to four HUD positions.
- Original Splosh Jolt hunger, low-health heart and faint-heart detection.
- Game tick matching, pauses/lag, clock restarts and possible missing-time events.
- Time ranges, configurable game uptime (0.01–72 hours), cancellation and progress.
- Manual hunger/heart code cracking, including timestamps and low-health codes.
- Timeline and jolt inspector, HTML/JSON export, and desktop Jolt JSON import.
- Witness images for possible cuts in exported reports.
- A clearly labelled synthetic sample with a known 15.25-second clock jump.

Use a current desktop Chrome or Edge over HTTPS. Other browsers work when they
provide WebCodecs and a decoder for the video's codec. Container support includes
MP4/MOV and WebM/MKV through Mediabunny; actual codec support is browser/device
dependent. No detected clock jump is not proof that a video is unedited.

## Publish on GitHub Pages

In this repository open **Settings → Pages**, then set **Build and deployment →
Source → GitHub Actions**. The included workflow builds the WebAssembly engine,
runs the detector and parity checks, bundles the static app and deploys Pages.
If the first deployment ran before Pages was enabled, rerun its failed job.

Only builds use Actions minutes. Scanning videos runs in the visitor's browser.
All scripts/assets are hosted with the app, without runtime CDN dependencies.

## Develop

Install Node.js 22+, Python 3, a C++20 compiler with `std::format` (GCC 13+),
and [Emscripten 4.0.18](https://emscripten.org/docs/getting_started/downloads.html).
Activate Emscripten's environment, then:

```sh
npm ci
npm run build:engine
npm test
npm run build
npm run dev
```

Open `http://localhost:4173` for local development. WebCodecs requires a secure
context; localhost qualifies, while an arbitrary HTTP hostname does not. Hosted
GitHub Pages uses HTTPS. `EMXX` and `CXX` can select compiler paths.

See [porting details and validation](docs/PORTING.md) for exact source changes,
test coverage, storage behavior and the limits of desktop parity.

## Credits

Based on [Splosh's Jolt 2.5](https://splosh.icu/jolt), preserved under its
[MIT license](vendor/jolt/LICENSE). The original source commit and SHA-256
manifest are included. Local media reading uses unmodified
[Mediabunny 1.62.1](https://github.com/Vanilagy/mediabunny), MPL-2.0.

Jolt Studio's interface and browser adapter are [MIT licensed](LICENSE).

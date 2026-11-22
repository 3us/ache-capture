# ache-capture

The Windows screen and game recorder behind [ache](https://ache.gg), built on
OBS Studio's engine ([libobs](https://github.com/obsproject/obs-studio)).

It records the screen and any fullscreen game, **including exclusive fullscreen**,
using OBS's own signed game-capture hook. The picture is scaled and encoded on the
GPU (NVIDIA NVENC, AMD AMF or Intel QuickSync; x264 as a last resort) and written
to stdout as a stream of timestamped frames.

ache runs it as a separate program and cuts its output into a replay buffer with
ffmpeg. Nothing else of ache is in here.

## Use

```
ache-capture --monitor \\.\DISPLAY1 --canvas 5120x1440 --size 4096x1152 --fps 60 \
             --encoder nvenc --codec h264 --kbps 20000 [--adapter 0] [--no-game]
```

| Option | Meaning |
|---|---|
| `--monitor` | The monitor to record when no game is fullscreen, by its Windows device name |
| `--canvas` | That monitor's size; games are fitted into it |
| `--size` | The size to encode at (scaled on the GPU). Hardware H.264 stops at 4096 wide |
| `--fps` | Frames per second |
| `--encoder` | `nvenc`, `amf`, `qsv` or `x264` (ache's names like `h264_nvenc` work too) |
| `--codec` | `h264` (default) or `hevc` |
| `--kbps` | Constant bitrate |
| `--adapter` | GPU index (DXGI order) to render on |
| `--no-game` | Screen only, no game hook |

It stops when stdin closes or reads `q`. Logs go to stderr.

Output is IVF: a 32-byte header, then for each frame a little-endian `u32` size, a
`u64` timestamp in 1/fps units (skipped frames leave a gap) and the frame, Annex-B,
with the codec headers before every keyframe. A keyframe comes every second.

```
ache-capture ... | ffmpeg -f ivf -i pipe:0 -c:v copy out.mp4
```

## Build

On macOS (or Linux), cross-compiling for Windows x64:

```
./build.sh
```

Needs Homebrew `llvm` and `lld`, and a Windows SDK sysroot from
[cargo-xwin](https://github.com/rust-cross/cargo-xwin) (`XWIN` to point elsewhere).
The script downloads the official OBS Studio release it builds against (pinned
version and checksums at the top), and lays out `dist/ache-capture/` the way libobs
expects:

```
bin/64bit/   ache-capture.exe, obs.dll, libobs-d3d11.dll and their DLLs
data/libobs/ libobs's shaders
core/        win-capture (with the game hook), obs-nvenc, obs-ffmpeg (AMF), obs-qsv11, obs-x264
```

The OBS files are copied unchanged from that release, so the game hook keeps OBS's
signature. OBS's DLLs need the Microsoft Visual C++ 2015-2022 runtime
(`vcruntime140.dll`, `msvcp140.dll`), which most Windows PCs with games already have.

## License

GPL-2.0-or-later, like libobs. See `LICENSE`; OBS's own notice is in
`dist/ache-capture/OBS-COPYING.txt`.

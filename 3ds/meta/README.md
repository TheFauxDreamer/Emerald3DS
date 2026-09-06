# Home-menu identity assets

Drop three files in this directory and the build picks them up. Nothing else to
edit. Until they exist the build still works: every rule that uses them is
wrapped in a `wildcard` guard, so a missing file degrades to "no icon / no
banner" instead of a build failure.

These are **packaging** assets. They are read at build time by `smdhtool` and
`bannertool` and are never linked into the ELF, which is what separates them
from `3ds/graphics/` (see `3ds/UI_SKIN_PLAN.md`), where in-game art is compiled
in via `INCGFX`. Different pipeline, different directory.

| File | Spec | What it is |
|---|---|---|
| `icon.png` | 48x48 PNG | The tile on the Home menu grid. |
| `banner.png` | 256x128 PNG | The top-screen visual when the title is highlighted. |
| `banner.wav` | 16-bit PCM WAV, **stereo**, **3 s or less** | The jingle that plays alongside it. |

## The specs are exact, not advisory

Each of these was read out of the tools' source rather than assumed, because
each one fails in a way that is easy to misread:

- **48x48 icon.** `smdhtool` derives the small 24x24 icon from it. Not square,
  or not 48, and it errors.
- **256x128 banner.** `bannertool` hard-errors on any other size rather than
  scaling to fit (`BANNER_GFX_WIDTH` / `BANNER_GFX_HEIGHT`, enforced in
  `load_image`). Exporting at 2x and hoping it downscales does not work.
- **Audio is mandatory.** There is no such thing as a silent banner here.
  `makebanner` returns an error when given no audio, so `banner.png` alone
  builds nothing at all.
- **Audio must be 3 seconds or less, and stereo.** This is the one that bites,
  because absolutely nothing tells you when you break it. 3dbrew's CBMD page:
  the BCWAV "total channels must be 2, and the length of the audio must be 3
  seconds or less, otherwise the sound will play incorrectly or **the model may
  fail to load**." That last clause is the trap. Over-length audio does not cost
  you the jingle, it costs you the *whole banner*, image included, and the Home
  menu just shows nothing where the banner should be.

  Every stage before that succeeds: `bannertool` builds the oversized `.bnr`
  without complaint, `makerom` packs it, CI goes green, the CIA installs. The
  only symptom is a blank banner on the console. A 38 s track produced a 6.5 MB
  banner where a normal one is ~300 KB, and 99.5% of it was audio.
- **Plain 16-bit PCM.** CI pins a 2022 build of `bannertool` because newer ones
  will not run in the devkitARM container (see the comment on the Install
  bannertool step in `.github/workflows/build-3ds.yml`). That older build does
  not accept OGG or exotic WAV encodings. Convert first:
  `ffmpeg -i in.wav -acodec pcm_s16le -ar 32000 banner.wav`

## Notes on the art itself

- The banner is a flat image. Animated and 3D banners need a CGFX authored in
  separate tooling and are out of scope.
- Both images support alpha, but the Home menu composites them over its own
  background, so avoid relying on transparency reading a particular way.
- Keep the jingle short and make it loop cleanly. It repeats for as long as the
  title stays highlighted.

## Checking your files before you commit

```sh
file icon.png banner.png   # expect "PNG image data, 48 x 48" / "256 x 128"
file banner.wav            # expect "WAVE audio ... 16 bit, stereo"

# The 3 s limit, which `file` will not tell you:
python3 -c 'import wave;w=wave.open("banner.wav");print("%.2f s, %d ch" % (w.getnframes()/w.getframerate(), w.getnchannels()))'
```

A banner much over ~400 KB means the audio is too long, whatever the clock says.

## The jingle loops

It plays for as long as the title stays highlighted, rather than once.

That needs two tool steps instead of one, which is why the Makefile has a
`.cwav` rule sitting between the wav and the banner. `makebanner -a file.wav`
converts the audio itself and hardcodes looping off, with no flag to change it.
`makecwav -l true` does the same conversion with looping on, and
`makebanner -ca` accepts the finished CWAV. Same output, loop flag set.

The loop covers the whole file: it restarts at frame 0 when it reaches the end.
Looping a section instead means passing frame numbers to `makecwav`
(`-s`/`--loopstartframe`, `-f`/`--loopendframe`), which are a property of the
music rather than of the build, so they are not wired in. Make the file itself
loop cleanly end-to-start and there is nothing to configure.

# Give Emerald3DS its own Home-menu identity

> **Status: planned, not started.** Written 2026-09-06, re-verified against the
> tree on 2026-09-06 after five unrelated commits landed. Nothing in this
> document has been applied.
>
> **Where to pick it up:** everything here is now checked against the repo,
> including the `bannertool` release and its CLI flags, which were the one open
> decision in the first draft. That decision is settled under "Change 2". The
> only remaining blocker is that the three art files do not exist yet.
>
> **Quickest possible win**, if you only want the icon and not the banner: drop
> a 48x48 PNG at `3ds/icon.png`, change `APP_ICON` on `3ds/Makefile:37` to
> `icon.png`, and apply the one-line SMDH prerequisite fix under "Change 1".
> That is self-contained and needs no new tooling.

## Context

The port currently ships with devkitPro's generic homebrew icon and no banner
at all. Three separate things make up what a 3DS shows for a title, and right
now only one and a half of them are set:

| What you see | Where it comes from | State today |
|---|---|---|
| **Icon**, the tile on the Home grid | SMDH, built by `smdhtool` | devkitPro's `default_icon.png` |
| **Text** under the icon: title, description, author | Same SMDH | Placeholder strings |
| **Banner**, the top-screen visual when the title is highlighted | A `.bnr`, passed to `makerom` | **Not configured. makerom is never given one.** |

Goal: supply all three from files in the repo, so the build produces a title
that looks like its own game rather than an untitled homebrew slot.

Two things are in scope beyond the assets themselves:

1. **A rebuild papercut.** `$(TARGET).smdh` has no dependency on the icon file
   (`3ds/Makefile:123`), so once it exists, editing the PNG will not rebuild
   it. You get the old icon with no warning until a `make clean`. Fixed here.
   This is not a hypothetical class of bug in this file: `3ds/Makefile:63-70`
   already carries a long comment on why `-MMD -MP` is "load bearing, not
   hygiene", because without it a header edit silently relinks stale objects.
   The missing icon prerequisite is that same bug one layer up, in the only
   rule the dependency files do not cover.
2. **Banner tooling.** `.bnr` files need `bannertool`, which is not a devkitPro
   package. That is the same situation as `makerom`, which CI already fetches
   from upstream pinned by SHA256.

---

## Assets to create

All three are new source files, committed to the repo. Nothing in `.gitignore`
excludes them.

| File | Spec | Notes |
|---|---|---|
| `3ds/icon.png` | **48x48** PNG | The Home-menu tile. |
| `3ds/banner.png` | **256x128** PNG | The top-screen visual. Exact, not advisory: `bannertool` hard-errors on any other size rather than scaling (`BANNER_GFX_WIDTH` / `BANNER_GFX_HEIGHT`, checked in `load_image`). Flat image only; an animated or 3D banner needs a CGFX authored elsewhere and is out of scope. |
| `3ds/banner.wav` | Short **16-bit PCM** WAV, about 3 s | `bannertool` **requires** audio and will not build a banner without it (verified in source: `makebanner` returns -1 when both `--audio` and `--cwavaudio` are absent). Converted to CWAV internally. This is the jingle that plays when the title is highlighted. Keep it plain 16-bit PCM: the build pinned in Change 2 is the older codebase, and the maintained fork's wider format support (OGG, anything `dr_wav` decodes) does not apply to it. |

Until the art exists the build keeps working. Both the existing icon rule and
the new banner rule are wrapped in `wildcard` guards, so a missing asset
degrades to "no icon / no banner" rather than a build failure. That guard is
already the established pattern at `3ds/Makefile:119`.

---

## Change 1: `3ds/Makefile`

**Variables** (lines 34-37). Replace the placeholder text and point at the new
assets:

```make
APP_TITLE       := <your title>
APP_DESCRIPTION := <your description>
APP_AUTHOR      := <your name>
APP_ICON        := icon.png

# Banner inputs. Both are required for a banner to be built at all.
APP_BANNER_IMG  := banner.png
APP_BANNER_WAV  := banner.wav
```

Paths are relative to `3ds/`, because the Makefile is always run as
`make -C 3ds`.

**Tool variable**, beside the existing `MAKEROM ?= makerom` (line 97):

```make
BANNERTOOL ?= bannertool
```

**SMDH rule** (line 123). Add the icon as a prerequisite so editing it
rebuilds:

```make
$(TARGET).smdh: $(APP_ICON) | $(BUILD)
```

**New banner rule**, mirroring the icon block's structure and its rationale,
which is that a missing asset must not break the build:

```make
ifneq ($(and $(wildcard $(APP_BANNER_IMG)),$(wildcard $(APP_BANNER_WAV))),)
BANNER_DEP  := $(TARGET).bnr
BANNER_FLAG := -banner $(TARGET).bnr

$(TARGET).bnr: $(APP_BANNER_IMG) $(APP_BANNER_WAV) | $(BUILD)
	$(BANNERTOOL) makebanner -i $(APP_BANNER_IMG) -a $(APP_BANNER_WAV) -o $@
else
BANNER_DEP  :=
BANNER_FLAG :=
endif
```

**Wire it into both package targets** (lines 130-136). Add `$(BANNER_DEP)` to
the prerequisites and `$(BANNER_FLAG)` to the `makerom` command, alongside the
existing `$(SMDH_DEP)` and `$(ICON_FLAG)`.

**`clean`** (line 177). Add `$(TARGET).bnr` to the existing wrapped list,
which already removes `$(BUILD)/*.d` alongside the objects.

> The `3dsx` target gets no banner, because the format only carries an SMDH. It
> is already known-broken here anyway (unaligned relocations, see the header
> comment in `3ds/emerald3ds.rsf`), so this changes nothing in practice.

> Unrelated to `3ds/UI_SKIN_PLAN.md`, despite both being about art. That plan
> puts PNGs under `3ds/graphics/skin/` and compiles them into the ELF via
> `INCGFX`. These three are packaging assets: read at build time by `smdhtool`
> and `bannertool`, never linked, and so they sit at the top of `3ds/`.

---

## Change 2: `.github/workflows/build-3ds.yml`

Add a `bannertool` step before "Package .cia and .3ds", following the existing
**Install makerom** step as the template. That step already does the right
thing: pinned URL, `sha256sum -c`, and a comment naming it as a third-party
binary entering the build.

**Which build, and why it is not the obvious one.** The first draft left this
open. It is now settled, and the answer is counter-intuitive enough to be worth
recording so nobody redoes the search:

- **The original is gone.** `Steveice10/bannertool` has been **deleted** from
  GitHub, not merely archived. Its release URLs 404, so "pin that repo's last
  release" is not an available option at all.
- **The maintained fork does not run here.** `carstene1ns/3ds-bannertool` is the
  active successor (1.2.3, June 2026), but its README states outright that the
  Linux prebuilts need **glibc 2.39+**, and the binary confirms it by importing
  `GLIBC_2.38`. The devkitARM container is older than that. This is the *exact*
  wall already documented on the makerom step above ("version `GLIBC_2.38' not
  found"), so the natural choice fails for a reason this repo has been bitten by
  once already. Building it from source instead is possible but needs cmake
  3.28+, which the container also predates, so it would mean a second job.
- **What does run.** `Epicpkmn11/bannertool` v1.2.2 ships a
  `linux-x86_64/bannertool` importing only `GLIBC_2.2.5` and `GLIBC_2.14`, the
  same bar as makerom v0.18.3, which the container already runs today.

```yaml
      # bannertool, like makerom, is not a devkitPro package -- another
      # third-party binary entering the build, so it is pinned by hash too.
      #
      # Deliberately Epicpkmn11's 2022 mirror. Upstream Steveice10/bannertool
      # has been DELETED from GitHub, and the maintained fork
      # (carstene1ns/3ds-bannertool) publishes Linux builds needing glibc 2.39+
      # -- the same GLIBC_2.38 wall documented on makerom above. This build
      # needs only GLIBC_2.14 and runs here as-is.
      - name: Install bannertool
        env:
          BANNERTOOL_URL: https://github.com/Epicpkmn11/bannertool/releases/download/v1.2.2/bannertool.zip
          BANNERTOOL_SHA256: e4259c08fe8944ebadd5f4b96f9a8603e5427338074cfc46323fbbc3410d51ed
        run: |
          curl -sSL -o /tmp/bannertool.zip "$BANNERTOOL_URL"
          echo "$BANNERTOOL_SHA256  /tmp/bannertool.zip" | sha256sum -c -
          # -j and an explicit member: this zip nests one directory per platform,
          # unlike makerom's flat one. Copying that step verbatim would drop four
          # platform directories into /usr/local/bin and leave nothing on PATH.
          unzip -oj /tmp/bannertool.zip 'linux-x86_64/bannertool' -d /usr/local/bin
          chmod +x /usr/local/bin/bannertool
          # Fail here, with a clear message, rather than mid-package.
          bannertool --version
```

The `makebanner` flags used in Change 1 were read out of the tool's own source
rather than guessed: `-i/--image` takes the PNG, `-a/--audio` the WAV, and
`-o/--output` the `.bnr`.

---

## Change 3: `3ds/emerald3ds.rsf` (optional)

Line 18, `Title: Emerald3DS`, is the CXI's internal title, separate from
`APP_TITLE`, which is what the Home menu shows. Worth aligning if you are
rebranding.

**Do not change `UniqueId` (line 28) or `ProductCode` (line 19).** The title ID
is the console's identity for the game. Change it and the CIA installs as a
brand-new title, leaving any existing save data attached to the old ID and
unreachable.

---

## Verification

There is no local devkitARM or makerom here, so CI is the realistic path. The
workflow already uploads `emerald3ds.cia`, `.3ds` and `.smdh` as artifacts.

1. **Build.** Push and let `build-3ds.yml` run, or locally
   `make -C 3ds clean && make -C 3ds`.
   `clean` matters once, because an existing `emerald3ds.smdh` will not rebuild
   on a title-text change alone: make cannot see that a variable moved.
2. **Icon and text.** Install the `.cia` on a console, or open the `.3ds` in
   Azahar. Confirm the Home grid tile is your art and that the title,
   description and author read correctly under it.
3. **Banner.** Highlight the title on the Home menu. The top screen should show
   `banner.png` and play `banner.wav`. This is the part with no prior art in
   this repo, so check it explicitly rather than assuming.
4. **Rebuild fix.** Edit a pixel of `3ds/icon.png`, run `make -C 3ds` *without*
   cleaning, and confirm `emerald3ds.smdh` is regenerated by checking its
   mtime. That is the papercut this plan fixes; without it the step silently
   no-ops.
5. **Regression.** Confirm the game still boots. None of this touches the ELF,
   only packaging, so reaching the title screen is sufficient.

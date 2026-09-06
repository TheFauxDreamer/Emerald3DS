# Give Emerald3DS its own Home-menu identity

> **Status: planned, not started.** Written 2026-09-06. Nothing in this document
> has been applied to the tree. Shelved while another issue was being fixed.
>
> **Where to pick it up:** everything is verified against the repo except the
> `bannertool` release and its CLI flags, which are called out as the one open
> decision under "Change 2". Start there, since the banner is the only part that
> needs a tool the build does not already have.
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
   (`3ds/Makefile:119`), so once it exists, editing the PNG will not rebuild
   it. You get the old icon with no warning until a `make clean`. Fixed here.
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
| `3ds/banner.png` | **256x128** PNG | The top-screen visual. Flat image only; an animated or 3D banner needs a CGFX authored elsewhere and is out of scope. |
| `3ds/banner.wav` | Short WAV, about 3 s | `bannertool` **requires** audio and will not build a banner without it. Converted to CWAV internally. This is the jingle that plays when the title is highlighted. |

Until the art exists the build keeps working. Both the existing icon rule and
the new banner rule are wrapped in `wildcard` guards, so a missing asset
degrades to "no icon / no banner" rather than a build failure. That guard is
already the established pattern at `3ds/Makefile:115`.

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

**Tool variable**, beside the existing `MAKEROM ?= makerom` (line 93):

```make
BANNERTOOL ?= bannertool
```

**SMDH rule** (line 119). Add the icon as a prerequisite so editing it
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

**Wire it into both package targets** (lines 126-132). Add `$(BANNER_DEP)` to
the prerequisites and `$(BANNER_FLAG)` to the `makerom` command, alongside the
existing `$(SMDH_DEP)` and `$(ICON_FLAG)`.

**`clean`** (line 170). Add `$(TARGET).bnr`.

> The `3dsx` target gets no banner, because the format only carries an SMDH. It
> is already known-broken here anyway (unaligned relocations, see the header
> comment in `3ds/emerald3ds.rsf`), so this changes nothing in practice.

---

## Change 2: `.github/workflows/build-3ds.yml`

Add a `bannertool` step before "Package .cia and .3ds", following the existing
**Install makerom** step as the template. That step already does the right
thing: pinned URL, `sha256sum -c`, and a comment naming it as a third-party
binary entering the build.

**Unresolved, and needs a decision at implementation time.** Upstream
`Steveice10/bannertool` is archived. Pick a source (that repo's last release,
or a maintained fork), pin the exact release URL and its SHA256, and verify the
`makebanner` flags against `bannertool --help` for whichever build you choose.
I could not verify the CLI or a good pinned hash from here and am not going to
invent one. Everything else in this plan is checked against the tree.

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

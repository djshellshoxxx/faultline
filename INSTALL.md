# Installing Vivisect

Each release zip contains the VST3, CLAP and Standalone builds (Linux adds LV2,
macOS adds AU).

Vivisect 0.10.0 is a **beta**. It is published as a GitHub *pre-release*
(tag `v0.10.0-beta.N`), so it does not show up as the "latest" release. Expect
rough edges and please report them.

## Windows

| File | Copy to |
|------|---------|
| `Vivisect.vst3` (a folder — copy the whole folder) | `C:\Program Files\Common Files\VST3\` |
| `Vivisect.clap` | `C:\Program Files\Common Files\CLAP\` |
| `Vivisect.exe` (standalone) | anywhere, e.g. `C:\Program Files\Vivisect\` |

## Linux

| File | Copy to |
|------|---------|
| `Vivisect.vst3` | `~/.vst3/` |
| `Vivisect.clap` | `~/.clap/` |
| `Vivisect.lv2`  | `~/.lv2/` |
| `Vivisect` (standalone) | anywhere; `chmod +x Vivisect` if needed |

## macOS

Download the zip that matches your Mac:

| Zip | For |
|-----|-----|
| `Vivisect-<version>-macos-arm64.zip` | Apple Silicon (M1 and newer) |
| `Vivisect-<version>-macos-x86_64.zip` | Intel Macs |

Requires macOS 11 or newer. Not sure which Mac you have? Apple menu > About
This Mac: "Chip: Apple M..." means arm64, "Processor: Intel" means x86_64.

| File | Copy to |
|------|---------|
| `Vivisect.vst3` | `~/Library/Audio/Plug-Ins/VST3/` |
| `Vivisect.clap` | `~/Library/Audio/Plug-Ins/CLAP/` |
| `Vivisect.component` (Audio Unit) | `~/Library/Audio/Plug-Ins/Components/` |
| `Vivisect.app` (standalone) | `/Applications/` |

(Use `/Library/Audio/Plug-Ins/...` instead for an all-users install.)

### Gatekeeper

The builds are ad-hoc signed but **not notarized**, so macOS will refuse to
open them at first. Remove the quarantine flag once, in Terminal, from the
folder you unzipped into:

```
xattr -dr com.apple.quarantine Vivisect.vst3 Vivisect.clap Vivisect.component Vivisect.app
```

Do this before copying, or point it at the installed copies. For the
standalone app you can instead right-click `Vivisect.app`, choose **Open**,
then **Open** again in the dialog. If a plug-in is still blocked in your DAW,
open System Settings > Privacy & Security and click **Open Anyway**.

For Audio Units, run your DAW's rescan (Logic: Settings > Plug-in Manager >
Reset & Rescan Selection). If it still does not appear, log out and back in so
`AudioComponentRegistrar` refreshes.

Then rescan plug-ins in your DAW (restart it if it only scans on start-up).

## Uninstall

Delete the files you copied. User presets and logs live in the Vivisect user
data folder shown in the plug-in's Help page; delete that folder too to remove
everything.

## Presets

User presets are `.vsxpreset` files in the folder opened by
**MENU > Open Preset Folder**. Put presets you were sent directly in that
folder (not a sub-folder), then reopen the plug-in window.

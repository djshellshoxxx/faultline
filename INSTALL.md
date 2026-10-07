# Installing Vivisect

Each release zip contains the VST3, CLAP and Standalone builds (Linux adds LV2).

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

Then rescan plug-ins in your DAW (restart it if it only scans on start-up).

## Uninstall

Delete the files you copied. User presets and logs live in the Vivisect user
data folder shown in the plug-in's Help page; delete that folder too to remove
everything.

## Presets

User presets are `.vsxpreset` files in the folder opened by
**MENU > Open Preset Folder**. Put presets you were sent directly in that
folder (not a sub-folder), then reopen the plug-in window.

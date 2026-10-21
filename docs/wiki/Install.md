# Install

Each release zip has VST3, CLAP and Standalone builds. Linux adds LV2.

## Windows

| File | Copy to |
|---|---|
| `Vivisect.vst3` (copy the whole folder) | `C:\Program Files\Common Files\VST3\` |
| `Vivisect.clap` | `C:\Program Files\Common Files\CLAP\` |
| `Vivisect.exe` (standalone) | anywhere, e.g. `C:\Program Files\Vivisect\` |

## Linux

| File | Copy to |
|---|---|
| `Vivisect.vst3` | `~/.vst3/` |
| `Vivisect.clap` | `~/.clap/` |
| `Vivisect.lv2` | `~/.lv2/` |
| `Vivisect` (standalone) | anywhere, `chmod +x Vivisect` if needed |

Rescan plug-ins in your DAW, or restart it if it only scans at start-up.

## Uninstall

Delete the files you copied. User presets and logs are in the Vivisect user data folder listed on the plug-in's Help page. Delete that too to remove everything.

## Presets

User presets are `.vsxpreset` files in the folder from **MENU > Open Preset Folder**. Put presets you were sent directly in that folder, not in a sub-folder, then reopen the window.

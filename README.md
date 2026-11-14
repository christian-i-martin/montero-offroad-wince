# Montero Off-Road Windows CE

First milestone: **open Total Commander from the existing SD navigation entry point**, while retaining access to AutoKit. This does not replace the factory TPMS screen, expose CAN data, or flash the head unit.

## What the repository contains

Inspection of initial commit `6244fc1aadf80e83df296d6e5256ab4245e096d5` found:

| File | Finding |
| --- | --- |
| `Navigation/Navigation.exe` | AutoKit renamed as the navigation executable. Its Git blob SHA (`0724152188983e82768750dd9d5a48f652d04ac8`) matches the reference project's `AutoKit/BIN/AutoKit.exe`. |
| `Navigation/*.dll` and other assets | Existing support files. Keep them in place for AutoKit. |
| `Navigator/Navigator.exe` | A different, 2,057,216-byte executable. It is **not** replaced by the default setup. |
| `GenieTek/` and `autokit/` | Existing content; left unchanged. |

The default target is therefore `Navigation/Navigation.exe`. The repository alone cannot prove which entry point the head unit actually launches. No CPU type or Windows CE version is assumed from its branding.

## Prepare the launcher on your computer

Requirements: **Python 3.10 or newer**, and internet access for three small reference binaries. Python runs on your computer, not on Windows CE. No third-party Python packages are needed.

From your local repository:

```sh
git pull --ff-only
python3 tools/prepare_total_commander.py
```

On Windows, use `py -3 tools/prepare_total_commander.py` instead.

This downloads the MortScript launcher, MortScript runtime, and Windows CE Total Commander from a pinned reference commit. It verifies their sizes and Git blob hashes, checks their PE/CPU-family headers against your existing executable, and creates:

```text
dist/total-commander-sd-overlay/
  LaunchTotalCommander/
    Arthur.exe
    Arthur.mscr
    MortScript.exe
    cecmd.exe
  Navigation/
    Navigation.exe       <- MortScript launcher, in the OUTPUT only
    Navigation.mscr      <- same basename as the launcher
    MortScript.exe
    cecmd.exe
    OriginalApp.exe      <- byte-for-byte copy of your existing Navigation.exe
  manifest.json          <- source revision and output checksums
```

**The existing repository executables are not overwritten.** A raw repository download is not a ready-to-run Total Commander installation; run the preparation command first. The result is an **overlay**, not a complete SD-card image.

The launcher is self-contained in each folder. It uses `SystemPath("ScriptPath")` instead of assuming `\SDMEM`, `\Storage Card`, or another mount name. No Ford map-update procedure is involved.

### Use the actual working SD backup when it differs from this repository

```sh
python3 tools/prepare_total_commander.py --source-root "/path/to/working-sd-backup"
```

The executable saved as `OriginalApp.exe` comes from this source. Retain a separate full-card backup too: the builder backs up the entry-point executable, **not the entire card**.

## Install on a test SD card

1. Back up your **entire working card**. Use a separate test card cloned from it. Do not format or overwrite the only working copy.
2. Copy the generated `LaunchTotalCommander` folder to the test card's root.
3. Copy the **five files inside the generated `Navigation` folder** into the test card's **existing** `Navigation` folder. Allow replacing `Navigation.exe`, but keep all existing DLLs and other assets. **Do not replace the whole folder** with the generated folder; that would remove files AutoKit needs. In macOS Finder, avoid a folder-level Replace operation.
4. With the car parked, insert the test card and open the same Navigation function that previously opened AutoKit.
5. A menu should offer **Total Commander**, **Original app / CarPlay**, and **Exit**. Choose Total Commander for filesystem inspection.

The `LaunchTotalCommander/Arthur.exe` entry point is also available directly if you later find a navigation-path selector or another way to launch a file. It opens Total Commander directly because that folder has no `OriginalApp.exe`.

Do not run `MortScript.exe` by itself to start this setup. Launch the matching stub (`Navigation.exe` or `Arthur.exe`), with its same-named `.mscr` beside it.

### First inspection

Browse and photograph the root folders, the SD mount name, and any system-information screen. Record the Windows CE version, resolution, and processor information when available. Do not delete or rename internal files, edit the registry, import `.reg` files, load drivers, or open vehicle communication ports.

**Total Commander is not a read-only file manager.** Our script contains no registry edits, process-killing, driver installation, or CAN commands, but the third-party applications can save settings and Total Commander can modify files. Use it only for viewing and copying information during this milestone.

## Troubleshooting and rollback

- **AutoKit opens immediately with no menu:** confirm the generated `Navigation/Navigation.exe` was copied, not the unmodified repository version. The head unit may use a different entry point; do not overwrite other executables by trial and error.
- **A script is missing:** the launcher and script must share a basename, such as `Navigation.exe` / `Navigation.mscr`; `MortScript.exe` must be alongside them.
- **An application is missing:** keep `cecmd.exe` alongside its launcher. Copy all five generated files into the existing Navigation folder.
- **Not a valid application / architecture mismatch:** stop and restore the working card. Do not force past compatibility checks. Matching CPU families still does not establish OS/API compatibility.
- **CarPlay or camera behavior changes:** exit the test setup and restore the full working SD backup before normal use. Runtime behavior and camera priority have not been validated on this head unit.
- **Output already exists:** preserve or remove that build yourself, or choose a new `--output` path. The builder deliberately refuses to overwrite an existing output.

For full rollback, restore the original working card. To undo only the default executable replacement on a computer, copy `Navigation/OriginalApp.exe` back over `Navigation/Navigation.exe`; the complete card backup remains the preferred recovery method.

### A different, confirmed navigation entry point

Only after identifying the actual path:

```sh
python3 tools/prepare_total_commander.py \
  --source-root "/path/to/untouched-sd-backup" \
  --entrypoint "ActualFolder/ActualName.exe" \
  --output "dist/alternate-entrypoint"
```

The builder generates matching `.exe` / `.mscr` names and preserves that original executable. The alternate app must be a compatible Windows CE application; preserving its bytes does not guarantee it tolerates being run under a different filename.

### Offline preparation

An existing copy of the exact reference-project files can be used without network access:

```sh
python3 tools/prepare_total_commander.py --upstream-dir "/path/to/Ford-WINCE-Autokit"
```

The same pinned hashes are required. A different binary version is intentionally rejected rather than silently trusted. See [THIRD_PARTY.md](THIRD_PARTY.md).

## Validation

```sh
python3 -m unittest discover -s tests -v
python3 -m py_compile tools/prepare_total_commander.py
```

The automated tests use **synthetic PE fixtures** to check packaging, preservation of the original executable, matching launcher/script filenames, CRLF output, manifests, architecture checks, and rejection of corrupt files or unsafe paths. They do not execute MortScript, Total Commander, or AutoKit and do not establish hardware compatibility. An actual parked-head-unit test is still required.

## Next milestone

After file-manager access is confirmed, identify the original vehicle-information application and its data interfaces. Steering angle, yaw, pitch/roll, elevation, and voltage access remain separate investigation work, not implemented capabilities of this launcher.

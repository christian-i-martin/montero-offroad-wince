# Montero Off-Road Windows CE

## Current method: replace the ENTIRE SD-card Navigation folder

The head unit previously launched CarPlay after its `Navigation` directory was replaced with AutoKit. This version follows that same method: **Total Commander itself becomes `Navigation/Navigation.exe`**. There is no MortScript launcher, selection menu, or CarPlay fallback.

**Use the GENERATED folder below, not this repository's original `Navigation` folder.** The original is still the uploaded AutoKit snapshot, retained for reference and CPU-family comparison. Pulling this repository alone does not change that executable or install anything on the SD card.

## 1. Build on your computer

Python 3.10+ is required. From the repository root:

```sh
git pull --ff-only
python3 tools/prepare_total_commander.py
```

On Windows use `py -3` instead of `python3`. The tool downloads only the pinned Total Commander/CE executable, verifies its length and Git blob hash, checks its PE headers, and compares its CPU family with `Navigation/Navigation.exe`. These checks are not proof of head-unit compatibility.

The output is:

```text
dist/total-commander-direct/
  Navigation/
    Navigation.exe                  <- Total Commander, 337,920 bytes
  LaunchTotalCommander/
    LaunchTotalCommander.exe        <- optional identical direct entry point
  INSTALL.txt
  THIRD_PARTY.txt
  manifest.json
```

The generated `Navigation` folder contains only `Navigation.exe`: a byte-for-byte copy of the reference `TC/cecmd.exe` under the filename your head unit expects. It has no AutoKit assets, `OriginalApp.exe`, MortScript, or scripts. Windows CE API and device compatibility still need a parked-head-unit test.

**The previous `dist/total-commander-sd-overlay` is obsolete. Do not use it.** If the new output directory already exists, choose a different `--output` path rather than overwriting it.

## 2. Replace the folder on your SD card

1. Keep a computer-side copy of the current SD `Navigation` folder.
2. Remove that old `Navigation` folder from the SD card.
3. Copy **`dist/total-commander-direct/Navigation`** to the card's root as `Navigation`.
4. Confirm the path is **`Navigation/Navigation.exe`**, not `Navigation/Navigation/Navigation.exe`.
5. Eject safely, insert the card, restart the head unit normally, and open Navigation while parked.

**Expected: Total Commander opens directly. No menu and no CarPlay.** This is now a complete folder replacement, NOT a merge with the old AutoKit directory. The optional generated `LaunchTotalCommander` folder is not needed when using the head unit's Navigation button.

Leave `Navigator`, `GenieTek`, and other SD folders alone. Do not change internal storage, flash firmware, use a Ford map-update procedure, or perform a factory reset.

## 3. Verify the actual SD card before testing

After copying, replace the example path below with the actual mounted card root:

```sh
python3 tools/prepare_total_commander.py --verify-sd "/Volumes/YOUR_SD_CARD"
```

Windows example:

```powershell
py -3 tools/prepare_total_commander.py --verify-sd "E:/"
```

This check is offline and read-only. It confirms that the card's `Navigation/Navigation.exe` is Total Commander, and reports when it is still AutoKit, the old MortScript launcher, or another executable. Success begins with `VERIFIED:`.

Expected length: **337,920 bytes**. Expected Git blob SHA-1:

```text
1925f097d2e2ef09516154b1d9638d38cc97a8c9
```

This is a Git blob hash, not the ordinary SHA-1 printed by `shasum`.

## If CarPlay still opens

First verify the actual SD card, then fully exit the old application/restart the head unit normally rather than just returning to its home screen. A verified replacement does not establish which path the head unit launches. If CarPlay still appears after a full restart, another entry point or an internal copy may be in use; investigate that rather than replacing `Navigator.exe` or internal files speculatively.

If an application error appears instead, photograph the exact wording. Missing Windows CE APIs or other runtime incompatibilities can prevent startup. Restore your saved folder if needed. No successful hardware test is claimed by these tools.

## First successful launch

Browse to filesystem root `\` and photograph the folder names with the path visible. Identify the SD mount using its known folders, then inspect internal-storage listings without executing unfamiliar applications. Total Commander is a writable file manager and may save settings: do not delete internal files, edit the registry, import `.reg` files, load drivers, or open vehicle communication ports.

This milestone only provides file-manager access. It does not replace the TPMS screen or implement access to CAN, voltage, elevation, steering angle, yaw, pitch, or roll.

## Offline build and validation

An existing copy of the exact pinned binary can be used without internet access:

```sh
python3 tools/prepare_total_commander.py --cecmd "/path/to/TC/cecmd.exe"
```

Alternatively use `--upstream-dir /path/to/Ford-WINCE-Autokit`. Both require the pinned hash. Use `--source-root /path/to/sd-backup` to compare against that backup's `Navigation/Navigation.exe` instead of the repository snapshot.

```sh
python3 -m unittest discover -s tests -v
python3 -m py_compile tools/prepare_total_commander.py
```

The 20 offline tests use synthetic PE fixtures. They cover complete-folder output, absence of menu/AutoKit fallback files, preservation of the source, corruption/CPU checks, read-only SD verification, manifests, error handling, and cleanup. They do not execute Windows CE software, validate a live binary download in this environment, or test the physical head unit. See `THIRD_PARTY.md` for provenance.

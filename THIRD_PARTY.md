# Third-party components and provenance

## Current direct replacement

The preparation tool retrieves only Total Commander/CE:

- Reference repository: https://github.com/typlo-gh/Ford-WINCE-Autokit
- Pinned commit: `986cf5464c61eae40d101985027fa24b2e6fde79`
- Path: `TC/cecmd.exe`
- Length: **337,920 bytes**
- Git blob SHA-1: `1925f097d2e2ef09516154b1d9638d38cc97a8c9`
- Publisher information and terms: https://www.ghisler.com/cecmd.htm

Total Commander/CE is copyrighted freeware by Christian Ghisler / C. Ghisler & Co. Its copyright remains with its author. This project does not claim authorship or relicense it. Obtain and use it subject to the publisher's terms.

The executable is not patched. The build creates renamed copies at `Navigation/Navigation.exe` and `LaunchTotalCommander/LaunchTotalCommander.exe`. It does not download maps, vehicle firmware, plugins, fonts, or AutoKit libraries.

The binary is downloaded on the user's computer for local preparation, not committed by the tool. Existing uploaded SD binaries remain a reference snapshot. Hash checks establish equality to the pinned reference, not a security audit or runtime compatibility. A matching CPU family does not prove the required Windows CE APIs are present.

## Superseded MortScript workflow

Commit `7fd8236465c5cf9cdd26dd3e3f49e8e5ec12fa7b` built an overlay using `LaunchTotalCommander/Arthur.exe`, `MortScript.exe`, and a selection-menu script. The direct replacement no longer uses these components. Do not install previously generated overlays for the current workflow.

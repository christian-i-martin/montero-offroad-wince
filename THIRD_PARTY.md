# Third-party components and provenance

The preparation tool retrieves only the following existing reference binaries:

Reference repository: https://github.com/typlo-gh/Ford-WINCE-Autokit

Pinned commit: `986cf5464c61eae40d101985027fa24b2e6fde79`

| Upstream path | Bytes | Git blob SHA-1 |
| --- | ---: | --- |
| `LaunchTotalCommander/Arthur.exe` | 8192 | `87277a137a845cbcc5572ed1308aaaa3ecdd1170` |
| `LaunchTotalCommander/MortScript.exe` | 94720 | `9d1993bdb9ded9fb5e60e6397cea2e285a2eb6bd` |
| `TC/cecmd.exe` | 337920 | `1925f097d2e2ef09516154b1d9638d38cc97a8c9` |

The original reference script hardcodes `\SDMEM\TC\cecmd.exe`. This project's script instead resolves executables beside itself and optionally provides access to the preserved original navigation application.

MortScript and Total Commander remain third-party software. This repository does not assert ownership of those applications or apply a new license to them. Obtain and use them subject to their respective terms. The preparation tool downloads binaries for local preparation; it does not commit them into this repository or publish them elsewhere. Existing user-provided files are left unchanged.

Hash verification establishes that the downloaded bytes match the pinned reference, not that the applications have received a security audit or will work on every Windows CE device. PE-header checks are not a substitute for validating Windows CE version, API support, and behavior on the actual head unit.

Technical references:

- Reference launcher: https://github.com/typlo-gh/Ford-WINCE-Autokit/blob/986cf5464c61eae40d101985027fa24b2e6fde79/LaunchTotalCommander/Arthur.mscr
- MortScript-derived source showing the Choice function's title, text, default, timeout, and entries arguments: https://github.com/hjgode/jscripts/blob/master/MortScript/FunctionsDialogs.cpp

No fonts, navigation maps, vehicle firmware, or additional AutoKit libraries are downloaded by the preparation tool.

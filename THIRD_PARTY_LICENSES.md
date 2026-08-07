# Third-party licenses

TradingApp itself is licensed under **GPL-3.0-or-later** (see [LICENSE](LICENSE)).
This file inventories everything the project links against or ships, and under
which licence — so a binary release can be audited without reading the build
scripts.

The full texts of the licences referenced here live in [`LICENSES/`](LICENSES/),
named by their SPDX identifier.

## Why the project is GPL-3.0-or-later and not something more permissive

It is not only a preference. **Qt Charts is offered under a commercial licence or
GPLv3 — there is no LGPL option for it** (the module's own documentation header
reads "Qt Charts | Commercial or GPLv3"). Linking it into a distributed binary
therefore requires that binary to be GPLv3-compatible. The rest of the Qt modules
used here are available under LGPLv3, which GPL-3.0-or-later also satisfies.

This is why the project moved from MIT to GPL-3.0-or-later: MIT was not a licence
this binary could actually be distributed under.

## Shipped with the binaries

These are bundled into the AppImage (`tools/package_appimage.sh`, via
linuxdeploy) and the Windows portable ZIP (`tools/package_portable.ps1`, via
windeployqt).

| Component | Version | Licence | SPDX | Notes |
|---|---|---|---|---|
| Qt Core | 6.x | LGPL-3.0-only or GPL-3.0-or-later or commercial | `LGPL-3.0-only` | |
| Qt Gui | 6.x | LGPL-3.0-only or GPL-3.0-or-later or commercial | `LGPL-3.0-only` | |
| Qt Widgets | 6.x | LGPL-3.0-only or GPL-3.0-or-later or commercial | `LGPL-3.0-only` | |
| Qt Network | 6.x | LGPL-3.0-only or GPL-3.0-or-later or commercial | `LGPL-3.0-only` | |
| Qt Concurrent | 6.x | LGPL-3.0-only or GPL-3.0-or-later or commercial | `LGPL-3.0-only` | |
| **Qt Charts** | 6.x | **GPL-3.0-only or commercial — no LGPL option** | `GPL-3.0-only` | the module that makes this app GPL |
| Qt platform plugin (xcb), TLS backend, image-format plugins | 6.x | LGPL-3.0-only | `LGPL-3.0-only` | pulled in by linuxdeploy's Qt plugin |

**OpenSSL is deliberately not bundled.** Qt loads the system `libssl` at run
time, so the distributed artefacts carry no OpenSSL code and no OpenSSL licence
obligation. See the note in `tools/package_appimage.sh`.

No third-party source code is vendored into this repository. Everything under
`src/` is original work covered by [LICENSE](LICENSE).

## Build-time and quality tooling — NOT shipped

These run in the pipeline and never form part of a distributed binary, so their
licences do not propagate to the artefacts. They are listed for completeness
because `./setup.sh` installs them.

| Tool | Role | Licence |
|---|---|---|
| cppcheck | static analysis | GPL-3.0-or-later |
| clang-tidy, Clang Static Analyzer, llvm-cov | static analysis, MC/DC coverage | Apache-2.0 WITH LLVM-exception |
| clazy | Qt-specific static analysis | LGPL-2.0-or-later |
| GCC (`-fanalyzer`, gcov) | compiler, static analysis, coverage | GPL-3.0-or-later WITH GCC-exception-3.1 |
| lizard | code metrics | MIT |
| PMD CPD | copy-paste detection | BSD-style (PMD licence) |
| codespell | spelling | GPL-2.0 |
| valgrind | runtime memory checking | GPL-2.0-or-later |
| StrictDoc | requirements as code | Apache-2.0 |
| Doxygen | API documentation | GPL-2.0 |
| PlantUML | diagrams | GPL-3.0-or-later |
| reportlab | the quality-report PDF | BSD-3-Clause |

### Licence-bound tooling (commercial, not shipped, not required to build)

| Tool | Role |
|---|---|
| Axivion Suite | MISRA C++ 2023 / CERT / CWE analysis and architecture verification |
| Squish for Qt | GUI test automation |
| Squish Coco | MC/DC, function and call coverage |
| Qt Test Center | test-result management |

Every stage that needs one of these reports `skipped` (exit code 3) when it is
absent, so the pipeline is fully runnable with open-source tooling only.

## Obligations when redistributing a binary

GPL-3.0-or-later requires that recipients of a binary can obtain the
corresponding source. This project satisfies that by attaching, to every GitHub
release:

- `TradingApp-<version>-source.tar.gz` — the complete corresponding source of
  that exact tag, produced by `git archive`;
- `LICENSE` (GPL-3.0-or-later) and the `LICENSES/` directory;
- this file.

If you redistribute the binaries elsewhere, carry the same three things with
them. For the Qt libraries bundled inside the artefacts, the LGPL's relinking
obligation is met because they are shipped as separate shared libraries that a
recipient can replace.

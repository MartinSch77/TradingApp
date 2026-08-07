# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/coverage.sh — structural-coverage measurement
    for the test suite.

.DESCRIPTION
      tools\coverage.ps1 [-Mode auto]  — run every back end that is available:
                                         Coco (if licensed) + clang-cl MC/DC
                                         + OpenCppCoverage. TWO independent
                                         MC/DC measurements when both are there.
      tools\coverage.ps1 -Mode coco    — Squish Coco (Qt Group, $CocoDir):
                                         source-instrumented build measuring
                                         statement/decision/condition coverage
                                         AND true MC/DC, and feeding CocoAI
                                         test-case suggestions
                                         -> coverage\coco\index.html (+ summary.csv)
      tools\coverage.ps1 -Mode coco-gui — the SQUISH GUI suite's own coverage,
                                         reported SEPARATELY from the unit numbers
                                         (its own instrumented tree WITH src\ui)
                                         -> coverage\coco-gui\index.html
      tools\coverage.ps1 -Mode mcdc    — clang-cl source-based coverage with
                                         MC/DC (-fcoverage-mcdc); llvm-cov HTML
                                         + console summary incl. the MC/DC column
                                         -> coverage\mcdc\index.html
      tools\coverage.ps1 -Mode msvc    — OpenCppCoverage over the ordinary MSVC
                                         Debug build; HTML + Cobertura XML with
                                         LINE coverage
                                         -> coverage\msvc\index.html

    Differences from the Linux script, stated rather than hidden:
     * gcov/lcov is a GCC toolchain and does not apply to an MSVC build. The
       equivalent free line-coverage measurement on Windows is OpenCppCoverage,
       which reads the PDBs — it reports LINE coverage but no BRANCH coverage.
       Branch and MC/DC coverage therefore come from the clang-cl/llvm-cov path
       (mode mcdc) and from Coco (mode coco), which are the more capable
       measurements anyway.
     * The clang-18 "unsupported MC/DC boolean expression" limit of 6 conditions
       was lifted upstream in clang 19, but the sources keep every decision at
       <= 6 conditions regardless so the Linux and Windows numbers stay
       comparable (see the hasAny() keyword-group helpers in EventInsight.cpp
       and signalAgainstPosition in MainWindow.cpp).
     * Coco's instrumenting front end parses C++ up to C++20, so its build tree
       alone is configured with -DCMAKE_CXX_STANDARD=20. Every other build —
       including everything that ships or is analyzed — stays on C++23.
     * auto runs the back ends side by side instead of Coco-or-nothing, because
       two MC/DC measurements from independent instrumentation techniques are
       worth more than one.

    Scope: coverage is reported for the domain + services sources (src\domain,
    src\services). The UI layer has no automated GUI tests yet — that gap is
    tracked in the traceability report, not hidden by excluding it silently.
#>
[CmdletBinding()]
param(
    [ValidateSet('auto', 'msvc', 'mcdc', 'coco', 'coco-gui', 'coco-components', 'coco-ai')][string]$Mode = 'auto',
    [string]$CocoDir = $(if ($env:COCO_DIR) { $env:COCO_DIR } else { 'C:\Program Files\squishcoco' })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$Jobs = Get-JobCount

$QtPrefix = Resolve-QtPrefix -Quiet
if (-not $QtPrefix) { Write-Error "no usable Qt kit found — set QT_PREFIX"; exit 2 }
Add-QtToPath $QtPrefix
$KitCxx = Initialize-KitToolchain -QtPrefix $QtPrefix
if ($null -eq $KitCxx) { exit 2 }

$Generator = 'Ninja'
if (-not (Test-Tool 'ninja')) { $Generator = $null }

function Invoke-Configure {
    param([string]$BuildDir, [string[]]$ExtraArgs = @())
    # No -Compiler here: several modes deliberately override the compiler
    # (clang-cl for MC/DC, the Coco wrapper for Coco), so the kit compiler is
    # not what those trees are configured with.
    Reset-StaleCMakeCache -BuildDir $BuildDir -SourceDir $Root -Generator $Generator -QtPrefix $QtPrefix
    $a = @('-S', $Root, '-B', $BuildDir)
    if ($Generator) { $a += @('-G', $Generator) }
    $a += @("-DCMAKE_PREFIX_PATH=$QtPrefix", '-DCMAKE_BUILD_TYPE=Debug')
    $a += $ExtraArgs
    return (Invoke-Native -FilePath 'cmake' -Arguments $a)
}

function Get-TestExecutables {
    param(
        [string]$BuildDir,
        # Only the tests that drive a COMPONENT through its real seams — the broker
        # client and the feeds against the in-process mock HTTP server, the advisors
        # against a mocked endpoint, the simulated broker, the learning loop against
        # the real trainer, configuration against real files. Marked type "I" in
        # docs/test_spec.md; the same list as component_tests() in tools/coverage.sh.
        [switch]$ComponentsOnly
    )
    $all = @(Get-ChildItem (Join-Path $BuildDir 'tests') -Filter 'tst_*.exe' -File -ErrorAction SilentlyContinue)
    if (-not $ComponentsOnly) { return $all }
    $wanted = @('tst_etoroclient', 'tst_marketfeeds', 'tst_jsonhttp', 'tst_simulationengine',
        'tst_aiadvisor', 'tst_ollamaadvisor', 'tst_botnet', 'tst_config',
        'tst_economiccalendar', 'tst_positionsmodel', 'tst_tradescript')
    return @($all | Where-Object { $wanted -contains $_.BaseName })
}

# Coco ships one wrapper executable per native compiler; pick the one matching
# the kit in use. (cscl wraps cl.exe, csg++ wraps g++, csclang-cl wraps clang-cl.)
function Get-CocoWrapper {
    if (Test-QtIsMsvcKit $QtPrefix) { return (Join-Path $CocoDir 'cscl.exe') }
    return (Join-Path $CocoDir 'csg++.exe')
}

# Coco's options travel inside CMAKE_CXX_FLAGS — one space-separated string —
# so a path containing spaces ("C:\Program Files\Qt\…") cannot be passed
# literally. Spaces become '*' wildcards, which is harmless for exclude filters.
function ConvertTo-CocoWildcard {
    param([Parameter(Mandatory)][string]$Path)
    return ($Path -replace '\s', '*') + '*'
}

# Coco is usable when the matching compiler wrapper exists and the license is valid.
function Test-CocoUsable {
    if (-not (Test-Path (Get-CocoWrapper))) { return $false }
    $lic = Join-Path $CocoDir 'cocolic.exe'
    if (-not (Test-Path $lic)) { return $false }
    & $lic --check *> $null
    return ($LASTEXITCODE -eq 0)
}

# ---------------------------------------------------------------------------
# OpenCppCoverage — line coverage over the ordinary MSVC Debug build
# ---------------------------------------------------------------------------
function Invoke-MsvcCoverage {
    Write-Stage 'OpenCppCoverage (line coverage, MSVC)'
    if (-not (Test-Tool 'OpenCppCoverage')) {
        Write-Skip "OpenCppCoverage not installed — https://github.com/OpenCppCoverage/OpenCppCoverage/releases"
        return $false
    }
    $build = Join-Path $Root 'build-cov-msvc'
    if (-not (Invoke-Configure -BuildDir $build)) { return $false }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) { return $false }

    $out = Join-Path $Root 'coverage\msvc'
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    # One OpenCppCoverage run per test binary, merged via --input_coverage.
    $exes = Get-TestExecutables $build
    if ($exes.Count -eq 0) { Write-Warning "no test executables in $build\tests"; return $false }

    $covFiles = @()
    foreach ($exe in $exes) {
        $cov = Join-Path $env:TEMP "$($exe.BaseName).cov"
        & OpenCppCoverage --quiet `
            --sources "$Root\src\domain" --sources "$Root\src\services" `
            --excluded_sources "$Root\tests" `
            --export_type "binary:$cov" `
            -- $exe.FullName *> $null
        if (Test-Path $cov) { $covFiles += $cov }
    }
    if ($covFiles.Count -eq 0) { Write-Warning "OpenCppCoverage produced no data"; return $false }

    $covArgs = @('--quiet', '--sources', "$Root\src\domain", '--sources', "$Root\src\services",
        '--export_type', "html:$out", '--export_type', "cobertura:$out\cobertura.xml")
    foreach ($c in $covFiles) { $covArgs += @('--input_coverage', $c) }
    & OpenCppCoverage @covArgs
    Remove-Item $covFiles -Force -ErrorAction SilentlyContinue
    Write-Host "HTML: $out\index.html   (line coverage; branch/MC-DC come from the mcdc mode)"
    return $true
}

# ---------------------------------------------------------------------------
# clang-cl + llvm-cov — branch AND MC/DC coverage
# ---------------------------------------------------------------------------
function Invoke-McdcCoverage {
    Write-Stage 'clang-cl source-based coverage with MC/DC'
    # $env:LIB is restored on the way out (see Add-ToLibPath below). build_all.ps1
    # runs every stage in ONE PowerShell process, so leaking LLVM's compiler-rt
    # directory into later stages is not harmless: that directory also holds LLVM's
    # clang_rt.asan_dynamic-x86_64.lib, and the `sanitize` stage that runs after
    # `coverage` then linked its ASan binaries against the wrong ASan ABI — every
    # tst_*.exe died at startup with STATUS_ENTRYPOINT_NOT_FOUND. The ASan stage
    # defends itself too, but the leak is fixed here at the source.
    $savedLib = $env:LIB
    try {
        return (Invoke-McdcCoverageInner)
    } finally {
        $env:LIB = $savedLib
    }
}

function Invoke-McdcCoverageInner {
    $llvm = Get-LlvmToolset
    if (-not $llvm) {
        Write-Skip "no complete LLVM toolset found (needs clang-cl + llvm-cov + llvm-profdata; winget install LLVM.LLVM) — MC/DC coverage unavailable"
        return $false
    }
    Write-Host "LLVM toolset: $($llvm.Bin) (clang $($llvm.Version))" -ForegroundColor DarkGray

    $profileDir = Get-ClangRuntimeDir -Toolset $llvm -LibName 'clang_rt.profile-x86_64.lib'
    if (-not $profileDir) {
        Write-Skip "clang_rt.profile-x86_64.lib not found in $($llvm.Root) — MC/DC coverage unavailable"
        return $false
    }
    # The runtime goes on the linker's search path, not into a linker flag:
    # its path contains spaces and backslashes, and CMake strips the
    # backslashes out of CMAKE_EXE_LINKER_FLAGS (lld-link then reports
    # "could not open 'C:Program'").
    Add-ToLibPath $profileDir

    $build = Join-Path $Root 'build-cov-mcdc'
    $clangCl = $llvm.ClangCl -replace '\\', '/'
    # C++-only project: set only the CXX compiler (a -DCMAKE_C_COMPILER would
    # draw a "Manually-specified variables were not used" warning).
    $ok = Invoke-Configure -BuildDir $build -ExtraArgs @(
        "-DCMAKE_CXX_COMPILER=$clangCl",
        '-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping -Xclang -fcoverage-mcdc',
        '-DCMAKE_EXE_LINKER_FLAGS=clang_rt.profile-x86_64.lib'
    )
    if (-not $ok) { return $false }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) { return $false }

    $out = Join-Path $Root 'coverage\mcdc'
    if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
    Get-ChildItem $out -Filter '*.profraw' -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item (Join-Path $out 'merged.profdata') -Force -ErrorAction SilentlyContinue

    $exes = Get-TestExecutables $build
    if ($exes.Count -eq 0) { Write-Warning "no test executables in $build\tests"; return $false }
    foreach ($exe in $exes) {
        $env:LLVM_PROFILE_FILE = Join-Path $out "$($exe.BaseName).profraw"
        & $exe.FullName *> $null
    }
    Remove-Item env:LLVM_PROFILE_FILE -ErrorAction SilentlyContinue

    $profraw = @(Get-ChildItem $out -Filter '*.profraw' | ForEach-Object { $_.FullName })
    if ($profraw.Count -eq 0) { Write-Warning "no .profraw written — the instrumented binaries did not produce profiles"; return $false }
    & $llvm.LlvmProfdata merge -sparse @profraw -o (Join-Path $out 'merged.profdata')
    if ($LASTEXITCODE -ne 0) { return $false }

    # llvm-cov takes the first binary positionally and the rest via -object.
    $bins = @()
    for ($i = 0; $i -lt $exes.Count; $i++) {
        if ($i -eq 0) { $bins += $exes[$i].FullName } else { $bins += @('-object', $exes[$i].FullName) }
    }
    $sources = @()
    foreach ($pat in @('src\domain\*.cpp', 'src\domain\*.h', 'src\services\*.cpp', 'src\services\*.h')) {
        $sources += @(Get-ChildItem (Join-Path $Root $pat) -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    }

    & $llvm.LlvmCov report @bins -instr-profile (Join-Path $out 'merged.profdata') `
        --show-mcdc-summary @sources | Tee-Object -FilePath (Join-Path $out 'summary.txt')
    & $llvm.LlvmCov show @bins -instr-profile (Join-Path $out 'merged.profdata') `
        --show-mcdc --show-branches=count --format=html `
        --output-dir=$out @sources
    Write-Host "HTML: $out\index.html   (MC/DC column in summary.txt and per-file views)"
    return $true
}

# ---------------------------------------------------------------------------
# Squish Coco
# ---------------------------------------------------------------------------
function Invoke-CocoCoverage {
    Write-Stage 'Squish Coco (statement/decision/condition + true MC/DC)'
    if (-not (Test-CocoUsable)) {
        # Reported, not failed: Coco is license-bound and setup.ps1 cannot
        # install it. -Mode coco turns this into exit 3 ("stage skipped").
        Write-Skip "Squish Coco not usable - $(Get-CocoWrapper) missing or license invalid"
        Write-Host "         (check: `"$CocoDir\cocolic.exe`" --check). License-bound; .\setup.ps1 cannot install it." -ForegroundColor DarkGray
        return $false
    }
    $wrapper = Get-CocoWrapper
    Write-Host "Coco wrapper: $wrapper" -ForegroundColor DarkGray

    $build = if ($ComponentsOnly) { Join-Path $Root 'build-cov-coco-components' }
    else { Join-Path $Root 'build-cov-coco' }

    # The wrapper drives the native compiler; instrumentation only happens with
    # --cs-on. --cs-mcdc adds MC/DC, --cs-mcc multiple-condition coverage.
    #
    # The excludes are not cosmetic. Left to itself Coco also instruments the
    # inline/template code in the Qt, MSVC STL and Windows SDK headers, and each
    # translation unit instantiates those templates differently — cmmerge then
    # reports "source file qmetatype.h is differently instrumented in the
    # database" for every test binary and cmreport crashes on the result. Keeping
    # instrumentation to src\domain + src\services also matches the scope of the
    # gcov/mcdc reports on Linux.
    $csflags = @('--cs-on', '--cs-mcdc', '--cs-mcc')
    # In component mode src\ui stays instrumented-out too (the suite drives no GUI),
    # so the exclude list is the same either way — what differs is WHICH tests run.
    foreach ($p in @($QtPrefix, $build, "$Root\tests", "$Root\src\ui")) {
        $csflags += "--cs-exclude-file-abs-wildcard=$(ConvertTo-CocoWildcard $p)"
    }
    # Toolchain headers, wherever they are installed.
    $csflags += '--cs-exclude-file-abs-wildcard=*Microsoft*Visual*Studio*'
    $csflags += '--cs-exclude-file-abs-wildcard=*Windows*Kits*'
    $csflags = $csflags -join ' '

    # Coco's instrumenting front end parses the translation unit itself and, as
    # of the 2025-11 build, does not understand C++23: with /std:c++latest it
    # aborts with "Could not insert instrumentation in file …" after a wall of
    # "syntax error, unexpected requires" against the Qt headers. C++20 parses
    # cleanly, so this ONE build tree is configured down to C++20 (CMakeLists
    # honours the override). Every other build stays on C++23.
    $extra = @(
        "-DCMAKE_CXX_COMPILER=$($wrapper -replace '\\','/')",
        "-DCMAKE_CXX_FLAGS=$csflags",
        '-DCMAKE_CXX_STANDARD=20'
    )
    # Instrumented objects reference a per-TU coverage table (__cs_tb_…) that
    # Coco's own librarian and linker emit. BOTH have to be wrapped: this
    # project links the domain and services layers as static libraries, so
    # archiving them with plain lib.exe drops the tables and every test binary
    # then fails with LNK2001 on __cs_tb_*.
    if (Test-QtIsMsvcKit $QtPrefix) {
        $extra += "-DCMAKE_AR=$((Join-Path $CocoDir 'cslib.exe') -replace '\\','/')"
        $extra += "-DCMAKE_LINKER=$((Join-Path $CocoDir 'cslink.exe') -replace '\\','/')"
    }

    if (-not (Invoke-Configure -BuildDir $build -ExtraArgs $extra)) { return $false }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) { return $false }

    $out = Join-Path $Root 'coverage\coco'
    if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
    Remove-Item (Join-Path $out 'merged.csmes') -Force -ErrorAction SilentlyContinue

    # The instrumented binary writes its execution report as <exe>.csexe into
    # the CURRENT WORKING DIRECTORY, not next to the executable — so each test
    # is run from the tests directory, otherwise the .csexe files pile up in
    # the repository root and the import below finds nothing.
    $testDir = Join-Path $build 'tests'
    $exes = Get-TestExecutables $build -ComponentsOnly:$ComponentsOnly
    if ($exes.Count -eq 0) { Write-Warning "no test executables in $testDir"; return $false }

    $imported = 0
    foreach ($exe in $exes) {
        # Coco names the instrumentation database "<exe-with-suffix>.csmes",
        # i.e. tst_indicators.exe.csmes.
        $csmesFile = "$($exe.FullName).csmes"
        $csexeFile = "$($exe.FullName).csexe"
        Remove-Item $csexeFile -Force -ErrorAction SilentlyContinue
        Invoke-Native -FilePath $exe.FullName -WorkingDirectory $testDir | Out-Null
        if ((Test-Path $csexeFile) -and (Test-Path $csmesFile)) {
            & (Join-Path $CocoDir 'cmcsexeimport.exe') -m $csmesFile -e $csexeFile -t $exe.BaseName
            if ($LASTEXITCODE -eq 0) { $imported++ }
        } else {
            Write-Warning "no execution report for $($exe.BaseName) (expected $csexeFile)"
        }
    }
    if ($imported -eq 0) {
        Write-Warning "no execution data imported — cmreport would have nothing to report"
        return $false
    }

    $csmes = @(Get-ChildItem $testDir -Filter '*.csmes' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
    if ($csmes.Count -eq 0) { Write-Warning "no .csmes produced — instrumentation did not run"; return $false }
    & (Join-Path $CocoDir 'cmmerge.exe') -o (Join-Path $out 'merged.csmes') @csmes
    if ($LASTEXITCODE -ne 0) { return $false }

    # --coverage-mcdc puts the MC/DC level into the report; --select picks the
    # executions to include (all of them here — one per test binary).
    & (Join-Path $CocoDir 'cmreport.exe') -m (Join-Path $out 'merged.csmes') `
        '--select=.*' "--html=$out\index.html" --coverage-mcdc `
        '--title=TradingApp structural coverage incl. MC/DC (Squish Coco)'
    if ($LASTEXITCODE -ne 0) { Write-Warning "cmreport (HTML) exited $LASTEXITCODE"; return $false }
    # CSV alongside it, so the stage leaves a machine-readable summary too — as a
    # SECOND call, because cmreport writes one output file per invocation and
    # rejects --html and --csv-excel together ("Multiple output files defined").
    & (Join-Path $CocoDir 'cmreport.exe') -m (Join-Path $out 'merged.csmes') `
        '--select=.*' "--csv-excel=$out\summary.csv" --coverage-mcdc *> $null

    # Every level the merged database can answer, on the console. `--stat` is the
    # switch that prints a number (`--text=` writes a 0-byte file whatever sections
    # it is given — measured on 7.2.0), and the four levels are the reason Coco is
    # here: gcov reports lines, and a covered line can still hide an untested
    # condition combination.
    foreach ($level in @(
            @{ Name = 'statement'; Switch = '--coverage-statement-block' },
            @{ Name = 'decision'; Switch = '--coverage-decision' },
            @{ Name = 'condition'; Switch = '--coverage-condition' },
            @{ Name = 'mcdc'; Switch = '--coverage-mcdc' })) {
        $stat = & (Join-Path $CocoDir 'cmreport.exe') -m (Join-Path $out 'merged.csmes') `
            '--select=.*' $level.Switch --stat 2>$null
        $value = ($stat -join '').Trim()
        if (-not $value) { $value = 'unavailable' }
        Write-Host ('  {0,-10} {1}' -f $level.Name, $value)
    }

    Write-Host "imported $imported execution reports"
    Write-Host "HTML: $out\index.html   (open merged.csmes in coveragebrowser for MC/DC drill-down)"
    return $true
}

function Invoke-CocoGuiCoverage {
    # What the SQUISH GUI suite covers, reported SEPARATELY from the unit/integration
    # numbers - the Linux counterpart is `tools/coverage.sh coco-gui`, and the reasoning is
    # the same: the two suites answer different questions, so one blended percentage would
    # answer neither, and blending lets a well-covered domain hide an untouched UI.
    #
    # Two differences from Invoke-CocoCoverage, both deliberate: src\ui IS instrumented
    # here (excluding it would measure the GUI suite everywhere except the GUI), and the
    # instrumented artefact is the APP rather than the test binaries, because the Squish
    # suite drives TradingApp.
    Write-Stage 'Squish Coco - GUI suite only (separate from the unit suite)'
    if (-not (Test-CocoUsable)) {
        Write-Skip "Squish Coco not usable - $(Get-CocoWrapper) missing or license invalid"
        return $false
    }
    $wrapper = Get-CocoWrapper
    $build = Join-Path $Root 'build-cov-coco-gui'
    $csflags = @('--cs-on', '--cs-mcdc', '--cs-mcc')
    # Note the absence of "$Root\src\ui" from this list - that is the point of the mode.
    foreach ($p in @($QtPrefix, $build, "$Root\tests")) {
        $csflags += "--cs-exclude-file-abs-wildcard=$(ConvertTo-CocoWildcard $p)"
    }
    $csflags += '--cs-exclude-file-abs-wildcard=*Microsoft*Visual*Studio*'
    $csflags += '--cs-exclude-file-abs-wildcard=*Windows*Kits*'
    $csflags = $csflags -join ' '
    $extra = @(
        "-DCMAKE_CXX_COMPILER=$($wrapper -replace '\\','/')",
        "-DCMAKE_CXX_FLAGS=$csflags",
        '-DCMAKE_CXX_STANDARD=20'
    )
    if (Test-QtIsMsvcKit $QtPrefix) {
        $extra += "-DCMAKE_AR=$((Join-Path $CocoDir 'cslib.exe') -replace '\\','/')"
        $extra += "-DCMAKE_LINKER=$((Join-Path $CocoDir 'cslink.exe') -replace '\\','/')"
    }
    if (-not (Invoke-Configure -BuildDir $build -ExtraArgs $extra)) { return $false }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) { return $false }

    $app = Join-Path $build 'TradingApp.exe'
    if (-not (Test-Path $app)) { Write-Warning "no instrumented app at $app"; return $false }
    $out = Join-Path $Root 'coverage\coco-gui'
    if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force -Path $out | Out-Null }
    Remove-Item (Join-Path $out 'merged.csmes') -Force -ErrorAction SilentlyContinue

    # Under Squish the AUT's working directory is squishserver's, not ours, so the
    # execution report is PINNED rather than guessed: COVERAGESCANNER_ARGS is read by the
    # Coco runtime inside the AUT however it was started.
    $csexe = Join-Path $out 'squish-gui.csexe'
    Remove-Item $csexe -Force -ErrorAction SilentlyContinue
    $env:COVERAGESCANNER_ARGS = "--cs-exec=$csexe"
    $rc = 0
    try {
        & (Join-Path $Root 'tools\squish_run.ps1') -BuildDir (Split-Path $build -Leaf)
        $rc = $LASTEXITCODE
    } finally {
        Remove-Item Env:\COVERAGESCANNER_ARGS -ErrorAction SilentlyContinue
    }
    if ($rc -eq 3) { Write-Skip 'the Squish GUI suite did not run - no GUI coverage to report'; return $false }
    if (-not (Test-Path $csexe)) {
        # Reported as missing, never shown as 0% - an absent measurement is not untested code.
        Write-Warning "the GUI run produced no execution report ($csexe)"
        return $false
    }
    $csmes = "$app.csmes"
    & (Join-Path $CocoDir 'cmcsexeimport.exe') -m $csmes -e $csexe -t 'squish-suite_gui'
    if ($LASTEXITCODE -ne 0) { return $false }
    & (Join-Path $CocoDir 'cmmerge.exe') -o (Join-Path $out 'merged.csmes') $csmes
    if ($LASTEXITCODE -ne 0) { return $false }
    & (Join-Path $CocoDir 'cmreport.exe') -m (Join-Path $out 'merged.csmes') `
        '--select=.*' "--html=$out\index.html" --coverage-mcdc `
        '--title=TradingApp - Squish GUI suite coverage (separate from the unit suite)'
    & (Join-Path $CocoDir 'cmreport.exe') -m (Join-Path $out 'merged.csmes') `
        '--select=.*' "--csv-excel=$out\summary.csv" --coverage-mcdc *> $null
    & (Join-Path $CocoDir 'cmreport.exe') -m (Join-Path $out 'merged.csmes') `
        '--select=.*' "--junit=$Root\test-results\coco-gui.xml" *> $null
    Write-Host "GUI coverage (Squish suite only): $out\index.html"
    Write-Host "unit/integration coverage stays in coverage\coco - the two are never merged"
    return $true
}

# ---------------------------------------------------------------------------

switch ($Mode) {
    # Exit 3 = "stage skipped": the back end is simply not installed (or, for
    # Coco, not licensed). Exit 1 is reserved for a back end that IS present and
    # then failed. build_all.ps1 treats 3 as `skipped`, not as a failure.
    'msvc' {
        if (Invoke-MsvcCoverage) { exit 0 }
        if (-not (Test-Tool 'OpenCppCoverage')) { exit 3 }
        exit 1
    }
    'mcdc' {
        if (Invoke-McdcCoverage) { exit 0 }
        if (-not (Get-LlvmToolset)) { exit 3 }
        exit 1
    }
    'coco' {
        if (Invoke-CocoCoverage) { exit 0 }
        if (-not (Test-CocoUsable)) { exit 3 }
        exit 1
    }
    'coco-gui' {
        if (Invoke-CocoGuiCoverage) { exit 0 }
        # Not installed/licensed, or the licence-bound GUI suite did not run: both are
        # "skipped" (exit 3), never a build gate.
        exit 3
    }
    'auto' {
        # Unlike the Linux script, auto does not treat Coco and the free
        # toolchain as either/or: both measure MC/DC, by different means (Coco
        # instruments the source, clang instruments the IR), so running both
        # gives two independent MC/DC numbers to cross-check — the same
        # philosophy as the four static analyzers. A back end that is absent is
        # reported and skipped; the stage fails only if NONE produced a report.
        $done = @()
        if (Test-CocoUsable) {
            Write-Host "auto: Squish Coco found at $CocoDir with a valid license"
            if (Invoke-CocoCoverage) { $done += 'coco' }
        } else {
            Write-Stage 'Squish Coco'
            Write-Skip "$CocoDir missing or license invalid (check: `"$CocoDir\cocolic.exe`" --check)"
        }
        if (Invoke-McdcCoverage) { $done += 'mcdc' }
        if (Invoke-MsvcCoverage) { $done += 'msvc' }

        Write-Host ""
        if ($done.Count -eq 0) {
            Write-Error "no coverage back end available — install LLVM (winget install LLVM.LLVM), OpenCppCoverage, or a licensed Squish Coco"
            exit 1
        }
        Write-Host "coverage measured by: $($done -join ', ')" -ForegroundColor Green
        if ($done -contains 'coco') { Write-Host "  MC/DC (Coco):     coverage\coco\index.html + summary.csv" }
        if ($done -contains 'mcdc') { Write-Host "  MC/DC (llvm-cov): coverage\mcdc\index.html + summary.txt" }
        if ($done -contains 'msvc') { Write-Host "  line (OpenCppCov): coverage\msvc\index.html" }
        exit 0
    }
}

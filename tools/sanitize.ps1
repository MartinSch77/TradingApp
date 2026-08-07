# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of tools/sanitize.sh — dynamic runtime-error evidence
    over the test suite.

.DESCRIPTION
    Which checkers exist here differs from Linux, so the mapping is explicit:

      asan     MSVC build with AddressSanitizer (/fsanitize=address).
               Detects out-of-bounds, use-after-free/return/scope and double
               free. NOTE: the MSVC ASan has no LeakSanitizer — leaks are not
               reported on Windows.
      ubsan    clang-cl build with UndefinedBehaviorSanitizer, when LLVM is
               installed. Skipped (reported, not hidden) when it is not.
      tsan     NOT AVAILABLE. ThreadSanitizer has no Windows target in either
               MSVC or upstream LLVM. The race evidence for this project comes
               from the Linux TSan run; on Windows the concurrency requirements
               (REQ-N-006) are covered by Axivion's concurrency rules only.
      valgrind NOT AVAILABLE on Windows. ASan covers the memory-error classes;
               for leak checking specifically, run the Linux pipeline or use
               Application Verifier / Dr. Memory manually.
      all      run everything that IS available (asan, then ubsan)

    Every mode writes its raw output to analysis-results\sanitize-<mode>.raw.txt
    and a normalized findings file analysis-results\sanitize-<mode>.txt
    (file|line|severity|id|message, via tools\parse_sanitizer_log.py) that
    axivion/external_import.py brings onto the dashboard. A clean run leaves an
    empty findings file — the dashboard then shows nothing for that provider.

    A clean run demonstrates the absence of these error classes ON THE EXECUTED
    PATHS (the test suite). This is EVIDENCE, not PROOF — see docs\verification.md.

.EXAMPLE
    tools\sanitize.ps1
    tools\sanitize.ps1 -Mode asan
#>
[CmdletBinding()]
param([ValidateSet('asan', 'ubsan', 'tsan', 'valgrind', 'all')][string]$Mode = 'all')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\common.ps1"
$Root = Get-RepoRoot
$Jobs = Get-JobCount
$Out = Join-Path $Root 'analysis-results'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

$QtPrefix = Resolve-QtPrefix -Quiet
if (-not $QtPrefix) { Write-Error "no usable Qt kit found — set QT_PREFIX"; exit 2 }
Add-QtToPath $QtPrefix
if ($null -eq (Initialize-KitToolchain -QtPrefix $QtPrefix)) { exit 2 }

$Generator = 'Ninja'
if (-not (Test-Tool 'ninja')) { $Generator = $null }

# normalize <mode>: raw log -> pipe-format findings file for the dashboard
function Invoke-Normalize {
    param([string]$Name)
    Invoke-Python -Arguments @(
        (Join-Path $Root 'tools\parse_sanitizer_log.py'), $Name,
        (Join-Path $Out "sanitize-$Name.raw.txt"),
        (Join-Path $Out "sanitize-$Name.txt"), $Root) | Out-Null
}

function Invoke-Configure {
    param([string]$BuildDir, [string[]]$ExtraArgs)
    Reset-StaleCMakeCache -BuildDir $BuildDir -SourceDir $Root -Generator $Generator -QtPrefix $QtPrefix
    $a = @('-S', $Root, '-B', $BuildDir)
    if ($Generator) { $a += @('-G', $Generator) }
    $a += @("-DCMAKE_PREFIX_PATH=$QtPrefix", '-DCMAKE_BUILD_TYPE=Debug')
    $a += $ExtraArgs
    return (Invoke-Native -FilePath 'cmake' -Arguments $a)
}

# Run ctest and tee the combined output into the raw log.
function Invoke-CtestTee {
    param([string]$BuildDir, [string]$RawLog, [int]$Timeout = 600)
    $prev = (Get-Location).Path
    Set-Location $BuildDir
    try {
        & ctest --output-on-failure --timeout $Timeout 2>&1 | Tee-Object -FilePath $RawLog
        return ($LASTEXITCODE -eq 0)
    } finally { Set-Location $prev }
}

# ---------------------------------------------------------------------------

function Invoke-Asan {
    Write-Stage 'AddressSanitizer (MSVC /fsanitize=address)'
    if (-not (Test-Tool 'cl')) {
        Write-Skip "no MSVC toolset — ASan needs the Visual C++ compiler"
        Write-TextFile (Join-Path $Out 'sanitize-asan.txt') ''
        return $false
    }
    $build = Join-Path $Root 'build-san'
    # /RTC1 (the CMake Debug default) is rejected together with /fsanitize=address,
    # and ASan requires non-incremental linking — both are overridden here rather
    # than appended, so the flags actually reach the compiler in the right order.
    #
    # /fsanitize=address MUST be on the LINK line too, not only when compiling.
    # It is what makes the linker pull in clang_rt.asan_dynamic_runtime_thunk,
    # which exports operator new/delete from the executable. Without it the exe
    # still imports clang_rt.asan_dynamic-x86_64.dll, but that DLL imports
    # operator delete back out of the main module and cannot find it — Windows
    # then refuses to start the process with
    #   "The procedure entry point ??3@YAXPEAX_K@Z could not be located in <exe>".
    #
    # Both overrides need a CONFIG-SPECIFIC counterpart, or CMake appends its own
    # Debug defaults AFTER ours on the same line and the last flag wins:
    #   * CMAKE_EXE_LINKER_FLAGS_DEBUG defaults to "/debug /INCREMENTAL", which
    #     landed after our /INCREMENTAL:NO and turned incremental linking back on.
    #     That is what actually broke every tst_*.exe: the resulting binary cannot
    #     bind operator delete against the ASan runtime, so Windows kills it before
    #     main() with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) — the
    #     "??3@YAXPEAX_K@Z could not be located" dialog. Verified by A/B: identical
    #     flags apart from /INCREMENTAL:NO turn exit 0xC0000139 into exit 0.
    #     So the non-incremental request has to be made in the Debug-specific
    #     variable, where nothing can append past it.
    #   * /RTC1 moved out of CMAKE_CXX_FLAGS_DEBUG into its own abstraction in
    #     CMake 4.0 (CMAKE_MSVC_RUNTIME_CHECKS, policy CMP0197), so overriding
    #     CMAKE_CXX_FLAGS_DEBUG silently stopped removing it and ASan builds were
    #     compiling WITH runtime checks again. Clearing that variable is now the
    #     only way ("" = no runtime checks). This one is hygiene, not the crash fix:
    #     MSVC 14.44 accepts /RTC1 next to /fsanitize=address, but the two
    #     instrument the same stack and /RTC1 is not wanted in a sanitizer build.
    # Pin the ASan import library to the MSVC toolset, whatever $env:LIB already holds.
    #
    # There are TWO incompatible ASan implementations installed side by side, both
    # shipping a file called clang_rt.asan_dynamic-x86_64.lib:
    #   * MSVC's       -> the exe imports __asan_delete / __asan_delete_size
    #   * LLVM/clang's -> the exe imports the mangled ??2@YAPEAX_K@Z / ??3@YAXPEAX_K@Z
    # Whichever the LINKER picks decides which ABI the binary expects, while the DLL
    # that LOADS at startup is MSVC's. Link against LLVM's by accident and every
    # tst_*.exe dies before main() with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) —
    # the modal "??3@YAXPEAX_K@Z ... nicht gefunden" dialog, which HANGS the run.
    #
    # This is not hypothetical: tools\coverage.ps1 calls Add-ToLibPath on LLVM's
    # compiler-rt directory to find clang_rt.profile-x86_64.lib, that same directory
    # also contains LLVM's clang_rt.asan_dynamic-x86_64.lib, and build_all.ps1 runs
    # every stage in ONE PowerShell process with `coverage` BEFORE `sanitize`. So a
    # full build_all run poisoned the ASan link while `sanitize.ps1` on its own was
    # always fine — which is exactly how this hid for so long.
    #
    # MSVC's copy lives in VC\Tools\MSVC\<ver>\lib\x64; every clang compiler-rt
    # directory has \lib\clang\<major>\lib\windows in its path. Dropping the latter
    # for the duration of this stage leaves the MSVC one to win.
    $savedLib = $env:LIB
    try {
        $env:LIB = (($env:LIB -split ';') |
            Where-Object { $_ -and ($_ -notmatch '[\\/]lib[\\/]clang[\\/]') }) -join ';'

        $ok = Invoke-Configure -BuildDir $build -ExtraArgs @(
            '-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od',
            '-DCMAKE_MSVC_RUNTIME_CHECKS=',
            '-DCMAKE_CXX_FLAGS=/fsanitize=address /Oy-',
            '-DCMAKE_EXE_LINKER_FLAGS=/fsanitize=address /DEBUG',
            '-DCMAKE_EXE_LINKER_FLAGS_DEBUG=/debug /INCREMENTAL:NO'
        )
        if (-not $ok) { return $false }
        if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) { return $false }
    } finally {
        $env:LIB = $savedLib
    }

    # Put the ASan runtime NEXT TO the binaries instead of trusting PATH.
    #
    # An ASan-instrumented exe loads clang_rt.asan_dynamic-x86_64.dll at startup,
    # and this machine class carries several incompatible copies (MSVC Hostx64 and
    # Hostx86, the VS-bundled LLVM, a standalone LLVM). Whichever one PATH resolves
    # first decides whether the process starts at all:
    #   * none on PATH        -> exit 0xC0000135 STATUS_DLL_NOT_FOUND
    #   * a MISMATCHED copy   -> exit 0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND, i.e.
    #                            the "??3@YAXPEAX_K@Z ... nicht gefunden" dialog
    # Both are MODAL dialogs, so an interactive run HANGS instead of failing, and
    # ctest reports nothing useful. The executable's own directory is searched
    # before PATH, so copying the runtime that belongs to the cl.exe we just built
    # with makes the run deterministic no matter how the shell is set up (developer
    # prompt or not, LLVM installed or not).
    $clPath = (Get-Command cl -ErrorAction SilentlyContinue).Source
    if ($clPath) {
        $asanDll = Join-Path (Split-Path $clPath -Parent) 'clang_rt.asan_dynamic-x86_64.dll'
        if (Test-Path $asanDll) {
            foreach ($dir in @($build, (Join-Path $build 'tests'))) {
                if (Test-Path $dir) { Copy-Item $asanDll $dir -Force }
            }
            Write-Host "ASan runtime staged next to the binaries: $(Split-Path $asanDll -Leaf)" -ForegroundColor DarkGray
        } else {
            Write-Warning "clang_rt.asan_dynamic-x86_64.dll not found beside $clPath — the run now depends on PATH resolving a matching copy."
        }
    }

    # halt_on_error makes any finding fail the run loudly, as on Linux.
    $env:ASAN_OPTIONS = 'halt_on_error=1'
    $rc = Invoke-CtestTee -BuildDir $build -RawLog (Join-Path $Out 'sanitize-asan.raw.txt')
    Remove-Item env:ASAN_OPTIONS -ErrorAction SilentlyContinue
    Invoke-Normalize 'asan'
    if ($rc) { Write-Host "ASan: all tests clean" -ForegroundColor Green }
    return $rc
}

function Invoke-Ubsan {
    Write-Stage 'UndefinedBehaviorSanitizer (clang-cl)'
    $llvm = Get-LlvmToolset
    if (-not $llvm) {
        Write-Skip "clang-cl not installed (winget install LLVM.LLVM) — UBSan run unavailable"
        Write-TextFile (Join-Path $Out 'sanitize-ubsan.txt') ''
        return $true   # an absent optional tool must not fail the pipeline
    }
    Write-Host "LLVM toolset: $($llvm.Bin) (clang $($llvm.Version))" -ForegroundColor DarkGray

    $build = Join-Path $Root 'build-san-ubsan'
    $clangCl = $llvm.ClangCl -replace '\\', '/'
    # /Oy- is the clang-cl spelling of -fno-omit-frame-pointer; the GCC form is
    # accepted but ignored with a warning on every translation unit.
    #
    # TRAP MODE, and that is not a shortcut — it is the only thing that links.
    # LLVM ships the UBSan runtimes built against the RELEASE CRT, and the
    # clang-cl driver auto-references clang_rt.ubsan_standalone_cxx through a
    # defaultlib pragma, so a /MDd (debug CRT) build always dies with
    #   lld-link: error: /failifmismatch: mismatch detected for
    #             '_ITERATOR_DEBUG_LEVEL': … has value 2 … has value 0
    # Removing the library from the link line does not help (the pragma puts it
    # back) and switching the whole tree to the release CRT would stop it being
    # a debug build. -fsanitize-trap=undefined emits a trap instruction instead
    # of a call into that runtime, so NO runtime library is needed at all.
    #
    # Trade-off, stated plainly: undefined behaviour still HALTS the test — it
    # just aborts instead of printing "runtime error: …". So a UB hit surfaces as
    # a failed ctest case rather than as a parsed finding in
    # analysis-results\sanitize-ubsan.txt, and the Linux ASan+UBSan run remains
    # the source of readable UB diagnostics. vptr additionally needs the C++
    # runtime's RTTI support and is therefore off.
    # CMAKE_MSVC_RUNTIME_CHECKS is cleared for the same reason as in the ASan stage:
    # since CMake 4.0 (CMP0197) /RTC1 comes from there, not from CMAKE_CXX_FLAGS_DEBUG,
    # so overriding the latter alone leaves runtime checks on in a sanitizer build.
    $ok = Invoke-Configure -BuildDir $build -ExtraArgs @(
        "-DCMAKE_CXX_COMPILER=$clangCl",
        '-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od',
        '-DCMAKE_MSVC_RUNTIME_CHECKS=',
        '-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fsanitize-trap=undefined -fno-sanitize=vptr /Oy-'
    )
    if (-not $ok) {
        Write-Skip "clang-cl UBSan configure failed — see the CMake output above"
        Write-TextFile (Join-Path $Out 'sanitize-ubsan.txt') ''
        return $true
    }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) {
        Write-Skip "clang-cl UBSan build failed — the UBSan runtime is not available for every MSVC/LLVM combination"
        Write-TextFile (Join-Path $Out 'sanitize-ubsan.txt') ''
        return $true
    }
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    $rc = Invoke-CtestTee -BuildDir $build -RawLog (Join-Path $Out 'sanitize-ubsan.raw.txt')
    Remove-Item env:UBSAN_OPTIONS -ErrorAction SilentlyContinue
    Invoke-Normalize 'ubsan'
    if ($rc) { Write-Host "UBSan: all tests clean" -ForegroundColor Green }
    return $rc
}

function Show-Tsan {
    Write-Stage 'ThreadSanitizer'
    Write-Host "NOT AVAILABLE on Windows: TSan has no Windows target in MSVC or upstream LLVM." -ForegroundColor Yellow
    Write-Host "Race evidence for REQ-N-006 comes from the Linux pipeline (tools/sanitize.sh tsan)." -ForegroundColor DarkGray
    Write-TextFile (Join-Path $Out 'sanitize-tsan.txt') ''
}

function Show-Valgrind {
    Write-Stage 'valgrind memcheck'
    Write-Host "NOT AVAILABLE on Windows: valgrind is Linux/macOS only." -ForegroundColor Yellow
    Write-Host "ASan above covers the memory-error classes except leaks (MSVC ASan has no LSan)." -ForegroundColor DarkGray
    Write-Host "For leak checking on Windows use the Linux pipeline, or Application Verifier / Dr. Memory manually." -ForegroundColor DarkGray
    Write-TextFile (Join-Path $Out 'sanitize-valgrind.txt') ''
}

switch ($Mode) {
    'asan' { if (Invoke-Asan) { exit 0 } else { exit 1 } }
    'ubsan' { if (Invoke-Ubsan) { exit 0 } else { exit 1 } }
    'tsan' { Show-Tsan; exit 0 }
    'valgrind' { Show-Valgrind; exit 0 }
    'all' {
        $fail = 0
        if (-not (Invoke-Asan)) { $fail = 1 }
        if (-not (Invoke-Ubsan)) { $fail = 1 }
        Show-Tsan
        Show-Valgrind
        exit $fail
    }
}

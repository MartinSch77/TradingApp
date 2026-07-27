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
    $ok = Invoke-Configure -BuildDir $build -ExtraArgs @(
        '-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od',
        '-DCMAKE_CXX_FLAGS=/fsanitize=address /Oy-',
        '-DCMAKE_EXE_LINKER_FLAGS=/fsanitize=address /INCREMENTAL:NO /DEBUG'
    )
    if (-not $ok) { return $false }
    if (-not (Invoke-Native -FilePath 'cmake' -Arguments @('--build', $build, '-j', "$Jobs"))) { return $false }

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
    $ok = Invoke-Configure -BuildDir $build -ExtraArgs @(
        "-DCMAKE_CXX_COMPILER=$clangCl",
        '-DCMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od',
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

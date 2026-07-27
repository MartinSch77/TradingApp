# Shared helpers for the Windows (PowerShell) build scripts — the counterpart
# of the plumbing the *.sh scripts get for free from a POSIX shell:
# a repo root, a compiler environment, a Qt kit, a core count and tool probes.
#
# Dot-source it, never run it:  . "$PSScriptRoot\common.ps1"
#
# Targets Windows PowerShell 5.1 (the version shipped with Windows), so no
# ternary, no ?? and no && / || chain operators anywhere in these scripts.

Set-StrictMode -Version Latest

# --- repository layout -----------------------------------------------------

# tools/common.ps1 -> repo root is one level up.
$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-RepoRoot { return $script:RepoRoot }

# --- process helpers -------------------------------------------------------

function Get-JobCount {
    if ($env:NUMBER_OF_PROCESSORS) { return [int]$env:NUMBER_OF_PROCESSORS }
    return [Environment]::ProcessorCount
}

function Test-Tool {
    param([Parameter(Mandatory)][string]$Name)
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Get-ToolPath {
    param([Parameter(Mandatory)][string]$Name)
    $c = Get-Command $Name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

# Run a native command and return $true on exit code 0. Native stderr is left
# alone (redirecting it in 5.1 turns clean runs into NativeCommandError).
#
# The command's stdout is re-emitted with Write-Host instead of being returned.
# Two requirements have to hold at once:
#  * it must NOT go down the pipeline, or the caller's
#    `$ok = Invoke-Native ...` receives the whole build log followed by the
#    boolean, and `if ($ok)` is then true for any command that printed
#    something — including failing ones;
#  * it MUST still be redirectable, so `script.ps1 *> log.txt` and the CI log
#    upload capture it. `| Out-Host` satisfies the first but breaks the second:
#    it writes past every redirection and the log comes out nearly empty.
# Write-Host goes to the information stream, which `*>` does capture, and which
# is not the pipeline. Native stderr is left alone (adding 2>&1 in 5.1 turns
# clean runs into NativeCommandError records).
function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory
    )
    $prev = $null
    if ($WorkingDirectory) { $prev = (Get-Location).Path; Set-Location $WorkingDirectory }
    try {
        & $FilePath @Arguments | ForEach-Object { Write-Host $_ }
        return ($LASTEXITCODE -eq 0)
    } finally {
        if ($prev) { Set-Location $prev }
    }
}

# Run one process per work item, at most $Throttle at a time, each with its own
# stdout/stderr file — the PowerShell 5.1 stand-in for `xargs -P $(nproc)`.
# Per-process log files keep concurrent output from interleaving mid-line.
# Returns the list of captured log files (stdout and stderr merged per item).
function Invoke-ThrottledProcesses {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][object[]]$ArgumentSets,   # one string[] per process
        [int]$Throttle = 0,
        [string]$WorkingDirectory
    )
    if ($Throttle -le 0) { $Throttle = Get-JobCount }
    $tmp = Join-Path $env:TEMP ("throttled-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    $running = @()
    $logs = @()
    $i = 0
    foreach ($set in $ArgumentSets) {
        while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Throttle) {
            Start-Sleep -Milliseconds 100
        }
        $stdout = Join-Path $tmp "$i.out"
        $stderr = Join-Path $tmp "$i.err"
        $p = @{
            FilePath               = $FilePath
            ArgumentList           = $set
            NoNewWindow            = $true
            PassThru               = $true
            RedirectStandardOutput = $stdout
            RedirectStandardError  = $stderr
        }
        if ($WorkingDirectory) { $p.WorkingDirectory = $WorkingDirectory }
        $running += Start-Process @p
        $logs += @($stdout, $stderr)
        $i++
    }
    foreach ($p in $running) { $p.WaitForExit() }
    return @{ Logs = $logs; TempDir = $tmp }
}

# --- MSVC toolchain --------------------------------------------------------

# Import the environment of a Visual Studio developer command prompt into this
# PowerShell session, so cl.exe / link.exe / the Windows SDK are on PATH for
# every later cmake and ninja call. Idempotent: a session already inside a
# developer prompt (VSCMD_ARG_TGT_ARCH set) is left untouched.
function Import-MsvcEnvironment {
    param([ValidateSet('x64', 'x86', 'arm64')][string]$Arch = 'x64')

    if ((Test-Path env:VSCMD_ARG_TGT_ARCH) -and $env:VSCMD_ARG_TGT_ARCH -eq $Arch) {
        return $true
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Write-Warning "vswhere.exe not found — is Visual Studio (or the Build Tools) installed?"
        return $false
    }
    $install = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $install) {
        Write-Warning "no Visual Studio installation with the C++ toolset found (install the 'Desktop development with C++' workload)"
        return $false
    }
    # The batch files are not named after the architecture one-to-one:
    # x64 -> vcvars64.bat, x86 -> vcvars32.bat, arm64 -> vcvarsarm64.bat.
    $batch = @{ 'x64' = 'vcvars64.bat'; 'x86' = 'vcvars32.bat'; 'arm64' = 'vcvarsarm64.bat' }[$Arch]
    $vcvars = Join-Path $install "VC\Auxiliary\Build\$batch"
    if (-not (Test-Path $vcvars)) {
        Write-Warning "$batch not found under $install"
        return $false
    }

    # cmd.exe applies the batch file, then dumps the resulting environment;
    # everything that differs is copied into this session.
    $lines = & $env:COMSPEC /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "vcvars$Arch.bat failed (exit $LASTEXITCODE)"
        return $false
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
        }
    }
    return $true
}

# --- Qt kit ----------------------------------------------------------------

# Kit preference: the MSVC kits first (they match the default compiler), then
# the MinGW ones. Overridable per run with QT_PREFIX (full path to the kit) or
# QT_KIT (kit directory name, e.g. mingw_64).
$script:QtKitPreference = @('msvc2022_64', 'msvc2019_64', 'llvm-mingw_64', 'mingw_64')

# Every usable Qt kit on the machine, best first. "Usable" means it carries the
# CMake package config for Qt6Charts, which this project links against — a
# qtbase-only kit would configure and then fail inside find_package.
function Get-QtKits {
    param([string]$Kit)

    if (-not $Kit -and $env:QT_KIT) { $Kit = $env:QT_KIT }
    $kits = $script:QtKitPreference
    if ($Kit) { $kits = @($Kit) }

    $roots = @('C:\Qt', (Join-Path $env:USERPROFILE 'Qt'), 'D:\Qt') | Where-Object { Test-Path $_ }
    $candidates = @()
    foreach ($root in $roots) {
        foreach ($verDir in Get-ChildItem $root -Directory -ErrorAction SilentlyContinue) {
            $v = $null
            if (-not [Version]::TryParse($verDir.Name, [ref]$v)) { continue }
            if ($v.Major -lt 6) { continue }
            for ($i = 0; $i -lt $kits.Count; $i++) {
                $p = Join-Path $verDir.FullName $kits[$i]
                if (Test-Path (Join-Path $p 'lib\cmake\Qt6Charts')) {
                    $candidates += [pscustomobject]@{ Path = $p; Version = $v; Rank = $i }
                    break
                }
            }
        }
    }
    return @($candidates | Sort-Object Rank, @{ Expression = 'Version'; Descending = $true })
}

# The kit to build with: newest usable one, MSVC preferred. QT_PREFIX overrides
# it outright, QT_KIT / -Kit restrict it to one kit flavour.
function Resolve-QtPrefix {
    param([string]$Kit, [switch]$Quiet)

    if ($env:QT_PREFIX) {
        if (-not (Test-Path $env:QT_PREFIX)) {
            Write-Warning "QT_PREFIX points at a missing directory: $env:QT_PREFIX"
        }
        return $env:QT_PREFIX
    }
    $best = Get-QtKits -Kit $Kit | Select-Object -First 1
    if (-not $best) { return $null }
    if (-not $Quiet) { Write-Host "Qt kit: $($best.Path)" -ForegroundColor DarkGray }
    return $best.Path
}

# The test and app executables load the Qt DLLs from the kit's bin/ directory.
function Add-QtToPath {
    param([Parameter(Mandatory)][string]$QtPrefix)
    $bin = Join-Path $QtPrefix 'bin'
    if ((Test-Path $bin) -and ($env:PATH -notlike "*$bin*")) {
        $env:PATH = "$bin;$env:PATH"
    }
}

# Is this kit built with MSVC (vs. MinGW)? Decides which compiler environment
# and which sanitizer/coverage back end the other scripts can use.
function Test-QtIsMsvcKit {
    param([Parameter(Mandatory)][string]$QtPrefix)
    return ((Split-Path $QtPrefix -Leaf) -like 'msvc*')
}

# --- LLVM toolset ----------------------------------------------------------

# There are usually TWO clang installations on a Windows dev box: the one
# bundled with Visual Studio (VC\Tools\Llvm) and a standalone LLVM. Picking
# clang-cl from one and llvm-cov/llvm-profdata from the other produces
# "unsupported instrumentation profile format version" at report time, so the
# tools must come from a single installation. Newest complete toolset wins.
function Get-LlvmToolset {
    $bins = @()
    if (Test-Path "$env:ProgramFiles\LLVM\bin") { $bins += "$env:ProgramFiles\LLVM\bin" }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($install in (& $vswhere -latest -products * -property installationPath)) {
            $bins += (Join-Path $install 'VC\Tools\Llvm\x64\bin')
        }
    }
    $onPath = Get-ToolPath 'clang-cl'
    if ($onPath) { $bins += (Split-Path $onPath -Parent) }

    $found = @()
    foreach ($bin in ($bins | Select-Object -Unique)) {
        $clang = Join-Path $bin 'clang-cl.exe'
        $cov = Join-Path $bin 'llvm-cov.exe'
        $prof = Join-Path $bin 'llvm-profdata.exe'
        if (-not ((Test-Path $clang) -and (Test-Path $cov) -and (Test-Path $prof))) { continue }
        $ver = [Version]'0.0'
        $out = (& $clang --version 2>&1) -join ' '
        if ("$out" -match 'clang version (\d+)\.(\d+)\.(\d+)') {
            $ver = [Version]"$($matches[1]).$($matches[2]).$($matches[3])"
        }
        $found += [pscustomobject]@{
            Bin          = $bin
            ClangCl      = $clang
            LlvmCov      = $cov
            LlvmProfdata = $prof
            Root         = (Split-Path $bin -Parent)
            Version      = $ver
        }
    }
    if ($found.Count -eq 0) { return $null }
    return ($found | Sort-Object Version -Descending | Select-Object -First 1)
}

# Locate a compiler-rt library (clang_rt.profile-x86_64.lib, the ubsan
# runtimes, …) inside a toolset. Returns the containing DIRECTORY: it goes on
# the linker's LIB search path rather than into a linker flag, because the path
# contains spaces and backslashes that CMake's flag strings mangle.
function Get-ClangRuntimeDir {
    param([Parameter(Mandatory)]$Toolset, [Parameter(Mandatory)][string]$LibName)
    $lib = Get-ChildItem (Join-Path $Toolset.Root 'lib\clang') -Recurse -Filter $LibName -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($lib) { return $lib.DirectoryName }
    return $null
}

function Add-ToLibPath {
    param([Parameter(Mandatory)][string]$Dir)
    if (($env:LIB -split ';') -notcontains $Dir) { $env:LIB = "$Dir;$env:LIB" }
}

# --- CMake cache hygiene ---------------------------------------------------

# A CMake build tree records the absolute source and binary paths it was
# generated with, and refuses to be reused if either changed:
#
#   CMake Error: The current CMakeCache.txt directory C:/…/build/CMakeCache.txt
#   is different than the directory /mnt/c/…/build where CMakeCache.txt was created.
#
# This repository invites exactly that: the same working tree is reachable as
# C:\AxivionRepoCheck\TradingApp from Windows and as
# /mnt/c/AxivionRepoCheck/TradingApp from WSL, and both platforms default to
# build/. Switching sides — or switching generator, e.g. Ninja to Visual
# Studio — then leaves a tree that cannot be configured and cannot be built.
# Rather than surfacing CMake's error, detect the mismatch and start clean.
function Reset-StaleCMakeCache {
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string]$SourceDir,
        [string]$Generator,
        # Qt kit this run will configure with. Switching kits (msvc2022_64 <->
        # mingw_64) also switches the compiler, and CMake refuses that in an
        # existing tree with "The CMAKE_CXX_COMPILER has changed".
        [string]$QtPrefix,
        # C++ compiler this run will configure with. A tree left behind by a
        # failed attempt can pin a different one (e.g. an older MinGW picked up
        # from PATH), and CMake then reuses it instead of the one we want.
        [string]$Compiler
    )
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) { return }

    $norm = { param($p) ($p -replace '\\', '/').TrimEnd('/').ToLowerInvariant() }
    $reason = $null
    foreach ($line in (Get-Content $cache -ErrorAction SilentlyContinue)) {
        if ($line -match '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$') {
            if ((& $norm $matches[1]) -ne (& $norm $SourceDir)) {
                $reason = "it was generated for source dir '$($matches[1])'"
            }
        } elseif ($line -match '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$') {
            if ((& $norm $matches[1]) -ne (& $norm $BuildDir)) {
                $reason = "it was generated in build dir '$($matches[1])'"
            }
        } elseif ($Generator -and $line -match '^CMAKE_GENERATOR:INTERNAL=(.*)$') {
            if ($matches[1] -ne $Generator) {
                $reason = "it was generated with the '$($matches[1])' generator, not '$Generator'"
            }
        } elseif ($QtPrefix -and $line -match '^CMAKE_PREFIX_PATH:[A-Z]+=(.*)$') {
            if ($matches[1] -and (& $norm $matches[1]) -ne (& $norm $QtPrefix)) {
                $reason = "it was generated for the Qt kit '$($matches[1])', not '$QtPrefix'"
            }
        } elseif ($Compiler -and $line -match '^CMAKE_CXX_COMPILER:[A-Z]+=(.*)$') {
            if ($matches[1] -and (& $norm $matches[1]) -ne (& $norm $Compiler)) {
                $reason = "it was generated with the compiler '$($matches[1])', not '$Compiler'"
            }
        }
        if ($reason) { break }
    }
    if (-not $reason) { return }

    Write-Host "discarding the build tree in $BuildDir - $reason" -ForegroundColor Yellow
    Write-Host "(a tree configured from WSL and one configured from Windows cannot be shared)" -ForegroundColor DarkGray
    Remove-Item $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $BuildDir) {
        Write-Warning "could not remove $BuildDir - close whatever is holding it open and retry"
    }
}

# --- Axivion Suite ---------------------------------------------------------

# More than one Suite can be installed side by side, and the install directory
# says nothing about which is newer — on the reference machine 7.12.1 sits in
# "Program Files" while 7.12.3 sits in "Program Files (x86)". Picking the first
# path that happens to match is how you end up with axivion_ci from one version
# chainloading the CMake launcher toolchain of another, which loops until CMake
# errors out. So: enumerate, ask each one its version, take the newest.
function Get-AxivionSuite {
    $roots = @(
        "$env:ProgramFiles\Bauhaus"
        "${env:ProgramFiles(x86)}\Bauhaus"
        (Join-Path $env:USERPROFILE 'bauhaus-suite')
        'C:\bauhaus-suite'
    )
    if ($env:AXIVIONBASE) { $roots += (Join-Path $env:AXIVIONBASE 'bauhaus-suite') }
    if ($env:AXIVIONPORTABLE_PATH) {
        $roots += $env:AXIVIONPORTABLE_PATH
        $roots += @(Get-ChildItem $env:AXIVIONPORTABLE_PATH -Directory -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName })
    }
    $onPath = Get-ToolPath 'axivion_ci'
    if ($onPath) { $roots += (Split-Path (Split-Path $onPath -Parent) -Parent) }

    $found = @()
    foreach ($root in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
        $ci = Join-Path $root 'bin\axivion_ci.exe'
        if (-not (Test-Path $ci)) { continue }
        $ver = [Version]'0.0'
        $info = Join-Path $root 'bin\axivion_suite_info.exe'
        if (Test-Path $info) {
            $out = (& $info 2>&1) -join ' '
            if ("$out" -match 'Version:\s*(\d+(?:\.\d+)+)') { $ver = [Version]$matches[1] }
        }
        $found += [pscustomobject]@{ Root = $root; Bin = (Join-Path $root 'bin'); Ci = $ci; Version = $ver }
    }
    if ($found.Count -eq 0) { return $null }
    return ($found | Sort-Object Version -Descending | Select-Object -First 1)
}

# --- kit toolchain ---------------------------------------------------------

# Put the compiler that matches the Qt kit in place, and return the C++ compiler
# to configure with ('' for MSVC, where CMake finds cl.exe via the imported
# developer environment). Returns $null if the toolchain is unusable.
#
# For MinGW this is not optional plumbing: a MinGW Qt kit ships no compiler, and
# the matching toolchain in <QtRoot>\Tools is not on PATH unless Qt Creator put
# it there. Its bin directory must be on PATH even when the compiler is named
# explicitly — cc1plus loads its DLLs from there, and without it every
# compilation fails with exit code 1 and no diagnostic at all.
function Initialize-KitToolchain {
    param([Parameter(Mandatory)][string]$QtPrefix)

    if (Test-QtIsMsvcKit $QtPrefix) {
        if (-not (Import-MsvcEnvironment)) {
            Write-Error "the Qt kit is an MSVC kit but no MSVC toolset was found - run .\setup.ps1 install, or pick the MinGW kit with -QtKit mingw_64"
            return $null
        }
        return ''
    }

    # Sort by the version number embedded in the directory name, NOT by the
    # name: as strings "mingw810_64" (GCC 8.1) sorts above "mingw1310_64"
    # (GCC 13.1), and GCC 8 cannot compile this C++23 project - the failure
    # surfaces as an unhelpful try_compile error inside find_package(Qt6).
    $qtRoot = Split-Path (Split-Path $QtPrefix -Parent) -Parent
    $mingw = Get-ChildItem (Join-Path $qtRoot 'Tools') -Directory -Filter 'mingw*_64' -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\g++.exe') } |
        ForEach-Object {
            $v = 0
            if ($_.Name -match '(\d+)') { $v = [int]$matches[1] }
            [pscustomobject]@{ Dir = $_.FullName; Ver = $v }
        } | Sort-Object Ver -Descending | Select-Object -First 1

    if (-not $mingw) {
        Write-Warning "no MinGW toolchain found under $qtRoot\Tools - put one on PATH yourself"
        return ''
    }
    $bin = Join-Path $mingw.Dir 'bin'
    $env:PATH = "$bin;" + (($env:PATH -split ';' | Where-Object { $_ -ne $bin }) -join ';')
    $gxx = & (Join-Path $bin 'g++.exe') -dumpfullversion 2>$null
    Write-Host "MinGW toolchain: $bin (g++ $gxx)" -ForegroundColor DarkGray
    # Named explicitly so a different MinGW already on PATH - or one cached in
    # an existing build tree - cannot win.
    return ((Join-Path $bin 'g++.exe') -replace '\\', '/')
}

# --- python ----------------------------------------------------------------

# The scripts call the interpreter, not a shebang; prefer a real python.exe
# over the WindowsApps stub, which is an App-Execution alias that opens the
# Microsoft Store when Python is not installed from there.
function Get-Python {
    foreach ($name in @('python', 'python3', 'py')) {
        $c = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $c) { continue }
        if ($c.Source -like '*WindowsApps*') { continue }
        if ($name -eq 'py') { return @{ Exe = $c.Source; Args = @('-3') } }
        return @{ Exe = $c.Source; Args = @() }
    }
    return $null
}

function Invoke-Python {
    param([Parameter(Mandatory)][string[]]$Arguments, [string]$WorkingDirectory)
    $py = Get-Python
    if (-not $py) {
        Write-Warning "no Python interpreter found (install Python 3 and put it on PATH)"
        return $false
    }
    return (Invoke-Native -FilePath $py.Exe -Arguments ($py.Args + $Arguments) -WorkingDirectory $WorkingDirectory)
}

# Console scripts installed by `pip install --user` land in a Scripts directory
# that is not always on PATH; make sure it is before probing for strictdoc etc.
#
# Computed in PowerShell rather than with `python -c`: Windows PowerShell 5.1
# strips double quotes when it builds a native command line, so any probe
# containing a quoted string silently returns nothing.
function Get-PythonScriptsDirs {
    $py = Get-Python
    if (-not $py) { return @() }
    $dirs = @()
    # The interpreter's own Scripts directory (system-wide and venv installs).
    $dirs += (Join-Path (Split-Path $py.Exe -Parent) 'Scripts')
    # The pip --user directory: %APPDATA%\Python\Python<major><minor>\Scripts.
    $ver = & $py.Exe --version 2>&1
    if ("$ver" -match 'Python\s+(\d+)\.(\d+)') {
        $dirs += (Join-Path $env:APPDATA "Python\Python$($matches[1])$($matches[2])\Scripts")
    }
    # Any other user-scope interpreter that has installed console scripts.
    $dirs += @(Get-ChildItem (Join-Path $env:APPDATA 'Python') -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'Scripts' })
    return @($dirs | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique)
}

function Add-PythonScriptsToPath {
    foreach ($d in (Get-PythonScriptsDirs)) {
        if (($env:PATH -split ';') -notcontains $d) { $env:PATH = "$d;$env:PATH" }
    }
}

# --- tool discovery --------------------------------------------------------

# Several of the tools this project uses install to a fixed location but do NOT
# extend PATH (cppcheck, Graphviz and LLVM all behave that way under a silent
# `winget install`). Rather than making every script fail with "not installed"
# on a machine where the tool is sitting right there, put the well-known
# directories on the PATH of this process. Idempotent and cheap.
$script:WellKnownToolDirs = @(
    "$env:ProgramFiles\Cppcheck"
    "${env:ProgramFiles(x86)}\Cppcheck"
    "$env:ProgramFiles\LLVM\bin"
    "$env:ProgramFiles\Graphviz\bin"
    "$env:ProgramFiles\doxygen\bin"
    "$env:ProgramFiles\OpenCppCoverage"
    "$env:LOCALAPPDATA\bin"
)

function Initialize-ToolPath {
    # Re-read the persisted PATH first: a tool installed earlier in this same
    # session is not yet visible in the PATH this process inherited.
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    foreach ($p in @($machine, $user)) {
        if (-not $p) { continue }
        foreach ($d in ($p -split ';')) {
            if ($d -and ($env:PATH -notlike "*$d*")) { $env:PATH = "$env:PATH;$d" }
        }
    }
    foreach ($d in $script:WellKnownToolDirs) {
        if ((Test-Path $d) -and ($env:PATH -notlike "*$d*")) { $env:PATH = "$d;$env:PATH" }
    }
}

# --- output ----------------------------------------------------------------

function Write-Stage {
    param([Parameter(Mandatory)][string]$Name)
    Write-Host ""
    Write-Host ("=" * 20 + " $Name " + "=" * 20) -ForegroundColor Cyan
}

function Write-Skip {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "SKIPPED: $Message" -ForegroundColor Yellow
}

# Write a text file without a BOM: the analyzer logs are consumed by the
# Axivion import and by python readers that do not expect one.
function Write-TextFile {
    param([Parameter(Mandatory)][string]$Path, [string]$Content = '')
    $dir = Split-Path $Path -Parent
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Content, (New-Object System.Text.UTF8Encoding $false))
}

function Get-LineCount {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path $Path)) { return 0 }
    $lines = @(Get-Content $Path -ErrorAction SilentlyContinue | Where-Object { $_.Trim() -ne '' })
    return $lines.Count
}

# --- run on dot-source -----------------------------------------------------

Initialize-ToolPath
Add-PythonScriptsToPath

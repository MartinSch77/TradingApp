# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
    Windows counterpart of ./setup.sh — provision this machine with every tool
    the project needs, idempotently, and keep them current.

.DESCRIPTION
    .\setup.ps1              install everything that is missing
    .\setup.ps1 install      same
    .\setup.ps1 update       update all managed tools to their latest versions
    .\setup.ps1 status       report found/missing tools and versions (read-only)

    What it manages — every OPEN-SOURCE tool the pipeline needs:
      winget  Visual Studio 2022 Build Tools (MSVC C++ toolset + Windows SDK),
              CMake, Ninja, Git, GitHub CLI, LLVM (clang-cl / clang-tidy /
              llvm-cov / llvm-profdata — the MC/DC coverage back end), cppcheck,
              Doxygen, Graphviz, OpenCppCoverage, syft, grype, trivy,
              a JRE (for PlantUML), Python 3
      pip     strictdoc, doorstop, codespell, sphinx + myst-parser, gcovr,
              clang-format (pinned by wheel: one version on every platform),
              reportlab (PDF quality report),
              aqtinstall  (user scope — no admin needed)
      aqt     Qt $QtVersion (win64_msvc2022_64 + qtcharts, qtgraphs) into C:\Qt — the
              layout the build scripts expect (override with QT_PREFIX)
      web     PlantUML jar (pinned in tools/fetch_plantuml.ps1)

    NOT installable here — LICENSE-BOUND, so they are detected and reported,
    and the stages that need them SKIP with a message instead of failing:
      Axivion Suite  (axivion\start_analysis.ps1 -> stage `skipped`)
      Squish Coco    (tools\coverage.ps1 -Mode coco -> stage `skipped`;
                      auto mode just uses the other back ends)
    Also manual: the eToro/Anthropic API keys (copy apiKeyEtoro.example.json to
    apiKeyEtoro.json — the app runs in SIMULATION mode without them).

    Tools with no Windows counterpart are reported as such, not silently
    skipped: clazy, valgrind and ThreadSanitizer do not exist on Windows.
    See docs/windows.md for what replaces them.

.NOTES
    winget may prompt for elevation for the machine-wide packages. Everything
    else stays under the user profile.
#>
[CmdletBinding()]
param(
    [ValidateSet('install', 'update', 'status', 'android', 'ml', 'squish')]
    [string]$Mode = 'install',

    # Qt version installed by aqt when no usable kit is found.
    # 6.10.3, not 6.11.1 like setup.sh: this parameter drives aqtinstall, and aqt
    # (3.3.0, the newest release) cannot fetch the Windows desktop metadata for
    # 6.11.x — it fails with "Failed to download checksum for the file
    # 'Updates.xml'" while linux/mac resolve 6.11.1 fine (verified 2026-07-29).
    # Qt's own online installer DOES offer 6.11.1 for Windows: a machine that has
    # it keeps building against it, because the build scripts take the newest kit
    # present. $env:QT_VERSION overrides this. See docs\windows.md.
    [string]$QtVersion = $(if ($env:QT_VERSION) { $env:QT_VERSION } else { '6.10.3' }),

    # Skip the multi-gigabyte Visual Studio Build Tools install even when no
    # MSVC toolset is present (useful when you intend to build with MinGW).
    [switch]$NoVisualStudio
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

. "$PSScriptRoot\tools\common.ps1"

$Root = Get-RepoRoot
$QtDir = 'C:\Qt'

# winget id -> the command that proves the package is present.
# Everything here is open source / freely redistributable, so setup.ps1 installs
# it. The license-bound tools (Axivion Suite, Squish Coco) are only detected and
# reported — see Show-Status.
$WingetPkgs = [ordered]@{
    'Kitware.CMake'                   = 'cmake'
    'Ninja-build.Ninja'               = 'ninja'
    'Git.Git'                         = 'git'
    'GitHub.cli'                      = 'gh'
    'LLVM.LLVM'                       = 'clang-cl'          # clang-cl, clang-tidy, llvm-cov, llvm-profdata
    'Cppcheck.Cppcheck'               = 'cppcheck'
    'DimitriVanHeesch.Doxygen'        = 'doxygen'
    'Graphviz.Graphviz'               = 'dot'
    'OpenCppCoverage.OpenCppCoverage' = 'OpenCppCoverage'    # the gcov/lcov substitute
    'Anchore.Syft'                    = 'syft'               # SBOM
    'Anchore.Grype'                   = 'grype'              # vulnerability scan
    'AquaSecurity.Trivy'              = 'trivy'              # repo/misconfig/secret scan
    'EclipseAdoptium.Temurin.21.JRE'  = 'java'               # runs the PlantUML jar
    'Python.Python.3.13'              = 'python'
}

# Android target (REQ-N-001) — installed only by `.\setup.ps1 android`.
$AndroidCmdlineZip = 'https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip'
$AndroidPlatform = 'android-35'          # matches QT_ANDROID_TARGET_SDK_VERSION
$AndroidCompilePlatform = 'android-36'   # matches QT_ANDROID_COMPILE_SDK_VERSION: Qt 6.11's
                                         # androidx.core dependency refuses anything older
$AndroidBuildTools = '35.0.1'
$AndroidSystemImage = "system-images;$AndroidPlatform;google_apis;x86_64"
$AndroidNdkFallback = '27.2.12479018'    # only when no Qt kit is installed to ask

# pip distribution -> the console script it installs
$PipPkgs = [ordered]@{
    'strictdoc'   = 'strictdoc'
    'doorstop'    = 'doorstop'
    'codespell'   = 'codespell'
    'sphinx'      = 'sphinx-build'
    'myst-parser' = ''          # library only, imported by sphinx
    'gcovr'       = 'gcovr'
    'aqtinstall'  = 'aqt'
    'lizard'      = 'lizard'
    'reportlab'   = ''          # library only, imported by tools\make_report.py
    # From pip rather than the LLVM installer: the wheel pins ONE clang-format
    # version on every platform, and a formatting check that answers differently on
    # two machines is worse than no check (.clang-format explains the CI check).
    'clang-format' = 'clang-format'
}

# The supply-chain scanners used to be fetched from GitHub releases into
# %LOCALAPPDATA%\bin; they are all on winget now (see $WingetPkgs), so the
# bespoke downloader is gone. Kept as a name list for the status report.
$SupplyChainTools = @('syft', 'grype', 'trivy')

# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------

function Report {
    param([string]$Name, [string]$State, [string]$Detail = '')
    $color = 'Gray'
    if ($State -eq 'ok') { $color = 'Green' }
    elseif ($State -eq 'MISSING') { $color = 'Red' }
    elseif ($State -eq 'n/a') { $color = 'DarkGray' }
    else { $color = 'Yellow' }
    Write-Host ("  {0,-14} {1,-8} {2}" -f $Name, $State, $Detail) -ForegroundColor $color
}

function Get-ToolVersion {
    param([string]$Name)
    try {
        switch ($Name) {
            'cmake' { return ((cmake --version | Select-Object -First 1) -split '\s+')[2] }
            'ninja' { return (ninja --version) }
            'cl' {
                if (-not (Test-Path env:VSINSTALLDIR)) { return '' }
                $out = & $env:COMSPEC /c "cl 2>&1" | Select-Object -First 1
                if ($out -match '(\d+\.\d+\.\d+(\.\d+)?)') { return $matches[1] }
                return ''
            }
            'java' {
                $out = & java -version 2>&1 | Select-Object -First 1
                if ("$out" -match '"([^"]+)"') { return $matches[1] }
                return ''
            }
            'aqt' {
                # aqt has no --version; the subcommand is spelled "version".
                $out = (& aqt version 2>&1) -join ' '
                if ("$out" -match '(\d+(?:\.\d+)+)') { return $matches[1] }
                return ''
            }
            'plantuml' {
                $jar = Join-Path $Root 'tools\third-party\plantuml.jar'
                if (-not (Test-Path $jar)) { return '' }
                $out = & java -jar $jar -version 2>&1 | Select-Object -First 1
                if ("$out" -match '(\d+\.\d[\d.]*)') { return $matches[1] }
                return ''
            }
            'pmd' {
                # PMD prints an ASCII banner before the version line.
                $bat = Get-PmdLauncher
                if (-not $bat) { return '' }
                $out = (& $bat --version 2>&1) -join ' '
                if ("$out" -match 'PMD (\d+(?:\.\d+)+)') { return $matches[1] }
                return ''
            }
            default {
                # Some tools print the version on a later line (LLVM prints a
                # banner first), so match over the whole output, and stop the
                # version at the last digit — "2.55.0.windows.3" must not become
                # "2.55.0." and "LLVM version 20.1.0" must not become "20.1".
                $out = (& $Name --version 2>&1) -join ' '
                if ("$out" -match '(\d+(?:\.\d+)+)') { return $matches[1] }
                return (("$out" -split "`n")[0]).Trim()
            }
        }
    } catch { return '' }
}

function Find-Coco {
    # cscl.exe is Coco's cl.exe wrapper — the marker for a usable Windows install.
    foreach ($p in @(
            'C:\Program Files\squishcoco\cscl.exe',
            'C:\Program Files (x86)\squishcoco\cscl.exe')) {
        if (Test-Path $p) { return (Split-Path $p -Parent) }
    }
    if ($env:COCO_DIR -and (Test-Path $env:COCO_DIR)) { return $env:COCO_DIR }
    return $null
}

# ---------------------------------------------------------------------------
# Android (separate mode: ~6 GB of SDK/NDK/system image plus a Qt kit per ABI, which
# nobody building the desktop app should have to download)
# ---------------------------------------------------------------------------
# Counterpart of `./setup.sh android`. Everything lands under the user profile - no
# admin rights. The one thing it cannot arrange is hardware acceleration: on Windows
# that is the "Windows Hypervisor Platform" optional feature plus virtualisation in
# firmware, so it is reported and left to the user.
function Install-Android {
    $sdk = $env:ANDROID_HOME
    if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
    if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }

    Write-Stage "Android SDK ($sdk)"
    $mgr = Join-Path $sdk 'cmdline-tools\latest\bin\sdkmanager.bat'
    if (-not (Test-Path $mgr)) {
        Write-Host '-- command-line tools'
        $tools = Join-Path $sdk 'cmdline-tools'
        New-Item -ItemType Directory -Path $tools -Force | Out-Null
        $zip = Join-Path $tools 'cmdline-tools.zip'
        Invoke-WebRequest -Uri $AndroidCmdlineZip -OutFile $zip
        Expand-Archive -Path $zip -DestinationPath $tools -Force
        Remove-Item $zip -Force
        # The zip unpacks as cmdline-tools\; sdkmanager insists on cmdline-tools\latest\
        # or it cannot locate the SDK root.
        if (-not (Test-Path (Join-Path $tools 'latest'))) {
            Rename-Item -Path (Join-Path $tools 'cmdline-tools') -NewName 'latest'
        }
    }
    'y' * 20 | & $mgr --licenses | Out-Null

    # The NDK version is not a free choice: Qt is built against exactly one, and a
    # mismatch is a link-time or run-time surprise. Read it out of an installed kit.
    $ndk = $AndroidNdkFallback
    $json = Get-ChildItem -Path (Join-Path $env:USERPROFILE 'Qt') -Filter 'Core.json' -Recurse `
        -ErrorAction SilentlyContinue | Where-Object { $_.FullName -like '*android_*' } |
        Select-Object -First 1
    if ($json) {
        $m = Select-String -Path $json.FullName -Pattern '"ndk_version"\s*:\s*"([0-9.]+)"' |
            Select-Object -First 1
        if ($m) { $ndk = $m.Matches[0].Groups[1].Value }
    }
    Write-Host "-- platform-tools, $AndroidPlatform + $AndroidCompilePlatform, build-tools $AndroidBuildTools, NDK $ndk, emulator, system image"
    & $mgr --install 'platform-tools' "platforms;$AndroidPlatform" "platforms;$AndroidCompilePlatform" `
        "build-tools;$AndroidBuildTools" "ndk;$ndk" 'emulator' $AndroidSystemImage
    if ($LASTEXITCODE -ne 0) { Write-Warning 'sdkmanager reported an error - rerun; partial downloads resume' }

    Write-Stage 'Qt for Android'
    # aqt serves Android from the `all_os` host for Qt >= 6.8 (NOT `windows`), one kit
    # per ABI. x86_64 is what an emulator runs natively; arm64_v8a is what a phone needs.
    foreach ($abi in @('android_x86_64', 'android_arm64_v8a')) {
        $kit = Join-Path $env:USERPROFILE "Qt\$QtVersion\$abi"
        if (Test-Path $kit) { Write-Host "$abi already installed ($QtVersion)"; continue }
        if (-not (Test-Tool 'aqt')) { Write-Warning 'aqt missing - run .\setup.ps1 install first'; return }
        & aqt install-qt all_os android $QtVersion $abi -m qtcharts qtgraphs -O (Join-Path $env:USERPROFILE 'Qt')
        if ($LASTEXITCODE -ne 0) { Write-Warning "aqt could not install $abi for $QtVersion" }
    }

    Write-Stage 'emulator prerequisite: hardware acceleration'
    $emu = Join-Path $sdk 'emulator\emulator.exe'
    if (Test-Path $emu) {
        $check = & $emu -accel-check 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host '  ok - the emulator can accelerate' -ForegroundColor Green
        } else {
            Write-Host "  NOT available: $($check -join ' ')" -ForegroundColor Yellow
            Write-Host '  enable the "Windows Hypervisor Platform" feature (optionalfeatures.exe)'
            Write-Host '  and virtualisation in firmware, then reboot.'
        }
    }
    Write-Host ''
    Write-Host 'Build:  .\tools\build_android.ps1                  (APK in downloads\)'
    Write-Host 'Run:    .\tools\build_android.ps1 -Run             (emulator + screenshot)'
}

function Show-Status {
    Add-PythonScriptsToPath
    Import-MsvcEnvironment | Out-Null

    Write-Host "== toolchain status ==" -ForegroundColor Cyan
    foreach ($t in @('cl', 'cmake', 'ninja', 'git', 'gh', 'clang-cl', 'clang-tidy',
            'llvm-cov', 'cppcheck', 'doxygen', 'dot', 'java', 'python',
            'strictdoc', 'doorstop', 'codespell', 'sphinx-build', 'gcovr', 'aqt',
            'lizard')) {
        if ($t -eq 'cl') {
            if (Test-Path env:VSINSTALLDIR) { Report 'cl (MSVC)' 'ok' (Get-ToolVersion 'cl') }
            else { Report 'cl (MSVC)' 'MISSING' 'install the VS 2022 C++ workload' }
            continue
        }
        if (Test-Tool $t) { Report $t 'ok' (Get-ToolVersion $t) } else { Report $t 'MISSING' '' }
    }

    # reportlab is a LIBRARY (no console script), so ask pip about it. Deliberately
    # NOT `python -c "import reportlab"`: PowerShell 5.1 strips embedded double
    # quotes from a native command line, so such a probe silently reports nothing
    # (docs\windows.md). Every argument here is a single token, and the version is
    # parsed in PowerShell.
    $py = Get-Python
    if ($py) {
        $show = & $py.Exe @($py.Args + @('-m', 'pip', 'show', 'reportlab')) 2>$null
        $line = $show | Where-Object { $_ -like 'Version:*' } | Select-Object -First 1
        if ($line) { Report 'reportlab' 'ok' ($line -replace '^Version:\s*', '') }
        else { Report 'reportlab' 'MISSING' 'PDF report stage skips (pip install reportlab)' }
    }

    # Offline crowd-model training env (REQ-F-041): optional by design — the trainer
    # exits 3 ("skipped") without it, and the app itself never needs it.
    $mlPy = Join-Path (Get-MlVenvDir) 'Scripts\python.exe'
    if (Test-Path $mlPy) {
        $show = & $mlPy -m pip show onnxmltools 2>$null
        $line = $show | Where-Object { $_ -like 'Version:*' } | Select-Object -First 1
        if ($line) { Report 'ml env' 'ok' (Get-MlVenvDir) }
        else { Report 'ml env' 'manual' 'venv present but incomplete: .\setup.ps1 ml' }
    } else {
        Report 'ml env' 'missing' 'optional: .\setup.ps1 ml (offline crowd-model training)'
    }

    $qt = Resolve-QtPrefix -Quiet
    if ($qt) { Report 'Qt' 'ok' $qt } else { Report 'Qt' 'MISSING' "expected $QtDir\$QtVersion\msvc2022_64 (with qtcharts + qtgraphs)" }

    if (Test-Path (Join-Path $Root 'tools\third-party\plantuml.jar')) {
        Report 'plantuml' 'ok' (Get-ToolVersion 'plantuml')
    } else {
        Report 'plantuml' 'missing' 'fetched on demand by tools\make_docs.ps1'
    }

    if (Get-PmdLauncher) {
        Report 'pmd' 'ok' (Get-ToolVersion 'pmd')
    } else {
        Report 'pmd' 'missing' 'copy-paste detection skips; .\setup.ps1 install fetches it'
    }

    foreach ($t in $SupplyChainTools) {
        if (Test-Tool $t) { Report $t 'ok' (Get-ToolVersion $t) } else { Report $t 'MISSING' 'supply-chain scan' }
    }
    if (Test-Tool 'OpenCppCoverage') { Report 'OpenCppCov' 'ok' (Get-ToolPath 'OpenCppCoverage') }
    else { Report 'OpenCppCov' 'MISSING' 'line coverage for MSVC builds' }

    Write-Host "== no Windows counterpart (see docs\windows.md) ==" -ForegroundColor Cyan
    Report 'clazy'    'n/a' 'Linux-only; Qt rules covered by Axivion Qt-* ruleset'
    Report 'valgrind' 'n/a' 'Linux-only; ASan (MSVC /fsanitize=address) is the Windows checker'
    Report 'tsan'     'n/a' 'ThreadSanitizer has no Windows target'
    Report 'perf'     'n/a' 'use the QBENCHMARK suite (tests\tst_benchmarks) or VS Diagnostics'

    Write-Host "== license-bound (manual) ==" -ForegroundColor Cyan
    $ax = Get-AxivionSuite
    if ($ax) { Report 'axivion' 'ok' "$($ax.Root) ($($ax.Version))" }
    else { Report 'axivion' 'manual' 'install the Axivion Suite (license required)' }
    # The Axivion MCP servers in .mcp.json take their paths from the environment
    # (tools\mcp_env.ps1 resolves them; exit 3 = no Suite installed).
    # Checks the User environment scope (the durable store), not this process: a
    # shell started before -Persist ran would otherwise report it as incomplete.
    & (Join-Path $Root 'tools\mcp_env.ps1') *> $null
    if ($LASTEXITCODE -eq 0) {
        $mcpPython = [Environment]::GetEnvironmentVariable('AXIVION_MCP_PYTHON', 'User')
        if ($mcpPython) { Report 'ax MCP' 'ok' $mcpPython }
        else { Report 'ax MCP' 'manual' 'run .\tools\mcp_env.ps1 -Persist, then restart the shell/IDE' }
    } else {
        Report 'ax MCP' 'n/a' 'no Axivion Suite — the .mcp.json servers stay unavailable'
    }
    $coco = Find-Coco
    if ($coco) {
        $lic = Join-Path $coco 'cocolic.exe'
        $state = 'license invalid'
        if (Test-Path $lic) { & $lic --check *> $null; if ($LASTEXITCODE -eq 0) { $state = 'licensed' } }
        Report 'coco' 'ok' "$coco ($state)"
    } else {
        Report 'coco' 'manual' 'optional: Squish Coco (license required)'
    }
    if (Test-Path (Join-Path $Root 'apiKeyEtoro.json')) {
        Report 'api keys' 'ok' 'apiKeyEtoro.json present'
    } else {
        Report 'api keys' 'manual' 'copy apiKeyEtoro.example.json -> apiKeyEtoro.json (app runs in SIMULATION without)'
    }
}

# ---------------------------------------------------------------------------
# install steps
# ---------------------------------------------------------------------------

function Install-WingetPackages {
    param([switch]$Upgrade)
    Write-Stage 'winget packages'
    if (-not (Test-Tool 'winget')) {
        Write-Warning "winget not available — install 'App Installer' from the Microsoft Store, or install the tools manually."
        return
    }
    foreach ($id in $WingetPkgs.Keys) {
        $probe = $WingetPkgs[$id]
        if ((-not $Upgrade) -and $probe -and (Test-Tool $probe)) {
            Write-Host "  $id already present ($probe)" -ForegroundColor DarkGray
            continue
        }
        $verb = 'install'
        if ($Upgrade) { $verb = 'upgrade' }
        Write-Host "  winget $verb $id" -ForegroundColor Gray
        & winget $verb --id $id --silent --accept-package-agreements --accept-source-agreements --disable-interactivity | Out-Null
        # 0x8A15002B / -1978335189 = "no applicable upgrade found" — not a failure.
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189 -and $LASTEXITCODE -ne -1978335212) {
            Write-Warning "  winget $verb $id exited $LASTEXITCODE"
        }
    }
    Update-SessionPath
}

function Install-VisualStudio {
    Write-Stage 'MSVC C++ toolset'
    if (Import-MsvcEnvironment) {
        Write-Host "  already present: $env:VSINSTALLDIR" -ForegroundColor DarkGray
        return
    }
    if ($NoVisualStudio) {
        Write-Skip "-NoVisualStudio given — build with the MinGW kit instead (QT_KIT=mingw_64)"
        return
    }
    Write-Host "  installing Visual Studio 2022 Build Tools with the C++ workload (several GB)..." -ForegroundColor Gray
    & winget install --id Microsoft.VisualStudio.2022.BuildTools --silent `
        --accept-package-agreements --accept-source-agreements --disable-interactivity `
        --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended" | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning "  Build Tools install exited $LASTEXITCODE" }
}

function Install-PipPackages {
    param([switch]$Upgrade)
    Write-Stage 'python tools (pip --user)'
    $py = Get-Python
    if (-not $py) { Write-Warning "no Python interpreter found — install Python 3 first"; return }
    $pipArgs = @('-m', 'pip', 'install', '--user', '--disable-pip-version-check', '--quiet')
    if ($Upgrade) { $pipArgs += '--upgrade' }
    $pipArgs += @($PipPkgs.Keys)
    & $py.Exe @($py.Args + $pipArgs)
    if ($LASTEXITCODE -ne 0) { Write-Warning "  pip install exited $LASTEXITCODE" }
    Add-PythonScriptsToPath
}

function Install-Qt {
    Write-Stage "Qt $QtVersion"
    $existing = Resolve-QtPrefix -Quiet
    if ($existing) { Write-Host "  usable kit already present: $existing" -ForegroundColor DarkGray; return }
    Add-PythonScriptsToPath
    if (-not (Test-Tool 'aqt')) { Write-Warning "  aqt missing — the pip step must run first"; return }
    Write-Host "  aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -m qtcharts qtgraphs -O $QtDir" -ForegroundColor Gray
    & aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -m qtcharts qtgraphs -O $QtDir
    if ($LASTEXITCODE -ne 0) { Write-Warning "  aqt exited $LASTEXITCODE" }
}

# PMD CPD: the copy-paste detector tools\cpd_scan.py drives (a Java tool — it
# needs the JRE winget installs above).
function Install-Pmd {
    Write-Stage 'PMD (copy-paste detection)'
    & (Join-Path $Root 'tools\fetch_pmd.ps1')
}

function Install-PlantUml {
    Write-Stage 'PlantUML'
    if (Test-Path (Join-Path $Root 'tools\third-party\plantuml.jar')) {
        Write-Host "  already present" -ForegroundColor DarkGray
        return
    }
    & (Join-Path $Root 'tools\fetch_plantuml.ps1')
}

# ---------------------------------------------------------------------------
# PATH plumbing
# ---------------------------------------------------------------------------

# Freshly installed packages extend the machine/user PATH, but this process
# still holds the PATH it started with — re-read both scopes.
function Update-SessionPath {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:PATH = (@($machine, $user, $env:PATH) | Where-Object { $_ }) -join ';'
}

# A silent `winget install` of cppcheck, Graphviz or LLVM does not extend PATH.
# The build scripts cope (tools\common.ps1 knows the well-known locations), but
# persisting them makes the tools usable from any shell, as on Linux.
function Add-ToolDirsToUserPath {
    Write-Stage 'PATH'
    foreach ($d in @(
            "$env:ProgramFiles\Cppcheck",
            "$env:ProgramFiles\LLVM\bin",
            "$env:ProgramFiles\Graphviz\bin",
            "$env:ProgramFiles\doxygen\bin",
            "$env:ProgramFiles\OpenCppCoverage")) {
        if (Test-Path $d) { Add-ToUserPath $d }
    }
    foreach ($d in (Get-PythonScriptsDirs)) { Add-ToUserPath $d }
}

function Add-ToUserPath {
    param([Parameter(Mandatory)][string]$Dir)
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $user) { $user = '' }
    if (($user -split ';') -notcontains $Dir) {
        [Environment]::SetEnvironmentVariable('Path', ($user.TrimEnd(';') + ';' + $Dir).TrimStart(';'), 'User')
        Write-Host "  added $Dir to the user PATH" -ForegroundColor DarkGray
    }
    if (($env:PATH -split ';') -notcontains $Dir) { $env:PATH = "$Dir;$env:PATH" }
}

# The Axivion MCP servers configured in .mcp.json resolve their paths from the
# environment, because the JSON must stay free of machine-specific paths and
# Claude Code's ${VAR} interpolation cannot branch on the platform. Exit 3 =
# no (license-bound) Suite installed, which is not an error here.
function Install-AxivionMcpEnv {
    Write-Stage 'Axivion MCP environment (.mcp.json)'
    & (Join-Path $Root 'tools\mcp_env.ps1') -Persist
}

# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Licence-bound test tools: Squish (GUI tests), Squish Test Center (result store)
# and Squish Coco (coverage). None can be installed unattended — they are licensed
# products behind a Qt account — so this mode does the two things that ARE possible:
# report exactly what this machine has, and print the remaining steps with the real
# paths filled in. Nothing here gates a build: every stage that uses these tools
# exits 3 ("skipped") without them, and the quality PDF lists the missing licence.
# Lockstep with `./setup.sh squish`.
function Show-SquishStatus {
    Write-Host '== licence-bound test tools ==' -ForegroundColor Cyan
    $cocoDir = if ($env:COCO_DIR) { $env:COCO_DIR } else { 'C:\Program Files\squishcoco' }
    $squishDir = if ($env:SQUISH_DIR) { $env:SQUISH_DIR }
    elseif ($env:SQUISH_PREFIX) { $env:SQUISH_PREFIX }
    else { 'C:\Program Files\froglogic\Squish' }

    $runner = Get-Command squishrunner -ErrorAction SilentlyContinue
    $runnerPath = if ($runner) { $runner.Source } else { Join-Path $squishDir 'bin\squishrunner.exe' }
    if (Test-Path $runnerPath) {
        & $runnerPath --version *> $null
        if ($LASTEXITCODE -eq 0) { Report 'squishrunner' 'ok' $runnerPath }
        else { Report 'squishrunner' 'NO LICENCE' $runnerPath }
    }
    else { Report 'squishrunner' 'MISSING' '' }

    $cscl = Join-Path $cocoDir 'cscl.exe'
    $cocolic = Join-Path $cocoDir 'cocolic.exe'
    if (Test-Path $cscl) {
        & $cocolic --check *> $null
        if ($LASTEXITCODE -eq 0) { Report 'Squish Coco' 'ok' $cocoDir }
        else { Report 'Squish Coco' 'NO LICENCE' "$cocoDir (installed)" }
    }
    else { Report 'Squish Coco' 'MISSING' '' }

    if ($env:TESTCENTER_URL -and $env:TESTCENTER_TOKEN) {
        Report 'Test Center' 'ok' $env:TESTCENTER_URL
    }
    else { Report 'Test Center' 'MISSING' 'set TESTCENTER_URL and TESTCENTER_TOKEN' }

    @'

--- Squish for Qt (GUI tests) ------------------------------------------------
1. https://account.qt.io -> Downloads -> Squish (the GUI tool, NOT Coco). Take the
   Windows package for Qt 6.
2. Install it, then point this project at it if it went somewhere unusual:
       $env:SQUISH_DIR = 'C:\Program Files\froglogic\Squish'
3. Install the licence (file or server), then:
       & "$env:SQUISH_DIR\bin\squishrunner.exe" --version     # must exit 0
4. Run the suite:
       .\tools\squish_run.ps1 -BuildDir build
   Every run is FORCED into simulation (TRADINGAPP_FORCE_SIMULATION=1 plus an
   isolated APPDATA), so a GUI test can never reach a real eToro account — the
   guarantee is in the app, not in the script (tests\tst_config.cpp TS-CFG-007).

--- Squish Test Center (every test result in one place) ----------------------
1. Same download page -> Squish Test Center; install it (default port 8800).
2. Create a project named TradingApp and an API token in its settings.
3. $env:TESTCENTER_URL = 'http://localhost:8800'
   $env:TESTCENTER_TOKEN = '<token>'
   .\tools\testcenter_upload.ps1 -DryRun     # what it would send
   .\tools\testcenter_upload.ps1             # send it

--- Squish Coco (coverage incl. MC/DC and per-test call coverage) ------------
       .\tools\coverage.ps1 -Mode coco
       .\tools\coverage.ps1 -Mode coco-components
See cocoSetupInstructions.txt for the licence steps.
'@ | Write-Host
}

# ---------------------------------------------------------------------------
# Offline crowd-model training environment (REQ-F-041) — OPTIONAL by design.
# A venv of its own, because the app must keep building, running and testing
# WITHOUT any of it: tools\ml\train_crowd_model.py exits 3 ("skipped") when
# this mode was never run. Counterpart of `./setup.sh ml`.
# ---------------------------------------------------------------------------

function Get-MlVenvDir {
    if ($env:ML_VENV_DIR) { return $env:ML_VENV_DIR }
    return (Join-Path $env:USERPROFILE '.local\tradingapp-ml')
}

function Install-MlEnv {
    $venv = Get-MlVenvDir
    Write-Stage "Crowd-model training environment ($venv)"
    $venvPy = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path $venvPy)) {
        $py = Get-Python
        if (-not $py) { Write-Warning 'no Python interpreter found - run .\setup.ps1 install first'; return }
        & $py.Exe @($py.Args + @('-m', 'venv', $venv))
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $venvPy)) { Write-Warning "venv creation failed under $venv"; return }
    }
    & $venvPy -m pip install --quiet --upgrade pip
    & $venvPy -m pip install --quiet -r (Join-Path $Root 'tools\ml\requirements.txt')
    if ($LASTEXITCODE -ne 0) { Write-Warning 'pip install failed - see tools\ml\requirements.txt'; return }
    # The import check is a temp FILE, not a -c one-liner: PowerShell 5.1 strips
    # embedded double quotes from native command lines (docs\windows.md).
    $probe = Join-Path $env:TEMP 'tradingapp-ml-probe.py'
    @'
import numpy, onnx, onnxmltools, onnxruntime, skl2onnx, sklearn, xgboost
print("ml env:", "numpy", numpy.__version__, "| scikit-learn", sklearn.__version__,
      "| xgboost", xgboost.__version__, "| onnxruntime", onnxruntime.__version__)
'@ | Set-Content -Path $probe -Encoding UTF8
    & $venvPy $probe
    Remove-Item $probe -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -ne 0) { Write-Warning 'the ml environment does not import cleanly'; return }

    # The C++ ONNX Runtime for the OPTIONAL in-app inference (REQ-F-042): provisioned beside
    # the training environment. Without it the build stays green - the inference seam just
    # reports itself unavailable. Counterpart of the setup.sh ml download.
    $ortVersion = if ($env:ONNXRUNTIME_VERSION) { $env:ONNXRUNTIME_VERSION } else { '1.28.0' }
    $ortDir = if ($env:ONNXRUNTIME_DIR) { $env:ONNXRUNTIME_DIR } else { Join-Path $env:USERPROFILE '.local\onnxruntime' }
    if (-not (Test-Path (Join-Path $ortDir 'include\onnxruntime_cxx_api.h'))) {
        $ortName = "onnxruntime-win-x64-$ortVersion"
        $zip = Join-Path $env:TEMP "$ortName.zip"
        Write-Host "-- downloading ONNX Runtime $ortVersion (win-x64, C++ runtime)"
        Invoke-WebRequest -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$ortVersion/$ortName.zip" -OutFile $zip
        $parent = Split-Path $ortDir -Parent
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        Expand-Archive -Path $zip -DestinationPath $parent -Force
        Remove-Item $zip -ErrorAction SilentlyContinue
        if (Test-Path $ortDir) { Remove-Item -Recurse -Force $ortDir }
        Move-Item (Join-Path $parent $ortName) $ortDir
    }
    Write-Host "onnxruntime C++: $ortDir"
    Write-Host '   a build configured AFTER this picks it up; RUNNING with inference needs'
    Write-Host "   $ortDir\lib\onnxruntime.dll beside the executable or on PATH"
    Write-Host ''
    Write-Host 'Train offline with (see docs/crowd-ai.md, Phase 4):'
    Write-Host '  python tools\ml\crowd_dataset.py build ...        # stdlib-only, no venv needed'
    Write-Host "  $venvPy tools\ml\train_crowd_model.py --dataset dataset.csv --manifest manifest.json --out-dir ml-out"
}

switch ($Mode) {
    'install' {
        Install-WingetPackages
        Install-VisualStudio
        Install-PipPackages
        Install-Qt
        Install-PlantUml
        Install-Pmd
        Add-ToolDirsToUserPath
        Install-AxivionMcpEnv
        Write-Host ""
        Show-Status
        Write-Host ""
        Write-Host "Done. Build everything with: .\build_all.ps1" -ForegroundColor Green
        Write-Host "(open a new shell first, so the updated PATH is picked up)" -ForegroundColor DarkGray
    }
    'update' {
        Install-WingetPackages -Upgrade
        Install-PipPackages -Upgrade
        Write-Stage 'Qt'
        Add-PythonScriptsToPath
        if (Test-Tool 'aqt') {
            $latest = (& aqt list-qt windows desktop 2>$null) -split '\s+' |
                Where-Object { $_ -match '^6\.' } |
                Sort-Object { [Version]$_ } | Select-Object -Last 1
            if ($latest -and $latest -ne $QtVersion -and -not (Test-Path "$QtDir\$latest\msvc2022_64")) {
                Write-Host "newer Qt available: $latest (installed: $QtVersion)."
                Write-Host "install with:  .\setup.ps1 install -QtVersion $latest"
                Write-Host "then build with:  `$env:QT_PREFIX='$QtDir\$latest\msvc2022_64'; .\build_all.ps1"
            } else {
                Write-Host "Qt $QtVersion is current (or the newer version is already installed)"
            }
        }
        Write-Stage 'PlantUML'
        Write-Host "pinned to the version in tools\fetch_plantuml.ps1 — bump `$Version there,"
        Write-Host "delete tools\third-party\plantuml.jar and rerun .\setup.ps1 install."
        Write-Stage 'PMD'
        Write-Host "pinned to the version in tools\fetch_pmd.ps1 — bump `$Version there and"
        Write-Host "rerun .\setup.ps1 install (the fetch script drops the old dist)."
        Write-Host ""
        Show-Status
    }
    'android' { Install-Android }
    'ml' { Install-MlEnv }
    'squish' { Show-SquishStatus }
    'status' {
        Show-Status
        # A status REPORT never gates: missing optional/license-bound tools are the
        # point of the listing, not a failure (same contract as `setup.sh status`,
        # which ends on an echo and so exits 0). Without this the branch leaked the
        # $LASTEXITCODE of whichever probe ran last, and CI read the report as a
        # failed step.
        exit 0
    }
}

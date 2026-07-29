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
              aqtinstall  (user scope — no admin needed)
      aqt     Qt $QtVersion (win64_msvc2022_64 + qtcharts) into C:\Qt — the
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
    [ValidateSet('install', 'update', 'status')]
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

    $qt = Resolve-QtPrefix -Quiet
    if ($qt) { Report 'Qt' 'ok' $qt } else { Report 'Qt' 'MISSING' "expected $QtDir\$QtVersion\msvc2022_64 (with qtcharts)" }

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
    Write-Host "  aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -m qtcharts -O $QtDir" -ForegroundColor Gray
    & aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -m qtcharts -O $QtDir
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
    'status' { Show-Status }
}

<#
.SYNOPSIS
    cppcheck's free ADDONS on Windows - MISRA first, the other three after it.
    Informational by design: none of them gates a build.

.DESCRIPTION
    The PowerShell counterpart of tools/misra_cppcheck.sh, kept in lockstep.

    The MISRA addon ships with cppcheck and is free, but it implements MISRA **C**
    2012 while this codebase is C++23, and the mismatch dominates the output.
    Measured over the whole project with cppcheck 2.13:

        404  misra-config       the addon could not parse the file at all
                               (Q_OBJECT, Q_DECLARE_METATYPE and friends)
        110  misra-c2012-12.3   "comma operator" reported on C++ TEMPLATE ARGUMENT
                               LISTS - QHash<QString, double> is not a comma
                               operator, it is C++ the addon cannot parse
         13  misra-c2012-17.2   no-recursion, mostly asynchronous continuations the
                               addon reads as self-calls
        ---
        527  total, of which 514 are the language mismatch rather than the code

    A checker whose 97% is a language mismatch cannot gate a build. MISRA C++ 2023
    for this project IS enforced - by Axivion, whose configuration here is
    MISRA-only.

    The MISRA rule TEXTS are copyrighted and cannot ship with cppcheck or with this
    repository; pass a rule-texts file extracted from your own copy of the standard
    to get readable messages instead of bare rule numbers.

.PARAMETER BuildDir
    The build tree holding compile_commands.json. Defaults to build.

.PARAMETER RuleTexts
    Path to your own MISRA rule-texts file (optional).
#>
param(
    [string]$BuildDir = 'build',
    [string]$RuleTexts = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Root 'analysis-results'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if (-not (Get-Command cppcheck -ErrorAction SilentlyContinue)) {
    Write-Host 'SKIPPED: cppcheck not installed (.\setup.ps1 installs it)' -ForegroundColor Yellow
    exit 3
}
$db = Join-Path $Root "$BuildDir\compile_commands.json"
if (-not (Test-Path $db)) {
    Write-Error "no compile database in $BuildDir - configure that build tree first"
    exit 1
}

$addon = 'misra'
if ($RuleTexts) {
    # cppcheck takes addon arguments as a JSON file; write one so the rule texts
    # reach misra.py.
    $addon = Join-Path $Out 'misra-addon.json'
    $json = @{ script = 'misra'; args = @("--rule-texts=$RuleTexts") } | ConvertTo-Json -Compress
    Set-Content -Path $addon -Value $json -Encoding ascii
    Write-Host "using rule texts from $RuleTexts"
}
else {
    Write-Host 'no rule-texts file given - messages will be bare rule IDs'
    Write-Host '(the MISRA texts are copyrighted; extract them from your own copy)' -ForegroundColor DarkGray
}

Write-Host "== cppcheck MISRA addon ($(cppcheck --version)) ==" -ForegroundColor Cyan
$misraOut = Join-Path $Out 'cppcheck-misra.txt'
# --std=c++20: the addon's front end does not parse C++23, and the point is to get
# as far as it can rather than to fail on the first `if constexpr`.
& cppcheck "--project=$db" "--addon=$addon" --std=c++20 --enable=style --library=qt `
    "-i$Root\$BuildDir" '--template={file}|{line}|{severity}|{id}|{message}' `
    "--output-file=$misraOut" --quiet
$total = 0; $comma = 0; $config = 0
if (Test-Path $misraOut) {
    $lines = Get-Content $misraOut
    $total = ($lines | Select-String 'misra').Count
    $comma = ($lines | Select-String 'misra-c2012-12.3').Count
    $config = ($lines | Select-String 'misra-config').Count
}
Write-Host "MISRA-C-2012 findings: $total (analysis-results\cppcheck-misra.txt)"
Write-Host "  of which $config are misra-config (the addon could not parse the file at all)"
Write-Host "  and $comma are rule 12.3 on C++ TEMPLATE ARGUMENT LISTS - false by construction"
Write-Host '  MISRA C++ 2023 for this codebase is enforced by Axivion: .\build_all.ps1 axivion'

Write-Host ''
Write-Host '== the other free cppcheck addons (informational) ==' -ForegroundColor Cyan
$addonsOut = Join-Path $Out 'cppcheck-addons.txt'
& cppcheck "--project=$db" --addon=threadsafety --addon=findcasts --addon=misc `
    --enable=style --library=qt "-i$Root\$BuildDir" `
    '--template={file}|{line}|{severity}|{id}|{message}' `
    "--output-file=$addonsOut" --quiet
if (Test-Path $addonsOut) {
    $rows = Get-Content $addonsOut
    Write-Host "addon findings: $($rows.Count) (analysis-results\cppcheck-addons.txt)"
    $rows | ForEach-Object { ($_ -split '\|')[3] } | Group-Object |
        Sort-Object Count -Descending |
        ForEach-Object { Write-Host ("  {0,4} {1}" -f $_.Count, $_.Name) }
    Write-Host '  misc-implicitlyVirtual wants virtual repeated on an override - the'
    Write-Host '  opposite of modern C++; threadsafety flags getenv in Config::load, which'
    Write-Host '  runs once before any thread exists; findcasts is a cast INVENTORY.'
}
# Informational by design: never fails the caller.
exit 0

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Format, or check formatting of, first-party C++ sources. The Windows counterpart of
# tools/format.sh, and deliberately the same behaviour: the same directories, the same file
# extensions, the same pinned major version, the same exit codes.
#
#   pwsh tools/format.ps1 -Check   verify only; non-zero exit when a file would change
#   pwsh tools/format.ps1 -Fix     rewrite files in place
#
# clang-format output is not stable across major versions, so the major is pinned. A
# different major would reformat the whole tree and make every diff unreadable.

[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Check')][switch]$Check,
    [Parameter(ParameterSetName = 'Fix')][switch]$Fix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RequiredMajor = 23
$RepoRoot = Split-Path -Parent $PSScriptRoot

# Find clang-format: an explicit override first, then the usual Visual Studio location, then
# whatever is on PATH. The Visual Studio copy is the one a Windows contributor is most likely
# to already have, and it is not on PATH by default.
function Find-ClangFormat {
    if ($env:CLANG_FORMAT) {
        return $env:CLANG_FORMAT
    }

    $candidates = @()

    $versioned = Get-Command "clang-format-$RequiredMajor" -ErrorAction SilentlyContinue
    if ($versioned) { $candidates += $versioned.Source }

    $onPath = Get-Command 'clang-format' -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }

    $candidates += 'C:\Program Files\LLVM\bin\clang-format.exe'

    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $root) { continue }
        $vsGlob = Join-Path $root 'Microsoft Visual Studio\*\*\VC\Tools\Llvm\*\bin\clang-format.exe'
        $candidates += (Get-ChildItem -Path $vsGlob -ErrorAction SilentlyContinue |
                        ForEach-Object { $_.FullName })
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    return $null
}

$clangFormat = Find-ClangFormat
if (-not $clangFormat) {
    Write-Error @'
clang-format not found.
  Install LLVM (winget install LLVM.LLVM), or use the copy that ships with Visual Studio's
  "C++ Clang tools for Windows" component, or set CLANG_FORMAT to its path.
'@
    exit 1
}

$versionLine = & $clangFormat --version
if ($versionLine -notmatch 'version (\d+)') {
    Write-Error "could not read a version from: $versionLine"
    exit 1
}
$major = [int]$Matches[1]

if ($major -ne $RequiredMajor) {
    Write-Error @"
clang-format major $major found, $RequiredMajor required.
  $versionLine
  Formatting differs between majors; a mismatch rewrites the whole tree.
  Set CLANG_FORMAT to a version $RequiredMajor binary.
"@
    exit 1
}

# The same four directories and the same extensions as the shell script. Sorted, so the two
# process files in the same order and a diff between their output means a real difference.
$searchRoots = @('engine', 'apps', 'tests', 'benchmarks') |
    ForEach-Object { Join-Path $RepoRoot $_ } |
    Where-Object { Test-Path -LiteralPath $_ }

$files = @(
    Get-ChildItem -Path $searchRoots -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in '.cpp', '.hpp', '.h', '.inl' } |
        Sort-Object -Property FullName |
        ForEach-Object { $_.FullName }
)

if ($files.Count -eq 0) {
    Write-Output 'no source files found'
    exit 0
}

$formatterName = Split-Path -Leaf $clangFormat

if ($Fix) {
    & $clangFormat -i --style=file @files
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Output "formatted $($files.Count) file(s) with $formatterName $major"
    exit 0
}

& $clangFormat --dry-run --Werror --style=file @files
if ($LASTEXITCODE -eq 0) {
    Write-Output "formatting clean: $($files.Count) file(s)"
    exit 0
}

Write-Error 'formatting differences above. Run: pwsh tools/format.ps1 -Fix'
exit 1

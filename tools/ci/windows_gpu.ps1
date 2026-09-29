# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Build Atlas on a Windows machine with a GPU, run the GPU tests on it, and say plainly whether they
passed (ADR-0025 D4).

.DESCRIPTION
Hosted Windows runners have no GPU, so CI excludes the `gpu` label there; this is how Windows GPU
coverage is had instead, on a real machine, run over SSH. `tools/ci/windows_gpu.sh` drives it from
the development machine: it checks out a pushed commit here, runs this, and copies the screenshots
back.

**Two sessions, and why the tests run in the second.** An SSH login lands in session 0, which has
no user desktop. On Vulkan every GPU test passes there, but session 0's display has no scaling,
and the bug the laptop found in M28 lived exactly there: `Window::display_scale` returned the
user's interface scaling, 1.75 on this desktop and 1 in session 0, so the case that catches it
passes in session 0 under the broken code too. (On Direct3D 12 session 0 could not even make a
swapchain, and every test skipped while CTest called it a pass.) So this script builds from
session 0 and runs the tests and the screenshots in the logged-in user's desktop session, through
a scheduled task that is run once, waited on and deleted. The owner allowed that in M28. Windows
open and close on that desktop while it runs; the desktop must be unlocked.

**What fails the run**, each said by name:
- any `gpu` test that fails, and any that is skipped: a skip here means no device, which is the
  failure this script exists to catch;
- a device that is not Vulkan, the only backend Atlas ships shaders for on Windows;
- a Vulkan validation error anywhere in the output;
- a screenshot that is missing or empty;
- with -BenchRuns, a benchmark group that skipped or wrote no results.

.PARAMETER Preset
The configure preset: windows-msvc-debug or windows-msvc-release. The test preset is its `-gpu`.

.PARAMETER Phase
All, from SSH. Desktop is what the scheduled task runs; not for a person.

.PARAMETER Out
Where the logs, the test report and the screenshots go. Emptied at the start of a run.

.PARAMETER BenchRuns
Run the machine's own rows and the benchmark groups that need a graphics device (machine, quads,
cell_field, allocations) this many times in the desktop session, after the tests, writing
bench-<group>-<run>.json. Release presets only: a debug build's timings describe the debug build.
#>
param(
    [ValidateSet("windows-msvc-debug", "windows-msvc-release")]
    [string]$Preset = "windows-msvc-debug",
    [ValidateSet("All", "Desktop")]
    [string]$Phase = "All",
    [string]$Out = "C:\src\atlas-gpu-out",
    [int]$TimeoutMinutes = 30,
    [int]$BenchRuns = 0
)

# Continue, not Stop: Windows PowerShell 5.1 turns a native command's stderr into a terminating
# error under Stop, which ends the run at CMake's first warning. Exit codes decide instead.
$ErrorActionPreference = "Continue"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
# The machine first: without it a slow laptop reads as a slow engine, which is what M28 did.
$BenchGroups = "machine", "quads", "cell_field", "allocations"
if ($BenchRuns -gt 0 -and $Preset -notlike "*-release") {
    "benchmarks measure a release build; use -Preset windows-msvc-release"
    exit 2
}

# The Visual Studio toolset CI compiles with, its own CMake and Ninja first on the path. Another
# CMake on the path, such as MSYS2's, is not what CI uses and is kept out of the way.
function Enter-Toolchain {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) {
        throw "no Visual Studio with the C++ toolset; see docs/adr/0025-windows-vulkan.md"
    }
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
    $cmakeTools = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
    $env:Path = "$cmakeTools\CMake\bin;$cmakeTools\Ninja;" + $env:Path
}

# What the scheduled task runs, in the desktop session: the GPU tests and two screenshots.
function Invoke-DesktopPhase {
    Enter-Toolchain
    Set-Location $Repo
    $bin = Join-Path $Repo "build\$Preset\bin"
    "session $((Get-Process -Id $PID).SessionId), interactive $([Environment]::UserInteractive)" |
        Out-File (Join-Path $Out "desktop.txt")

    ctest --preset "$Preset-gpu" -L gpu -V --output-junit (Join-Path $Out "gpu.xml") *> (Join-Path $Out "gpu.log")
    "ctest exit $LASTEXITCODE" | Out-File (Join-Path $Out "desktop.txt") -Append

    # The first frames Atlas draws on this machine, as the README's screenshots are made.
    & (Join-Path $bin "atlas_sandbox.exe") --scene --frames 30 --screenshot (Join-Path $Out "sandbox-scene.ppm") *> (Join-Path $Out "sandbox.log")
    "sandbox exit $LASTEXITCODE" | Out-File (Join-Path $Out "desktop.txt") -Append
    & (Join-Path $bin "atlas_lab.exe") --grid 64 --map-mode owner --frames 30 --screenshot (Join-Path $Out "lab-64-cells.ppm") *> (Join-Path $Out "lab.log")
    "lab exit $LASTEXITCODE" | Out-File (Join-Path $Out "desktop.txt") -Append

    # The benchmarks, groups interleaved within each run so that whatever else the machine is
    # doing falls on all of them alike. The implicit-layer setting is the GPU test presets', so
    # an overlay's layer is not timed as if it were the engine.
    $env:VK_LOADER_LAYERS_DISABLE = "~implicit~"
    foreach ($run in 1..([Math]::Max($BenchRuns, 0))) {
        if ($BenchRuns -eq 0) { break }
        foreach ($group in $BenchGroups) {
            & (Join-Path $bin "atlas_bench.exe") --quiet --filter $group --json (Join-Path $Out "bench-$group-$run.json") *> (Join-Path $Out "bench-$group-$run.txt")
        }
    }
    "done" | Out-File (Join-Path $Out "desktop.txt") -Append
}

# Run this script's desktop phase once in the logged-in user's session, wait, delete the task.
# Returns what happened, as a sentence; the caller judges the outcome by the files it left.
function Invoke-InDesktopSession {
    $name = "AtlasWindowsGpu"
    # Whoever is logged in at the desktop, as Windows names them. The SSH login's own environment
    # names the same person in a form the task scheduler cannot always map to an account.
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    if (-not $user) {
        return "nobody is logged in at the desktop"
    }
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Phase Desktop -Preset $Preset -Out `"$Out`" -BenchRuns $BenchRuns"
    $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $arguments
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    try {
        Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Force -ErrorAction Stop | Out-Null
    } catch {
        return "could not register the task for ${user}: $($_.Exception.Message)"
    }
    try {
        Start-ScheduledTask -TaskName $name -ErrorAction Stop
        Start-Sleep -Seconds 5
        $started = Get-Date
        while ((Get-ScheduledTask -TaskName $name).State -in "Running", "Queued") {
            if (((Get-Date) - $started).TotalMinutes -gt $TimeoutMinutes) {
                Stop-ScheduledTask -TaskName $name
                return "timed out after $TimeoutMinutes minutes"
            }
            Start-Sleep -Seconds 5
        }
        return "ran for $user, task result $((Get-ScheduledTaskInfo -TaskName $name).LastTaskResult)"
    } catch {
        return "could not run the task: $($_.Exception.Message)"
    } finally {
        Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    }
}

if ($Phase -eq "Desktop") {
    Invoke-DesktopPhase
    exit 0
}

# ---------------------------------------------------------------------------- from SSH
$problems = New-Object System.Collections.Generic.List[string]
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

Enter-Toolchain
Set-Location $Repo
$commit = (git rev-parse --short=12 HEAD)
"atlas-engine $commit, preset $Preset, on $env:COMPUTERNAME"

# Benchmarks on, as CI configures: the option is off by default and nothing else would build them.
cmake --preset $Preset -DATLAS_BUILD_BENCHMARKS=ON *> (Join-Path $Out "configure.log")
if ($LASTEXITCODE -ne 0) { "configure failed; see $Out\configure.log"; exit 1 }
cmake --build --preset $Preset *> (Join-Path $Out "build.log")
if ($LASTEXITCODE -ne 0) { "build failed; see $Out\build.log"; exit 1 }
"built"
if ($BenchRuns -gt 0 -and -not (Test-Path (Join-Path $Repo "build\$Preset\bin\atlas_bench.exe"))) {
    "no atlas_bench.exe in build\$Preset\bin; see $Out\configure.log"
    exit 1
}

"desktop session: " + (Invoke-InDesktopSession)
$desktop = Join-Path $Out "desktop.txt"
if (-not (Test-Path $desktop) -or -not (Select-String -Path $desktop -Pattern "^done" -Quiet)) {
    $problems.Add("the desktop phase did not finish: is the user logged in and the desktop unlocked?")
} else {
    Get-Content $desktop | Where-Object { $_ -ne "done" } | ForEach-Object { "  $_" }
}

# The tests, from CTest's own report rather than its summary line, which counts a skip as a pass.
$report = Join-Path $Out "gpu.xml"
if (Test-Path $report) {
    [xml]$junit = Get-Content $report
    $cases = @($junit.SelectNodes("//testcase"))
    $failed = @($cases | Where-Object { $_.SelectSingleNode("failure") -or $_.SelectSingleNode("error") })
    $skipped = @($cases | Where-Object { $_.SelectSingleNode("skipped") })
    "gpu tests: $($cases.Count) run, $($failed.Count) failed, $($skipped.Count) skipped"
    if ($cases.Count -eq 0) { $problems.Add("no gpu test ran") }
    foreach ($case in $failed) { $problems.Add("failed: $($case.name)") }
    foreach ($case in $skipped) { $problems.Add("skipped, which here means no device: $($case.name)") }
} else {
    $problems.Add("no test report at $report")
}

# Which backend the devices came up on, and anything Vulkan's validation layer said.
$logs = Get-ChildItem $Out -Filter *.log | ForEach-Object FullName
$backends = @(Select-String -Path $logs -Pattern "graphics device ready: backend=(\w+)" |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
"backends seen: " + ($(if ($backends) { $backends -join ", " } else { "none" }))
foreach ($backend in $backends) {
    if ($backend -ne "vulkan") { $problems.Add("a device came up on $backend, not vulkan") }
}
$validationErrors = @(Select-String -Path $logs -Pattern "Validation Error|VUID-")
"validation errors: $($validationErrors.Count)"
foreach ($line in ($validationErrors | Select-Object -First 5)) {
    $problems.Add("validation: $($line.Line.Trim())")
}

foreach ($shot in "sandbox-scene.ppm", "lab-64-cells.ppm") {
    $path = Join-Path $Out $shot
    if (-not (Test-Path $path) -or (Get-Item $path).Length -eq 0) {
        $problems.Add("no screenshot: $shot")
    } else {
        "screenshot: $path ($((Get-Item $path).Length) bytes)"
    }
}

# Every benchmark group, every run: present, and not skipped for want of a device.
foreach ($run in 1..([Math]::Max($BenchRuns, 0))) {
    if ($BenchRuns -eq 0) { break }
    foreach ($group in $BenchGroups) {
        $json = Join-Path $Out "bench-$group-$run.json"
        $text = Join-Path $Out "bench-$group-$run.txt"
        if (-not (Test-Path $json) -or (Select-String -Path $text -Pattern "skipped" -Quiet)) {
            $problems.Add("benchmark $group, run ${run}: skipped or no results; see $text")
        } elseif (@((Get-Content $json -Raw | ConvertFrom-Json).results).Count -eq 0) {
            $problems.Add("benchmark $group, run ${run}: no results")
        }
    }
}
if ($BenchRuns -gt 0) { "benchmarks: $BenchRuns run(s) of " + ($BenchGroups -join ", ") }

if ($problems.Count -gt 0) {
    "FAILED:"
    foreach ($problem in $problems) { "  $problem" }
    exit 1
}
"windows gpu: passed on $commit"
exit 0

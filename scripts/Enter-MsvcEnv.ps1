<#
.SYNOPSIS
    Import the MSVC build environment into the current PowerShell session.

.DESCRIPTION
    Locates the newest Visual Studio installation that carries the C++ toolset
    (via vswhere), runs its VsDevCmd.bat in a throwaway cmd.exe, and copies the
    resulting environment block back into this process.

    Environment variables are process-wide, so the effect survives the script
    exiting -- there is no need to dot-source. It does not survive opening a new
    shell; run the script again there.

.PARAMETER Arch
    Target architecture for the produced binaries. Default amd64.

.PARAMETER HostArch
    Architecture of the compiler itself. The x64 compiler is not bound by the
    2 GB address space of the x86 one, so amd64 is the default.

.PARAMETER ToolsetVersion
    Pin a specific MSVC toolset, e.g. 14.44. Default is the installation's
    newest. Maps to VsDevCmd's -vcvars_ver.

.PARAMETER WindowsSdkVersion
    Pin a specific Windows SDK, e.g. 10.0.22621.0. Maps to -winsdk.

.PARAMETER Prerelease
    Also consider Preview channel installations.

.PARAMETER Force
    Re-import even when this session already has an MSVC environment loaded.
    Note that VsDevCmd prepends rather than replaces, so each forced reload
    leaves another copy of the toolset directories on PATH/INCLUDE/LIB. Harmless
    but untidy; a fresh shell is the cleaner way to switch architectures.

.PARAMETER Quiet
    Suppress the summary line.

.EXAMPLE
    .\scripts\Enter-MsvcEnv.ps1
    Loads the x64 toolset targeting x64.

.EXAMPLE
    .\scripts\Enter-MsvcEnv.ps1 -Arch arm64
    Cross-compiles for ARM64 with the x64-hosted compiler.
#>
[CmdletBinding()]
param(
    [ValidateSet('amd64', 'x86', 'arm64', 'arm')]
    [string]$Arch = 'amd64',

    [ValidateSet('amd64', 'x86')]
    [string]$HostArch = 'amd64',

    # Both are pasted onto a cmd.exe command line unquoted (VsDevCmd.bat's own
    # parser chokes on quoted arguments), so restrict them to version syntax.
    [ValidatePattern('^[0-9][0-9.]*$')]
    [string]$ToolsetVersion,

    [ValidatePattern('^[0-9][0-9.]*$')]
    [string]$WindowsSdkVersion,

    [switch]$Prerelease,

    [switch]$Force,

    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The C++ workload component. Asking vswhere for it skips installations that
# only carry, say, the .NET workload and would hand us a VsDevCmd without cl.exe.
$VcToolsComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'

function Find-VsWhere {
    # vswhere ships with the VS Installer at a fixed, versioned-forever path.
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $installer) { return $installer }

    # Fall back to a copy on PATH (chocolatey, winget, a vendored one).
    $onPath = Get-Command vswhere.exe -CommandType Application -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw 'vswhere.exe not found. Install Visual Studio (or the Build Tools), or put vswhere.exe on PATH.'
}

function Find-VsInstallPath {
    param([string]$VsWhere, [bool]$IncludePrerelease)

    $vsWhereArgs = @(
        '-latest'
        '-products', '*'
        '-requires', $VcToolsComponent
        '-property', 'installationPath'
    )
    if ($IncludePrerelease) { $vsWhereArgs += '-prerelease' }

    $found = & $VsWhere @vsWhereArgs | Select-Object -First 1
    if (-not $found) {
        throw "No Visual Studio installation with the C++ toolset ($VcToolsComponent) was found." +
              ' Install the "Desktop development with C++" workload.'
    }
    return $found
}

if ($env:VSCMD_VER -and -not $Force) {
    if (-not $Quiet) {
        Write-Host "MSVC environment already loaded (VS $env:VSCMD_VER, target $env:VSCMD_ARG_TGT_ARCH). Use -Force to reload." -ForegroundColor DarkGray
    }
    return
}

$vsWhere = Find-VsWhere
$vsRoot = Find-VsInstallPath -VsWhere $vsWhere -IncludePrerelease:$Prerelease.IsPresent

$devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $devCmd)) {
    throw "VsDevCmd.bat missing from the installation at '$vsRoot'. The install may be damaged; repair it from the VS Installer."
}

$devCmdArgs = @("-arch=$Arch", "-host_arch=$HostArch", '-no_logo')
if ($ToolsetVersion) { $devCmdArgs += "-vcvars_ver=$ToolsetVersion" }
if ($WindowsSdkVersion) { $devCmdArgs += "-winsdk=$WindowsSdkVersion" }

# A sentinel separates any chatter VsDevCmd emits from the environment dump, so
# a stray warning line cannot be mistaken for a variable.
$sentinel = '___MSVC_ENV_BEGIN___'

# VsDevCmd.bat's argument parser compares %1 literally, so a quoted "-arch=amd64"
# does not match and the script bails with "The syntax of the command is
# incorrect". Pass the switches bare; the parameter validation above keeps them
# free of whitespace and shell metacharacters.
#
# /d skips any cmd AutoRun registry hook, which could otherwise inject variables
# that have nothing to do with MSVC. chcp 65001 forces UTF-8 out of cmd so paths
# survive the trip back on non-Latin system locales. Clearing VSCMD_VER disarms
# VsDevCmd's own once-per-session guard, which the child would otherwise trip on
# an inherited environment -- that guard's job is done by -Force up above.
$command = "chcp 65001 >nul&& set VSCMD_VER=&& `"$devCmd`" $($devCmdArgs -join ' ')&& echo $sentinel&& set"

$previousEncoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

    # VsDevCmd writes harmless warnings to stderr; with $ErrorActionPreference
    # left at Stop, merging them into the output stream can abort the script.
    $ErrorActionPreference = 'Continue'
    $output = [string[]](& cmd.exe /d /s /c $command 2>&1 | ForEach-Object { "$_" })
    $exitCode = $LASTEXITCODE
}
finally {
    [Console]::OutputEncoding = $previousEncoding
    $ErrorActionPreference = 'Stop'
}

if ($exitCode -ne 0) {
    Write-Host ($output -join [Environment]::NewLine)
    throw "VsDevCmd.bat failed with exit code $exitCode."
}

# Compared trimmed: cmd echoes any whitespace that sits between the sentinel and
# the following '&&' as part of the line.
$sentinelIndex = -1
for ($i = 0; $i -lt $output.Count; $i++) {
    if ($output[$i].Trim() -eq $sentinel) { $sentinelIndex = $i; break }
}
if ($sentinelIndex -lt 0) {
    Write-Host ($output -join [Environment]::NewLine)
    throw 'VsDevCmd.bat produced no environment block.'
}

$imported = 0
foreach ($line in $output[($sentinelIndex + 1)..($output.Count - 1)]) {
    # cmd also exports per-drive cursors like "=D:=D:\workspace"; requiring a
    # non-empty name before the first '=' drops them. Values may contain '='.
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -LiteralPath "env:$($Matches[1])" -Value $Matches[2]
        $imported++
    }
}

$cl = Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $cl) {
    throw "Imported $imported variables from VsDevCmd.bat, but cl.exe is still not on PATH. Try -Force, or check the C++ toolset installation."
}

if (-not $Quiet) {
    $sdk = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { 'none' }
    Write-Host "MSVC $env:VCToolsVersion  host=$HostArch  target=$Arch  SDK $sdk" -ForegroundColor Green
    Write-Verbose "cl.exe: $($cl.Source)"
}

<#
.SYNOPSIS
    Import the MSVC build environment into the current PowerShell session.

.DESCRIPTION
    Locates the newest Visual Studio installation that carries the C++ toolset
    (via vswhere), runs its VsDevCmd.bat in a throwaway cmd.exe, and copies the
    resulting environment block back into this process.

    Environment variables are process-wide, so the effect survives the script
    exiting -- there is no need to dot-source. It does not outlive the *process*,
    though: a new shell, a `pwsh -c ...` one-liner, or any harness that spawns a
    fresh shell per command gets none of it. Such a caller has to enter the
    environment and run the build in the same invocation.

.PARAMETER Arch
    Target architecture for the produced binaries. Default amd64.

.PARAMETER HostArch
    Architecture of the compiler itself. The x64 compiler is not bound by the
    2 GB address space of the x86 one, so amd64 is the default.

.PARAMETER ToolsetVersion
    Pin a specific MSVC toolset, e.g. 14.44. Maps to VsDevCmd's -vcvars_ver.

    Default is the newest toolset that actually has a cl.exe for the requested
    host/target pair -- which is not always the newest one installed. A partially
    uninstalled or interrupted VS update can leave a toolset's headers and libs
    on disk with the compiler binaries gone; VsDevCmd would happily select it and
    hand back an environment where cl.exe does not exist. Measured on a real box
    2026-09-09: 14.50 and 14.51 had no bin\Hostx64\x64\cl.exe, only 14.44 did.

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

.EXAMPLE
    .\scripts\Enter-MsvcEnv.ps1 -ListToolsets
    Prints the installed toolsets and whether each one still has a compiler,
    without touching the environment. Start here when a build says cl.exe is
    missing, or when the summary line names an older toolset than you expected.

.NOTES
    keel builds with Ninja and a single CMAKE_BUILD_TYPE -- never a multi-config
    (per-config) generator. See cmake/KeelOutputDir.cmake for why the shim output
    directory is flattened, and the keel.layers.native-output-dir guard that
    keeps it that way.
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

    [switch]$Quiet,

    # Report what is installed and bail out. Changes nothing.
    [switch]$ListToolsets
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

# ⚠️ vswhere is asked for -utf8, but PowerShell decodes a native command's stdout
# with [Console]::OutputEncoding -- the console's ANSI codepage unless someone
# changed it. On a Chinese box (cp936/GBK) that is not merely mojibake: GBK is a
# double-byte encoding whose lead bytes claim a trail byte, so an odd-length run
# of UTF-8 bytes *swallows the ASCII character that follows it* -- and in
# vswhere's JSON that character is the closing quote of the localized
# "description". The string never closes and the whole document stops parsing.
# Measured 2026-09-10 at cp936:
#   "description": "...高效工作<U+FFFD>,          <- closing quote eaten
#   ConvertFrom-Json: unexpected character 'c'. Path '[0].description', line 17.
# The cmd.exe call further down already carries this guard; these did not.
function Invoke-VsWhere {
    param([string]$VsWhere, [string[]]$Arguments)

    $previousEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        & $VsWhere @Arguments
    }
    finally { [Console]::OutputEncoding = $previousEncoding }
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

    $found = Invoke-VsWhere -VsWhere $VsWhere -Arguments $vsWhereArgs | Select-Object -First 1
    if ($found) { return $found }

    # Nothing matched the strict query. That is usually the right answer, but it
    # is also what an *incomplete* installation looks like: vswhere hides those
    # unless asked with -all, and an incomplete install reports no components at
    # all -- even ones whose files are sitting right there on disk. Measured
    # 2026-09-09: VS 18 with isComplete=False, isLaunchable=False, state=13
    # (Local|NoRebootRequired|NoErrors, no Registered bit), -requires returning
    # nothing, yet a working 14.44 toolset present under VC\Tools\MSVC.
    #
    # So look again with -all and say loudly what was found. The toolset scan
    # below is the real gate -- it checks for an actual cl.exe rather than
    # trusting the component database -- so falling through to it here trades a
    # confident "not installed" for an accurate "this install is damaged, and
    # here is the part of it that still works".
    $relaxedArgs = @('-latest', '-all', '-products', '*', '-format', 'json', '-utf8')
    if ($IncludePrerelease) { $relaxedArgs += '-prerelease' }

    $json = (Invoke-VsWhere -VsWhere $VsWhere -Arguments $relaxedArgs) -join [Environment]::NewLine
    $instance = $null
    if ($json.Trim()) {
        try {
            # ⚠️ Windows PowerShell 5.1's ConvertFrom-Json emits a JSON *array* as
            # a single Object[] instead of enumerating it, so a pipeline
            # 'Select-Object -First 1' selects the whole array -- and then
            # $instance.installationPath is a *list* of paths, which stays
            # invisible only while exactly one VS is installed. Land it, index it.
            $decoded = $json | ConvertFrom-Json
            $instances = @($decoded)
            if ($instances.Count -gt 0) { $instance = $instances[0] }
        }
        catch {
            # ⚠️ This blob is read for two diagnostic flags and nothing else, yet
            # it carries localized free text -- so never let it decide whether the
            # build may run. Retry with -property, which yields a bare path and
            # has no JSON to come apart.
            Write-Warning "Could not parse vswhere's JSON ($($_.Exception.Message)). Retrying with -property installationPath."
            $bareArgs = @('-latest', '-all', '-products', '*', '-property', 'installationPath')
            if ($IncludePrerelease) { $bareArgs += '-prerelease' }
            $barePath = Invoke-VsWhere -VsWhere $VsWhere -Arguments $bareArgs | Select-Object -First 1
            if ($barePath) { $instance = [pscustomobject]@{ installationPath = $barePath } }
        }
    }

    if (-not $instance) {
        throw "No Visual Studio installation with the C++ toolset ($VcToolsComponent) was found." +
              ' Install the "Desktop development with C++" workload.'
    }

    $state = @(
        "isComplete=$(Get-InstanceFlag $instance 'isComplete')"
        "isLaunchable=$(Get-InstanceFlag $instance 'isLaunchable')"
    ) -join ', '
    Write-Warning ("vswhere reports no *complete* installation carrying $VcToolsComponent; " +
                   "falling back to '$($instance.installationPath)' ($state). " +
                   'This installation is damaged -- repair it from the Visual Studio Installer. ' +
                   'Continuing with whatever toolset still has a compiler.')
    return $instance.installationPath
}

function Get-InstanceFlag {
    # StrictMode makes a missing property fatal, and vswhere's JSON shape varies
    # with version -- read defensively.
    param($Instance, [string]$Name)
    $prop = $Instance.PSObject.Properties[$Name]
    if ($prop) { return $prop.Value }
    return 'unknown'
}

# ── Toolsets on disk ────────────────────────────────────────────────────────
#
# ⚠️ The component database says what was *meant* to be installed; this looks
# for the file we are about to need. Those disagree exactly when it hurts most.
function Get-Toolsets {
    param([string]$VsRoot, [string]$HostDir, [string]$TargetDir)

    $root = Join-Path $VsRoot 'VC\Tools\MSVC'
    if (-not (Test-Path -LiteralPath $root)) { return @() }

    $items = foreach ($dir in Get-ChildItem -LiteralPath $root -Directory) {
        $parsed = $null
        [void][version]::TryParse($dir.Name, [ref]$parsed)
        $cl = Join-Path $dir.FullName "bin\$HostDir\$TargetDir\cl.exe"
        [pscustomobject]@{
            Version  = $dir.Name
            Parsed   = $parsed
            Compiler = $cl
            HasCl    = Test-Path -LiteralPath $cl
        }
    }
    # Unparseable names sort last rather than throwing.
    return @($items | Sort-Object -Property @{ Expression = 'Parsed'; Descending = $true },
                                            @{ Expression = 'Version'; Descending = $true })
}

function Format-ToolsetTable {
    param($Toolsets)
    if (-not $Toolsets) { return '  (none found under VC\Tools\MSVC)' }
    ($Toolsets | ForEach-Object {
        $mark = if ($_.HasCl) { 'cl.exe present' } else { 'NO cl.exe' }
        "  {0,-16} {1}" -f $_.Version, $mark
    }) -join [Environment]::NewLine
}

# ⚠️ Never fold this table into a throw message: PowerShell's error view collapses
# a multi-line message onto one line, so the table turns to mush at exactly the
# moment it was supposed to explain the failure. Print it, then throw short.
function Write-ToolsetReport {
    param($Toolsets, [string]$HostDir, [string]$TargetDir)
    Write-Host "Installed MSVC toolsets (looking for bin\$HostDir\$TargetDir\cl.exe):"
    Write-Host (Format-ToolsetTable -Toolsets $Toolsets)
}

if ($env:VSCMD_VER -and -not $Force -and -not $ListToolsets) {
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

# cl.exe lives under bin\Host<host>\<target>\, and those directory names are not
# the names VsDevCmd takes: amd64 is spelled x64 here.
$archDirs = @{ amd64 = 'x64'; x86 = 'x86'; arm64 = 'arm64'; arm = 'arm' }
$hostDir = "Host$($archDirs[$HostArch])"
$targetDir = $archDirs[$Arch]

$toolsets = Get-Toolsets -VsRoot $vsRoot -HostDir $hostDir -TargetDir $targetDir
$usable = @($toolsets | Where-Object { $_.HasCl })

if ($ListToolsets) {
    Write-Host "Visual Studio: $vsRoot"
    Write-Host "Toolsets for host=$HostArch target=$Arch (bin\$hostDir\$targetDir\cl.exe):"
    Write-Host (Format-ToolsetTable -Toolsets $toolsets)
    return
}

# ── Which toolset ───────────────────────────────────────────────────────────
#
# ⭐ Pinned or not, the choice is made here rather than left to VsDevCmd's
# default, so that "newest installed" can never win over "newest that can
# actually compile". The summary line then names what was picked -- a build that
# silently moves to an older toolset is worth one printed line.
$selected = $null
if ($ToolsetVersion) {
    $selected = $usable |
        Where-Object { $_.Version -eq $ToolsetVersion -or $_.Version.StartsWith("$ToolsetVersion.") } |
        Select-Object -First 1
    if (-not $selected) {
        Write-ToolsetReport -Toolsets $toolsets -HostDir $hostDir -TargetDir $targetDir
        throw "No usable MSVC toolset matches -ToolsetVersion $ToolsetVersion for host=$HostArch target=$Arch."
    }
} else {
    if (-not $usable) {
        Write-ToolsetReport -Toolsets $toolsets -HostDir $hostDir -TargetDir $targetDir
        throw ("Visual Studio at '$vsRoot' has no MSVC toolset with a compiler for host=$HostArch target=$Arch." +
               ' Repair the installation, or install the "Desktop development with C++" workload.')
    }
    $selected = $usable[0]
    if ($toolsets[0].Version -ne $selected.Version) {
        Write-Warning ("Newest installed toolset $($toolsets[0].Version) has no bin\$hostDir\$targetDir\cl.exe " +
                       "(headers and libs may still be there -- that is what a half-removed toolset looks like); " +
                       "using $($selected.Version) instead.")
    }
}

# VsDevCmd's -vcvars_ver takes the two-component form (14.44), which it resolves
# through VC\Auxiliary\Build; the full 14.44.35207 is what lands in
# %VCToolsVersion% and is verified after the import.
$selectedShort = if ($selected.Parsed) { "$($selected.Parsed.Major).$($selected.Parsed.Minor)" } else { $selected.Version }

$devCmdArgs = @("-arch=$Arch", "-host_arch=$HostArch", '-no_logo', "-vcvars_ver=$selectedShort")
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
# ⚠️ The tail has to be non-empty too: with the sentinel as the last line,
# ($sentinelIndex + 1)..($output.Count - 1) counts *backwards* and re-walks the
# whole transcript as if it were variables.
if ($sentinelIndex -lt 0 -or $sentinelIndex -ge $output.Count - 1) {
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
    Write-ToolsetReport -Toolsets $toolsets -HostDir $hostDir -TargetDir $targetDir
    throw ("Imported $imported variables from VsDevCmd.bat, but cl.exe is still not on PATH." +
           " Requested toolset $selectedShort. Try -Force, or repair the C++ toolset installation.")
}

# ⚠️ VsDevCmd resolves -vcvars_ver itself and does *not* fail loudly when the
# request cannot be honoured -- so confirm we got what was asked for rather than
# whatever the installation defaults to. This is the check that would have
# caught the half-removed 14.51 on its own.
if ($env:VCToolsVersion -and -not $env:VCToolsVersion.StartsWith("$selectedShort.")) {
    Write-Warning ("Asked VsDevCmd for toolset $selectedShort but the environment reports " +
                   "$env:VCToolsVersion. cl.exe: $($cl.Source)")
}

# ── MSBuild's toolset lookup, for the vcxproj builds in deps/ ───────────────
#
# ⚠️ This is a *workaround*, and it is deliberately narrow. The front door for
# keel's own vendored-ICU build is a command-line global property
# (/p:VCToolsVersion=...), which cmake/KeelIcu.cmake now passes; command-line
# globals cannot be overridden from inside a project, which is exactly why they
# work. This function exists only for the vcxproj builds keel does *not*
# generate -- today that is deps/libbitcask's own vendored ICU, whose
# BitcaskICU.cmake passes /p:PlatformToolset without the matching
# /p:VCToolsVersion (reported upstream 2026-09-10).
#
# The failure it prevents, measured on this box 2026-09-10:
#   error MSB8052: MSVC toolset version "14.51.36231" is incompatible with
#   platform toolset "v143"
# in a shell whose cl.exe *is* 14.44. The chain is entirely by Microsoft's
# design, three steps, none of them a broken install:
#   1. Microsoft.Cpp.Default.props clears <VCToolsVersion /> -- so whatever
#      vcvars exported stops counting right there. Setting the environment
#      variable VCToolsVersion does NOT help (measured).
#   2. Microsoft.Cpp.VCTools.props:28 looks for
#      Auxiliary\Build\Microsoft.VCToolsVersion.v<N>.default.props and falls
#      back to the version-independent one when it is missing.
#   3. VS 18 does not ship ...v143.default.props (the same-named .txt is still
#      there). The fallback picks newest-first (v145 -> v143 -> ...) and lands
#      on 14.51 -- Microsoft's own comment in that file concedes "there is a
#      possibility of the project's PlatformToolset setting and the chosen
#      props file being out of sync".
#
# _VCToolsVersionProps is what step 2 computes, and it is guarded by
# Condition="'$(_VCToolsVersionProps)' == ''", so an environment variable wins.
# Pointing it at the side-by-side props for the toolset we actually selected
# reconnects step 2 to reality.
#
# ⚠️ It is an underscore-prefixed private MSBuild property and may change
# without notice, so: only when the per-toolset props is genuinely missing,
# only when the caller has not set it, and only pointing at a file that really
# does declare the version we picked. If any of that fails we leave it alone --
# a build that stops with MSB8052 is far better than one silently compiled by
# a toolset nobody chose.
function Set-VCToolsVersionPropsBridge {
    param([string]$VsRoot, [string]$ToolsVersion)

    if ($env:_VCToolsVersionProps) { return }        # caller already decided
    if (-not $ToolsVersion) { return }

    $auxBuild = Join-Path $VsRoot 'VC\Auxiliary\Build'
    if (-not (Test-Path -LiteralPath $auxBuild)) { return }

    # Which platform toolset owns this compiler? The .txt files are the mapping
    # MSBuild itself publishes; read them rather than hardcoding 14.4x -> v143.
    $ownerToolset = $null
    foreach ($txt in Get-ChildItem -LiteralPath $auxBuild -Filter 'Microsoft.VCToolsVersion.v*.default.txt' -ErrorAction SilentlyContinue) {
        if ((Get-Content -LiteralPath $txt.FullName -Raw).Trim() -eq $ToolsVersion) {
            if ($txt.Name -match '\.(v\d+)\.default\.txt$') { $ownerToolset = $Matches[1] }
            break
        }
    }
    if (-not $ownerToolset) { return }

    # Present => step 2 resolves correctly on its own; touching anything here
    # would only risk overriding a healthy installation.
    if (Test-Path -LiteralPath (Join-Path $auxBuild "Microsoft.VCToolsVersion.$ownerToolset.default.props")) { return }

    # Find the side-by-side props that declares exactly our version. The
    # directory is named after the *redist* line (e.g. 14.44.17.14), which is
    # not the toolset version -- so match on content, not on the folder name.
    foreach ($props in Get-ChildItem -LiteralPath $auxBuild -Recurse -Depth 1 -Filter 'Microsoft.VCToolsVersion.*.props' -ErrorAction SilentlyContinue) {
        if ($props.DirectoryName -eq $auxBuild) { continue }   # the default/v145 ones
        $text = Get-Content -LiteralPath $props.FullName -Raw
        if ($text -match "<VCToolsVersion[^>]*>\s*$([regex]::Escape($ToolsVersion))\s*</VCToolsVersion>") {
            $env:_VCToolsVersionProps = $props.FullName
            Write-Warning ("VS 18 does not ship Microsoft.VCToolsVersion.$ownerToolset.default.props, so MSBuild would " +
                           "resolve VCToolsVersion to the newest toolset instead of $ToolsVersion (MSB8052). " +
                           "Bridged _VCToolsVersionProps -> $($props.FullName). " +
                           'This only affects vcxproj builds that do not pin /p:VCToolsVersion themselves.')
            return
        }
    }
}

Set-VCToolsVersionPropsBridge -VsRoot $vsRoot -ToolsVersion $env:VCToolsVersion

if (-not $Quiet) {
    $sdk = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { 'none' }
    Write-Host "MSVC $env:VCToolsVersion  host=$HostArch  target=$Arch  SDK $sdk" -ForegroundColor Green
    Write-Verbose "cl.exe: $($cl.Source)"
}

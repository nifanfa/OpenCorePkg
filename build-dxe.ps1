[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $Inf = "Platform\OpenRuntime\OpenRuntime.inf",

    [ValidateSet("DEBUG", "RELEASE", "NOOPT")]
    [string] $Configuration = "DEBUG",

    [ValidateRange(1, 256)]
    [int] $Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$architecture = "X64"

$repo = Split-Path -Parent $PSCommandPath
$udk = Join-Path $repo "UDK"
$repoParent = Split-Path -Parent $repo
$infPath = if ([IO.Path]::IsPathRooted($Inf)) { $Inf } else { Join-Path $repo $Inf }
$infPath = [IO.Path]::GetFullPath($infPath)

if (-not (Test-Path -LiteralPath $infPath -PathType Leaf)) {
    throw "INF file not found: $infPath"
}

$repoPrefix = $repo.TrimEnd("\") + "\"
if (-not $infPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The INF must be inside $repo"
}

if (-not (Test-Path -LiteralPath (Join-Path $udk "edksetup.bat") -PathType Leaf)) {
    throw "UDK is missing. Expected: $udk"
}

$nasm = Get-ChildItem (Join-Path $repo ".tools\nasm") -Filter nasm.exe -Recurse |
    Select-Object -First 1
$iasl = Get-ChildItem (Join-Path $repo ".tools\iasl") -Filter iasl.exe -Recurse |
    Select-Object -First 1
if ($null -eq $nasm) { throw "nasm.exe is missing under $repo\.tools\nasm" }
if ($null -eq $iasl) { throw "iasl.exe is missing under $repo\.tools\iasl" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found. Install Visual Studio C++ build tools."
}

$vsInstall = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1).Trim()
if (-not $vsInstall) {
    throw "No Visual Studio installation with the x86/x64 C++ tools was found."
}

$v143File = Join-Path $vsInstall "VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt"
$defaultToolsFile = Join-Path $vsInstall "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
$toolsFile = if (Test-Path -LiteralPath $v143File) { $v143File } else { $defaultToolsFile }
if (-not (Test-Path -LiteralPath $toolsFile -PathType Leaf)) {
    throw "Visual C++ toolset version file was not found under $vsInstall"
}
$vcToolsVersion = (Get-Content -LiteralPath $toolsFile -Raw).Trim()
$vcVars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvarsall.bat"

$relativeInf = $infPath.Substring($repoPrefix.Length).Replace("\", "/")
$edkInf = "OpenCorePkg/$relativeInf"
$infText = Get-Content -LiteralPath $infPath -Raw
$baseNameMatch = [regex]::Match($infText, "(?m)^\s*BASE_NAME\s*=\s*(\S+)\s*$")
if (-not $baseNameMatch.Success) {
    throw "BASE_NAME was not found in $infPath"
}
$baseName = $baseNameMatch.Groups[1].Value

$env:WORKSPACE = $udk
$env:PACKAGES_PATH = "$udk;$repoParent"
$env:CONF_PATH = Join-Path $udk "Conf"
$env:EDK_TOOLS_PATH = Join-Path $udk "BaseTools"
$env:BASE_TOOLS_PATH = $env:EDK_TOOLS_PATH
$env:PYTHON_COMMAND = (Get-Command python -ErrorAction Stop).Source
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
$env:NASM_PREFIX = (Split-Path -Parent $nasm.FullName).TrimEnd("\") + "\"
$env:IASL_PREFIX = (Split-Path -Parent $iasl.FullName).TrimEnd("\") + "\"
$env:VS170COMNTOOLS = (Join-Path $vsInstall "Common7\Tools").TrimEnd("\") + "\"
$env:VS2022_PREFIX = (Join-Path $vsInstall "VC\Tools\MSVC\$vcToolsVersion").TrimEnd("\") + "\"

# OpenCore contains UTF-8 source/comments.  Keep any caller-provided CL flags
# while forcing MSVC to interpret source and execution strings as UTF-8.
$env:CL = if ([string]::IsNullOrWhiteSpace($env:CL)) {
    "/utf-8"
} elseif ($env:CL -notmatch "(^|\s)/utf-8(\s|$)") {
    "$($env:CL.Trim()) /utf-8"
} else {
    $env:CL
}

$baseToolsReady = Test-Path -LiteralPath (Join-Path $udk "BaseTools\Bin\Win32\GenFfs.exe")
$setupArguments = if ($baseToolsReady) { "VS2022" } else { "Rebuild VS2022" }
$buildArguments = @(
    "-a", $architecture,
    "-b", $Configuration,
    "-t", "VS2022",
    "-p", "OpenCorePkg/OpenCorePkg.dsc",
    "-m", $edkInf,
    "-n", $Jobs
) -join " "

# Acidanthera BaseTools host utilities are built as IA32, matching upstream CI.
$command = "call `"$vcVars`" x86 -vcvars_ver=$vcToolsVersion" +
    " && call `"$udk\edksetup.bat`" $setupArguments" +
    " && build $buildArguments"

Write-Host "Building $edkInf ($architecture $Configuration, VS2022/v$vcToolsVersion)"
Push-Location $udk
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "EDK II build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$output = Join-Path $udk "Build\OpenCorePkg\${Configuration}_VS2022\$architecture\$baseName.efi"
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
    throw "Build succeeded, but the expected output was not found: $output"
}

Write-Host "Output: $output"

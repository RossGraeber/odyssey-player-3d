param(
    [string]$InputPath = 'H:\3D\Batman_Begins__2005__Remastered_MVC_1080p_dgc.iso',
    [ValidateRange(1, 600)]
    [int]$Seconds = 60,
    [ValidateRange(0, 86400)]
    [int]$SeekSeconds = 1800
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$depsDir = Join-Path $repoRoot 'build\mvc_probe\deps'
$lavDir = Join-Path $depsDir 'lav-0.83'
$buildDir = Join-Path $repoRoot 'build\mvc_probe\cmake'
$exePath = Join-Path $buildDir 'Release\mvc_probe.exe'

& (Join-Path $repoRoot 'tools\bootstrap-private-deps.ps1') -DepsDirectory $depsDir

& cmake -S $PSScriptRoot -B $buildDir -A x64 "-DLAV_INCLUDE_DIR=$($lavDir -replace '\\', '/')/include"
if ($LASTEXITCODE -ne 0) { throw "MVC probe configure failed with exit code $LASTEXITCODE" }
& cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "MVC probe build failed with exit code $LASTEXITCODE" }

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
function Invoke-Probe([string]$Path) {
    & $exePath --input $Path --lav-dir $lavDir --seconds $Seconds --seek-seconds $SeekSeconds | Out-Host
    $probeExitCode = $LASTEXITCODE
    return $probeExitCode
}

Write-Host "Direct input attempt: $resolvedInput"
$result = Invoke-Probe $resolvedInput
if ([IO.Path]::GetExtension($resolvedInput) -ine '.iso' -or $result -eq 0) {
    exit $result
}

$diskImage = Get-DiskImage -ImagePath $resolvedInput -ErrorAction Stop
$mountedHere = $false
try {
    if (-not $diskImage.Attached) {
        Write-Host 'Direct ISO access did not prove MVC; mounting read-only for BDMV access.'
        Mount-DiskImage -ImagePath $resolvedInput -Access ReadOnly -ErrorAction Stop | Out-Null
        $mountedHere = $true
    } else {
        Write-Host 'Direct ISO access did not prove MVC; reusing the existing mount.'
    }

    $volume = Get-DiskImage -ImagePath $resolvedInput -ErrorAction Stop | Get-Volume -ErrorAction Stop |
        Where-Object DriveLetter | Select-Object -First 1
    if (-not $volume) { throw 'The mounted image has no drive-letter volume.' }

    $indexPath = "$($volume.DriveLetter):\BDMV\index.bdmv"
    if (-not (Test-Path -LiteralPath $indexPath)) { throw "Mounted image has no $indexPath" }
    $result = Invoke-Probe $indexPath
} finally {
    if ($mountedHere) {
        Dismount-DiskImage -ImagePath $resolvedInput -ErrorAction Stop | Out-Null
    }
}

exit $result

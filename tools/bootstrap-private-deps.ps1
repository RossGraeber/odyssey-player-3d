param(
    [string]$DepsDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build'))
if ([string]::IsNullOrWhiteSpace($DepsDirectory)) {
    $DepsDirectory = Join-Path $buildRoot 'mvc_probe\deps'
} elseif (-not [IO.Path]::IsPathRooted($DepsDirectory)) {
    $DepsDirectory = Join-Path $repoRoot $DepsDirectory
}
$depsDir = [IO.Path]::GetFullPath($DepsDirectory)
$buildPrefix = $buildRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $depsDir.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Dependency staging must remain under the ignored build directory: $buildRoot"
}

function Assert-NoReparseInStagingPath {
    if (Test-Path -LiteralPath $buildRoot) {
        $buildItem = Get-Item -LiteralPath $buildRoot -Force
        if (($buildItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to stage dependencies through a reparse point: $buildRoot"
        }
    }
    $relative = $depsDir.Substring($buildPrefix.Length)
    $current = $buildRoot.TrimEnd('\', '/')
    foreach ($component in ($relative -split '[\\/]' | Where-Object { $_ })) {
        $current = Join-Path $current $component
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to stage dependencies through a reparse point: $current"
            }
        }
    }
}

$artifacts = @(
    [pscustomobject]@{
        Name = 'LAVFilters-0.83-x64.zip'
        Uri = 'https://github.com/Nevcairiel/LAVFilters/releases/download/0.83/LAVFilters-0.83-x64.zip'
        Sha256 = '0126982F47157BB86A6DBB43C4F332F7F98BEBA9AD552C19B65F9DB2E7D4F186'
    },
    [pscustomobject]@{
        Name = 'libmfxsw64-v3.7z'
        Uri = 'https://files.1f0.de/lavf/plugins/libmfxsw64-v3.7z'
        Sha256 = '5CEFD058F2523A2486C6E875E4D256563AC45554A87F3623DC84F87A09409B5B'
    },
    [pscustomobject]@{
        Name = 'LAVFilters-source-0.83.zip'
        Uri = 'https://github.com/Nevcairiel/LAVFilters/archive/refs/tags/0.83.zip'
        Sha256 = 'B0889E7C7570E65ACD00C5A3677EEF2C7851B1E60F4D0F591645D81AB084A98F'
    }
)

$approvedFiles = [ordered]@{
    'avcodec-lav-63.dll' = 'EF802E806876290E22825B09F82C17AA30E0E4AEA97960E196D207C70CEE9648'
    'avfilter-lav-12.dll' = '45C9D402F3F4D3CE7A5C0AA342BFDF9A2018D61CBA3913ACF31A7BD74B7ECF65'
    'avformat-lav-63.dll' = 'E53DBDC11022576A58B2F383CC1AA2C1FEDC1398565A107AD6683F328E446B64'
    'avutil-lav-61.dll' = '8AEA5F91FA28745777F4C1A8F33AF0EBA63486282C403D8386979AEEEA8FB6C7'
    'CHANGELOG.txt' = '32FAFB1FDEE98F412F73FD1DF444B115FD969F8FD8F500DCE3722DD0BF7626C2'
    'COPYING' = '189B1AF95D661151E054CEA10C91B3D754E4DE4D3FECFB074C1FB29476F7167B'
    'include/IBitRateInfo.h' = '0CA0876A6FB7B46CB96ABCE55322F7584C9A63AAA0647EC6BD97613E30255083'
    'include/IBufferInfo.h' = 'CAB476E481819A09D66871A41049D64FDA1565B84AF0B551CC342C8FE6DFF9A8'
    'include/ID3DVideoMemoryConfiguration.h' = '5612172BDA8B2A14F54A80354FEE61D91A2607B83E1230405CBB9AC2C87591AF'
    'include/IDSMResourceBag.h' = '4AA40E65561D110F1D3FE25D69EF7634478790A48AA4D22CCBA45B5836BA809F'
    'include/IGraphRebuildDelegate.h' = '32ADCDC851556A62CD3520B32337D21A7706FB3040C77B14755C21378401F000'
    'include/IKeyFrameInfo.h' = '54F1CB340E57B8BBA169D749E7BD94F19F2B540FECC49CB8D8B53DDB7E64549B'
    'include/ILAVDynamicAllocator.h' = 'EC73D41A62DD2164327A60088477B93B2CF11A45409BF1E3CA43DA575A91B4C9'
    'include/IMediaSample3D.h' = '25A5E8160D82BC875FE23C5BC65C89F87231A787CB3A942056EA2E4BCC6B7552'
    'include/IMediaSideData.h' = '8E568D2BE195785FE32216405925217C79BC6C254BDFFA4BA86EBBFD1BBC1674'
    'include/IPinSegmentEx.h' = '7D718F9E4F7EE7B2C39D18E98B512B2C673F64D66E2770507F22FB620ABD521F'
    'include/ISpecifyPropertyPages2.h' = '4392A843BA8FD5683DB1DB9092D3B7672CF3D5FEA2553046EE02FF5744874688'
    'include/IStreamSourceControl.h' = '4FFA5F85B521775CF57BB4EC33B08B38CFA027DC80E04684400CF290FCF4F35C'
    'include/ITrackInfo.h' = 'BCDB7487D2EE127962FDDBAD24AF5188FED7B090EB63221661CDB0BF7B9AFBD8'
    'include/IURLSourceFilterLAV.h' = '66971B171F6B22820363248341A7D41A52100F094894243299EE5E0FE5DC0056'
    'include/LAVAudioSettings.h' = '15D387D1A012B6898DA4DE74AEA819140A697547A920EF187145521A364EBADC'
    'include/LAVSplitterSettings.h' = 'DEA10582271AE3DB5770B4E5BF40AED9ACFBCDDC5B9B4543B322A827BB8D15CB'
    'include/LAVVideoSettings.h' = '4311AF09558DF9B0829E6D079DA6FD2FC58FD23D869344576A717E76C604ED94'
    'include/README.txt' = '6F3B986F76DAF9D0AD8F67A0BCAE662927E1B071A4F18DC631477B7B13238A57'
    'install_audio.bat' = 'A52AA19201F10EC7AEB7C117C37DE73E1C1FFBF8420B0B85E338F3938CCCD1AA'
    'install_splitter.bat' = 'C98E3AD90A75ADD0FC392BA83CAD1A8FD52B0703416C998EA3FE5252FE17568D'
    'install_video.bat' = '735308046F6E4A65E530014CDED95A6BF17E04EE7EDDF1B8D89F18973584D0EF'
    'IntelQuickSyncDecoder.dll' = '4D62C5F2D3FB88136EBDBD60D459E593E395F64A8D18752FC1FDCB60172D78A2'
    'LAVAudio.ax' = '6C1D1F9660841E682029757B1B103B2F48C5A377042DEA8430F08529E1AE75EE'
    'LAVFilters.Dependencies.manifest' = 'DA51121E8F595A63473F3D602568B737CB4DBA3EB9DE036D8C051E8B95A6903B'
    'LAVSplitter.ax' = 'D16A1A53EB886C7EC68B5D1C3B88C9D25DD3B1556A5EE270D8EC3EF9751FB40E'
    'LAVVideo.ax' = '30DC9E00AEBFDE9AD85ADD4866FAAF10A8D5BAD7574D02B9229EF545C7B40D78'
    'libbluray.dll' = '67301D2D3EA7BC6E86CB283F4C807CC6A70F51714D057709F00EEB84866EC6A1'
    'libmfxsw64.dll' = '704384B644EAB441BE4D60D2000FD8CF63B1206093ACCAE753E98F4B36C281DC'
    'README.md' = 'E1AA1E99041F3009200307565085FDD1D77848D615D16C57EEB3C47622F8C664'
    'swresample-lav-7.dll' = 'E111FB790DEDA2F08B3324529B67991870E83E3F6EC9B2FE6AED80B1634CB0B1'
    'swscale-lav-10.dll' = 'C4615D1B7101E2B05FC59BF553905704348E12B89EF9D69EC1967BEC56F46A3C'
    'uninstall_audio.bat' = '6EC4FECD20AD206CAEB83FABB3781C3A91A69ED28D3D6A28EF075387A7FBB599'
    'uninstall_splitter.bat' = '9D4D54E0B5F06E7051463E98CD91F23E506D53A2D35032BDBA611AA22E334317'
    'uninstall_video.bat' = '61EB95B249FF52B24C9E6DA482CDDF19367223E674BF6D0A979F77748E243A5B'
}

$sourceHeaders = [ordered]@{
    'IMediaSample3D.h' = 'C5DBCD821653C7F12BF18C2EB47B08CFB354D22797CE8A7EB88EA5DAA57CA48E'
    'LAVSplitterSettings.h' = 'A404AA89EE28A7A5B0537B232348A32B0D7EFF0753907DA2158FC396C008EB6E'
    'LAVVideoSettings.h' = 'E9BE8D8F4CC41F7708E43F6F351D8C7D21B66E94C076366AB6B2C926479CA084'
    'LAVAudioSettings.h' = '6DD77C38442412073F7B9FAF608FF235CC3A0182506BEDC0FFB61CE33018A157'
}

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Find-Vcpkg7Zip {
    $toolsRoot = Join-Path $repoRoot 'third_party\vcpkg\downloads\tools'
    $candidate = Get-ChildItem -LiteralPath $toolsRoot -Directory -Filter '7zip-*-windows' `
        -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName '7z.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $candidate) {
        throw @"
vcpkg's 7-Zip tool is missing. Prepare the manifest dependencies first:
  .\third_party\vcpkg\bootstrap-vcpkg.bat -disableMetrics
  .\third_party\vcpkg\vcpkg.exe install --triplet x64-windows --x-manifest-root=. --x-install-root=.\vcpkg_installed
Then rerun .\tools\bootstrap-private-deps.ps1.
"@
    }
    return $candidate
}

function Ensure-Archive($Artifact) {
    $path = Join-Path $depsDir $Artifact.Name
    if (Test-Path -LiteralPath $path) {
        $actual = Get-FileSha256 $path
        if ($actual -ne $Artifact.Sha256) {
            throw "Cached archive has the wrong SHA-256 and was preserved: $path`nExpected $($Artifact.Sha256), got $actual.`nMove or delete that file explicitly, then rerun this script."
        }
        return $path
    }

    $temporary = Join-Path $depsDir ('.download-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Write-Host "Downloading $($Artifact.Name)"
        Invoke-WebRequest -Uri $Artifact.Uri -OutFile $temporary -UseBasicParsing
        $actual = Get-FileSha256 $temporary
        if ($actual -ne $Artifact.Sha256) {
            throw "Downloaded $($Artifact.Name) has the wrong SHA-256: expected $($Artifact.Sha256), got $actual"
        }
        Move-Item -LiteralPath $temporary -Destination $path
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    return $path
}

function Get-ManifestText {
    $fileEntries = @(
        foreach ($entry in $approvedFiles.GetEnumerator()) {
            [ordered]@{ path = $entry.Key; sha256 = $entry.Value }
        }
    )
    $artifactEntries = @(
        foreach ($artifact in $artifacts) {
            [ordered]@{ name = $artifact.Name; sha256 = $artifact.Sha256; uri = $artifact.Uri }
        }
    )
    $manifest = [ordered]@{
        schemaVersion = 1
        artifacts = $artifactEntries
        files = $fileEntries
        sourcePublicHeaders = @($sourceHeaders.Keys)
    }
    return ($manifest | ConvertTo-Json -Depth 5) + "`n"
}

function Get-StageIssues([string]$StagePath, [string]$ExpectedManifest) {
    $issues = [Collections.Generic.List[string]]::new()
    foreach ($entry in $approvedFiles.GetEnumerator()) {
        $path = Join-Path $StagePath ($entry.Key.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $issues.Add("missing $($entry.Key)")
            continue
        }
        $actual = Get-FileSha256 $path
        if ($actual -ne $entry.Value) {
            $issues.Add("hash mismatch for $($entry.Key)")
        }
    }

    $manifestName = '.odyssey-private-deps.json'
    $expectedNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $approvedFiles.Keys) { [void]$expectedNames.Add($name) }
    [void]$expectedNames.Add($manifestName)
    if (Test-Path -LiteralPath $StagePath -PathType Container) {
        $prefixLength = ([IO.Path]::GetFullPath($StagePath)).TrimEnd('\').Length + 1
        foreach ($file in Get-ChildItem -LiteralPath $StagePath -Recurse -File) {
            $relative = $file.FullName.Substring($prefixLength).Replace('\', '/')
            if (-not $expectedNames.Contains($relative)) {
                $issues.Add("unexpected file $relative")
            }
        }
    }

    $manifestPath = Join-Path $StagePath $manifestName
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $actualManifest = [IO.File]::ReadAllText($manifestPath)
        if ($actualManifest -ne $ExpectedManifest) {
            $issues.Add("$manifestName does not match the pinned artifact manifest")
        }
    }
    return $issues
}

function Write-Manifest([string]$StagePath, [string]$Text) {
    $manifestPath = Join-Path $StagePath '.odyssey-private-deps.json'
    $temporary = Join-Path $StagePath ('.manifest-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $manifestPath
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Invoke-7ZipExtract([string]$SevenZip, [string]$Archive, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination | Out-Null
    & $SevenZip x $Archive "-o$Destination" -y | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "7-Zip failed to extract $Archive (exit $LASTEXITCODE)"
    }
}

function Remove-OwnedTemporaryDirectory([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return }
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $depsDir.TrimEnd('\') + [IO.Path]::DirectorySeparatorChar + '.stage-'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside this script's staging paths: $full"
    }
    Remove-Item -LiteralPath $full -Recurse -Force
}

Assert-NoReparseInStagingPath
New-Item -ItemType Directory -Path $depsDir -Force | Out-Null
$sevenZip = Find-Vcpkg7Zip
$archivePaths = @{}
foreach ($artifact in $artifacts) {
    $archivePaths[$artifact.Name] = Ensure-Archive $artifact
}

$manifestText = Get-ManifestText
$lavDir = Join-Path $depsDir 'lav-0.83'
if (Test-Path -LiteralPath $lavDir) {
    if (-not (Test-Path -LiteralPath $lavDir -PathType Container)) {
        throw "The approved staging path exists but is not a directory: $lavDir"
    }
    $issues = @(Get-StageIssues $lavDir $manifestText)
    if ($issues.Count -gt 0) {
        throw "Existing LAV staging was preserved because it is not the approved payload: $lavDir`n - $($issues -join "`n - ")`nMove that directory aside explicitly, then rerun this script."
    }
    $manifestPath = Join-Path $lavDir '.odyssey-private-deps.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        Write-Manifest $lavDir $manifestText
    }
    Write-Host "Approved private dependencies are ready: $lavDir"
    return
}

$token = [guid]::NewGuid().ToString('N')
$lavStage = Join-Path $depsDir ".stage-lav-$token"
$mfxStage = Join-Path $depsDir ".stage-mfx-$token"
$sourceStage = Join-Path $depsDir ".stage-source-$token"
try {
    Invoke-7ZipExtract $sevenZip $archivePaths['LAVFilters-0.83-x64.zip'] $lavStage
    Invoke-7ZipExtract $sevenZip $archivePaths['libmfxsw64-v3.7z'] $mfxStage
    Invoke-7ZipExtract $sevenZip $archivePaths['LAVFilters-source-0.83.zip'] $sourceStage

    $mfxSource = Join-Path $mfxStage 'libmfxsw64.dll'
    if (-not (Test-Path -LiteralPath $mfxSource -PathType Leaf)) {
        throw 'The pinned MFX archive did not contain libmfxsw64.dll at its approved path.'
    }
    Copy-Item -LiteralPath $mfxSource -Destination (Join-Path $lavStage 'libmfxsw64.dll')

    $taggedInclude = Join-Path $sourceStage 'LAVFilters-0.83\include'
    foreach ($entry in $sourceHeaders.GetEnumerator()) {
        $sourcePath = Join-Path $taggedInclude $entry.Key
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf) -or
            (Get-FileSha256 $sourcePath) -ne $entry.Value) {
            throw "The pinned source archive has an unexpected public header: $($entry.Key)"
        }
        $releaseText = ([IO.File]::ReadAllText((Join-Path $lavStage "include\$($entry.Key)"))) -replace "`r`n", "`n"
        $sourceText = ([IO.File]::ReadAllText($sourcePath)) -replace "`r`n", "`n"
        if ($releaseText -ne $sourceText) {
            throw "The release and tagged-source public headers differ: $($entry.Key)"
        }
    }

    Write-Manifest $lavStage $manifestText
    $issues = @(Get-StageIssues $lavStage $manifestText)
    if ($issues.Count -gt 0) {
        throw "New LAV staging failed validation:`n - $($issues -join "`n - ")"
    }
    Move-Item -LiteralPath $lavStage -Destination $lavDir
    Write-Host "Approved private dependencies are ready: $lavDir"
} finally {
    Remove-OwnedTemporaryDirectory $lavStage
    Remove-OwnedTemporaryDirectory $mfxStage
    Remove-OwnedTemporaryDirectory $sourceStage
}

[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$InstallDir = $env:PULS_INSTALL_DIR,
    [switch]$NoPathUpdate,
    [switch]$Uninstall,
    [switch]$Help,
    [string]$RepositoryUrl = $env:PULS_INSTALL_REPOSITORY_URL
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor `
    [Net.SecurityProtocolType]::Tls12

$MessageCatalog = @{
    Usage = "0KPRgdGC0LDQvdC+0LLQutCwINC4INGD0LTQsNC70LXQvdC40LUgUHVscyDRh9C10YDQtdC3IEdpdEh1YiBSZWxlYXNlcwoK0JjRgdC/0L7Qu9GM0LfQvtCy0LDQvdC40LU6CiAgLlxpbnN0YWxsLnBzMSBbLVZlcnNpb24gPHZhbHVlPl0gWy1JbnN0YWxsRGlyIDxwYXRoPl0gWy1Ob1BhdGhVcGRhdGVdCiAgLlxpbnN0YWxsLnBzMSAtVW5pbnN0YWxsIFstSW5zdGFsbERpciA8cGF0aD5dIFstTm9QYXRoVXBkYXRlXQoK0J/QsNGA0LDQvNC10YLRgNGLOgogIC1WZXJzaW9uIDx2YWx1ZT4gICAgICDRg9GB0YLQsNC90L7QstC40YLRjCDQutC+0L3QutGA0LXRgtC90YPRjiDQstC10YDRgdC40Y4sINC90LDQv9GA0LjQvNC10YAgMC4xLjAKICAtSW5zdGFsbERpciA8cGF0aD4gICAg0LrQsNGC0LDQu9C+0LMg0YPRgdGC0LDQvdC+0LLQutC4IMK3INC/0L4g0YPQvNC+0LvRh9Cw0L3QuNGOICVMT0NBTEFQUERBVEElXFByb2dyYW1zXFB1bHNcYmluCiAgLU5vUGF0aFVwZGF0ZSAgICAgICAgINC90LUg0LTQvtCx0LDQstC70Y/RgtGMINC60LDRgtCw0LvQvtCzINGD0YHRgtCw0L3QvtCy0LrQuCDQsiDQv9C+0LvRjNC30L7QstCw0YLQtdC70YzRgdC60LjQuSBQQVRICiAgLVVuaW5zdGFsbCAgICAgICAgICAgINGD0LTQsNC70LjRgtGMIFB1bHMg0Lgg0LfQsNC/0LjRgdGMINC40Lcg0L/QvtC70YzQt9C+0LLQsNGC0LXQu9GM0YHQutC+0LPQviBQQVRICiAgLUhlbHAgICAgICAgICAgICAgICAgINC/0L7QutCw0LfQsNGC0Ywg0Y3RgtGDINGB0L/RgNCw0LLQutGD"
    WindowsOnly = "0KPRgdGC0LDQvdC+0LLRidC40LogaW5zdGFsbC5wczEg0L/RgNC10LTQvdCw0LfQvdCw0YfQtdC9INGC0L7Qu9GM0LrQviDQtNC70Y8gV2luZG93cy4="
    LocalAppDataMissing = "0J3QtSDRg9C00LDQu9C+0YHRjCDQvtC/0YDQtdC00LXQu9C40YLRjCBMT0NBTEFQUERBVEE7INGD0LrQsNC20LjRgtC1IC1JbnN0YWxsRGlyLg=="
    RemoveTargetDirectory = "ezB9INGP0LLQu9GP0LXRgtGB0Y8g0LrQsNGC0LDQu9C+0LPQvtC8OyDRg9C00LDQu9C10L3QuNC1INC+0YHRgtCw0L3QvtCy0LvQtdC90L4u"
    Removed = "UHVscyDRg9C00LDQu9GR0L06IHswfQ=="
    AlreadyRemoved = "UHVscyDRg9C20LUg0YPQtNCw0LvRkdC9OiB7MH0="
    PathRemoved = "0JrQsNGC0LDQu9C+0LMg0YPQtNCw0LvRkdC9INC40Lcg0L/QvtC70YzQt9C+0LLQsNGC0LXQu9GM0YHQutC+0LPQviBQQVRILg=="
    RepositoryHTTPS = "0JjRgdGC0L7Rh9C90LjQuiDRg9GB0YLQsNC90L7QstC60Lgg0LTQvtC70LbQtdC9INC40YHQv9C+0LvRjNC30L7QstCw0YLRjCBnaXRodWIuY29tINC/0L4gSFRUUFMu"
    UnsupportedManifest = "UkVMRUFTRV9NQU5JRkVTVC5qc29uINC40LzQtdC10YIg0L3QtdC/0L7QtNC00LXRgNC20LjQstCw0LXQvNGD0Y4g0YHRhdC10LzRgy4="
    InvalidVersion = "0J3QtdC60L7RgNGA0LXQutGC0L3QsNGPINCy0LXRgNGB0LjRjyB7MH0u"
    ArchitectureMissing = "0J3QtSDRg9C00LDQu9C+0YHRjCDQvtC/0YDQtdC00LXQu9C40YLRjCDQsNGA0YXQuNGC0LXQutGC0YPRgNGDIFdpbmRvd3Mu"
    ArchitectureUnsupported = "0J3QtdC/0L7QtNC00LXRgNC20LjQstCw0LXQvNCw0Y8g0LDRgNGF0LjRgtC10LrRgtGD0YDQsCB7MH0u"
    ManifestMissingProperty = "UkVMRUFTRV9NQU5JRkVTVC5qc29uINC90LUg0YHQvtC00LXRgNC20LjRgiDQv9C+0LvQtSB7MH0u"
    ManifestVersionMismatch = "0JLQtdGA0YHQuNGPIFJFTEVBU0VfTUFOSUZFU1QuanNvbiDQvdC1INGB0L7QstC/0LDQtNCw0LXRgiDRgSDQt9Cw0L/RgNC+0YjQtdC90L3QvtC5IHswfS4="
    ManifestTargetCount = "0JIgUkVMRUFTRV9NQU5JRkVTVC5qc29uINC90LXRgiDQtdC00LjQvdGB0YLQstC10L3QvdC+0LPQviDQv9Cw0LrQtdGC0LAg0LTQu9GPIHdpbmRvd3MvezB9Lg=="
    ManifestUnexpectedAsset = "UkVMRUFTRV9NQU5JRkVTVC5qc29uINGD0LrQsNC30YvQstCw0LXRgiDQvdC10L7QttC40LTQsNC90L3Ri9C5INC/0LDQutC10YIgezB9Lg=="
    ManifestInvalidDigest = "UkVMRUFTRV9NQU5JRkVTVC5qc29uINGB0L7QtNC10YDQttC40YIg0L3QtdC60L7RgNGA0LXQutGC0L3Ri9C5IFNIQS0yNTYu"
    Downloading = "0JfQsNCz0YDRg9C30LrQsCBQdWxzIHswfSDQtNC70Y8gd2luZG93cy97MX3igKY="
    ChecksumMissingAsset = "0JIgU0hBMjU2U1VNUy50eHQg0L3QtdGCINC10LTQuNC90YHRgtCy0LXQvdC90L7QuSDQutC+0YDRgNC10LrRgtC90L7QuSDQt9Cw0L/QuNGB0Lgg0LTQu9GPIHswfS4="
    ChecksumMissingManifest = "0JIgU0hBMjU2U1VNUy50eHQg0L3QtdGCINC10LTQuNC90YHRgtCy0LXQvdC90L7QuSDQutC+0YDRgNC10LrRgtC90L7QuSDQt9Cw0L/QuNGB0Lgg0LTQu9GPIFJFTEVBU0VfTUFOSUZFU1QuanNvbi4="
    ManifestChecksumMismatch = "0JrQvtC90YLRgNC+0LvRjNC90LDRjyDRgdGD0LzQvNCwIFJFTEVBU0VfTUFOSUZFU1QuanNvbiDQvdC1INGB0L7QstC/0LDQu9CwLg=="
    DigestSourcesMismatch = "U0hBLTI1NiDQv9Cw0LrQtdGC0LAg0YDQsNC30LvQuNGH0LDQtdGC0YHRjyDQsiBtYW5pZmVzdCDQuCBTSEEyNTZTVU1TLnR4dC4="
    ArchiveChecksumMismatch = "0JrQvtC90YLRgNC+0LvRjNC90LDRjyDRgdGD0LzQvNCwINCw0YDRhdC40LLQsCDQvdC1INGB0L7QstC/0LDQu9CwLg=="
    BinaryMissing = "0JIg0LDRgNGF0LjQstC1INC90LUg0L3QsNC50LTQtdC9IHB1bHMuZXhlLg=="
    InstallTargetDirectory = "ezB9INGP0LLQu9GP0LXRgtGB0Y8g0LrQsNGC0LDQu9C+0LPQvtC8OyDRg9GB0YLQsNC90L7QstC60LAg0L7RgdGC0LDQvdC+0LLQu9C10L3QsC4="
    Updated = "0L7QsdC90L7QstC70ZHQvQ=="
    Installed = "0YPRgdGC0LDQvdC+0LLQu9C10L0="
    PathAdded = "0JrQsNGC0LDQu9C+0LMg0LTQvtCx0LDQstC70LXQvSDQsiDQv9C+0LvRjNC30L7QstCw0YLQtdC70YzRgdC60LjQuSBQQVRILiDQntGC0LrRgNC+0LnRgtC1INC90L7QstGL0Lkg0YLQtdGA0LzQuNC90LDQuy4="
}

function Get-Message {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [object[]]$Arguments = @()
    )

    if (-not $MessageCatalog.ContainsKey($Name)) {
        throw "Unknown localized message: $Name"
    }
    $template = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($MessageCatalog[$Name]))
    if ($Arguments.Count -eq 0) {
        return $template
    }
    return [string]::Format([Globalization.CultureInfo]::CurrentCulture, $template, $Arguments)
}

function Show-Usage {
    Write-Output (Get-Message -Name Usage)
}

function Get-SHA256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return (($algorithm.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) -join "")
        } finally {
            $algorithm.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

if ($Help) {
    Show-Usage
    exit 0
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw (Get-Message -Name WindowsOnly)
}

$usesDefaultInstallDir = [string]::IsNullOrWhiteSpace($InstallDir)
if ($usesDefaultInstallDir) {
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        throw (Get-Message -Name LocalAppDataMissing)
    }
    $InstallDir = Join-Path $localAppData "Programs\Puls\bin"
}
$InstallDir = [IO.Path]::GetFullPath($InstallDir)

if ($Uninstall) {
    $targetBinary = Join-Path $InstallDir "puls.exe"
    if (Test-Path -LiteralPath $targetBinary -PathType Container) {
        throw (Get-Message -Name RemoveTargetDirectory -Arguments $targetBinary)
    }
    if (Test-Path -LiteralPath $targetBinary -PathType Leaf) {
        Remove-Item -Force -LiteralPath $targetBinary
        Write-Host (Get-Message -Name Removed -Arguments $targetBinary)
    } else {
        Write-Host (Get-Message -Name AlreadyRemoved -Arguments $targetBinary)
    }

    if (-not $NoPathUpdate) {
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $normalizedInstallDir = $InstallDir.TrimEnd("\")
        $pathEntries = @($userPath -split ";" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $remainingEntries = @($pathEntries | Where-Object {
            -not $_.Trim().TrimEnd("\").Equals($normalizedInstallDir, [StringComparison]::OrdinalIgnoreCase)
        })
        if ($remainingEntries.Count -ne $pathEntries.Count) {
            [Environment]::SetEnvironmentVariable("Path", ($remainingEntries -join ";"), "User")
            $processEntries = @($env:Path -split ";" | Where-Object {
                -not [string]::IsNullOrWhiteSpace($_) -and
                -not $_.Trim().TrimEnd("\").Equals($normalizedInstallDir, [StringComparison]::OrdinalIgnoreCase)
            })
            $env:Path = $processEntries -join ";"
            Write-Host (Get-Message -Name PathRemoved)
        }
    }

    if ($usesDefaultInstallDir -and (Test-Path -LiteralPath $InstallDir -PathType Container)) {
        if (@(Get-ChildItem -Force -LiteralPath $InstallDir).Count -eq 0) {
            Remove-Item -Force -LiteralPath $InstallDir
            $productDir = Split-Path -Parent $InstallDir
            if ((Test-Path -LiteralPath $productDir -PathType Container) -and
                @(Get-ChildItem -Force -LiteralPath $productDir).Count -eq 0) {
                Remove-Item -Force -LiteralPath $productDir
            }
        }
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($RepositoryUrl)) {
    $RepositoryUrl = "https://github.com/Cheviiot/Puls"
}
$RepositoryUrl = $RepositoryUrl.TrimEnd("/")
if ($RepositoryUrl -notmatch '^https://github\.com/' -and $RepositoryUrl -notmatch '^http://(127\.0\.0\.1|localhost)(:\d+)?(?:/|$)') {
    throw (Get-Message -Name RepositoryHTTPS)
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $manifest = Invoke-RestMethod -UseBasicParsing `
        -Uri "$RepositoryUrl/releases/latest/download/RELEASE_MANIFEST.json"
    if ([int]$manifest.schema_version -ne 1 -or [string]$manifest.product -ne "Puls") {
        throw (Get-Message -Name UnsupportedManifest)
    }
    $Version = [string]$manifest.version
}
$Version = $Version.Trim()
if ($Version.StartsWith("v")) {
    $Version = $Version.Substring(1)
}
if ($Version -notmatch '^[0-9A-Za-z._-]+$' -or $Version -eq "." -or $Version -eq "..") {
    throw (Get-Message -Name InvalidVersion -Arguments $Version)
}

$architecture = $env:PROCESSOR_ARCHITEW6432
if ([string]::IsNullOrWhiteSpace($architecture)) {
    $architecture = $env:PROCESSOR_ARCHITECTURE
}
if ([string]::IsNullOrWhiteSpace($architecture)) {
    throw (Get-Message -Name ArchitectureMissing)
}
switch ($architecture.ToUpperInvariant()) {
    "AMD64" { $targetArch = "amd64" }
    "ARM64" { $targetArch = "arm64" }
    default { throw (Get-Message -Name ArchitectureUnsupported -Arguments $architecture) }
}

$expectedAsset = "Puls_${Version}_windows_${targetArch}.zip"
$releaseUrl = "$RepositoryUrl/releases/download/v$Version"
$temporaryDir = Join-Path ([IO.Path]::GetTempPath()) ("puls-install-" + [Guid]::NewGuid().ToString("N"))
$stagedBinary = $null
$backupBinary = $null

try {
    New-Item -ItemType Directory -Path $temporaryDir | Out-Null
    $manifestPath = Join-Path $temporaryDir "RELEASE_MANIFEST.json"
    $checksumsPath = Join-Path $temporaryDir "SHA256SUMS.txt"
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseUrl/RELEASE_MANIFEST.json" -OutFile $manifestPath
    $releaseManifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $manifestProperties = @($releaseManifest.PSObject.Properties.Name)
    foreach ($requiredProperty in @("schema_version", "product", "version", "assets")) {
        if ($manifestProperties -notcontains $requiredProperty) {
            throw (Get-Message -Name ManifestMissingProperty -Arguments $requiredProperty)
        }
    }
    if ([int]$releaseManifest.schema_version -ne 1 -or [string]$releaseManifest.product -ne "Puls") {
        throw (Get-Message -Name UnsupportedManifest)
    }
    if ([string]$releaseManifest.version -ne $Version) {
        throw (Get-Message -Name ManifestVersionMismatch -Arguments $Version)
    }
    $matchingAssets = @($releaseManifest.assets | Where-Object {
        $assetProperties = @($_.PSObject.Properties.Name)
        $assetProperties -contains "os" -and
            $assetProperties -contains "arch" -and
            $assetProperties -contains "file" -and
            $assetProperties -contains "sha256" -and
            [string]$_.os -eq "windows" -and [string]$_.arch -eq $targetArch
    })
    if ($matchingAssets.Count -ne 1) {
        throw (Get-Message -Name ManifestTargetCount -Arguments $targetArch)
    }
    $asset = [string]$matchingAssets[0].file
    $manifestChecksum = [string]$matchingAssets[0].sha256
    if ($asset -cne $expectedAsset) {
        throw (Get-Message -Name ManifestUnexpectedAsset -Arguments $asset)
    }
    if ($manifestChecksum -notmatch '^[0-9A-Fa-f]{64}$') {
        throw (Get-Message -Name ManifestInvalidDigest)
    }
    $manifestChecksum = $manifestChecksum.ToLowerInvariant()

    $archivePath = Join-Path $temporaryDir $asset

    Write-Host (Get-Message -Name Downloading -Arguments @($Version, $targetArch))
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseUrl/$asset" -OutFile $archivePath
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseUrl/SHA256SUMS.txt" -OutFile $checksumsPath

    $assetPattern = [Regex]::Escape($asset)
    $checksumLines = @(Get-Content -LiteralPath $checksumsPath | Where-Object {
        $_ -match "^[0-9A-Fa-f]{64}  $assetPattern$"
    })
    if ($checksumLines.Count -ne 1) {
        throw (Get-Message -Name ChecksumMissingAsset -Arguments $asset)
    }
    $expectedChecksum = $checksumLines[0].Substring(0, 64).ToLowerInvariant()
    $manifestPattern = [Regex]::Escape("RELEASE_MANIFEST.json")
    $manifestChecksumLines = @(Get-Content -LiteralPath $checksumsPath | Where-Object {
        $_ -match "^[0-9A-Fa-f]{64}  $manifestPattern$"
    })
    if ($manifestChecksumLines.Count -ne 1) {
        throw (Get-Message -Name ChecksumMissingManifest)
    }
    $expectedManifestChecksum = $manifestChecksumLines[0].Substring(0, 64).ToLowerInvariant()
    $actualManifestChecksum = Get-SHA256 -Path $manifestPath
    if ($actualManifestChecksum -ne $expectedManifestChecksum) {
        throw (Get-Message -Name ManifestChecksumMismatch)
    }
    if ($manifestChecksum -ne $expectedChecksum) {
        throw (Get-Message -Name DigestSourcesMismatch)
    }
    $actualChecksum = Get-SHA256 -Path $archivePath
    if ($actualChecksum -ne $expectedChecksum) {
        throw (Get-Message -Name ArchiveChecksumMismatch)
    }

    $extractDir = Join-Path $temporaryDir "extracted"
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir
    $binaryPath = Join-Path $extractDir "Puls_${Version}_windows_${targetArch}\puls.exe"
    if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        throw (Get-Message -Name BinaryMissing)
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $targetBinary = Join-Path $InstallDir "puls.exe"
    if (Test-Path -LiteralPath $targetBinary -PathType Container) {
        throw (Get-Message -Name InstallTargetDirectory -Arguments $targetBinary)
    }
    $installAction = if (Test-Path -LiteralPath $targetBinary -PathType Leaf) {
        Get-Message -Name Updated
    } else {
        Get-Message -Name Installed
    }
    $stagedBinary = Join-Path $InstallDir (".puls." + [Guid]::NewGuid().ToString("N") + ".exe")
    [IO.File]::Copy($binaryPath, $stagedBinary, $true)
    if (Test-Path -LiteralPath $targetBinary -PathType Leaf) {
        $backupBinary = Join-Path $InstallDir (".puls-backup." + [Guid]::NewGuid().ToString("N") + ".exe")
        [IO.File]::Replace($stagedBinary, $targetBinary, $backupBinary)
    } else {
        [IO.File]::Move($stagedBinary, $targetBinary)
    }
    $stagedBinary = $null
    if ($null -ne $backupBinary -and (Test-Path -LiteralPath $backupBinary)) {
        Remove-Item -Force -LiteralPath $backupBinary
    }
    $backupBinary = $null

    if (-not $NoPathUpdate) {
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $normalizedInstallDir = $InstallDir.TrimEnd("\")
        $pathEntries = @($userPath -split ";" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $alreadyPresent = $false
        foreach ($entry in $pathEntries) {
            if ($entry.Trim().TrimEnd("\").Equals($normalizedInstallDir, [StringComparison]::OrdinalIgnoreCase)) {
                $alreadyPresent = $true
                break
            }
        }
        if (-not $alreadyPresent) {
            $newUserPath = if ([string]::IsNullOrWhiteSpace($userPath)) {
                $normalizedInstallDir
            } else {
                $userPath.TrimEnd(";") + ";" + $normalizedInstallDir
            }
            [Environment]::SetEnvironmentVariable("Path", $newUserPath, "User")
            $env:Path = $env:Path.TrimEnd(";") + ";" + $normalizedInstallDir
            Write-Host (Get-Message -Name PathAdded)
        }
    }

    Write-Host "Puls $Version ${installAction}: $targetBinary"
} finally {
    if ($null -ne $stagedBinary -and (Test-Path -LiteralPath $stagedBinary)) {
        Remove-Item -Force -LiteralPath $stagedBinary
    }
    if ($null -ne $backupBinary -and (Test-Path -LiteralPath $backupBinary)) {
        Remove-Item -Force -LiteralPath $backupBinary
    }
    if (Test-Path -LiteralPath $temporaryDir) {
        Remove-Item -Recurse -Force -LiteralPath $temporaryDir
    }
}

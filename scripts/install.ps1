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

function Show-Usage {
    @"
Установка и удаление Puls через GitHub Releases

Использование:
  .\install.ps1 [-Version <value>] [-InstallDir <path>] [-NoPathUpdate]
  .\install.ps1 -Uninstall [-InstallDir <path>] [-NoPathUpdate]

Параметры:
  -Version <value>      установить конкретную версию, например 0.1.0
  -InstallDir <path>    каталог установки · по умолчанию %LOCALAPPDATA%\Programs\Puls\bin
  -NoPathUpdate         не добавлять каталог установки в пользовательский PATH
  -Uninstall            удалить Puls и запись из пользовательского PATH
  -Help                 показать эту справку
"@
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
    throw "Установщик install.ps1 предназначен только для Windows."
}

$usesDefaultInstallDir = [string]::IsNullOrWhiteSpace($InstallDir)
if ($usesDefaultInstallDir) {
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        throw "Не удалось определить LOCALAPPDATA; укажите -InstallDir."
    }
    $InstallDir = Join-Path $localAppData "Programs\Puls\bin"
}
$InstallDir = [IO.Path]::GetFullPath($InstallDir)

if ($Uninstall) {
    $targetBinary = Join-Path $InstallDir "puls.exe"
    if (Test-Path -LiteralPath $targetBinary -PathType Container) {
        throw "$targetBinary является каталогом; удаление остановлено."
    }
    if (Test-Path -LiteralPath $targetBinary -PathType Leaf) {
        Remove-Item -Force -LiteralPath $targetBinary
        Write-Host "Puls удалён: $targetBinary"
    } else {
        Write-Host "Puls уже удалён: $targetBinary"
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
            Write-Host "Каталог удалён из пользовательского PATH."
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
    throw "Источник установки должен использовать github.com по HTTPS."
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $manifest = Invoke-RestMethod -UseBasicParsing `
        -Uri "$RepositoryUrl/releases/latest/download/RELEASE_MANIFEST.json"
    if ([int]$manifest.schema_version -ne 1 -or [string]$manifest.product -ne "Puls") {
        throw "RELEASE_MANIFEST.json имеет неподдерживаемую схему."
    }
    $Version = [string]$manifest.version
}
$Version = $Version.Trim()
if ($Version.StartsWith("v")) {
    $Version = $Version.Substring(1)
}
if ($Version -notmatch '^[0-9A-Za-z._-]+$' -or $Version -eq "." -or $Version -eq "..") {
    throw "Некорректная версия $Version."
}

$architecture = $env:PROCESSOR_ARCHITEW6432
if ([string]::IsNullOrWhiteSpace($architecture)) {
    $architecture = $env:PROCESSOR_ARCHITECTURE
}
if ([string]::IsNullOrWhiteSpace($architecture)) {
    throw "Не удалось определить архитектуру Windows."
}
switch ($architecture.ToUpperInvariant()) {
    "AMD64" { $targetArch = "amd64" }
    "ARM64" { $targetArch = "arm64" }
    default { throw "Неподдерживаемая архитектура $architecture." }
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
            throw "RELEASE_MANIFEST.json не содержит поле $requiredProperty."
        }
    }
    if ([int]$releaseManifest.schema_version -ne 1 -or [string]$releaseManifest.product -ne "Puls") {
        throw "RELEASE_MANIFEST.json имеет неподдерживаемую схему."
    }
    if ([string]$releaseManifest.version -ne $Version) {
        throw "Версия RELEASE_MANIFEST.json не совпадает с запрошенной $Version."
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
        throw "В RELEASE_MANIFEST.json нет единственного пакета для windows/$targetArch."
    }
    $asset = [string]$matchingAssets[0].file
    $manifestChecksum = [string]$matchingAssets[0].sha256
    if ($asset -cne $expectedAsset) {
        throw "RELEASE_MANIFEST.json указывает неожиданный пакет $asset."
    }
    if ($manifestChecksum -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "RELEASE_MANIFEST.json содержит некорректный SHA-256."
    }
    $manifestChecksum = $manifestChecksum.ToLowerInvariant()

    $archivePath = Join-Path $temporaryDir $asset

    Write-Host "Загрузка Puls $Version для windows/$targetArch…"
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseUrl/$asset" -OutFile $archivePath
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseUrl/SHA256SUMS.txt" -OutFile $checksumsPath

    $assetPattern = [Regex]::Escape($asset)
    $checksumLines = @(Get-Content -LiteralPath $checksumsPath | Where-Object {
        $_ -match "^[0-9A-Fa-f]{64}  $assetPattern$"
    })
    if ($checksumLines.Count -ne 1) {
        throw "В SHA256SUMS.txt нет единственной корректной записи для $asset."
    }
    $expectedChecksum = $checksumLines[0].Substring(0, 64).ToLowerInvariant()
    $manifestPattern = [Regex]::Escape("RELEASE_MANIFEST.json")
    $manifestChecksumLines = @(Get-Content -LiteralPath $checksumsPath | Where-Object {
        $_ -match "^[0-9A-Fa-f]{64}  $manifestPattern$"
    })
    if ($manifestChecksumLines.Count -ne 1) {
        throw "В SHA256SUMS.txt нет единственной корректной записи для RELEASE_MANIFEST.json."
    }
    $expectedManifestChecksum = $manifestChecksumLines[0].Substring(0, 64).ToLowerInvariant()
    $actualManifestChecksum = Get-SHA256 -Path $manifestPath
    if ($actualManifestChecksum -ne $expectedManifestChecksum) {
        throw "Контрольная сумма RELEASE_MANIFEST.json не совпала."
    }
    if ($manifestChecksum -ne $expectedChecksum) {
        throw "SHA-256 пакета различается в manifest и SHA256SUMS.txt."
    }
    $actualChecksum = Get-SHA256 -Path $archivePath
    if ($actualChecksum -ne $expectedChecksum) {
        throw "Контрольная сумма архива не совпала."
    }

    $extractDir = Join-Path $temporaryDir "extracted"
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir
    $binaryPath = Join-Path $extractDir "Puls_${Version}_windows_${targetArch}\puls.exe"
    if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        throw "В архиве не найден puls.exe."
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $targetBinary = Join-Path $InstallDir "puls.exe"
    if (Test-Path -LiteralPath $targetBinary -PathType Container) {
        throw "$targetBinary является каталогом; установка остановлена."
    }
    $installAction = if (Test-Path -LiteralPath $targetBinary -PathType Leaf) {
        "обновлён"
    } else {
        "установлен"
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
            Write-Host "Каталог добавлен в пользовательский PATH. Откройте новый терминал."
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

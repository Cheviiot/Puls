[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$InstallDir = $env:PULS_INSTALL_DIR,
    [switch]$NoPathUpdate,
    [switch]$NoShortcut,
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
    Usage = "0KPRgdGC0LDQvdC+0LLQutCwINC4INGD0LTQsNC70LXQvdC40LUgUHVscyDRh9C10YDQtdC3IEdpdEh1YiBSZWxlYXNlcwoK0JjRgdC/0L7Qu9GM0LfQvtCy0LDQvdC40LU6CiAgLlxpbnN0YWxsLnBzMSBbLVZlcnNpb24gPHZhbHVlPl0gWy1JbnN0YWxsRGlyIDxwYXRoPl0gWy1Ob1BhdGhVcGRhdGVdIFstTm9TaG9ydGN1dF0KICAuXGluc3RhbGwucHMxIC1Vbmluc3RhbGwgWy1JbnN0YWxsRGlyIDxwYXRoPl0gWy1Ob1BhdGhVcGRhdGVdIFstTm9TaG9ydGN1dF0KCtCf0LDRgNCw0LzQtdGC0YDRizoKICAtVmVyc2lvbiA8dmFsdWU+ICAgICAg0YPRgdGC0LDQvdC+0LLQuNGC0Ywg0LrQvtC90LrRgNC10YLQvdGD0Y4g0LLQtdGA0YHQuNGOLCDQvdCw0L/RgNC40LzQtdGAIDAuMy4wCiAgLUluc3RhbGxEaXIgPHBhdGg+ICAgINC60LDRgtCw0LvQvtCzINGD0YHRgtCw0L3QvtCy0LrQuCDCtyDQv9C+INGD0LzQvtC70YfQsNC90LjRjiAlTE9DQUxBUFBEQVRBJVxQcm9ncmFtc1xQdWxzXGJpbgogIC1Ob1BhdGhVcGRhdGUgICAgICAgICDQvdC1INC00L7QsdCw0LLQu9GP0YLRjCDQutCw0YLQsNC70L7QsyDRg9GB0YLQsNC90L7QstC60Lgg0LIg0L/QvtC70YzQt9C+0LLQsNGC0LXQu9GM0YHQutC40LkgUEFUSAogIC1Ob1Nob3J0Y3V0ICAgICAgICAgICDQvdC1INGB0L7Qt9C00LDQstCw0YLRjCDRj9GA0LvRi9C6INCz0YDQsNGE0LjRh9C10YHQutC+0LPQviDQv9GA0LjQu9C+0LbQtdC90LjRjwogIC1Vbmluc3RhbGwgICAgICAgICAgICDRg9C00LDQu9C40YLRjCBQdWxzINC4INC30LDQv9C40YHRjCDQuNC3INC/0L7Qu9GM0LfQvtCy0LDRgtC10LvRjNGB0LrQvtCz0L4gUEFUSAogIC1IZWxwICAgICAgICAgICAgICAgICDQv9C+0LrQsNC30LDRgtGMINGN0YLRgyDRgdC/0YDQsNCy0LrRgwo="
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
    IconMissing = "0JIg0LDRgNGF0LjQstC1INC90LUg0L3QsNC50LTQtdC9IEljb24uaWNvLg=="
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

function Get-PulsShortcutPath {
    $programs = $env:PULS_SHORTCUT_DIR
    if ([string]::IsNullOrWhiteSpace($programs)) {
        $programs = [Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)
    }
    if ([string]::IsNullOrWhiteSpace($programs)) {
        return $null
    }
    return (Join-Path $programs "Puls.lnk")
}

function Remove-PulsShortcut {
    if ($NoShortcut) {
        return
    }
    $shortcutPath = Get-PulsShortcutPath
    if ($null -eq $shortcutPath -or -not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
        return
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    if ([string]$shortcut.Description -eq "Puls GUI launcher managed by Puls installer") {
        Remove-Item -Force -LiteralPath $shortcutPath
    }
}

function Test-OwnedRuntime {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$RuntimeVersion)
    $marker = Join-Path $Path ".puls-runtime-owner"
    if (-not (Test-Path -LiteralPath $Path -PathType Container) -or
        (Get-Item -Force -LiteralPath $Path).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
        -not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        return $false
    }
    return (Get-Content -Raw -LiteralPath $marker).Trim() -ceq "puls-installer-v1:$RuntimeVersion"
}

function Get-OwnedCurrentRuntime {
    param([Parameter(Mandatory = $true)][string]$RuntimeRoot)
    $current = Join-Path $RuntimeRoot "current"
    if (-not (Test-Path -LiteralPath $current -PathType Container)) { return $null }
    $item = Get-Item -Force -LiteralPath $current
    if (-not $item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) { return $null }
    $resolved = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $current).Path)
    $versions = [IO.Path]::GetFullPath((Join-Path $RuntimeRoot "versions")).TrimEnd("\") + "\"
    if (-not $resolved.StartsWith($versions, [StringComparison]::OrdinalIgnoreCase)) { return $null }
    $runtimeVersion = Split-Path -Leaf $resolved
    if (-not (Test-OwnedRuntime -Path $resolved -RuntimeVersion $runtimeVersion)) { return $null }
    return $resolved
}

function Install-RuntimeTree {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$RuntimeVersion)
    $runtimeRoot = Join-Path $InstallDir ".puls-runtime"
    $versions = Join-Path $runtimeRoot "versions"
    foreach ($directory in @($runtimeRoot, $versions)) {
        if ((Test-Path -LiteralPath $directory) -and
            ((Get-Item -Force -LiteralPath $directory).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
             -not (Test-Path -LiteralPath $directory -PathType Container))) {
            throw "Unsafe runtime directory: $directory"
        }
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    $stage = Join-Path $versions (".stage." + [Guid]::NewGuid().ToString("N"))
    $versionPath = Join-Path $versions $RuntimeVersion
    $versionBackup = Join-Path $versions (".backup." + [Guid]::NewGuid().ToString("N"))
    $current = Join-Path $runtimeRoot "current"
    $currentBackup = Join-Path $runtimeRoot (".current-backup." + [Guid]::NewGuid().ToString("N"))
    $shim = Join-Path $InstallDir "puls.cmd"
    $shimStage = Join-Path $InstallDir (".puls." + [Guid]::NewGuid().ToString("N") + ".cmd")
    $shimBackup = Join-Path $InstallDir (".puls-backup." + [Guid]::NewGuid().ToString("N") + ".cmd")
    $legacyExe = Join-Path $InstallDir "puls.exe"
    $legacyBackup = Join-Path $InstallDir (".puls-backup." + [Guid]::NewGuid().ToString("N") + ".exe")
    $movedVersion = $false; $movedCurrent = $false; $madeCurrent = $false; $newVersionInstalled = $false
    $movedShim = $false; $madeShim = $false; $movedLegacy = $false; $hasOldShim = $false
    try {
        Copy-Item -Recurse -Force -LiteralPath $Source -Destination $stage
        Set-Content -Encoding ASCII -NoNewline -LiteralPath (Join-Path $stage ".puls-runtime-owner") -Value "puls-installer-v1:$RuntimeVersion"
        $runtimeExe = Join-Path $stage "bin\puls.exe"
        if (-not (Test-Path -LiteralPath $runtimeExe -PathType Leaf)) { throw (Get-Message -Name BinaryMissing) }
        $global:LASTEXITCODE = 0
        try { & $runtimeExe --version *> $null } catch { throw "Runtime executable failed its --version check." }
        if ($LASTEXITCODE -ne 0) { throw "Runtime executable failed its --version check." }
        $hadCurrent = Test-Path -LiteralPath $current
        if ($hadCurrent -and $null -eq (Get-OwnedCurrentRuntime -RuntimeRoot $runtimeRoot)) { throw "Current runtime is not managed by Puls" }
        if ($hadCurrent) {
            $movedCurrent = $true
            Move-Item -LiteralPath $current -Destination $currentBackup | Out-Null
        }
        if (Test-Path -LiteralPath $versionPath) {
            if (-not (Test-OwnedRuntime -Path $versionPath -RuntimeVersion $RuntimeVersion)) { throw "Runtime version already exists and is not managed by Puls" }
            $movedVersion = $true
            Move-Item -Force -LiteralPath $versionPath -Destination $versionBackup | Out-Null
        }
        $newVersionInstalled = $true
        Move-Item -LiteralPath $stage -Destination $versionPath | Out-Null

        $madeCurrent = $true
        New-Item -ItemType Junction -Path $current -Target $versionPath | Out-Null

        if (Test-Path -LiteralPath $shim) {
            if ((Get-Item -Force -LiteralPath $shim).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) { throw "puls.cmd must be a regular file" }
            $oldShim = Get-Content -Raw -LiteralPath $shim
            if ($oldShim -notmatch '(?m)^REM Puls installer managed v1\r?$') { throw "puls.cmd exists and is not managed by Puls" }
            $hasOldShim = $true
        }
        if (Test-Path -LiteralPath $legacyExe) {
            if (-not (Test-Path -LiteralPath $legacyExe -PathType Leaf) -or
                (Get-Item -Force -LiteralPath $legacyExe).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
                throw (Get-Message -Name InstallTargetDirectory -Arguments $legacyExe)
            }
            Move-Item -LiteralPath $legacyExe -Destination $legacyBackup | Out-Null
            $movedLegacy = $true
        }
        $shimText = "@echo off`r`nREM Puls installer managed v1`r`n`"%~dp0.puls-runtime\current\bin\puls.exe`" %*`r`nexit /b %errorlevel%`r`n"
        [IO.File]::WriteAllText($shimStage, $shimText, [Text.Encoding]::ASCII)
        if ($hasOldShim) {
            [IO.File]::Replace($shimStage, $shim, $shimBackup)
            $movedShim = $true
        } else {
            [IO.File]::Move($shimStage, $shim)
        }
        $madeShim = $true
        if ($movedShim) { Remove-Item -Force -LiteralPath $shimBackup -ErrorAction SilentlyContinue }
        if ($movedLegacy) { Remove-Item -Force -LiteralPath $legacyBackup -ErrorAction SilentlyContinue }
        if ($movedCurrent) { Remove-Item -Force -LiteralPath $currentBackup -ErrorAction SilentlyContinue }
        if ($movedVersion) { Remove-Item -Recurse -Force -LiteralPath $versionBackup -ErrorAction SilentlyContinue }
        foreach ($oldRuntime in Get-ChildItem -Force -Directory -LiteralPath $versions) {
            if ($oldRuntime.Name -ne $RuntimeVersion -and (Test-OwnedRuntime -Path $oldRuntime.FullName -RuntimeVersion $oldRuntime.Name)) {
                Remove-Item -Recurse -Force -LiteralPath $oldRuntime.FullName -ErrorAction SilentlyContinue
            }
        }
        return (Join-Path $InstallDir "puls.cmd")
    } catch {
        if ($madeShim -and (Test-Path -LiteralPath $shim)) { Remove-Item -Force -LiteralPath $shim }
        if (Test-Path -LiteralPath $shimBackup) {
            if (Test-Path -LiteralPath $shim) { Remove-Item -Force -LiteralPath $shim }
            Move-Item -LiteralPath $shimBackup -Destination $shim | Out-Null
        }
        if (Test-Path -LiteralPath $legacyBackup) { Move-Item -LiteralPath $legacyBackup -Destination $legacyExe | Out-Null }
        if ($madeCurrent -and (Test-Path -LiteralPath $current) -and
            [string]::Equals((Get-OwnedCurrentRuntime -RuntimeRoot $runtimeRoot), [IO.Path]::GetFullPath($versionPath), [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -Force -LiteralPath $current
        }
        if ($newVersionInstalled -and (Test-OwnedRuntime -Path $versionPath -RuntimeVersion $RuntimeVersion)) {
            Remove-Item -Recurse -Force -LiteralPath $versionPath
        }
        if (Test-Path -LiteralPath $versionBackup) { Move-Item -LiteralPath $versionBackup -Destination $versionPath | Out-Null }
        if (Test-Path -LiteralPath $currentBackup) { Move-Item -LiteralPath $currentBackup -Destination $current | Out-Null }
        throw
    } finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
        if (Test-Path -LiteralPath $shimStage) { Remove-Item -Force -LiteralPath $shimStage }
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
        if ((Get-Item -Force -LiteralPath $targetBinary).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to remove a linked puls.exe."
        }
        Remove-Item -Force -LiteralPath $targetBinary
        Write-Host (Get-Message -Name Removed -Arguments $targetBinary)
    } else {
        Write-Host (Get-Message -Name AlreadyRemoved -Arguments $targetBinary)
    }

    $managedShim = Join-Path $InstallDir "puls.cmd"
    $canRemoveRuntime = $true
    if (Test-Path -LiteralPath $managedShim -PathType Leaf) {
        if (-not (Get-Item -Force -LiteralPath $managedShim).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -and
            (Get-Content -Raw -LiteralPath $managedShim) -match '(?m)^REM Puls installer managed v1\r?$') {
            Remove-Item -Force -LiteralPath $managedShim
            Write-Host (Get-Message -Name Removed -Arguments $managedShim)
        } else {
            $canRemoveRuntime = $false
        }
    } elseif (Test-Path -LiteralPath $managedShim) {
        $canRemoveRuntime = $false
    }
    $runtimeRoot = Join-Path $InstallDir ".puls-runtime"
    $versions = Join-Path $runtimeRoot "versions"
    $ownedCurrent = Get-OwnedCurrentRuntime -RuntimeRoot $runtimeRoot
    $current = Join-Path $runtimeRoot "current"
    if ((Test-Path -LiteralPath $current) -and $null -eq $ownedCurrent) { $canRemoveRuntime = $false }
    if ($canRemoveRuntime -and $null -ne $ownedCurrent -and (Test-Path -LiteralPath $current)) {
        Remove-Item -Force -LiteralPath $current
    }
    if ($canRemoveRuntime -and (Test-Path -LiteralPath $versions -PathType Container) -and
        -not (Get-Item -Force -LiteralPath $versions).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
        foreach ($runtimeDir in Get-ChildItem -Force -Directory -LiteralPath $versions) {
            if (Test-OwnedRuntime -Path $runtimeDir.FullName -RuntimeVersion $runtimeDir.Name) {
                Remove-Item -Recurse -Force -LiteralPath $runtimeDir.FullName
            }
        }
        if (@(Get-ChildItem -Force -LiteralPath $versions).Count -eq 0) { Remove-Item -Force -LiteralPath $versions }
    }
    if ($canRemoveRuntime -and (Test-Path -LiteralPath $runtimeRoot -PathType Container) -and
        -not (Get-Item -Force -LiteralPath $runtimeRoot).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -and
        @(Get-ChildItem -Force -LiteralPath $runtimeRoot).Count -eq 0) {
        Remove-Item -Force -LiteralPath $runtimeRoot
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

    Remove-PulsShortcut
    $targetIcon = Join-Path $InstallDir "Puls.ico"
    if (Test-Path -LiteralPath $targetIcon -PathType Leaf) {
        Remove-Item -Force -LiteralPath $targetIcon
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
    if (([int]$manifest.schema_version -ne 1 -and [int]$manifest.schema_version -ne 2) -or [string]$manifest.product -ne "Puls") {
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
    if (([int]$releaseManifest.schema_version -ne 1 -and [int]$releaseManifest.schema_version -ne 2) -or [string]$releaseManifest.product -ne "Puls") {
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
    $hasGUI = $false
    if ([int]$releaseManifest.schema_version -eq 2) {
        $assetProperties = @($matchingAssets[0].PSObject.Properties.Name)
        foreach ($requiredProperty in @("kind", "capabilities")) {
            if ($assetProperties -notcontains $requiredProperty) {
                throw (Get-Message -Name ManifestMissingProperty -Arguments $requiredProperty)
            }
        }
        if ([string]$matchingAssets[0].kind -ne "archive") {
            throw (Get-Message -Name ManifestUnexpectedAsset -Arguments $asset)
        }
        $capabilities = @($matchingAssets[0].capabilities | ForEach-Object { [string]$_ })
        if ($capabilities -notcontains "cli") {
            throw (Get-Message -Name ManifestUnexpectedAsset -Arguments $asset)
        }
        $hasGUI = $capabilities -contains "gui"
    }
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
    [void][Reflection.Assembly]::LoadWithPartialName("System.IO.Compression.FileSystem")
    $archiveReader = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $expectedRoot = "Puls_${Version}_windows_${targetArch}/"
        $entryNames = @{}
        foreach ($entry in $archiveReader.Entries) {
            $entryName = [string]$entry.FullName
            if ($entryName.Contains("\") -or $entryName.StartsWith("/") -or $entryName -match '(^|/)\.\.(?:/|$)' -or
                $entryName -match '^[A-Za-z]:' -or -not $entryName.StartsWith($expectedRoot, [StringComparison]::Ordinal)) {
                throw "Archive contains an unsafe path"
            }
            if ($entryNames.ContainsKey($entryName)) { throw "Archive contains a duplicate path" }
            $entryNames[$entryName] = $true
            $unixMode = ([int]$entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixMode -eq 0xA000) { throw "Archive contains an unsupported symbolic link" }
        }
    } finally {
        $archiveReader.Dispose()
    }
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDir
    $packageRoot = Join-Path $extractDir "Puls_${Version}_windows_${targetArch}"
    $runtimeSource = Join-Path $packageRoot "bin"
    $isRuntime = Test-Path -LiteralPath (Join-Path $runtimeSource "puls.exe") -PathType Leaf
    $binaryPath = Join-Path $packageRoot "puls.exe"
    if (-not $isRuntime -and -not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        throw (Get-Message -Name BinaryMissing)
    }
    $iconPath = $null
    if ($hasGUI) {
        $iconPath = Join-Path $packageRoot "assets\Icon.ico"
        if (-not (Test-Path -LiteralPath $iconPath -PathType Leaf)) {
            throw (Get-Message -Name IconMissing)
        }
    }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    if ($isRuntime) {
        $previousRuntime = Get-OwnedCurrentRuntime -RuntimeRoot (Join-Path $InstallDir ".puls-runtime")
        $targetBinary = Install-RuntimeTree -Source $packageRoot -RuntimeVersion $Version
        $runtimeCurrentExe = Join-Path $InstallDir ".puls-runtime\current\bin\puls.exe"
        $installAction = if ($null -ne $previousRuntime) { Get-Message -Name Updated } else { Get-Message -Name Installed }
    } else {
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
    }

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

    if ($hasGUI -and -not $NoShortcut) {
        $targetIcon = Join-Path $InstallDir "Puls.ico"
        $shortcutTarget = Join-Path $InstallDir "puls.exe"
        if ($isRuntime) {
            $targetIcon = Join-Path $InstallDir ".puls-runtime\current\assets\Icon.ico"
            $shortcutTarget = Join-Path $InstallDir ".puls-runtime\current\bin\puls.exe"
        } else {
            [IO.File]::Copy($iconPath, $targetIcon, $true)
        }
        $shortcutPath = Get-PulsShortcutPath
        if ($null -ne $shortcutPath) {
            $shortcutDirectory = Split-Path -Parent $shortcutPath
            New-Item -ItemType Directory -Force -Path $shortcutDirectory | Out-Null
            $shell = New-Object -ComObject WScript.Shell
            $shortcut = $shell.CreateShortcut($shortcutPath)
            if ($isRuntime) {
                $shortcut.TargetPath = $shortcutTarget
                $shortcut.Arguments = "gui"
            } else {
                $shortcut.TargetPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
                $quotedBinary = $targetBinary.Replace("'", "''")
                $shortcut.Arguments = "-NoProfile -WindowStyle Hidden -Command `"& '$quotedBinary' gui`""
            }
            $shortcut.WorkingDirectory = $InstallDir
            $shortcut.IconLocation = "$targetIcon,0"
            $shortcut.Description = "Puls GUI launcher managed by Puls installer"
            $shortcut.Save()
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

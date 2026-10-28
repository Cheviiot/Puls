param()

$ErrorActionPreference = "Stop"
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw "This integration test must run on Windows."
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $repoRoot "scripts\install.ps1"
$scratch = Join-Path (Join-Path $repoRoot ".scratch") ("puls-cpp-installer-test-" + [Guid]::NewGuid().ToString("N"))
$releases = Join-Path $scratch "releases"
$testHome = Join-Path $scratch "home"
$installDir = Join-Path $scratch "custom install"
$originalUserPath = [Environment]::GetEnvironmentVariable("Path", "User")
$serverJob = $null
$primaryError = $null
$cleanupErrors = @()

function Get-FileHash256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { return (($algorithm.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) -join "") }
        finally { $algorithm.Dispose() }
    } finally { $stream.Dispose() }
}

function New-TestRelease([string]$Version, [string]$Label, [switch]$IncludeTraversalEntry) {
    $packageName = "Puls_${Version}_windows_amd64"
    $packageRoot = Join-Path $scratch $packageName
    New-Item -ItemType Directory -Force -Path (Join-Path $packageRoot "bin"), (Join-Path $packageRoot "lib"), (Join-Path $packageRoot "plugins"), (Join-Path $packageRoot "assets") | Out-Null
    $className = "PulsSmoke" + [Guid]::NewGuid().ToString("N")
    $smokeSource = 'public static class __PULS_SMOKE_CLASS__ { public static int Main(string[] args) { System.Console.WriteLine("Puls runtime smoke"); return 0; } }'.Replace("__PULS_SMOKE_CLASS__", $className)
    Add-Type -TypeDefinition $smokeSource -Language CSharp -OutputType ConsoleApplication -OutputAssembly (Join-Path $packageRoot "bin\puls.exe")
    [IO.File]::WriteAllText((Join-Path $packageRoot "lib\runtime.data"), "runtime $Label", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $packageRoot "plugins\runtime.plugin"), "plugin $Label", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $packageRoot "assets\Icon.ico"), "icon $Label", [Text.Encoding]::ASCII)
    $archiveName = "$packageName.zip"
    $archivePath = Join-Path $releases "$Version\$archiveName"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $archivePath) | Out-Null
    # Match release_cpp.py's ZIP paths. Compress-Archive on Windows can emit
    # backslash-separated entry names, which the installer correctly rejects.
    [void][Reflection.Assembly]::LoadWithPartialName("System.IO.Compression.FileSystem")
    $archiveStream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $archive = $null
    try {
        $archive = [IO.Compression.ZipArchive]::new($archiveStream, [IO.Compression.ZipArchiveMode]::Create, $true)
        $null = $archive.CreateEntry("$packageName/")
        foreach ($file in Get-ChildItem -LiteralPath $packageRoot -File -Recurse) {
            $relativePath = $file.FullName.Substring($packageRoot.Length).TrimStart([char[]]@([char]'\', [char]'/')).Replace('\', '/')
            $entryName = "$packageName/$relativePath"
            if ($entryName.Contains('\')) { throw "Test ZIP builder emitted a non-canonical path: $entryName" }
            $entry = $archive.CreateEntry($entryName, [IO.Compression.CompressionLevel]::Optimal)
            $sourceStream = [IO.File]::OpenRead($file.FullName)
            try {
                $entryStream = $entry.Open()
                try { $sourceStream.CopyTo($entryStream) }
                finally { $entryStream.Dispose() }
            } finally { $sourceStream.Dispose() }
        }
        if ($IncludeTraversalEntry) {
            $entry = $archive.CreateEntry("$packageName/../../escape-from-archive.txt", [IO.Compression.CompressionLevel]::Optimal)
            $entryStream = $entry.Open()
            try {
                $payload = [Text.Encoding]::ASCII.GetBytes("must not be extracted")
                $entryStream.Write($payload, 0, $payload.Length)
            } finally { $entryStream.Dispose() }
        }
    } finally {
        try { if ($null -ne $archive) { $archive.Dispose() } }
        finally { $archiveStream.Dispose() }
    }
    $assetHash = Get-FileHash256 $archivePath
    $manifest = @{
        schema_version = 2; product = "Puls"; version = $Version
        assets = @(@{ os = "windows"; arch = "amd64"; file = $archiveName; sha256 = $assetHash; kind = "archive"; capabilities = @("cli", "gui") })
    } | ConvertTo-Json -Depth 6
    $manifestPath = Join-Path (Split-Path -Parent $archivePath) "RELEASE_MANIFEST.json"
    [IO.File]::WriteAllText($manifestPath, $manifest + "`n", [Text.Encoding]::UTF8)
    $manifestHash = Get-FileHash256 $manifestPath
    [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $archivePath) "SHA256SUMS.txt"), "$assetHash  $archiveName`n$manifestHash  RELEASE_MANIFEST.json`n", [Text.Encoding]::ASCII)
    Remove-Item -Recurse -Force -LiteralPath $packageRoot
}

function Assert-CurrentRuntimeVersion([string]$ExpectedVersion, [string]$CurrentPath) {
    $item = Get-Item -Force -LiteralPath $CurrentPath
    $targets = @($item.Target)
    if ($targets.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$targets[0])) {
        throw "Current runtime junction does not expose exactly one target."
    }
    $target = [string]$targets[0]
    if (-not [IO.Path]::IsPathRooted($target)) { $target = Join-Path (Split-Path -Parent $CurrentPath) $target }
    $actualTarget = [IO.Path]::GetFullPath($target).TrimEnd("\")
    $expectedTarget = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $CurrentPath) "versions\$ExpectedVersion")).TrimEnd("\")
    if (-not [string]::Equals($actualTarget, $expectedTarget, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Current runtime junction targets '$actualTarget', expected '$expectedTarget'."
    }
}

function Invoke-TestInstaller([string[]]$Arguments) {
    if (Test-Path -LiteralPath (Join-Path $PSHOME "pwsh.exe")) {
        $powerShell = Join-Path $PSHOME "pwsh.exe"
    } else {
        $powerShell = Join-Path $PSHOME "powershell.exe"
    }
    $output = & $powerShell -NoProfile -ExecutionPolicy Bypass -File $installer @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Installer failed: $($output -join [Environment]::NewLine)" }
    return $output
}

try {
    New-Item -ItemType Directory -Force -Path $releases, $testHome, $installDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $installDir "keep.txt"), "user data", [Text.Encoding]::UTF8)
    New-TestRelease "1.0.0" "one"
    New-TestRelease "2.0.0" "two"
    New-TestRelease "3.0.0" "three"
    New-TestRelease "4.0.0" "unsafe" -IncludeTraversalEntry

    $portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $portProbe.Start(); $port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port; $portProbe.Stop()
    $baseUrl = "http://127.0.0.1:$port"
    $stopFile = Join-Path $scratch "server.stop"
    $serverJob = Start-Job -ArgumentList $port, $releases, $stopFile -ScriptBlock {
        param($Port, $Root, $StopFile)
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
        $listener.Start()
        try {
            while (-not (Test-Path -LiteralPath $StopFile)) {
                if (-not $listener.Pending()) { Start-Sleep -Milliseconds 25; continue }
                $client = $listener.AcceptTcpClient()
                $stream = $null
                $reader = $null
                try {
                    $stream = $client.GetStream(); $reader = [IO.StreamReader]::new($stream)
                    $requestLine = $reader.ReadLine(); while ($reader.ReadLine() -ne "") { }
                    $urlPath = ($requestLine -split " ")[1]
                    $parts = $urlPath.Trim("/").Split("/")
                    $version = $parts[$parts.Length - 2].TrimStart("v")
                    $file = [IO.Path]::GetFileName($urlPath)
                    $path = Join-Path (Join-Path $Root $version) $file
                    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                        $body = [Text.Encoding]::ASCII.GetBytes("missing"); $status = "404 Not Found"
                    } else {
                        $body = [IO.File]::ReadAllBytes($path); $status = "200 OK"
                    }
                    $header = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $status`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n")
                    $stream.Write($header, 0, $header.Length); $stream.Write($body, 0, $body.Length); $stream.Flush()
                } finally {
                    try { if ($null -ne $reader) { $reader.Dispose() } }
                    finally {
                        try { if ($null -ne $stream) { $stream.Dispose() } }
                        finally { $client.Dispose() }
                    }
                }
            }
        } finally { $listener.Stop() }
    }
    Start-Sleep -Milliseconds 500
    $env:PULS_INSTALL_REPOSITORY_URL = $baseUrl
    $env:PULS_INSTALL_DIR = $installDir
    $env:LOCALAPPDATA = $testHome

    Invoke-TestInstaller @("-Version", "1.0.0", "-InstallDir", $installDir, "-NoShortcut") | Out-Null
    $current = Join-Path $installDir ".puls-runtime\current"
    if (-not (Test-Path -LiteralPath $current -PathType Container) -or
        -not (Test-Path -LiteralPath (Join-Path $installDir "puls.cmd") -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $installDir ".puls-runtime\current\lib\runtime.data"))) {
        throw "Fresh C++ runtime installation is incomplete."
    }
    if (-not ([Environment]::GetEnvironmentVariable("Path", "User").Contains($installDir))) {
        throw "Installer did not add InstallDir to the user PATH."
    }

    Invoke-TestInstaller @("-Version", "2.0.0", "-InstallDir", $installDir, "-NoPathUpdate", "-NoShortcut") | Out-Null
    Assert-CurrentRuntimeVersion -ExpectedVersion "2.0.0" -CurrentPath $current

    try {
        Invoke-TestInstaller @("-Version", "4.0.0", "-InstallDir", $installDir, "-NoPathUpdate", "-NoShortcut") | Out-Null
        throw "Expected traversal archive rejection."
    } catch {
        if ($_.Exception.Message -notmatch "unsafe path") { throw }
    }
    Assert-CurrentRuntimeVersion -ExpectedVersion "2.0.0" -CurrentPath $current
    if ((Get-Content -Raw -LiteralPath (Join-Path $installDir "keep.txt")) -ne "user data") {
        throw "Traversal archive changed unrelated user data."
    }
    if (Test-Path -LiteralPath (Join-Path $installDir ".puls-runtime\versions\1.0.0")) { throw "Old owned runtime was not cleaned up." }
    Invoke-TestInstaller @("-Version", "2.0.0", "-InstallDir", $installDir, "-NoPathUpdate", "-NoShortcut") | Out-Null
    Assert-CurrentRuntimeVersion -ExpectedVersion "2.0.0" -CurrentPath $current

    [IO.File]::WriteAllText((Join-Path $installDir "puls.cmd"), "@echo off`r`nREM user replacement`r`n", [Text.Encoding]::ASCII)
    try { Invoke-TestInstaller @("-Version", "3.0.0", "-InstallDir", $installDir, "-NoPathUpdate", "-NoShortcut") | Out-Null; throw "Expected unmanaged shim rejection." }
    catch { if ($_.Exception.Message -notmatch "not managed by Puls") { throw } }
    Assert-CurrentRuntimeVersion -ExpectedVersion "2.0.0" -CurrentPath $current
    if (Test-Path -LiteralPath (Join-Path $installDir ".puls-runtime\versions\3.0.0")) { throw "Failed update left an unowned staged version." }
    [IO.File]::WriteAllText((Join-Path $installDir "puls.cmd"), "@echo off`r`nREM Puls installer managed v1`r`n", [Text.Encoding]::ASCII)

    Invoke-TestInstaller @("-Uninstall", "-InstallDir", $installDir, "-NoShortcut") | Out-Null
    if ([Environment]::GetEnvironmentVariable("Path", "User").Contains($installDir)) { throw "Uninstall did not remove InstallDir from the user PATH." }
    if (Test-Path -LiteralPath $current) { throw "Uninstall left the runtime junction." }
    if (Test-Path -LiteralPath (Join-Path $installDir "puls.cmd")) { throw "Uninstall left the managed shim." }
    if ((Get-Content -Raw -LiteralPath (Join-Path $installDir "keep.txt")) -ne "user data") { throw "Uninstall removed unrelated user data." }
    Write-Output "C++ Windows installer integration checks passed."
} catch {
    $primaryError = $_
} finally {
    try { [Environment]::SetEnvironmentVariable("Path", $originalUserPath, "User") }
    catch { $cleanupErrors += "restore user PATH: $($_.Exception.Message)" }
    if ($null -ne $serverJob) {
        try { [IO.File]::WriteAllText($stopFile, "stop", [Text.Encoding]::ASCII) }
        catch { $cleanupErrors += "signal HTTP server: $($_.Exception.Message)" }
        try {
            $null = Wait-Job -Job $serverJob -Timeout 10
            if ($serverJob.State -notin @("Completed", "Failed", "Stopped")) {
                Stop-Job -Job $serverJob -ErrorAction Stop
                $null = Wait-Job -Job $serverJob -Timeout 5
            }
            if ($serverJob.State -notin @("Completed", "Failed", "Stopped")) {
                throw "HTTP fixture server job did not stop."
            }
        } catch { $cleanupErrors += "stop HTTP server: $($_.Exception.Message)" }
        try { Remove-Job -Job $serverJob -Force -ErrorAction Stop }
        catch { $cleanupErrors += "remove HTTP server job: $($_.Exception.Message)" }
    }
    try {
        if (Test-Path -LiteralPath $scratch) {
            $removed = $false
            for ($attempt = 0; $attempt -lt 4 -and -not $removed; $attempt++) {
                try {
                    Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction Stop
                    $removed = $true
                } catch {
                    if ($attempt -eq 3) { $cleanupErrors += "remove test scratch: $($_.Exception.Message)" }
                    else { Start-Sleep -Milliseconds 250 }
                }
            }
        }
    } catch { $cleanupErrors += "remove test scratch: $($_.Exception.Message)" }
    if ($null -ne $primaryError -and $cleanupErrors.Count -gt 0) {
        try { Write-Warning ("Cleanup also reported errors: " + ($cleanupErrors -join "; ")) }
        catch { }
    }
}
if ($null -ne $primaryError) { throw $primaryError }
if ($cleanupErrors.Count -gt 0) { throw ("Windows installer test cleanup failed: " + ($cleanupErrors -join "; ")) }

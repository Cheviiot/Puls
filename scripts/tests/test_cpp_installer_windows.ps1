param()

$ErrorActionPreference = "Stop"
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw "This integration test must run on Windows."
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $repoRoot "scripts\install.ps1"
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("puls-cpp-installer-test-" + [Guid]::NewGuid().ToString("N"))
$releases = Join-Path $scratch "releases"
$home = Join-Path $scratch "home"
$installDir = Join-Path $scratch "custom install"
$originalUserPath = [Environment]::GetEnvironmentVariable("Path", "User")
$serverJob = $null

function Get-FileHash256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { return (($algorithm.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) -join "") }
        finally { $algorithm.Dispose() }
    } finally { $stream.Dispose() }
}

function New-TestRelease([string]$Version, [string]$Label) {
    $packageName = "Puls_${Version}_windows_amd64"
    $packageRoot = Join-Path $scratch $packageName
    New-Item -ItemType Directory -Force -Path (Join-Path $packageRoot "bin"), (Join-Path $packageRoot "lib"), (Join-Path $packageRoot "plugins"), (Join-Path $packageRoot "assets") | Out-Null
    $className = "PulsSmoke" + [Guid]::NewGuid().ToString("N")
    $smokeSource = 'public static class {0} { public static int Main(string[] args) { System.Console.WriteLine("Puls runtime smoke"); return 0; } }' -f $className
    Add-Type -TypeDefinition $smokeSource -Language CSharp -OutputType ConsoleApplication -OutputAssembly (Join-Path $packageRoot "bin\puls.exe")
    [IO.File]::WriteAllText((Join-Path $packageRoot "lib\runtime.data"), "runtime $Label", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $packageRoot "plugins\runtime.plugin"), "plugin $Label", [Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $packageRoot "assets\Icon.ico"), "icon $Label", [Text.Encoding]::ASCII)
    $archiveName = "$packageName.zip"
    $archivePath = Join-Path $releases "$Version\$archiveName"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $archivePath) | Out-Null
    Compress-Archive -Path $packageRoot -DestinationPath $archivePath -Force
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
    New-Item -ItemType Directory -Force -Path $releases, $home, $installDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $installDir "keep.txt"), "user data", [Text.Encoding]::UTF8)
    New-TestRelease "1.0.0" "one"
    New-TestRelease "2.0.0" "two"
    New-TestRelease "3.0.0" "three"

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
                } finally { $client.Dispose() }
            }
        } finally { $listener.Stop() }
    }
    Start-Sleep -Milliseconds 500
    $env:PULS_INSTALL_REPOSITORY_URL = $baseUrl
    $env:PULS_INSTALL_DIR = $installDir
    $env:LOCALAPPDATA = $home

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
    if ((Resolve-Path -LiteralPath $current).Path -notlike "*\versions\2.0.0") { throw "Runtime update did not switch current." }
    if (Test-Path -LiteralPath (Join-Path $installDir ".puls-runtime\versions\1.0.0")) { throw "Old owned runtime was not cleaned up." }
    Invoke-TestInstaller @("-Version", "2.0.0", "-InstallDir", $installDir, "-NoPathUpdate", "-NoShortcut") | Out-Null
    if ((Resolve-Path -LiteralPath $current).Path -notlike "*\versions\2.0.0") { throw "Same-version runtime reinstall failed." }

    [IO.File]::WriteAllText((Join-Path $installDir "puls.cmd"), "@echo off`r`nREM user replacement`r`n", [Text.Encoding]::ASCII)
    try { Invoke-TestInstaller @("-Version", "3.0.0", "-InstallDir", $installDir, "-NoPathUpdate", "-NoShortcut") | Out-Null; throw "Expected unmanaged shim rejection." }
    catch { if ($_.Exception.Message -notmatch "not managed by Puls") { throw } }
    if ((Resolve-Path -LiteralPath $current).Path -notlike "*\versions\2.0.0") { throw "Failed update did not roll back current." }
    if (Test-Path -LiteralPath (Join-Path $installDir ".puls-runtime\versions\3.0.0")) { throw "Failed update left an unowned staged version." }
    [IO.File]::WriteAllText((Join-Path $installDir "puls.cmd"), "@echo off`r`nREM Puls installer managed v1`r`n", [Text.Encoding]::ASCII)

    Invoke-TestInstaller @("-Uninstall", "-InstallDir", $installDir, "-NoShortcut") | Out-Null
    if ([Environment]::GetEnvironmentVariable("Path", "User").Contains($installDir)) { throw "Uninstall did not remove InstallDir from the user PATH." }
    if (Test-Path -LiteralPath $current) { throw "Uninstall left the runtime junction." }
    if (Test-Path -LiteralPath (Join-Path $installDir "puls.cmd")) { throw "Uninstall left the managed shim." }
    if ((Get-Content -Raw -LiteralPath (Join-Path $installDir "keep.txt")) -ne "user data") { throw "Uninstall removed unrelated user data." }
    Write-Output "C++ Windows installer integration checks passed."
} finally {
    [Environment]::SetEnvironmentVariable("Path", $originalUserPath, "User")
    if ($null -ne $serverJob) {
        [IO.File]::WriteAllText($stopFile, "stop", [Text.Encoding]::ASCII)
        Stop-Job -Job $serverJob -ErrorAction SilentlyContinue
        Remove-Job -Job $serverJob -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $scratch) { Remove-Item -Recurse -Force -LiteralPath $scratch }
}

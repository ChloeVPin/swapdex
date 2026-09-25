# Swapdex installer for Windows.
#
# Downloads the build for this machine, installs it, and registers it to start at
# sign in. Swapdex attaches to the Codex app you already have and never installs
# or replaces Codex itself.

$ErrorActionPreference = "Stop"

$Repository = "ChloeVPin/swapdex"

function Write-Step($Message) {
    Write-Host "  $Message"
}

function Get-Architecture {
    if ([System.Environment]::Is64BitOperatingSystem) {
        return "x64"
    }
    return "x86"
}

function Get-ExistingSwapdex {
    $candidates = @(
        (Join-Path $env:APPDATA "swapdex\swapdex.exe"),
        (Join-Path $env:LOCALAPPDATA "swapdex\swapdex.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    return $null
}

Write-Host "Swapdex installer"
Write-Step "platform: windows"
Write-Step "arch:     $(Get-Architecture)"

$codex = Get-Command codex -ErrorAction SilentlyContinue
$codexApp = @(
    (Join-Path $env:LOCALAPPDATA "Programs\Codex\Codex.exe"),
    (Join-Path $env:LOCALAPPDATA "Programs\ChatGPT\ChatGPT.exe")
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $codex -and -not $codexApp) {
    Write-Step "note:     Codex was not found. Install Codex first, then run swapdex install."
}

$version = if ($env:SWAPDEX_VERSION) { $env:SWAPDEX_VERSION } else { "latest" }
$arch = Get-Architecture
$asset = "swapdex-windows-$arch.zip"

if ($version -eq "latest") {
    $url = "https://github.com/$Repository/releases/latest/download/$asset"
} else {
    $url = "https://github.com/$Repository/releases/download/v$version/$asset"
}

$workdir = Join-Path ([System.IO.Path]::GetTempPath()) ("swapdex-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $workdir -Force | Out-Null

try {
    Write-Step "downloading: $asset"
    $archive = Join-Path $workdir $asset
    try {
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
    } catch {
        # A missing asset and a broken network look the same to the client, so name
        # the build that is missing rather than leaving the reader to guess.
        $status = $null
        try { $status = (Invoke-WebRequest -Uri $url -Method Head -UseBasicParsing).StatusCode } catch { $status = $_.Exception.Response.StatusCode.value__ }
        if ($status -eq 404) {
            Write-Host "swapdex: there is no published build for windows-$arch yet. Releases are at $Repository/releases" -ForegroundColor Red
            exit 1
        }
        Write-Host "swapdex: the download failed. Check your network and try again: $url" -ForegroundColor Red
        exit 1
    }

    Expand-Archive -Path $archive -DestinationPath $workdir -Force
    $binary = Get-ChildItem -Path $workdir -Filter "swapdex.exe" -Recurse | Select-Object -First 1
    if (-not $binary) {
        Write-Host "swapdex: the downloaded archive did not contain the Swapdex binary" -ForegroundColor Red
        exit 1
    }

    $target = Join-Path $env:APPDATA "swapdex"
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -Path $binary.FullName -Destination (Join-Path $target "swapdex.exe") -Force
    $assetSource = Join-Path (Split-Path $binary.FullName) "inject.js"
    if (Test-Path $assetSource) {
        Copy-Item -Path $assetSource -Destination (Join-Path $target "inject.js") -Force
    }

    & (Join-Path $target "swapdex.exe") install
    if ($LASTEXITCODE -ne 0) {
        Write-Host "swapdex: installation did not finish" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    Write-Host ""
    Write-Host "Swapdex is installed and will start with Codex."
    Write-Host "Commands: swapdex status, swapdex list, swapdex add, swapdex uninstall"
    Write-Host "To reinstall a newer version, run this command again."
} finally {
    Remove-Item -Path $workdir -Recurse -Force -ErrorAction SilentlyContinue
}

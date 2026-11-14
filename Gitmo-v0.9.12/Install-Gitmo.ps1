# Installs (or updates) Gitmo into SnowRunner's Bin folder.
# Run it with "Install Gitmo.bat" (double-click), or:
#   powershell -ExecutionPolicy Bypass -File Install-Gitmo.ps1 [-GameDir "<...\Sources\Bin>"] [-Uninstall] [-DetectOnly]
param(
    [string]$GameDir = "",
    [switch]$Uninstall,
    [switch]$DetectOnly,
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Done([int]$code) {
    if (-not $NoPause) { Write-Host ""; Read-Host "Press Enter to close" | Out-Null }
    exit $code
}

function Find-SnowRunnerBin {
    $found = @()
    $steam = $null
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty $k -ErrorAction Stop
            if ($p.SteamPath) { $steam = $p.SteamPath; break }
            if ($p.InstallPath) { $steam = $p.InstallPath; break }
        } catch {}
    }
    if ($steam) {
        $steam = $steam -replace '/', '\'
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
        foreach ($l in ($libs | Select-Object -Unique)) {
            $bin = Join-Path $l 'steamapps\common\SnowRunner\Sources\Bin'
            if (Test-Path (Join-Path $bin 'SnowRunner.exe')) { $found += $bin }
        }
    }
    return $found
}

function Test-IsGitmo([string]$dll) {
    # Gitmo's version.dll carries its name inside; another mod's version.dll doesn't.
    $bytes = [IO.File]::ReadAllBytes($dll)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains('Gitmo %s loaded')
}

Write-Host ""
Write-Host "  Gitmo for SnowRunner - $(if ($Uninstall) { 'uninstall' } else { 'install' })" -ForegroundColor Green
Write-Host "  ------------------------------------------"
Write-Host ""

# 1. where is the game?
if (-not $GameDir) {
    $bins = @(Find-SnowRunnerBin)
    if ($bins.Count -ge 1) {
        $GameDir = $bins[0]
        Write-Host "Found SnowRunner (Steam): $GameDir"
    } else {
        Write-Host "Couldn't find SnowRunner in your Steam libraries." -ForegroundColor Yellow
        Write-Host "In Steam: right-click SnowRunner > Manage > Browse local files, open Sources\Bin,"
        Write-Host "then copy the address from the address bar."
        $GameDir = (Read-Host "Paste the Bin folder here").Trim('" ')
    }
}
if (-not (Test-Path (Join-Path $GameDir 'SnowRunner.exe'))) {
    Write-Host "That folder doesn't contain SnowRunner.exe: $GameDir" -ForegroundColor Red
    Done 1
}
if ($DetectOnly) { Write-Host "(detect only, nothing changed)"; Done 0 }

# 2. the game must be closed (Windows won't let a running game's files be replaced)
if (Get-Process -Name SnowRunner -ErrorAction SilentlyContinue) {
    Write-Host "SnowRunner is running. Close the game, then run this again." -ForegroundColor Red
    Done 1
}

$dst = Join-Path $GameDir 'version.dll'

# ---- uninstall ----
if ($Uninstall) {
    if ((Test-Path $dst) -and -not (Test-IsGitmo $dst)) {
        Write-Host "The version.dll in that folder belongs to another mod, not Gitmo. Leaving it alone." -ForegroundColor Yellow
        Done 1
    }
    $state = Join-Path $GameDir 'Gitmo-transfers.txt'
    if ((Test-Path $state) -and (Select-String -Path $state -Pattern '^OUT ' -Quiet)) {
        Write-Host "A transfer you sent hasn't finished settling yet (Gitmo-transfers.txt)." -ForegroundColor Yellow
        Write-Host "If you uninstall now, Gitmo can't refund it if it fails. Play once more with the"
        Write-Host "other player online first, or type YES to uninstall anyway."
        if ((Read-Host "Uninstall anyway?") -ne 'YES') { Write-Host "Nothing changed."; Done 1 }
    }
    $removed = 0
    foreach ($f in 'version.dll', 'Gitmo.ini', 'Gitmo.log', 'Gitmo.previous.log', 'Gitmo-transfers.txt', 'Gitmo-transfers.txt.tmp', 'Gitmo-activity.txt', 'Gitmo-activity.txt.tmp', 'Gitmo-sms.txt', 'Gitmo-sms.txt.tmp', 'Gitmo-radio.ini', 'Gitmo-radio.ini.tmp') {
        $p = Join-Path $GameDir $f
        if (Test-Path $p) { Remove-Item $p -Force; Write-Host "  removed $f"; $removed++ }
    }
    $photos = Join-Path $GameDir 'Gitmo-photos'
    if (Test-Path $photos) { Remove-Item $photos -Recurse -Force; Write-Host '  removed Gitmo-photos (SMS photos)'; $removed++ }
    Get-ChildItem $GameDir -Filter 'version.dll.gitmo-*' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force; Write-Host "  removed $($_.Name)" }
    Write-Host ""
    Write-Host $(if ($removed) { "Gitmo is uninstalled." } else { "Gitmo wasn't installed there." }) -ForegroundColor Green
    Done 0
}

# ---- install / update ----
$src = Join-Path $here 'version.dll'
if (-not (Test-Path $src)) { Write-Host "version.dll is missing next to this script. Unzip the whole download first." -ForegroundColor Red; Done 1 }

if (Test-Path $dst) {
    if (-not (Test-IsGitmo $dst)) {
        Write-Host "That folder already has a version.dll from another mod." -ForegroundColor Red
        Write-Host "Only one mod can use that file name, so Gitmo can't be installed next to it."
        Write-Host "Nothing changed."
        Done 1
    }
    $backup = Join-Path $GameDir 'version.dll.gitmo-previous'
    Copy-Item $dst $backup -Force
    Write-Host "Updating Gitmo (the old version is kept as version.dll.gitmo-previous)."
} else {
    Write-Host "Installing Gitmo."
}
Copy-Item $src $dst -Force
if ((Get-FileHash $src).Hash -ne (Get-FileHash $dst).Hash) { Write-Host "Copy check failed. Try again with the game closed." -ForegroundColor Red; Done 1 }

$ini = Join-Path $GameDir 'Gitmo.ini'
if (Test-Path $ini) {
    Write-Host "Kept your settings (Gitmo.ini)."
} else {
    Copy-Item (Join-Path $here 'Gitmo.ini') $ini
    Write-Host "Added Gitmo.ini (settings)."
}

if (Test-Path (Join-Path $GameDir 'dxgi.dll')) { Write-Host "VR mod found: the phone can also show in your headset (phone > Settings > VR)." }

Write-Host ""
Write-Host "Done. Start SnowRunner and press Delete to open the phone." -ForegroundColor Green
Write-Host "To send money, both players need Gitmo, Steam, and to be in a map in the same game."
Done 0

param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$Env = "esp32-s3-devkitc-1-release"
)

$ErrorActionPreference = "Stop"

function Require-CleanGit {
    $status = git status --porcelain
    if ($status) {
        Write-Error "Git working tree is not clean. Commit or stash changes first."
    }
}

function Require-MainBranch {
    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne "main") {
        Write-Error "Release must be created from main. Current branch: $branch"
    }
}

function Get-PlatformIOExe {
    $candidate = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
    if (Test-Path $candidate) {
        return $candidate
    }

    $exe = Get-Command platformio.exe -ErrorAction SilentlyContinue
    if ($exe) {
        return $exe.Source
    }

    Write-Error "PlatformIO executable not found. Install PlatformIO or add it to PATH."
}

Require-CleanGit
Require-MainBranch

$pio = Get-PlatformIOExe
& $pio run --environment $Env

$fwPath = Join-Path ".pio/build/$Env" "firmware.bin"
if (-not (Test-Path $fwPath)) {
    Write-Error "Firmware not found at $fwPath"
}

$tagExists = git tag -l $Version
if ($tagExists) {
    Write-Error "Tag $Version already exists. Choose a new version."
}

$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
    Write-Error "GitHub CLI not found. Install gh and run 'gh auth login'."
}

& gh release create $Version --target main --generate-notes
& gh release upload $Version $fwPath --clobber

Write-Host "Release $Version created with firmware.bin uploaded." -ForegroundColor Green

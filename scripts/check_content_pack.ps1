# Checks the content enhancement pack assets in -PackDir against their
# content-pack-assets.json (written with the catalog by pulse_pack_catalog):
# this version, exactly the pack executables, every LZMS asset with the pinned
# size and SHA-256. With -BuildDir, the executables there must be the very
# files the catalog pins - signing after the catalog was generated fails here.
param(
    [Parameter(Mandatory = $true)]
    [string]$PackDir,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/release_payload.ps1"
$version = (Get-Content -LiteralPath (Join-Path $repo 'version.txt') -TotalCount 1).Trim()
$manifestPath = Join-Path $PackDir 'content-pack-assets.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing content pack manifest: $manifestPath" }
$pack = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($pack.key -ne 'content') { throw "Unexpected content pack key: $($pack.key)" }
if ($pack.version -ne $version) { throw "Content pack is version $($pack.version), expected $version" }
if ($pack.base_url -ne "https://github.com/jimmgreen/pulse/releases/download/v$version/") {
    throw "Content pack points at another release: $($pack.base_url)"
}
$names = @($pack.files | ForEach-Object { $_.name } | Sort-Object)
$expected = @($PulseContentPackExecutables | Sort-Object)
if (($names -join '|') -ne ($expected -join '|')) { throw "Content pack files differ: $($names -join ', ')" }

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

foreach ($file in $pack.files) {
    if ($file.asset -ne "content-$($pack.channel)-$($file.name).lzms") { throw "Unexpected asset name: $($file.asset)" }
    $asset = Join-Path $PackDir $file.asset
    if (-not (Test-Path -LiteralPath $asset)) { throw "Missing content pack asset: $asset" }
    $item = Get-Item -LiteralPath $asset
    if ($item.Length -ne [int64]$file.packed_size -or (Get-Sha256 $asset) -ne $file.packed_sha256) {
        throw "Content pack asset does not match its catalog: $($file.asset)"
    }
    if ($BuildDir) {
        $binary = Join-Path $BuildDir $file.name
        if (-not (Test-Path -LiteralPath $binary)) { throw "Missing content pack binary: $binary" }
        if ((Get-Item -LiteralPath $binary).Length -ne [int64]$file.size -or (Get-Sha256 $binary) -ne $file.sha256) {
            throw "$($file.name) changed after its catalog was generated; rebuild the pulse target"
        }
    }
}
$bytes = ($pack.files | Measure-Object -Property packed_size -Sum).Sum
Write-Output "Content pack check passed: $($pack.channel) $version, $($pack.files.Count) files, $bytes download bytes."

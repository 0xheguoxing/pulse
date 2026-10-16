#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $repo ('bench_data/release-review-' + [guid]::NewGuid().ToString('N'))
$fixtureScripts = Join-Path $fixture 'scripts'
$build = Join-Path $fixture 'build'
New-Item -ItemType Directory -Path $fixtureScripts,$build -Force | Out-Null
$failures = 0
function Check([bool]$ok, [string]$label) {
    if ($ok) { Write-Output "[PASS] $label" } else { Write-Output "[FAIL] $label"; $script:failures++ }
}
foreach ($name in @('package_portable.ps1','check_release_payload.ps1','archive_symbols.ps1','release_payload.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $fixtureScripts
}
[IO.File]::WriteAllText((Join-Path $fixture 'version.txt'), '0.0.0')
$binaryNames = @('pulse.exe','Pulse.Index.exe','Pulse.Preview.exe',
    'pulse_shell.exe','pulse_elevated.exe','pulse_integration.exe','lumatext.dll','pdfium.dll')
# Content enhancement pack: signed and symbol-archived, never in the ZIP.
$packNames = @('Pulse.Content.exe','Pulse.Document.exe')
foreach ($name in $binaryNames + $packNames) { [IO.File]::WriteAllText((Join-Path $build $name), 'private non-executable fixture') }
foreach ($folder in @('licenses/PDFium','licenses/LumaText')) {
    $path = Join-Path $build $folder
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $path 'LICENSE.txt'), 'private license fixture')
}
foreach ($name in @('third_party/ib-pinyin-cpp/LICENSE.txt','third_party/md4c/LICENSE.md')) {
    $path = Join-Path $fixture $name
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, 'private license fixture')
}
$portableScript = Join-Path $fixtureScripts 'package_portable.ps1'
& pwsh -NoProfile -File $portableScript -BuildDir $build *> (Join-Path $fixture 'portable.log')
Check ($LASTEXITCODE -eq 0) 'production portable script packages private fixture'
$archivePath = Join-Path $fixture 'dist/Pulse-0.0.0-portable-win-x64.zip'
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($name in $binaryNames) {
        Check (@($archive.Entries | Where-Object { $_.Name -eq $name }).Count -eq 1) "portable contains $name exactly once"
    }
    foreach ($name in $packNames) {
        Check (@($archive.Entries | Where-Object { $_.Name -eq $name }).Count -eq 0) "portable leaves $name to the content pack"
    }
} finally { $archive.Dispose() }
$archiveHash = (Get-FileHash -LiteralPath $archivePath).Hash
Move-Item -LiteralPath (Join-Path $build 'pulse_elevated.exe') -Destination (Join-Path $build 'pulse_elevated.fixture')
& pwsh -NoProfile -File $portableScript -BuildDir $build *> (Join-Path $fixture 'missing-helper.log')
Check ($LASTEXITCODE -ne 0 -and (Get-Content (Join-Path $fixture 'missing-helper.log') -Raw).Contains('pulse_elevated.exe')) 'missing helper fails packaging'
Check ((Get-FileHash -LiteralPath $archivePath).Hash -eq $archiveHash) 'rejected package leaves previous private archive unchanged'
Move-Item -LiteralPath (Join-Path $build 'pulse_elevated.fixture') -Destination (Join-Path $build 'pulse_elevated.exe')

$winBuild = Join-Path $fixture 'build-win81'
New-Item -ItemType Directory -Path $winBuild | Out-Null
[IO.File]::WriteAllBytes((Join-Path $winBuild 'pulse.exe'), [Text.Encoding]::Unicode.GetBytes('PULSE_SELFTEST_CASE'))
Copy-Item -LiteralPath (Join-Path $repo 'build_win81_installer.bat') -Destination $fixture
& (Join-Path $fixture 'build_win81_installer.bat') /skipbuild *> (Join-Path $fixture 'win81-guard.log')
Check ($LASTEXITCODE -ne 0 -and (Get-Content (Join-Path $fixture 'win81-guard.log') -Raw).Contains('embedded selftest')) 'Win81 skipbuild rejects actual selftest marker before packaging'
$installerSource = Get-Content (Join-Path $repo 'build_win81_installer.bat') -Raw
$winSource = Get-Content (Join-Path $repo 'build_win81.bat') -Raw
Check ($installerSource.Contains('call build_win81.bat /release') -and $winSource.Contains('if /i "%~1"=="/release" set "PULSE_WIN81_SELFTEST=OFF"')) 'Win81 installer explicitly configures production mode'
Check ($winSource.Contains('-DPULSE_WITH_SELFTEST=%PULSE_WIN81_SELFTEST%')) 'Win81 configuration uses requested mode'

$releaseSource = Get-Content (Join-Path $repo 'build_release.bat') -Raw
$signedSource = Get-Content (Join-Path $PSScriptRoot 'package_release.ps1') -Raw
Check ($signedSource.Contains('"build_release.bat") /symbols') -and $releaseSource.Contains('if /i "%~1"=="/symbols" set "PULSE_BUILD_TYPE=RelWithDebInfo"')) 'signed package explicitly requests optimized debug symbols'
Check ([regex]::Matches($releaseSource, '-DCMAKE_BUILD_TYPE=%PULSE_BUILD_TYPE%').Count -eq 3) 'all release dependency branches carry symbol configuration'
Check ($signedSource.Contains('"pulse_elevated.exe"')) 'signed package includes elevated helper'
[IO.File]::WriteAllText((Join-Path $build 'pulse_build_id.txt'), 'fixture-build')
foreach ($name in ($binaryNames + $packNames) | Where-Object { $_.EndsWith('.exe') }) {
    [IO.File]::WriteAllText((Join-Path $build ([IO.Path]::ChangeExtension($name,'pdb'))), 'private symbol fixture')
}
& pwsh -NoProfile -File (Join-Path $fixtureScripts 'archive_symbols.ps1') -BuildDir 'build' *> (Join-Path $fixture 'symbols.log')
Check ($LASTEXITCODE -eq 0) 'production symbol archiver accepts complete private set'
$manifest = Get-Content (Join-Path $fixture 'dist/symbols/0.0.0/fixture-build/manifest.json') -Raw | ConvertFrom-Json
Check (@($manifest.artifacts | Where-Object { $_.file -eq 'pulse_elevated.pdb' }).Count -eq 1) 'symbol archive records elevated helper PDB'
Check (@($manifest.artifacts | Where-Object { $_.file -eq 'Pulse.Content.pdb' }).Count -eq 1) 'symbol archive records content pack PDB'
Move-Item -LiteralPath (Join-Path $build 'pulse_elevated.pdb') -Destination (Join-Path $build 'pulse_elevated.symbol-fixture')
& pwsh -NoProfile -File (Join-Path $fixtureScripts 'archive_symbols.ps1') -BuildDir 'build' *> (Join-Path $fixture 'missing-symbol.log')
Check ($LASTEXITCODE -ne 0) 'missing expected symbol remains a hard failure'
foreach ($name in @('package_portable.ps1','package_release.ps1','archive_symbols.ps1')) {
    $tokens = $null; $errors = $null
    [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$errors) | Out-Null
    Check ($errors.Count -eq 0) "$name syntax"
}
Write-Output "Fixture=$fixture"
exit [int]($failures -ne 0)

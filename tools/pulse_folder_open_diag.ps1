# Read-only diagnostic for Pulse folder-open takeover on Windows 10/11.
# It does not write registry values, launch Explorer/Pulse, or read file contents.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$Path
)

$ErrorActionPreference = 'SilentlyContinue'
$script:Report = New-Object 'System.Collections.Generic.List[string]'

function Add-Line([string]$Text = '') {
    [void]$script:Report.Add($Text)
}

function Redact-Text([string]$Text) {
    if ([string]::IsNullOrEmpty($Text)) { return '(empty)' }
    $home = $env:USERPROFILE
    if (-not [string]::IsNullOrEmpty($home)) {
        $Text = $Text -replace [regex]::Escape($home), '%USERPROFILE%'
    }
    $local = $env:LOCALAPPDATA
    if (-not [string]::IsNullOrEmpty($local)) {
        $Text = $Text -replace [regex]::Escape($local), '%LOCALAPPDATA%'
    }
    return $Text
}

function Format-RegistryValue($Value) {
    if ($null -eq $Value) { return '(null)' }
    if ($Value -is [byte[]]) { return "<binary $($Value.Length) bytes>" }
    if ($Value -is [string[]]) { return (Redact-Text ($Value -join '; ')) }
    if ($Value -is [string]) { return (Redact-Text $Value) }
    if ($Value -is [int] -or $Value -is [long]) {
        return ('0x{0:X} ({0})' -f $Value)
    }
    return (Redact-Text ([string]$Value))
}

function Read-KeyValues(
    [string]$Label,
    [Microsoft.Win32.RegistryKey]$Root,
    [string]$RelativeKey,
    [string[]]$Names
) {
    Add-Line("[$Label] $RelativeKey")
    $key = $null
    try { $key = $Root.OpenSubKey($RelativeKey, $false) } catch { }
    if ($null -eq $key) {
        Add-Line('  <key missing>')
        return
    }
    try {
        foreach ($name in $Names) {
            $display = if ([string]::IsNullOrEmpty($name)) { '(Default)' } else { $name }
            if ($key.GetValueNames() -contains $name) {
                $value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                $kind = $key.GetValueKind($name).ToString()
                Add-Line("  $display [$kind] = $(Format-RegistryValue $value)")
            } else {
                Add-Line("  $display = <value missing>")
            }
        }
    } finally {
        $key.Dispose()
    }
}

function Read-IntegrationSnapshot([string]$Group) {
    $key = $null
    $path = "Software\Pulse\ShellIntegration\Backups\v1\$Group"
    try { $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path, $false) } catch { }
    if ($null -eq $key) {
        Add-Line("  $Group snapshot: <missing>")
        return
    }
    try {
        $hasSnapshot = $key.GetValueNames() -contains 'Snapshot'
        $hasUpgrade = $key.GetValueNames() -contains 'UpgradeJournal'
        Add-Line("  $Group snapshot present=$hasSnapshot pending-upgrade=$hasUpgrade")
    } finally {
        $key.Dispose()
    }
}

function Describe-Target([string]$TargetPath) {
    if ([string]::IsNullOrWhiteSpace($TargetPath)) { return 'not supplied' }
    try {
        $item = Get-Item -LiteralPath $TargetPath -Force -ErrorAction Stop
    } catch {
        return 'not found / not a filesystem path'
    }
    if ($item.Extension -ieq '.lnk') {
        try {
            $shell = New-Object -ComObject WScript.Shell
            $shortcut = $shell.CreateShortcut($item.FullName)
            $target = $shortcut.TargetPath
            if ([string]::IsNullOrWhiteSpace($target)) { return 'shortcut (.lnk), target unavailable' }
            if (Test-Path -LiteralPath $target -PathType Container) { return 'shortcut (.lnk) pointing to a folder' }
            if (Test-Path -LiteralPath $target) { return 'shortcut (.lnk) pointing to a non-folder' }
            return 'shortcut (.lnk), target missing or non-filesystem'
        } catch {
            return 'shortcut (.lnk), target could not be inspected'
        }
    }
    if ($item.PSIsContainer) { return 'filesystem directory' }
    return 'not a directory'
}

function Get-DesktopSummary {
    try {
        $desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::Desktop)
        $items = @(Get-ChildItem -LiteralPath $desktop -Force -ErrorAction Stop)
        $folders = @($items | Where-Object { $_.PSIsContainer }).Count
        $shortcuts = @($items | Where-Object { -not $_.PSIsContainer -and $_.Extension -ieq '.lnk' }).Count
        return [pscustomobject]@{ Available = $true; Path = $desktop; Folders = $folders; Shortcuts = $shortcuts; Total = $items.Count; Error = '' }
    } catch {
        return [pscustomobject]@{ Available = $false; Path = ''; Folders = 0; Shortcuts = 0; Total = 0; Error = $_.Exception.Message }
    }
}

Add-Line 'Pulse folder-open diagnostic (read-only)'
Add-Line ('Collected: ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))
Add-Line ('OS version: ' + [Environment]::OSVersion.Version.ToString())
Add-Line ('64-bit PowerShell: ' + [Environment]::Is64BitProcess)
try {
    $osKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows NT\CurrentVersion', $false)
    if ($osKey) {
        Add-Line ('Windows display version: ' + (Redact-Text ([string]$osKey.GetValue('DisplayVersion', 'unknown'))))
        Add-Line ('Windows build: ' + (Redact-Text ([string]$osKey.GetValue('CurrentBuild', 'unknown'))))
        $osKey.Dispose()
    }
} catch { }

Add-Line ''
Add-Line 'Pulse process executable paths (command lines are deliberately not collected):'
$processes = @(Get-Process -Name 'pulse' -ErrorAction SilentlyContinue)
if ($processes.Count -eq 0) {
    Add-Line '  no pulse.exe process currently running'
} else {
    foreach ($process in $processes) {
        $processPath = '(path unavailable)'
        try { if ($process.Path) { $processPath = Redact-Text $process.Path } } catch { }
        Add-Line("  PID=$($process.Id) path=$processPath")
    }
}

Add-Line ''
Add-Line 'Per-user overrides (HKCU\Software\Classes):'
$classes = @('Directory', 'Drive', 'Folder')
$markers = @('', 'LegacyDisable', 'ProgrammaticAccessOnly', 'HideBasedOnVelocityId')
foreach ($class in $classes) {
    Read-KeyValues 'HKCU' ([Microsoft.Win32.Registry]::CurrentUser) "Software\Classes\$class\shell" @('')
    Read-KeyValues 'HKCU' ([Microsoft.Win32.Registry]::CurrentUser) "Software\Classes\$class\shell\open" $markers
    Read-KeyValues 'HKCU' ([Microsoft.Win32.Registry]::CurrentUser) "Software\Classes\$class\shell\open\command" @('', 'DelegateExecute')
}

Add-Line ''
Add-Line 'Effective merged associations (HKCR):'
foreach ($class in $classes) {
    Read-KeyValues 'HKCR' ([Microsoft.Win32.Registry]::ClassesRoot) "$class\shell" @('')
    Read-KeyValues 'HKCR' ([Microsoft.Win32.Registry]::ClassesRoot) "$class\shell\open" $markers
    Read-KeyValues 'HKCR' ([Microsoft.Win32.Registry]::ClassesRoot) "$class\shell\open\command" @('', 'DelegateExecute')
}

Add-Line ''
Add-Line 'Pulse integration backup presence (snapshot contents are not collected):'
foreach ($group in @('Directory', 'Drive', 'Folder')) { Read-IntegrationSnapshot $group }

Add-Line ''
Add-Line 'Desktop item counts (names are not collected):'
$desktopSummary = Get-DesktopSummary
if ($desktopSummary.Available) {
    Add-Line ('  Desktop path: ' + (Redact-Text $desktopSummary.Path))
    Add-Line ("  filesystem folders=$($desktopSummary.Folders) shortcuts (.lnk)=$($desktopSummary.Shortcuts) total items=$($desktopSummary.Total)")
} else {
    Add-Line ('  desktop directory could not be enumerated: ' + (Redact-Text $desktopSummary.Error))
}
Add-Line ('Selected desktop item kind: ' + (Describe-Target $Path))

Add-Line ''
Add-Line 'Interpretation hints:'
Add-Line '  - The effective Directory\shell\open\command should point to the intended Pulse.exe.'
Add-Line '  - A hide marker under Directory\shell\open can prevent the default open verb from being used.'
Add-Line '  - Folder is a separate, broader Shell class; it also covers virtual folders, so do not change it manually.'
Add-Line '  - If the selected item is a shortcut (.lnk), its launch route may differ from a real filesystem directory.'
Add-Line '  - This report is local only. Review/redact it before sharing; no registry changes or file contents are included.'

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$output = Join-Path $env:TEMP "Pulse-Folder-Open-Diagnostic-$stamp.txt"
$script:Report -join "`r`n" | Set-Content -LiteralPath $output -Encoding UTF8
Write-Output "Diagnostic report saved to: $output"

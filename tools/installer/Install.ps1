<#
 Gleam Loader installer. Everything it does is undone by Uninstall.bat.

   1. Copies version.dll (the loader) next to Absolver-Win64-Shipping.exe. It refuses to overwrite a version.dll that is not ours.
   2. Copies AbsolverModLauncher.exe into the game folder.
   3. Keeps the game's original Absolver.exe as Absolver.exe.eac and puts the launcher in its place, so Steam's Play button starts the game
      without EasyAntiCheat. Offline and private LAN play only.
   4. Creates the Mods folder.
   5. Asks GitHub once which Gleam Loader release is the newest and says so if this package is older. It never downloads anything.
      -NoUpdateCheck skips it (the loader itself never goes online).

 It never edits or removes EasyAntiCheat files and never touches saves. It does not move your paks, but the launcher, each time it starts,
 moves every extra .pak (and its .sig) directly in Content\Paks into its own folder under Mods. If a step fails, the steps already done are undone.
 Steam's "Verify integrity of game files" restores the original Absolver.exe; run this installer again afterwards.
 A log of every step is written to install.log next to this script.

 Usage:  Install.bat   |   Uninstall.bat   |   Install.ps1 [-GameDir "<game folder>"] [-Uninstall] [-Quiet] [-NoUpdateCheck]
#>
param([string]$GameDir, [switch]$Uninstall, [switch]$Quiet, [switch]$NoUpdateCheck)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$logFile = Join-Path $here 'install.log'
$launcherMarker = 'Absolver Mod Launcher'      # UTF-16 title string inside our launcher exe
$loaderMarkers = 'Gleam Loader loaded in', 'LanNative M'   # ASCII banner texts inside our version.dll (current, then the one of earlier builds)

function Say([string]$m) {
    Write-Host $m
    try { Add-Content -LiteralPath $logFile -Value ('[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m) } catch {}
}

function Find-GameRoot {
    $roots = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try { $p = Get-ItemProperty $k -ErrorAction Stop; foreach ($n in 'SteamPath', 'InstallPath') { if ($p.$n) { $roots += ($p.$n -replace '/', '\') } } } catch {}
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\\\', '\') } }
    }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $c = Join-Path $l 'steamapps\common\Absolver'
        if (Test-Path (Join-Path $c 'Absolver\Binaries\Win64\Absolver-Win64-Shipping.exe')) { return $c }
    }
    return $null
}

# Byte search, so a large file never has to be turned into a string.
function Has-Text([string]$path, [string]$text, [Text.Encoding]$encoding) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $b = [IO.File]::ReadAllBytes($path); $n = $encoding.GetBytes($text)
    for ($i = 0; $i -le $b.Length - $n.Length; $i++) {
        if ($b[$i] -ne $n[0]) { continue }
        $hit = $true; for ($j = 1; $j -lt $n.Length; $j++) { if ($b[$i + $j] -ne $n[$j]) { $hit = $false; break } }
        if ($hit) { return $true }
    }
    return $false
}
function Is-OurLauncher([string]$path) { Has-Text $path $launcherMarker ([Text.Encoding]::Unicode) }
function Is-OurLoader([string]$path)   { foreach ($m in $loaderMarkers) { if (Has-Text $path $m ([Text.Encoding]::ASCII)) { return $true } }; return $false }

# Version of a loader file from its version information; builds before 0.2.2 carried none of their own.
function Loader-Version([string]$path) {
    $vi = (Get-Item -LiteralPath $path).VersionInfo
    if ($vi.ProductName -eq 'Gleam Loader' -and $vi.ProductVersion) { return $vi.ProductVersion }
    return $null
}

# One request to GitHub's public release list: reads the newest release's tag, downloads nothing. Any failure only prints a note.
function Check-Latest([string]$have) {
    if ($NoUpdateCheck) { Say 'Update check skipped (-NoUpdateCheck).'; return }
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $r = Invoke-RestMethod -Uri 'https://api.github.com/repos/Dyonre/GleamLoader/releases/latest' -TimeoutSec 5 -Headers @{ 'User-Agent' = 'GleamLoader-installer' }
        $latest = ([string]$r.tag_name).TrimStart('v')
        if (-not $have) { Say "Newest Gleam Loader release: $latest ($($r.html_url))"; return }
        if ([version]$latest -gt [version]$have) { Say "A newer Gleam Loader is available: $latest (this package is $have). Download it from $($r.html_url)" }
        elseif ([version]$latest -lt [version]$have) { Say "This package ($have) is newer than the latest published release ($latest)." }
        else { Say "This is the newest Gleam Loader release ($have)." }
    } catch { Say "Could not check for a newer release (offline or GitHub unreachable). Installing anyway." }
}

function Run {
    Say ('--- {0} ---' -f $(if ($Uninstall) { 'Uninstall' } else { 'Install' }))
    if (-not $GameDir) { $script:GameDir = Find-GameRoot }
    if (-not $GameDir) {
        if ($Quiet) { throw 'Could not find Absolver through Steam. Run again with -GameDir "<game folder>".' }
        $script:GameDir = Read-Host 'Could not find Absolver automatically. Paste the full path of the main game folder (the one that contains Absolver.exe)'
    }
    $root = $GameDir.Trim('"').TrimEnd('\')
    $win64 = Join-Path $root 'Absolver\Binaries\Win64'
    if (-not (Test-Path -LiteralPath (Join-Path $win64 'Absolver-Win64-Shipping.exe'))) { throw "Absolver-Win64-Shipping.exe not found under $root" }
    Say "Game folder: $root"
    if (Get-Process -Name 'Absolver-Win64-Shipping', 'AbsolverModLauncher' -ErrorAction SilentlyContinue) {
        throw 'Absolver (or the launcher) is running. Close it, then run this again.'
    }

    $dll = Join-Path $win64 'version.dll'
    $launcherCopy = Join-Path $root 'AbsolverModLauncher.exe'
    $exe = Join-Path $root 'Absolver.exe'
    $eacBackup = Join-Path $root 'Absolver.exe.eac'

    if ($Uninstall) {
        if ((Test-Path -LiteralPath $exe) -and (Is-OurLauncher $exe)) {
            if (Test-Path -LiteralPath $eacBackup) {
                $tmp = "$exe.restoring"      # copy beside first and check it, so Absolver.exe is never missing
                Copy-Item -LiteralPath $eacBackup -Destination $tmp -Force
                if ((Get-Item -LiteralPath $tmp).Length -ne (Get-Item -LiteralPath $eacBackup).Length) { Remove-Item -LiteralPath $tmp -Force; throw 'Could not copy the original Absolver.exe back completely. Nothing was changed.' }
                Move-Item -LiteralPath $tmp -Destination $exe -Force
                Remove-Item -LiteralPath $eacBackup -Force
                Say 'Restored the original Absolver.exe.'
            } else {
                Say 'Absolver.exe is still the launcher but Absolver.exe.eac is missing. In Steam use Properties > Installed Files > Verify integrity to restore it.'
            }
        } else { Say 'Absolver.exe is not the launcher: left alone.' }
        if (Test-Path -LiteralPath $dll) {
            if (Is-OurLoader $dll) { Remove-Item -LiteralPath $dll -Force; Say 'Removed version.dll.' } else { Say 'version.dll here is not ours: left alone.' }
        }
        if (Test-Path -LiteralPath $launcherCopy) { Remove-Item -LiteralPath $launcherCopy -Force; Say 'Removed AbsolverModLauncher.exe.' }
        Say 'Your Mods folder was left in place.'
        return
    }

    foreach ($f in 'version.dll', 'AbsolverModLauncher.exe') {
        if (-not (Test-Path -LiteralPath (Join-Path $here $f))) { throw "$f is missing next to the installer." }
    }
    $pkgVer = Loader-Version (Join-Path $here 'version.dll')
    Say ('This package: Gleam Loader {0}' -f $(if ($pkgVer) { $pkgVer } else { '(no version information)' }))
    if ((Test-Path -LiteralPath $dll) -and (Is-OurLoader $dll)) {
        $curVer = Loader-Version $dll
        Say ('Installed now: Gleam Loader {0}' -f $(if ($curVer) { $curVer } else { '0.2.1 or older' }))
    }
    Check-Latest $pkgVer
    if ((Test-Path -LiteralPath $dll) -and -not (Is-OurLoader $dll)) {
        throw "$dll already exists and is not ours (another mod uses that file). Refusing to overwrite it."
    }
    if ((Test-Path -LiteralPath $launcherCopy) -and -not (Is-OurLauncher $launcherCopy)) {
        throw "$launcherCopy already exists and is not ours. Refusing to overwrite it."
    }

    # Each step records how to undo itself. If a step fails, the steps already done are undone in reverse order, so the game folder
    # is left as it was and Absolver.exe is never left missing.
    $srcDll = Join-Path $here 'version.dll'; $srcLauncher = Join-Path $here 'AbsolverModLauncher.exe'
    $undo = New-Object System.Collections.Generic.Stack[scriptblock]
    $newExe = "$exe.new"
    try {
        # version.dll (an older copy of ours is kept as .prev until the install has finished)
        if (Test-Path -LiteralPath $dll) {
            Copy-Item -LiteralPath $dll -Destination "$dll.prev" -Force
            $undo.Push({ Move-Item -LiteralPath "$dll.prev" -Destination $dll -Force }.GetNewClosure())
        } else {
            $undo.Push({ if (Test-Path -LiteralPath $dll) { Remove-Item -LiteralPath $dll -Force } }.GetNewClosure())
        }
        Copy-Item -LiteralPath $srcDll -Destination $dll -Force
        if ((Get-Item -LiteralPath $dll).Length -ne (Get-Item -LiteralPath $srcDll).Length) { throw 'version.dll was not copied completely.' }
        Say 'Installed version.dll.'

        # AbsolverModLauncher.exe
        if (Test-Path -LiteralPath $launcherCopy) {
            Copy-Item -LiteralPath $launcherCopy -Destination "$launcherCopy.prev" -Force
            $undo.Push({ Move-Item -LiteralPath "$launcherCopy.prev" -Destination $launcherCopy -Force }.GetNewClosure())
        } else {
            $undo.Push({ if (Test-Path -LiteralPath $launcherCopy) { Remove-Item -LiteralPath $launcherCopy -Force } }.GetNewClosure())
        }
        Copy-Item -LiteralPath $srcLauncher -Destination $launcherCopy -Force
        if ((Get-Item -LiteralPath $launcherCopy).Length -ne (Get-Item -LiteralPath $srcLauncher).Length) { throw 'AbsolverModLauncher.exe was not copied completely.' }
        Say 'Installed AbsolverModLauncher.exe.'

        # Absolver.exe: copy the launcher beside it first and check it, then swap. A failed copy changes nothing.
        $undo.Push({ if (Test-Path -LiteralPath $newExe) { Remove-Item -LiteralPath $newExe -Force } }.GetNewClosure())
        Copy-Item -LiteralPath $srcLauncher -Destination $newExe -Force
        if ((Get-Item -LiteralPath $newExe).Length -ne (Get-Item -LiteralPath $srcLauncher).Length) { throw 'The launcher was not copied completely.' }
        if ((Test-Path -LiteralPath $exe) -and (Is-OurLauncher $exe)) {
            Move-Item -LiteralPath $newExe -Destination $exe -Force
            Say 'Play button: already set up (launcher updated).'
        } else {
            if (Test-Path -LiteralPath $exe) {
                $dest = $eacBackup
                if (Test-Path -LiteralPath $dest) { $dest = Join-Path $root ('Absolver.exe.{0}.bak' -f (Get-Date -Format 'yyyyMMdd-HHmmss')) }   # never overwrite a backup
                Move-Item -LiteralPath $exe -Destination $dest
                $undo.Push({ if (-not (Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $dest)) { Move-Item -LiteralPath $dest -Destination $exe } }.GetNewClosure())
                Say ('Kept the original Absolver.exe as ' + (Split-Path $dest -Leaf) + '.')
            }
            Move-Item -LiteralPath $newExe -Destination $exe
            $undo.Push({ if ((Test-Path -LiteralPath $exe) -and (Is-OurLauncher $exe)) { Remove-Item -LiteralPath $exe -Force } }.GetNewClosure())
            Say 'Play button: Steam''s Play now starts the modded game. Clear any Absolver launch option you set earlier.'
        }
        if (-not (Is-OurLauncher $exe)) { throw 'Absolver.exe is not the launcher after the swap.' }
    }
    catch {
        $failure = $_.Exception.Message
        Say "ERROR: $failure"
        Say 'Undoing the steps already done ...'
        while ($undo.Count) { try { & ($undo.Pop()) } catch { Say ('  could not undo a step: ' + $_.Exception.Message) } }
        if (-not (Test-Path -LiteralPath $exe)) { Say 'WARNING: Absolver.exe is missing. In Steam use Properties > Installed Files > Verify integrity of game files to restore it.' }
        throw "Install failed and was rolled back: $failure"
    }
    foreach ($p in "$dll.prev", "$launcherCopy.prev") { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }

    $mods = Join-Path $root 'Mods'
    if (-not (Test-Path -LiteralPath $mods)) { New-Item -ItemType Directory -Path $mods | Out-Null; Say "Created $mods" } else { Say "Mods folder: $mods" }

    $paks = Join-Path $root 'Absolver\Content\Paks'
    if (Test-Path -LiteralPath $paks) {
        $extra = @(Get-ChildItem -LiteralPath $paks -Recurse -File -Filter *.pak | Where-Object { $_.Name -notmatch '^pakchunk[0-2]-' })
        if ($extra.Count) {
            Say ('Note: {0} extra .pak file(s) in Content\Paks (not moved). Put each mod in its own folder under Mods instead:' -f $extra.Count)
            foreach ($p in $extra) { Say ('  ' + $p.FullName) }
        }
    }
    Say 'Done. Start the game from Steam as usual. Run Uninstall.bat to go back to the stock game.'
}

try { Run }
catch { Say ('ERROR: ' + $_.Exception.Message); Say "Details are in $logFile"; if (-not $Quiet) { Read-Host 'Press Enter' }; exit 1 }
if (-not $Quiet) { Read-Host 'Press Enter' }

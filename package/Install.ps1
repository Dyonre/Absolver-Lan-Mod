<#
 LanNative installer (milestone 1). Copies version.dll (the LanNative loader) next to Absolver-Win64-Shipping.exe and creates
 <Win64>\LanNative\ip.txt. It does NOT touch UE4SS, mods.txt, the game exe or any game file, and refuses to overwrite a version.dll
 that is not LanNative's.
 Usage:  Install.bat                      (interactive)
         Install.ps1 -GameDir "D:\Games\Absolver\Absolver\Binaries\Win64" -Ip 192.168.1.50
         Install.ps1 -Uninstall
#>
param([string]$GameDir, [string]$Ip, [switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$tail = 'steamapps\common\Absolver\Absolver\Binaries\Win64'

function Find-GameDir {
    $roots = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try { $p = (Get-ItemProperty $k -ErrorAction Stop); foreach ($n in 'SteamPath','InstallPath') { if ($p.$n) { $roots += ($p.$n -replace '/', '\') } } } catch {}
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        # libraryfolders.vdf stores paths with doubled backslashes (D:\SteamLibrary)
        if (Test-Path $vdf) { foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += ($m.Groups[1].Value -replace '\\', '\') } }
    }
    $libs += 'C:\Program Files (x86)\Steam'
    foreach ($l in ($libs | Select-Object -Unique)) { $c = Join-Path $l $tail; if (Test-Path (Join-Path $c 'Absolver-Win64-Shipping.exe')) { return $c } }
    return $null
}
function Is-LanNative($path) {
    if (-not (Test-Path $path)) { return $false }
    $b = [IO.File]::ReadAllBytes($path)
    $needle = [Text.Encoding]::ASCII.GetBytes('LanNative M')
    for ($i = 0; $i -le $b.Length - $needle.Length; $i++) {
        $hit = $true
        for ($j = 0; $j -lt $needle.Length; $j++) { if ($b[$i + $j] -ne $needle[$j]) { $hit = $false; break } }
        if ($hit) { return $true }
    }
    return $false
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir) { $GameDir = Read-Host 'Could not find Absolver automatically. Paste the full path to ...\Absolver\Binaries\Win64' }
$GameDir = $GameDir.Trim('"').TrimEnd('\')
if (-not (Test-Path (Join-Path $GameDir 'Absolver-Win64-Shipping.exe'))) { throw "Absolver-Win64-Shipping.exe not found in $GameDir" }
Write-Host "Game folder: $GameDir"
$target = Join-Path $GameDir 'version.dll'
$data = Join-Path $GameDir 'LanNative'

if ($Uninstall) {
    if (Test-Path $target) {
        if (Is-LanNative $target) { Remove-Item $target -Force; Write-Host 'Removed version.dll (LanNative). The LanNative folder (log, ip.txt) was left in place.' }
        else { Write-Host 'version.dll here is not LanNative - left alone.' }
    } else { Write-Host 'Nothing to uninstall.' }
    return
}

if ((Test-Path $target) -and -not (Is-LanNative $target)) {
    throw "$target already exists and is not LanNative. Refusing to overwrite it (another mod uses that file)."
}
Copy-Item (Join-Path $here 'version.dll') $target -Force
New-Item -ItemType Directory -Force $data | Out-Null
if (-not $Ip) { $Ip = Read-Host 'Host IP to join with F7 (the HOST PC runs ipconfig; leave blank if this PC is only hosting)' }
$ipFile = Join-Path $data 'ip.txt'
if ($Ip) { Set-Content $ipFile $Ip.Trim() -Encoding ASCII } elseif (-not (Test-Path $ipFile)) { Set-Content $ipFile '192.168.1.50' -Encoding ASCII }
# One settings file (config.txt). If it does not exist yet it is created from the old per-setting files when those are present (they are renamed to
# *.old, never deleted), otherwise from the defaults. An existing config.txt is left alone.
$cfgFile = Join-Path $data 'config.txt'
if (-not (Test-Path $cfgFile)) {
    function Old-Setting($name, $default) {
        $f = Join-Path $data $name
        if (Test-Path $f) { $script:migrated += $f; $l = @(Get-Content $f | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }); if ($l.Count) { return $l } }
        return $default
    }
    $script:migrated = @()
    $hostLines = @(Old-Setting 'hostip.txt' 'auto') | ForEach-Object { "hostip = $($_.Trim())" }
    $cfg = @(
        '# LanNative settings, one "key = value" per line. Lines starting with # are comments. Changes apply the next time the game starts.',
        '',
        '# parryfix: on / off. Fixes the Forsaken "cursed parry" bug (patches one byte in memory at load).',
        "parryfix = $(@(Old-Setting 'parryfix.txt' 'on')[0].Trim())",
        '',
        '# pvpfix: on = LAN duels (1v1 / 3v3) work: send-back guard + duel flow. guard = send-back guard only. off = neither.',
        "pvpfix = $(@(Old-Setting 'pvpfix.txt' 'on')[0].Trim())",
        '',
        '# perteam: players per team in 3v3 maps. 1 = a 3v3 map starts with 2 testers, 2 = starts with 4, 3 or off = the game''s own 6.',
        "perteam = $(@(Old-Setting 'perteam.txt' '1')[0].Trim())",
        '',
        '# autoleave: on / off. In a duel map, go back to your own game when the other player''s connection is gone.',
        "autoleave = $(@(Old-Setting 'autoleave.txt' 'on')[0].Trim())",
        '',
        '# telemetry: off / basic / verbose, optionally followed by the snapshot interval in seconds (basic 10).',
        "telemetry = $(@(Old-Setting 'telemetry.txt' 'basic')[0].Trim())",
        '',
        '# menu: on / off. The F1 in-game menu.',
        "menu = $(@(Old-Setting 'menu.txt' 'on')[0].Trim())",
        '',
        '# npc: on / off. F1 menu > Spawn NPC... (host only) spawns computer-controlled fighters next to you; Remove all NPCs takes them away.',
        'npc = on',
        '# npcrepl: on / off. Also replicate the helper spawner actor to the joiner (try on if the joiner sees the NPC but it looks or acts wrong).',
        'npcrepl = off',
        '',
        '# hostip: address a HOST binds (Radmin / VPN). auto = every adapter. Repeat the line for several choices; the first is the default and',
        '# the F1 menu cycles them. A bound address means only that address accepts joiners.'
    ) + $hostLines
    Set-Content $cfgFile $cfg -Encoding ASCII
    foreach ($f in $script:migrated) { Rename-Item $f ((Split-Path $f -Leaf) + '.old') -Force }
    Write-Host "Created $cfgFile"
}
elseif (-not (Select-String -Path $cfgFile -Pattern '^\s*npc\s*=' -Quiet)) {
    Add-Content $cfgFile @('', '# npc: on / off. F1 menu > Spawn NPC... (host only) spawns computer-controlled fighters next to you; Remove all NPCs takes them away.', 'npc = on',
                           '# npcrepl: on / off. Also replicate the helper spawner actor to the joiner (try on if the joiner sees the NPC but it looks or acts wrong).', 'npcrepl = off') -Encoding ASCII
    Write-Host "Added the npc settings to $cfgFile"
}
foreach ($doc in 'commands.txt', 'maps.txt', 'altars.txt', 'altars.tsv') {
    if (Test-Path (Join-Path $here $doc)) { Copy-Item (Join-Path $here $doc) (Join-Path $data $doc) -Force }
}

# npcs.txt (the NPC types in the F1 menu): copied when missing, replaced (old copy kept) when the shipped list has a newer "npcs-list-version" marker
$npcSrc = Join-Path $here 'npcs.txt'; $npcDst = Join-Path $data 'npcs.txt'
function Npc-Version($f) { $m = Select-String -Path $f -Pattern '^#\s*npcs-list-version:\s*(\d+)' | Select-Object -First 1; if ($m) { [int]$m.Matches[0].Groups[1].Value } else { 0 } }
if (Test-Path $npcSrc) {
    if (Test-Path $npcDst) {
        # a list with an older (or no) "npcs-list-version" marker is replaced by the shipped one, the old copy is kept as npcs.txt.old (put your own lines back from there)
        if ((Npc-Version $npcDst) -lt (Npc-Version $npcSrc)) { Move-Item $npcDst ($npcDst + '.old') -Force; Copy-Item $npcSrc $npcDst; Write-Host "Updated npcs.txt to the new NPC list (your old one is kept as npcs.txt.old)" }
    } else { Copy-Item $npcSrc $npcDst }
}

$modsTxt = Join-Path $GameDir 'Mods\mods.txt'
if ((Test-Path $modsTxt) -and ((Get-Content $modsTxt) -match '^\s*LanDirect\s*:\s*1')) {
    Write-Host ''
    Write-Host 'NOTE: the old Lua mod "LanDirect" is still enabled in Mods\mods.txt. Set it to  LanDirect : 0  before testing LanNative,'
    Write-Host '      otherwise both would handle F6/F7.' -ForegroundColor Yellow
}
Write-Host ''
Write-Host 'Installed LanNative.'
Write-Host ''
Write-Host 'IMPORTANT: LanNative only runs when the game is started WITHOUT EasyAntiCheat. In Steam: Absolver > Properties > Launch Options, enter:  -NoEAC'  -ForegroundColor Yellow
Write-Host '           (offline / LAN play only; with EAC active LanNative does nothing and logs why).' -ForegroundColor Yellow
Write-Host '  Host:  load a character into the world, press F6 (allow Absolver through the firewall, UDP 7777)'
Write-Host "  Join:  load a character, press F7 (IP from $ipFile)"
Write-Host '  NPCs:  F1 > Spawn NPC... (host only, any map, no limit) / Remove all NPCs. NPC types: LanNative\npcs.txt'
Write-Host '  Menu:  press F1 in game (Up/Down, Enter, Left = back). Settings (menu, parry fix, PvP, telemetry, host address ...) are all in LanNative\config.txt.'
Write-Host "  Log:   $data\lannative.log"
Write-Host "  Settings: $cfgFile (parryfix, pvpfix, perteam, autoleave, telemetry, menu, hostip - one file, comments inside)"
Write-Host "  Telemetry CSVs and lannative.log land in $data"
Write-Host "  F8 writes a numbered marker line in the log. altars.txt / maps.txt / commands.txt were copied to $data"
Write-Host '  Undo:  Uninstall.bat'

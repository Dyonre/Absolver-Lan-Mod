# Install-Mod.ps1 - install / remove ONE mod package (from Pack-Mod.ps1) into a Mods folder. One folder per mod: <Mods>\<id>\.
#   install: powershell -ExecutionPolicy Bypass -File tools\sdk\Install-Mod.ps1 -Zip <package.zip> -ModsDir <Mods folder> [-AllowPlugin]
#   remove : powershell -ExecutionPolicy Bypass -File tools\sdk\Install-Mod.ps1 -Remove LuaDemo -ModsDir <Mods folder>
# Checks: the zip holds exactly one top folder <id> with a mod.json whose id matches, every file is listed in files.sha256 with the right hash (and nothing extra).
# An existing <id> folder is MOVED to <Mods>\..\ModsReplaced\<id>-<time> (never deleted); -Remove moves it to ModsRemoved\. plugin.dll is copied but only enabled
# (a line in plugins.txt) with -AllowPlugin. Afterwards the whole Mods folder is checked with reccheck.exe (the loader's own rules).
# A Mods folder inside the Steam game install (steamapps\common\Absolver) is refused.
param([string]$Zip = '', [string]$Remove = '', [Parameter(Mandatory)][string]$ModsDir, [switch]$AllowPlugin)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')); $utf8 = New-Object Text.UTF8Encoding($false)   # relative -Zip / -ModsDir paths resolve against the repo root, wherever the script is started from
function ImLog($m) { Write-Host ("{0:HH:mm:ss} [install] {1}" -f (Get-Date), $m) }
$mods = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ModsDir)) { $ModsDir } else { Join-Path $repo $ModsDir }))
if ($mods -match '\\steamapps\\common\\Absolver(\\|$)') { ImLog "REFUSED: $mods is the real game; install there only through the release gate"; exit 1 }
New-Item -ItemType Directory -Force $mods | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
function ImMoveAside($id, $kind) { $from = Join-Path $mods $id; if (-not (Test-Path -LiteralPath $from)) { return $null }; $to = Join-Path (Split-Path $mods -Parent) "$kind\$id-$stamp"; New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null; [IO.Directory]::Move($from, $to); ImLog "moved the old $id to $to"; return $to }
if ($Remove) {
  if ($Remove -notmatch '^[A-Za-z0-9_.-]{1,47}$') { ImLog 'REFUSED: bad id'; exit 1 }
  if (-not (ImMoveAside $Remove 'ModsRemoved')) { ImLog "nothing to remove: $Remove is not installed"; exit 1 }
  exit 0
}
if (-not $Zip) { ImLog 'give -Zip or -Remove'; exit 2 }
$zp = (Resolve-Path -LiteralPath $(if ([IO.Path]::IsPathRooted($Zip)) { $Zip } else { Join-Path $repo $Zip })).Path
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tmp = Join-Path ([IO.Path]::GetTempPath()) "gleam-unpack-$stamp"; New-Item -ItemType Directory -Force $tmp | Out-Null
try {
  [IO.Compression.ZipFile]::ExtractToDirectory($zp, $tmp)
  $tops = @(Get-ChildItem -LiteralPath $tmp -Force); if ($tops.Count -ne 1 -or -not $tops[0].PSIsContainer) { ImLog 'REFUSED: the zip must hold exactly one folder (the mod id)'; exit 1 }
  $id = $tops[0].Name; $dir = $tops[0].FullName
  $mj = Join-Path $dir 'mod.json'; if (-not (Test-Path -LiteralPath $mj)) { ImLog "REFUSED: $id has no mod.json"; exit 1 }
  $m = [IO.File]::ReadAllText($mj) | ConvertFrom-Json; if ($m.id -ne $id) { ImLog "REFUSED: folder '$id' but mod.json id '$($m.id)'"; exit 1 }
  $sf = Join-Path $dir 'files.sha256'; if (-not (Test-Path -LiteralPath $sf)) { ImLog 'REFUSED: no files.sha256 (pack it with tools\sdk\Pack-Mod.ps1)'; exit 1 }
  $want = @{}; foreach ($l in [IO.File]::ReadAllLines($sf)) { if ($l -match '^([0-9a-f]{64})  (.+)$') { $want[$Matches[2]] = $Matches[1] } }
  $have = @(Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $_.Name -ne 'files.sha256' -or $_.DirectoryName -ne $dir })
  foreach ($f in $have) { $rel = $f.FullName.Substring($dir.Length + 1).Replace('\', '/'); $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $want.ContainsKey($rel)) { ImLog "REFUSED: $rel is not listed in files.sha256"; exit 1 }
    if ($want[$rel] -ne $h) { ImLog "REFUSED: $rel has a different hash than files.sha256"; exit 1 }; $want.Remove($rel) }
  if ($want.Count) { ImLog "REFUSED: files.sha256 lists $($want.Count) missing file(s): $(@($want.Keys)[0])"; exit 1 }
  $old = ImMoveAside $id 'ModsReplaced'
  [IO.Directory]::Move($dir, (Join-Path $mods $id)); ImLog "installed $id $($m.version) into $mods\$id ($($have.Count) files)"
  if (Test-Path -LiteralPath (Join-Path $mods "$id\plugin.dll")) {
    $pt = Join-Path $mods 'plugins.txt'; $lines = @(); if (Test-Path -LiteralPath $pt) { $lines = @([IO.File]::ReadAllLines($pt)) }
    $listed = $lines | Where-Object { $_.Trim() -eq $id }
    if ($AllowPlugin -and -not $listed) { [IO.File]::WriteAllLines($pt, [string[]]($lines + $id), $utf8); ImLog "plugin $id ENABLED (plugins.txt)" }
    elseif (-not $listed) { ImLog "plugin.dll is native code: NOT enabled. Add the line '$id' to $pt yourself (or run with -AllowPlugin)" }
  }
  $rc = Join-Path $repo 'tools\lannative\test\reccheck.exe'
  if (Test-Path -LiteralPath $rc) { $o = & $rc $mods; $code = $LASTEXITCODE; $o | Select-Object -Last 3 | ForEach-Object { ImLog "  reccheck: $_" }; if ($code -ne 0) { ImLog "WARN: the Mods folder has problems now (full list: $rc $mods); the old version is in $old" } }
} finally { if (Test-Path -LiteralPath $tmp) { [IO.Directory]::Delete($tmp, $true) } }
exit 0

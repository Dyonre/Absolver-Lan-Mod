# Pack-Mod.ps1 - release package of ONE mod folder: <out>\<id>-<version>.zip with the folder <id>\ inside + files.sha256.
# Refuses (exit 1, no zip written) anything a mod must not ship: game files (loose .uasset/.uexp/.ubulk/.umap, .sig copies of the game's signature), saves (.sav),
# .env / keys (the ABSOLVER_PAK_KEY / ABSOLVER_SAVE_KEY values of the file given with -EnvFile or GLEAM_ENV_FILE, in text or hex, are searched in every file but never printed),
# executables other than plugin.dll, personal data (tools\release\scan_personal.py). Then checks rows.json / records with the loader's own code (reccheck.exe).
#   powershell -ExecutionPolicy Bypass -File tools\sdk\Pack-Mod.ps1 -ModDir examples\ContentMods\LuaDemo [-Out work\packages] [-EnvFile <key file>] [-Secret <extra text to refuse>]
param([Parameter(Mandatory)][string]$ModDir, [string]$Out = '', [string[]]$Secret = @(), [string]$EnvFile = $env:GLEAM_ENV_FILE)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')); $utf8 = New-Object Text.UTF8Encoding($false)   # relative paths resolve against the repo root, wherever the script is started from
if (-not $Out) { $Out = Join-Path $repo 'work\packages' }
function PmLog($m) { Write-Host ("{0:HH:mm:ss} [pack] {1}" -f (Get-Date), $m) }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = [IO.Path]::GetFullPath((Join-Path $repo $Out)) }          # .NET calls below need absolute paths
$src = (Resolve-Path -LiteralPath $(if ([IO.Path]::IsPathRooted($ModDir)) { $ModDir } else { Join-Path $repo $ModDir })).Path
$mj = Join-Path $src 'mod.json'; if (-not (Test-Path -LiteralPath $mj)) { PmLog "REFUSED: $src has no mod.json"; exit 1 }
$m = [IO.File]::ReadAllText($mj) | ConvertFrom-Json
if (-not $m.id -or $m.id -notmatch '^[A-Za-z0-9_.-]{1,47}$') { PmLog 'REFUSED: mod.json needs an "id" (letters, digits, _ . -)'; exit 1 }
$ver = if ($m.version) { [string]$m.version } else { '0' }
$bad = New-Object System.Collections.Generic.List[string]
$files = @(Get-ChildItem -LiteralPath $src -Recurse -File)
foreach ($f in $files) {
  $rel = $f.FullName.Substring($src.Length + 1); $ext = $f.Extension.ToLowerInvariant()
  if ($ext -in '.uasset', '.uexp', '.ubulk', '.umap') { $bad.Add("$rel : loose cooked game file (ship it inside your own .pak)") }
  elseif ($ext -eq '.sig') { $bad.Add("$rel : .sig is a copy of the game's own signature (the loader creates it; never ship it)") }
  elseif ($ext -eq '.sav') { $bad.Add("$rel : save file") }
  elseif ($f.Name -match '^\.env' -or $ext -in '.key', '.pem', '.pfx') { $bad.Add("$rel : secrets file") }
  elseif ($ext -in '.exe', '.bat', '.cmd', '.ps1', '.vbs', '.scr') { $bad.Add("$rel : executable / script file") }
  elseif ($ext -eq '.dll' -and $f.Name -ne 'plugin.dll') { $bad.Add("$rel : only plugin.dll may be native code") }
  elseif ($ext -in '.obj', '.lib', '.exp', '.pdb', '.ilk') { $bad.Add("$rel : build output") }
}
# secrets: values from the .env (never printed) + -Secret
$sec = New-Object System.Collections.Generic.List[string]; foreach ($s in $Secret) { if ($s.Length -ge 8) { $sec.Add($s) } }
$envf = $EnvFile      # -EnvFile or the GLEAM_ENV_FILE variable: a KEY=value file whose key values must not appear in a package; without one only -Secret values are searched
if (-not $envf) { PmLog 'no -EnvFile / GLEAM_ENV_FILE: the key-file check is skipped (only -Secret values are searched)' }
elseif (-not (Test-Path -LiteralPath $envf)) { PmLog "REFUSED: -EnvFile $envf does not exist"; exit 1 }
if ($envf) { foreach ($l in [IO.File]::ReadAllLines($envf)) { if ($l -match '^\s*(ABSOLVER_PAK_KEY|ABSOLVER_SAVE_KEY)\s*=\s*"?([^"]+)"?\s*$') { $v = $Matches[2].Trim(); if ($v.Length -ge 8) { $sec.Add($v) } } } }
if ($sec.Count) {
  foreach ($f in $files) {
    if ($f.Length -gt 512MB) { continue }
    $b = [IO.File]::ReadAllBytes($f.FullName); $t = [Text.Encoding]::GetEncoding(28591).GetString($b)    # bytes as Latin-1: finds ASCII text and ASCII-hex
    foreach ($s in $sec) { $hex = -join ([Text.Encoding]::ASCII.GetBytes($s) | ForEach-Object { $_.ToString('x2') })
      if ($t.Contains($s) -or $t.ToLowerInvariant().Contains($hex) -or $t.Contains([Text.Encoding]::GetEncoding(28591).GetString([Text.Encoding]::Unicode.GetBytes($s)))) { $bad.Add("$($f.FullName.Substring($src.Length + 1)) : contains a secret value (key / -Secret)"); break } }
  }
}
if ($bad.Count) { PmLog "REFUSED: $($bad.Count) problem(s):"; $bad | ForEach-Object { PmLog "  $_" }; exit 1 }
# stage <tmp>\Mods\<id>, check with the loader's code, scan for personal data, zip
$tmp = Join-Path ([IO.Path]::GetTempPath()) "gleam-stage-$($m.id)"; if (Test-Path -LiteralPath $tmp) { [IO.Directory]::Delete($tmp, $true) }
$dst = Join-Path $tmp "Mods\$($m.id)"; New-Item -ItemType Directory -Force $dst | Out-Null
& robocopy $src $dst /E /NFL /NDL /NJH /NJS /NP | Out-Null; if ($LASTEXITCODE -ge 8) { throw 'robocopy failed' }
$rc = Join-Path $repo 'tools\lannative\test\reccheck.exe'
if (Test-Path -LiteralPath $rc) { $o = & $rc (Join-Path $tmp 'Mods') --alone; $code = $LASTEXITCODE; $o | ForEach-Object { PmLog "  reccheck: $_" }; if ($code -ne 0) { PmLog 'REFUSED: reccheck found problems (above)'; exit 1 } }
else { PmLog 'WARN: tools\lannative\test\reccheck.exe not built: rows.json / records not checked' }
& python -I (Join-Path $repo 'tools\release\scan_personal.py') $dst | ForEach-Object { PmLog "  scan: $_" }; if ($LASTEXITCODE -ne 0) { PmLog 'REFUSED: personal data found (above)'; exit 1 }
$sums = @(Get-ChildItem -LiteralPath $dst -Recurse -File | Sort-Object FullName | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($dst.Length + 1).Replace('\', '/') })
[IO.File]::WriteAllLines((Join-Path $dst 'files.sha256'), [string[]]$sums, $utf8)
New-Item -ItemType Directory -Force $Out | Out-Null
$zip = Join-Path $Out "$($m.id)-$ver.zip"; if (Test-Path -LiteralPath $zip) { [IO.File]::Delete($zip) }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$za = [IO.Compression.ZipFile]::Open($zip, 'Create')                                    # entry names with '/' (Windows PowerShell's CreateFromDirectory writes '\')
try { foreach ($f in @(Get-ChildItem -LiteralPath $dst -Recurse -File | Sort-Object FullName)) { $en = "$($m.id)/" + $f.FullName.Substring($dst.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $en, [IO.Compression.CompressionLevel]::Optimal) } } finally { $za.Dispose() }
[IO.Directory]::Delete($tmp, $true)
if (Test-Path -LiteralPath (Join-Path $src 'plugin.dll')) { PmLog 'NOTE: ships plugin.dll (native code): players must list it in Mods\plugins.txt themselves' }
PmLog "OK $zip ($($sums.Count) files + files.sha256)"
exit 0

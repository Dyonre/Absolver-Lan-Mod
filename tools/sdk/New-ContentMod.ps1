<#
New-ContentMod.ps1 - scaffold a data-only mod (no plugin.dll): mod.json + content\<kind>\<id>.json (+ optional rows.json).

  powershell -ExecutionPolicy Bypass -File tools\sdk\New-ContentMod.ps1 -Name MyMask -Kind items [-Dest <local path>] [-RowsJson]

  -Kind  items (default; a clone of a stock mask, granted + equipped) | weapon (items with gear_row, Sword family) | styles (needs your own FightingStyleDB in a pak)
  -Dest  parent folder (default <repo>\work\sdk-out)
  -RowsJson  also write an empty rows.json (table / object edits, docs\MODDING-GUIDE.md sections 5-6)
Never touches the game folder: copy the finished folder into <game>\Mods\ yourself.
#>
param([Parameter(Mandatory)][string]$Name, [ValidateSet('items', 'weapon', 'styles')][string]$Kind = 'items', [string]$Dest, [switch]$RowsJson, [switch]$Force)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($Name -notmatch '^[A-Za-z0-9_.\-]{1,95}$') { throw "bad -Name '$Name': letters, digits, _ - . only, max 95" }
$id = ($Name -replace '[^A-Za-z0-9_]', '_')      # content ids: letters, digits, _ ; <= 63 chars
if ($id.Length -gt 63) { $id = $id.Substring(0, 63) }
if (-not $Dest) { $Dest = Join-Path $repo 'work\sdk-out' }
$dir = Join-Path $Dest $Name
if ((Test-Path -LiteralPath (Join-Path $dir 'mod.json')) -and -not $Force) { throw "$dir\mod.json exists (use -Force)" }
$kindDir = if ($Kind -eq 'styles') { 'styles' } else { 'items' }
New-Item -ItemType Directory -Force (Join-Path $dir "content\$kindDir") | Out-Null

$entry = switch ($Kind) {
    'items' { @'
{
  "items": [
    {
      "id": "@@ID@@",
      "base": "/Game/DB/Equipment/Masks/SillyFace_Mask_DataAsset.SillyFace_Mask_DataAsset",
      "name": "@@NAME@@",
      "description": "Made with New-ContentMod.ps1.",
      "grant": true,
      "equip": true
    }
  ]
}
'@ }
    'weapon' { @'
{
  "items": [
    {
      "id": "@@ID@@",
      "base": "/Game/DB/Weapons/Sword/SwordPike.SwordPike",
      "name": "@@NAME@@",
      "description": "A Sword-family weapon copied from the Moon Pike (own gear row).",
      "gear_row": { "table": "WeaponDataTable", "copy_of": "w006", "set": { "m_eGearRarity": "Rare" } },
      "grant": true,
      "equip": true
    }
  ]
}
'@ }
    'styles' { @'
{
  "styles": [
    {
      "id": "@@ID@@",
      "db": "REPLACE_ME: the path of your own FightingStyleDB asset, shipped in a pak",
      "style_id": 6,
      "index": 5
    }
  ]
}
'@ }
}
$mod = @'
{
  "id": "@@NAME@@",
  "name": "@@NAME@@",
  "version": "0.1.0",
  "api": 2,
  "author": "you",
  "description": "What this mod does.",
  "affects_gameplay": true
}
'@
$rows = @'
{
  "format": 1,
  "edits": [
    // { "table": "attacks", "row": "004", "set": { "m_iFrameBuildUp": 8 } }
  ]
}
'@
function W($rel, $text) { [IO.File]::WriteAllText((Join-Path $dir $rel), (($text -replace '@@NAME@@', $Name -replace '@@ID@@', $id) -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding $false)) }
W "content\$kindDir\$id.json" $entry; W 'mod.json' $mod
if ($RowsJson) { W 'rows.json' $rows }
Write-Host "created $dir (kind $Kind, id $id). Copy it to <game>\Mods\ ; check LanNative\lannative.log for 'content:' lines and F2 'Content'."
if ($Kind -eq 'styles') {
    Write-Host "NOTE: this style entry is a template. The loader skips it as INVALID until you replace 'db' with your own FightingStyleDB asset (in a pak in this mod folder)."
    Write-Host "      A new style also needs LanNative\symbols.bin and the style unlocked in your save; new style ids start at 6. See docs/sdk/README.md section 3."
}

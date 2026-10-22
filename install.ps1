# Installs the built plugin as a Mod Organizer 2 mod: <MO2>\mods\NewVegasHDR\NVSE\Plugins\NewVegasHDR.dll, plus the
# default NewVegasHDR.ini when the mod has none yet (an existing one keeps its settings), and enables the mod in the
# profile. Mod Organizer and the game have to be closed: MO2 rewrites the profile's mod list when it exits.
# To undo: untick NewVegasHDR in Mod Organizer, or remove its line from the profile's modlist.txt and its mods folder.
param(
  [Parameter(Mandatory)] [string]$Mo2,  # the MO2 instance folder (ModOrganizer.ini, mods\, profiles\)
  [string]$Profile,                     # MO2's selected profile by default
  [switch]$ResetIni)                    # replace the mod's ini with the default one
$ErrorActionPreference = 'Stop'

if (Get-Process ModOrganizer, FalloutNV, FalloutNVLauncher -ErrorAction SilentlyContinue) {
  throw 'Close Mod Organizer and the game first.'
}
if (-not $Profile) {
  $line = (Select-String -Path "$Mo2\ModOrganizer.ini" -Pattern '^selected_profile=' | Select-Object -First 1).Line
  $Profile = ($line -replace '^selected_profile=', '') -replace '^@ByteArray\((.*)\)$', '$1'
}
$modList = "$Mo2\profiles\$Profile\modlist.txt"
if (-not (Test-Path $modList)) { throw "No mod list at $modList." }

$plugins = "$Mo2\mods\NewVegasHDR\NVSE\Plugins"
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item "$PSScriptRoot\build\NewVegasHDR.dll" $plugins
if ($ResetIni -or -not (Test-Path "$plugins\NewVegasHDR.ini")) {
  Copy-Item "$PSScriptRoot\package\NewVegasHDR.ini" $plugins
}

# The mod list runs from the highest priority down and a new mod goes on top (no other mod has these files).
$lines = @(Get-Content $modList)
if ($lines -notcontains '+NewVegasHDR') {
  $header = @($lines | Where-Object { $_ -like '#*' })
  $mods = @($lines | Where-Object { $_ -notlike '#*' -and $_ -ne '-NewVegasHDR' })
  Set-Content -Path $modList -Value ($header + '+NewVegasHDR' + $mods) -Encoding utf8NoBOM
  Write-Host "Enabled NewVegasHDR in the profile '$Profile'."
}
Get-ChildItem $plugins | Select-Object Name, Length, LastWriteTime

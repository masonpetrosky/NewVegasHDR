# Builds NewVegasHDR and runs tests\harness.cpp: the game's HDR blend shaders against ours on DXVK, and the plugin's
# decode, without starting the game. Takes DXVK's d3d9.dll and the vanilla shaders from the game folder (only reading
# it), and Vanilla Plus Tonemapper's shaders from extracted\vpt (its Shaders\Loose\ISHDRBLENDINSHADER*.pso and .vso).
param(
  [string]$Dxvk,     # the d3d9.dll to test on; a copy of the game's by default
  [string]$GameDir,  # the game folder; the registry has it once the game's launcher has run
  [switch]$NoBuild)
$ErrorActionPreference = 'Stop'

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" }
if (-not $GameDir) {
  $key = 'HKLM:\SOFTWARE\WOW6432Node\Bethesda Softworks\FalloutNV'
  $GameDir = (Get-ItemProperty $key -ErrorAction SilentlyContinue).'Installed Path'
}
$work = Join-Path $PSScriptRoot 'build\harness-work'
$shaders = Join-Path $PSScriptRoot 'extracted'

if (-not $Dxvk) {
  if (-not $GameDir) { throw 'The game folder is not in the registry: pass -GameDir or -Dxvk.' }
  New-Item -ItemType Directory -Force "$work\dxvk" | Out-Null
  $Dxvk = "$work\dxvk\d3d9.dll"
  Copy-Item (Join-Path $GameDir 'd3d9.dll') $Dxvk
}
if (-not (Test-Path "$shaders\sdp017\ISHDRBLENDINSHADER.pso")) {
  if (-not $GameDir) { throw 'The game folder is not in the registry: pass -GameDir.' }
  # Shader package 017 holds the shader model 2 shaders the game picks on current GPUs.
  python -I "$PSScriptRoot\tools\sdp.py" extract (Join-Path $GameDir 'Data\Shaders\shaderpackage017.sdp') `
    "$shaders\sdp017" '^IS'
  if ($LASTEXITCODE) { throw 'Extracting the vanilla shaders failed.' }
}
if (-not (Test-Path "$shaders\vpt\ISHDRBLENDINSHADER.pso")) {
  throw "Copy Vanilla Plus Tonemapper's Shaders\Loose\ISHDRBLENDINSHADER*.pso and .vso into $shaders\vpt first."
}

New-Item -ItemType Directory -Force $work | Out-Null
$env:DXVK_LOG_PATH = $work
Push-Location $work
try {
  & "$PSScriptRoot\build\harness.exe" $Dxvk "$PSScriptRoot\build\NewVegasHDR.dll" $shaders $work
  $result = $LASTEXITCODE
} finally {
  Pop-Location
  Remove-Item Env:DXVK_LOG_PATH
}
exit $result

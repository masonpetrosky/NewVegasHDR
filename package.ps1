# Makes the release download: dist\NewVegasHDR-<version>.zip, laid out for the game's Data folder (NVSE\Plugins\ with
# the plugin, its default ini and the licence), so mod managers install it as it is.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" }
$match = Select-String -Path "$PSScriptRoot\CMakeLists.txt" -Pattern 'project\(NewVegasHDR VERSION ([0-9.]+)'
$version = $match.Matches[0].Groups[1].Value
$dist = Join-Path $PSScriptRoot 'dist'
$stage = Join-Path $dist 'stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugins = New-Item -ItemType Directory -Force (Join-Path $stage 'NVSE\Plugins')
Copy-Item "$PSScriptRoot\build\NewVegasHDR.dll", "$PSScriptRoot\package\NewVegasHDR.ini" $plugins
Copy-Item "$PSScriptRoot\LICENSE" (Join-Path $plugins 'NewVegasHDR-LICENSE.txt')

$zip = Join-Path $dist "NewVegasHDR-$version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -Recurse -Force $stage
Get-Item $zip | Select-Object Name, Length
"SHA-256 $((Get-FileHash $zip).Hash)"

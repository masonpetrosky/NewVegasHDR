# Downloads the build DXVK's own CI made of a master commit (a GitHub Actions artifact: needs `gh auth login`, and
# artifacts expire after 90 days) into third_party\dxvk\<short sha>, to test the plugin against DXVK's newest code.
# DXVK 3.0 to 3.1.1 don't enable VK_EXT_swapchain_colorspace on their Vulkan instance. NVIDIA's driver gives them an
# HDR swap chain anyway (tests\dxvk_hdr_probe.cpp shows it); other drivers may not. Master has the spec-correct fix
# (3c40ca1b94, 2026-09-28), which no release has yet. DXVK is under the zlib license (LICENSE in its repository).
param([string]$Sha = '02e2028c3fe954c8913b1f39bce2f581739f0c41')
$ErrorActionPreference = 'Stop'

$name = "dxvk-master-$Sha"
$dst = Join-Path $PSScriptRoot "..\third_party\dxvk\$($Sha.Substring(0, 10))"
$run = gh api "repos/doitsujin/dxvk/actions/artifacts?name=$name" --jq '.artifacts[] | select(.expired | not) | .workflow_run.id' |
  Select-Object -First 1
if (-not $run) { throw "No unexpired CI artifact $name; pick a newer master commit." }
New-Item -ItemType Directory -Force $dst | Out-Null
gh run download $run -R doitsujin/dxvk -n $name -D $dst
if ($LASTEXITCODE) { throw 'gh run download failed.' }
Set-Content -Path (Join-Path $dst 'VERSION') -Value "$Sha (CI run $run)"
Get-ChildItem $dst -Recurse -File | Select-Object @{n = 'File'; e = { $_.FullName.Substring((Resolve-Path $dst).Path.Length + 1) } }, Length

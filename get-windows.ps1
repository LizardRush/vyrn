<#
Vyrn for Windows 10/11 (x64), in one line. Downloads the source into a temporary folder, builds it, installs Vyrn to
%LOCALAPPDATA%\Programs\Vyrn (with a Start menu shortcut) and deletes the download and the build.

  irm https://raw.githubusercontent.com/LizardRush/vyrn/main/get-windows.ps1 | iex

Options (set before running): $env:VYRN_BRANCH (default main). Needs no administrator rights. The build tools (Qt,
libtorch) are kept in %LOCALAPPDATA%\Vyrn so a second install is quick.
#>
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Repo = 'LizardRush/vyrn'
$Branch = if ($env:VYRN_BRANCH) { $env:VYRN_BRANCH } else { 'main' }
$Work = Join-Path ([IO.Path]::GetTempPath()) ('vyrn-' + [guid]::NewGuid().ToString('N').Substring(0, 8))

try {
  New-Item -ItemType Directory -Path $Work | Out-Null
  Write-Host "==> Working in $Work (deleted when this finishes)"
  Write-Host "==> Downloading Vyrn ($Repo, $Branch)"
  $zip = Join-Path $Work 'vyrn.zip'
  Invoke-WebRequest -UseBasicParsing "https://codeload.github.com/$Repo/zip/refs/heads/$Branch" -OutFile $zip
  Expand-Archive -Path $zip -DestinationPath $Work -Force
  $src = Get-ChildItem $Work -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'install-windows.ps1') } | Select-Object -First 1
  if (-not $src) { throw "That download has no install-windows.ps1. Is $Repo/$Branch the right place?" }
  try {   # best effort: remember which commit this is, so the app can tell when a newer one exists
    $sha = ([string](Invoke-RestMethod -UseBasicParsing -TimeoutSec 10 -Headers @{ Accept = 'application/vnd.github.sha' } -Uri "https://api.github.com/repos/$Repo/commits/$Branch")).Trim()
    if ($sha -match '^[0-9a-fA-F]{40}$') { [System.IO.File]::WriteAllText((Join-Path $src.FullName '.vyrn-commit'), "$sha`n") }
  } catch { }
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $src.FullName 'install-windows.ps1') -Deps -App
  if ($LASTEXITCODE -ne 0) { throw "The installer stopped (exit code $LASTEXITCODE). The lines above say why." }
  Write-Host '==> Done. Open Vyrn from the Start menu.'
}
finally {
  Set-Location $env:USERPROFILE
  Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}

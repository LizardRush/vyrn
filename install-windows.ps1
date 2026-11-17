<#
.SYNOPSIS
Vyrn installer for Windows 10/11 (x64). Safe to run again: every step checks first and only changes what is missing.

.DESCRIPTION
In PowerShell, from this folder:

  powershell -ExecutionPolicy Bypass -File .\install-windows.ps1          everything below
  powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Deps    check the tools (Visual Studio 2022 Build
                                     Tools, CMake, Ninja, Python 3.10+ with numpy and Pillow), then put Qt 6.8,
                                     QScintilla 2.14 and libtorch in %LOCALAPPDATA%\Vyrn
  powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -App     build Vyrn, install it to
                                     %LOCALAPPDATA%\Programs\Vyrn and add it to the Start menu

Options: -Prefix <folder> (default %LOCALAPPDATA%\Programs\Vyrn), -Jobs <number> (default: one per core, at most
one per 2 GB of memory). Needs no administrator rights. macOS: ./install-mac.sh. Linux: ./install.sh.
#>
[CmdletBinding()]
param(
  [switch]$Deps,
  [switch]$App,
  [string]$Prefix = '',
  [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow while it draws a progress bar
Set-StrictMode -Version 2.0

$Here = $PSScriptRoot
$VyrnHome = Join-Path $env:LOCALAPPDATA 'Vyrn'
if (-not $Prefix) { $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\Vyrn' }
$Build = Join-Path $Here 'build'

$QtVersion = '6.8.3'
$QtArch = 'win64_msvc2022_64'
$QtRoot = Join-Path $VyrnHome 'Qt'
$QtDir = Join-Path $QtRoot "$QtVersion\msvc2022_64"
$QsciVersion = '2.14.1'
$QsciUrl = "https://www.riverbankcomputing.com/static/Downloads/QScintilla/$QsciVersion/QScintilla_src-$QsciVersion.zip"
$QsciSha256 = '2c805cc7914acb47e923acf176ddb75025cdf025c42e4e954f255ca3e99db7b2'
# The libtorch C++ API Vyrn is written against: the same version as Debian's libtorch-dev on the Raspberry Pi.
$TorchVersion = '2.6.0'
$TorchUrl = "https://download.pytorch.org/libtorch/cpu/libtorch-win-shared-with-deps-$TorchVersion%2Bcpu.zip"
$TorchDir = Join-Path $VyrnHome 'libtorch'

function Say([string]$Message)  { Write-Host "==> $Message" -ForegroundColor Cyan }
function Ok([string]$Message)   { Write-Host " ok $Message" -ForegroundColor Green }
function Warn([string]$Message) { Write-Host " !! $Message" -ForegroundColor Yellow }
function Fail([string]$Message) { Write-Host "error: $Message" -ForegroundColor Red; exit 1 }

# CMake wants forward slashes (a backslash before a quote confuses command lines).
function ConvertTo-CMakePath([string]$Path) { return $Path -replace '\\', '/' }

# Runs a program; stops the script with a clear message when it fails. Its output goes straight to the window.
function Invoke-Checked([string]$What, [string]$Exe, [string[]]$Arguments) {
  if (-not (Get-Command $Exe -ErrorAction SilentlyContinue)) { Fail "$What failed: $Exe is missing" }
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'   # text on stderr (warnings) is not a failure; the exit code is
  try { & $Exe @Arguments } finally { $ErrorActionPreference = $old }
  if ($LASTEXITCODE -ne 0) { Fail "$What failed (exit code $LASTEXITCODE). The messages above say why." }
}

# Runs a program quietly: $true when it worked.
function Test-Native([string]$Exe, [string[]]$Arguments) {
  if (-not (Get-Command $Exe -ErrorAction SilentlyContinue)) { return $false }
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $Exe @Arguments *> $null
    return ($LASTEXITCODE -eq 0)
  } catch {
    return $false
  } finally {
    $ErrorActionPreference = $old
  }
}

# Runs a program and returns what it printed (trimmed), or $null when it failed.
function Get-NativeOutput([string]$Exe, [string[]]$Arguments) {
  if (-not (Get-Command $Exe -ErrorAction SilentlyContinue)) { return $null }
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & $Exe @Arguments 2> $null
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($out | Out-String).Trim()
  } catch {
    return $null
  } finally {
    $ErrorActionPreference = $old
  }
}

function Save-Download([string]$Url, [string]$File, [string]$Sha256) {
  if ((Test-Path $File) -and $Sha256 -and ((Get-FileHash $File -Algorithm SHA256).Hash -eq $Sha256)) { return }
  New-Item -ItemType Directory -Force -Path (Split-Path $File) | Out-Null
  $part = "$File.part"
  Say "Downloading $Url"
  $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
  if ($curl) {
    Invoke-Checked 'The download' $curl.Source @('-fL', '--retry', '3', '-o', $part, $Url)
  } else {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try { Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing }
    catch { Fail "the download failed: $($_.Exception.Message). Check the internet connection and run this again." }
  }
  if ($Sha256 -and ((Get-FileHash $part -Algorithm SHA256).Hash -ne $Sha256)) {
    Remove-Item -Force $part
    Fail "the download of $Url is damaged (its checksum is wrong). Run this again."
  }
  Move-Item -Force $part $File
}

function Expand-Zip([string]$Zip, [string]$Destination) {
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  $tar = Join-Path $env:SystemRoot 'System32\tar.exe'   # Windows 10 1803+; much faster than Expand-Archive
  if (Test-Path $tar) {
    Invoke-Checked "Unpacking $Zip" $tar @('-xf', $Zip, '-C', $Destination)
  } else {
    Expand-Archive -Path $Zip -DestinationPath $Destination -Force
  }
}

function Get-DefaultJobs {
  $cores = [Environment]::ProcessorCount
  try {
    $gb = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $cores = [math]::Min($cores, [math]::Max(1, [math]::Floor($gb / 2)))   # libtorch code needs ~2 GB per job
  } catch { }
  return [int]$cores
}

# ---- tools -------------------------------------------------------------------------------------------------------

function Find-VisualStudio {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  $path = Get-NativeOutput $vswhere @('-latest', '-products', '*', '-version', '17.0',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
  if ($path -and (Test-Path (Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'))) { return $path }
  return $null
}

# Loads the Visual Studio x64 build environment (cl, link, nmake, and the CMake and Ninja that come with it) into
# this PowerShell session, like the "x64 Native Tools Command Prompt".
function Enter-Msvc {
  if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
  Say 'Checking Visual Studio 2022 Build Tools (the C++ compiler)'
  $vs = Find-VisualStudio
  if (-not $vs) {
    Fail ("the C++ compiler is missing. Install Visual Studio 2022 Build Tools with 'Desktop development with C++'.`n" +
      "  In PowerShell: winget install Microsoft.VisualStudio.2022.BuildTools --override " +
      "`"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"`n" +
      "  or download it from https://visualstudio.microsoft.com/downloads/ (Tools for Visual Studio). Then run this again.")
  }
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
  $lines = & cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { Fail "$vcvars failed: repair Visual Studio Build Tools in the Visual Studio Installer" }
  foreach ($line in $lines) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process') }
  }
  if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Fail "cl.exe is still missing after $vcvars" }
  Ok "Visual Studio in $vs"
}

function Assert-CMakeNinja {
  Say 'Checking CMake and Ninja'
  $cmake = Get-NativeOutput 'cmake' @('--version')
  if (-not $cmake) {
    Fail ("CMake is missing. Add 'C++ CMake tools for Windows' in the Visual Studio Installer (Modify > Individual " +
      "components), or: winget install Kitware.CMake. Then open a new PowerShell window and run this again.")
  }
  $version = [version](($cmake -split "`n")[0] -replace '^cmake version ([0-9]+\.[0-9]+\.[0-9]+).*$', '$1')
  if ($version -lt [version]'3.25') { Fail "CMake $version is too old (Vyrn needs 3.25 or newer): winget install Kitware.CMake" }
  if (-not (Get-NativeOutput 'ninja' @('--version'))) {
    Fail ("Ninja is missing. Add 'C++ CMake tools for Windows' in the Visual Studio Installer, or: winget install " +
      "Ninja-build.Ninja. Then open a new PowerShell window and run this again.")
  }
  Ok "CMake $version and Ninja"
}

# The Python Vyrn runs (core/src/platform/system_win.cpp): python.exe on PATH unless it is the Microsoft Store alias,
# else the py launcher. Sets $script:PyExe, $script:PyArgs and $script:PyPath (the real python.exe).
function Find-Python {
  Say 'Checking Python (3.10 or newer)'
  $script:PyExe = $null
  $script:PyArgs = @()
  $python = Get-Command python.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($python -and $python.Source -notlike '*\WindowsApps\*') {
    $script:PyExe = $python.Source
  } else {
    $py = Get-Command py.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($py) { $script:PyExe = $py.Source; $script:PyArgs = @('-3') }
  }
  if (-not $script:PyExe) {
    Fail ("Python is missing. Install Python 3.12 from https://www.python.org/downloads/ (tick 'Add python.exe to " +
      "PATH'), or: winget install Python.Python.3.12. Then open a new PowerShell window and run this again.")
  }
  $version = Get-NativeOutput $script:PyExe ($script:PyArgs + @('-c', 'import sys; print(*sys.version_info[:3], sep=chr(46))'))
  if (-not $version -or [version]$version -lt [version]'3.10') {
    Fail "$($script:PyExe) is Python $version, and Vyrn needs 3.10 or newer: install Python 3.12 (winget install Python.Python.3.12) and put it first on PATH."
  }
  $script:PyPath = Get-NativeOutput $script:PyExe ($script:PyArgs + @('-c', 'import sys; print(sys.executable)'))
  Ok "Python $version ($($script:PyPath))"
}

function Install-PythonPackages {
  if (Test-Native $script:PyExe ($script:PyArgs + @('-c', 'import numpy, PIL'))) { Ok 'numpy and Pillow work'; return }
  Say 'Installing numpy and Pillow for your user (pip)'
  Invoke-Checked 'pip install numpy pillow' $script:PyExe ($script:PyArgs + @('-m', 'pip', 'install', '--user', 'numpy', 'pillow'))
  Ok 'numpy and Pillow installed'
}

# ---- libraries -----------------------------------------------------------------------------------------------------

function Install-Qt {
  Say "Checking Qt $QtVersion in $QtDir"
  if (Test-Path (Join-Path $QtDir 'bin\qmake.exe')) { Ok "Qt $QtVersion is already there"; return }
  Say 'Installing aqtinstall (a Qt downloader) with pip, for your user'
  Invoke-Checked 'pip install aqtinstall' $script:PyExe ($script:PyArgs + @('-m', 'pip', 'install', '--user', '--upgrade', 'aqtinstall'))
  Say "Downloading Qt $QtVersion for Visual Studio (about 1 GB unpacked)"
  Invoke-Checked 'Installing Qt' $script:PyExe ($script:PyArgs + @('-m', 'aqt', 'install-qt', 'windows', 'desktop',
    $QtVersion, $QtArch, '--outputdir', $QtRoot))
  if (-not (Test-Path (Join-Path $QtDir 'bin\qmake.exe'))) { Fail "Qt is not where it should be: $QtDir" }
  Ok "Qt $QtVersion in $QtDir"
}

# QScintilla (the code editor widget) has no Windows download: build it from Riverbank's source with Qt's qmake
# and nmake, and install it into the Qt folder (lib\qscintilla2_qt6.lib + .dll, include\Qsci).
function Install-QScintilla {
  Say "Checking QScintilla $QsciVersion"
  $lib = Join-Path $QtDir 'lib\qscintilla2_qt6.lib'
  if (Test-Path $lib) { Ok "QScintilla is already built into Qt"; return }
  $srcRoot = Join-Path $VyrnHome 'src'
  $zip = Join-Path $srcRoot "QScintilla_src-$QsciVersion.zip"
  Save-Download $QsciUrl $zip $QsciSha256
  $dir = Join-Path $srcRoot "QScintilla_src-$QsciVersion"
  if (-not (Test-Path $dir)) { Expand-Zip $zip $srcRoot }
  Say 'Building QScintilla (a few minutes)'
  Push-Location (Join-Path $dir 'src')
  $oldCl = $env:CL
  $env:CL = '/MP'   # nmake runs one compiler at a time; this lets it use every core
  try {
    Invoke-Checked 'qmake (QScintilla)' (Join-Path $QtDir 'bin\qmake.exe') @('CONFIG-=debug_and_release', 'CONFIG+=release', 'qscintilla.pro')
    Invoke-Checked 'nmake (QScintilla)' 'nmake.exe' @('/nologo')
    Invoke-Checked 'nmake install (QScintilla)' 'nmake.exe' @('/nologo', 'install')
  } finally {
    $env:CL = $oldCl
    Pop-Location
  }
  if (-not (Test-Path $lib)) { Fail "QScintilla built, but $lib is missing" }
  Ok "QScintilla $QsciVersion in $QtDir"
}

function Install-Libtorch {
  Say "Checking libtorch $TorchVersion (the neural network library) in $TorchDir"
  $config = Join-Path $TorchDir 'share\cmake\Torch\TorchConfig.cmake'
  $marker = Join-Path $TorchDir '.vyrn-version'
  if ((Test-Path $config) -and (Test-Path $marker) -and ((Get-Content $marker -Raw).Trim() -eq $TorchVersion)) {
    Ok "libtorch $TorchVersion is already there"
    return
  }
  $zip = Join-Path $VyrnHome "libtorch-win-$TorchVersion.zip"
  Say 'libtorch for Windows is about 190 MB to download and 1 GB unpacked'
  Save-Download $TorchUrl $zip ''
  $unpack = Join-Path $VyrnHome 'libtorch-unpack'
  if (Test-Path $unpack) { Remove-Item -Recurse -Force $unpack }
  Expand-Zip $zip $unpack
  if (-not (Test-Path (Join-Path $unpack 'libtorch\share\cmake\Torch\TorchConfig.cmake'))) {
    Remove-Item -Force $zip
    Fail 'the libtorch download is damaged (no share\cmake\Torch in it). Run this again.'
  }
  if (Test-Path $TorchDir) { Remove-Item -Recurse -Force $TorchDir }
  Move-Item (Join-Path $unpack 'libtorch') $TorchDir
  Set-Content -Path $marker -Value $TorchVersion -Encoding ASCII
  Remove-Item -Recurse -Force $unpack
  Remove-Item -Force $zip
  Ok "libtorch $TorchVersion in $TorchDir"
}

function Invoke-Deps {
  Enter-Msvc
  Assert-CMakeNinja
  Find-Python
  Install-PythonPackages
  Install-Qt
  Install-QScintilla
  Install-Libtorch
}

# ---- Vyrn ----------------------------------------------------------------------------------------------------------

function Register-FileTypes([string]$Exe) {
  # Double-clicking a .vrlp (a shared project) or .vrl (a trained AI) opens Vyrn. Only for this user.
  $types = @(@('.vrlp', 'Vyrn.Project', 'Vyrn project'), @('.vrl', 'Vyrn.Brain', 'Vyrn trained AI'))
  foreach ($t in $types) {
    $ext = $t[0]; $progId = $t[1]; $description = $t[2]
    $classes = 'HKCU:\Software\Classes'
    New-Item -Force -Path "$classes\$ext" | Out-Null
    Set-ItemProperty -Path "$classes\$ext" -Name '(default)' -Value $progId
    New-Item -Force -Path "$classes\$progId\DefaultIcon" | Out-Null
    New-Item -Force -Path "$classes\$progId\shell\open\command" | Out-Null
    Set-ItemProperty -Path "$classes\$progId" -Name '(default)' -Value $description
    Set-ItemProperty -Path "$classes\$progId\DefaultIcon" -Name '(default)' -Value "`"$Exe`",0"
    Set-ItemProperty -Path "$classes\$progId\shell\open\command" -Name '(default)' -Value "`"$Exe`" `"%1`""
  }
}

# A build folder copied from another computer (or made in another folder) stops CMake with a cryptic message.
function Assert-BuildDir {
  $cache = Join-Path $Build 'CMakeCache.txt'
  if (-not (Test-Path $cache)) { return }
  $line = Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
  if ($line) {
    $madeIn = $line.Matches[0].Groups[1].Value
    if ((ConvertTo-CMakePath $madeIn) -ne (ConvertTo-CMakePath $Here)) {
      Fail "$Build was made for $madeIn (another computer or folder). Delete it, then run this again: Remove-Item -Recurse -Force `"$Build`""
    }
  }
}

function Invoke-App {
  Assert-BuildDir
  Enter-Msvc
  Assert-CMakeNinja
  Find-Python
  if (-not (Test-Path (Join-Path $QtDir 'lib\qscintilla2_qt6.lib'))) { Fail 'Qt or QScintilla is missing: run this with -Deps first' }
  if (-not (Test-Path (Join-Path $TorchDir 'share\cmake\Torch\TorchConfig.cmake'))) { Fail 'libtorch is missing: run this with -Deps first' }
  if ($Jobs -le 0) { $Jobs = Get-DefaultJobs }

  Say "Configuring the build in $Build"
  Invoke-Checked 'Configuring (cmake)' 'cmake' @('-S', $Here, '-B', $Build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DVYRN_BUILD_TESTS=OFF', '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
    "-DCMAKE_INSTALL_PREFIX=$(ConvertTo-CMakePath $Prefix)",
    "-DCMAKE_PREFIX_PATH=$(ConvertTo-CMakePath $QtDir)",
    "-DTorch_DIR=$(ConvertTo-CMakePath $TorchDir)/share/cmake/Torch",
    "-DPython3_EXECUTABLE=$(ConvertTo-CMakePath $script:PyPath)")
  Say "Building Vyrn with $Jobs jobs (15-30 minutes the first time)"
  Invoke-Checked 'Building' 'cmake' @('--build', $Build, '--parallel', "$Jobs")

  Say "Installing to $Prefix"
  Invoke-Checked 'Installing' 'cmake' @('--install', $Build)
  $bin = Join-Path $Prefix 'bin'
  $exe = Join-Path $bin 'vyrn.exe'
  if (-not (Test-Path $exe)) { Fail "the install did not create $exe" }

  Say 'Copying Qt next to Vyrn (windeployqt)'
  $deploy = @('--release', '--no-translations', '--dir', $bin, $exe)
  $qsci = Join-Path $bin 'qscintilla2_qt6.dll'
  if (Test-Path $qsci) { $deploy += $qsci }   # QScintilla's own Qt modules (PrintSupport)
  Invoke-Checked 'windeployqt' (Join-Path $QtDir 'bin\windeployqt.exe') $deploy

  $cli = Join-Path $bin 'vyrn-cli.exe'
  if (Test-Native $cli @('--version')) { Ok 'vyrn-cli starts' } else { Warn "vyrn-cli doesn't start; run it to see why: & `"$cli`" --version" }

  Say 'Adding Vyrn to the Start menu'
  $link = Join-Path ([Environment]::GetFolderPath('Programs')) 'Vyrn.lnk'
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($link)
  $shortcut.TargetPath = $exe
  $shortcut.WorkingDirectory = $bin
  $shortcut.Description = 'Teach an AI to play games'
  $shortcut.Save()
  Ok $link
  Register-FileTypes $exe
  Ok 'double-clicking a .vrlp or .vrl file opens Vyrn'

  Write-Host ''
  Write-Host '      Panic keys: Ctrl+Alt+Esc stops an AI that is pressing keys, right away.'
  Write-Host '      A game that runs as administrator ignores Vyrn''s keys: start the game normally.'
  Write-Host "      vyrn-cli for a terminal or a coding agent: `"$cli`""
  Ok 'Vyrn is installed: open it from the Start menu (type Vyrn)'
}

# ---- main ----------------------------------------------------------------------------------------------------------

if (-not $Here) { Fail 'run this as a script: powershell -ExecutionPolicy Bypass -File .\install-windows.ps1' }
if (-not [Environment]::Is64BitOperatingSystem) { Fail 'Vyrn needs 64-bit Windows' }
if (-not $Deps -and -not $App) { $Deps = $true; $App = $true }
if ($Deps) { Invoke-Deps }
if ($App) { Invoke-App }
Ok 'done'

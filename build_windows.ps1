# Builds the tools with the Visual Studio C compiler (no CMake needed). Run from the apeiron-sdl folder:  .\build_windows.ps1
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vcvars = @("$env:ProgramFiles\Microsoft Visual Studio\2022\Community","$env:ProgramFiles\Microsoft Visual Studio\2022\Professional","$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise","$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools","${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools") |
  ForEach-Object { Join-Path $_ 'VC\Auxiliary\Build\vcvars64.bat' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found; install the Visual Studio 2022 C++ tools' }
$bin = Join-Path $here 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
Push-Location $here
try {
  $cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS /Fo:bin\ /Fe:bin\apeiron_export.exe tools\apeiron_export.c src\rsrc.c src\pict.c src\ppat.c src\snd.c src\writers.c"
  cmd /c $cmd
  if ($LASTEXITCODE -ne 0) { throw 'build failed' }
} finally { Pop-Location }
Write-Host "built $bin\apeiron_export.exe"
# the scores reader
$cmd2 = "`"$vcvars`" >nul 2>&1 && cd /d `"$here`" && cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS /Fo:bin\ /Fe:bin\apeiron_scores.exe tools\apeiron_scores.c src\rsrc.c src\scores.c"
cmd /c $cmd2
if ($LASTEXITCODE -ne 0) { throw 'build of apeiron_scores failed' }
Write-Host "built $bin\apeiron_scores.exe"
# the SDL front end (needs third_party\sdl2_dl\SDL2-2.30.9, see README)
$sdl = Join-Path $here 'third_party\sdl2_dl\SDL2-2.30.9'
if (Test-Path $sdl) {
  # the icon of the user's Apeiron file for apeiron.exe (skipped if the data files are not at hand: bin\data, or the folder in $env:APEIRON_DATA)
  $res = ''
  $cmdi = "`"$vcvars`" >nul 2>&1 && cd /d `"$here`" && cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS /Fo:bin\ /Fe:bin\apeiron_icon.exe tools\apeiron_icon.c src\cicn.c src\rsrc.c"
  cmd /c $cmdi
  foreach ($d in @($env:APEIRON_DATA, (Join-Path $bin 'data'))) {
    if ($d -and (Test-Path $d) -and -not $res) {
      & "$bin\apeiron_icon.exe" $d "$bin\apeiron.ico"
      if ($LASTEXITCODE -eq 0) {
        Set-Content -Encoding ascii "$bin\apeiron.rc" '1 ICON "apeiron.ico"'
        cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$bin`" && rc /nologo apeiron.rc"
        if ($LASTEXITCODE -eq 0) { $res = 'bin\apeiron.res' }
      }
    }
  }
  $cmd3 = "`"$vcvars`" >nul 2>&1 && cd /d `"$here`" && cl /nologo /W3 /O2 /MT /std:c11 /D_CRT_SECURE_NO_WARNINGS /I`"$sdl\include`" /Fo:bin\ /Fe:bin\apeiron.exe src\apeiron_sdl.c src\game.c src\creatures.c src\rng.c src\mixer.c src\snd.c src\rsrc.c src\pict.c src\ppat.c src\scores.c src\font.c src\title.c src\cicn.c src\pictdraw.c src\fontdata.c src\writers.c $res /link /LIBPATH:`"$sdl\lib\x64`" SDL2.lib"
  cmd /c $cmd3
  if ($LASTEXITCODE -ne 0) { throw 'build of apeiron failed' }
  Copy-Item "$sdl\lib\x64\SDL2.dll" $bin -Force
  Write-Host "built $bin\apeiron.exe"
}


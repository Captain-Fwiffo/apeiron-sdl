# Builds the Musashi differential tests with the Visual Studio C compiler. Run from verify\musashi:  .\build_musashi.ps1
# Needs the Musashi sources in ..\musashi_src (git clone https://github.com/kstenerud/Musashi.git musashi_src).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vcvars = @("$env:ProgramFiles\Microsoft Visual Studio\2022\Community","$env:ProgramFiles\Microsoft Visual Studio\2022\Professional","$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise","$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools","${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools") |
  ForEach-Object { Join-Path $_ 'VC\Auxiliary\Build\vcvars64.bat' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found; install the Visual Studio 2022 C++ tools' }
$ms = Join-Path $here '..\musashi_src'
$bin = Join-Path $here 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
# 1. the generator writes m68kops.c/h next to itself
$gen = "`"$vcvars`" >nul 2>&1 && cd /d `"$ms`" && cl /nologo /D_CRT_SECURE_NO_WARNINGS /Fo:`"$bin\\`" /Fe:`"$bin\m68kmake.exe`" m68kmake.c && `"$bin\m68kmake.exe`""
cmd /c $gen
if ($LASTEXITCODE -ne 0) { throw 'm68kmake failed' }
# 2. the tests: Musashi is configured from the command line, the instruction hook is the harness's
$defs = "/FI`"$here\musashi_opts.h`""
foreach ($t in 'test_rng','test_creatures','test_step','test_engine','test_spawn','test_shots','test_loop','dasm') {
  $cmd = "`"$vcvars`" >nul 2>&1 && cd /d `"$here`" && cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS $defs /I`"$ms`" /Fo:`"$bin\\`" /Fe:`"$bin\$t.exe`" $t.c harness.c ..\..\src\rng.c ..\..\src\creatures.c `"$ms\m68kcpu.c`" `"$ms\m68kdasm.c`" `"$ms\m68kops.c`" `"$ms\softfloat\softfloat.c`""
  cmd /c $cmd
  if ($LASTEXITCODE -ne 0) { throw "build of $t failed" }
}
# 3. the whole-game replay comparison (it needs the port's game code too)
$t = 'test_replay'
$src = '..\..\src'
$cmd = "`"$vcvars`" >nul 2>&1 && cd /d `"$here`" && cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS $defs /I`"$ms`" /Fo:`"$bin\\`" /Fe:`"$bin\$t.exe`" $t.c harness.c $src\rng.c $src\creatures.c $src\game.c $src\mixer.c $src\snd.c $src\rsrc.c $src\pict.c $src\ppat.c $src\scores.c `"$ms\m68kcpu.c`" `"$ms\m68kdasm.c`" `"$ms\m68kops.c`" `"$ms\softfloat\softfloat.c`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build of $t failed" }
Write-Host "built $bin"

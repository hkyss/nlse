@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo MSVC was not found. Install Visual Studio 2022 Build Tools with the C++ workload.
  exit /b 1
)
call "%VCVARS%" >nul 2>&1 || exit /b 1
set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /LD "%~dp0nlse.cpp" "%~dp0winmm_exports.cpp" /Fo"%OUT%\\" /Fe"%OUT%\nlse.dll" /link /NOLOGO || exit /b 1
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /LD "%~dp0example_plugin.cpp" /Fo"%OUT%\\" /Fe"%OUT%\example_plugin.dll" /link /NOLOGO || exit /b 1
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc "%~dp0check.cpp" /Fo"%OUT%\\" /Fe"%OUT%\check.exe" /link /NOLOGO || exit /b 1
"%OUT%\check.exe" %1 || exit /b 1
copy /y "%OUT%\nlse.dll" "%OUT%\winmm.dll" >nul || exit /b 1
echo Built %OUT%\nlse.dll and %OUT%\winmm.dll

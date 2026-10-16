@echo off
rem Builds build\xinput1_3.dll with the Visual Studio 2019 x64 compiler.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /MT /W4 /EHsc /std:c++17 src\dllmain.cpp /Fobuild\ /Fdbuild\ ^
   /link /DEF:src\xinput1_3.def /OUT:build\xinput1_3.dll /IMPLIB:build\xinput1_3.lib /PDB:build\xinput1_3.pdb /DEBUG /INCREMENTAL:NO /OPT:REF kernel32.lib user32.lib

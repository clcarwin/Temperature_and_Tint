@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /W3 /fp:precise temptint.cpp /Fe:temptint.exe

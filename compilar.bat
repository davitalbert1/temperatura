@echo off
setlocal EnableExtensions
cd /d "%~dp0"

:: Compila download.cpp com o toolchain MinGW64 do MSYS2
set "GXX=D:\msys64\mingw64\bin\g++.exe"

if not exist "%GXX%" (
    for %%D in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
        if exist "%%D:\msys64\mingw64\bin\g++.exe" set "GXX=%%D:\msys64\mingw64\bin\g++.exe"
        if exist "%%D:\mingw64\mingw64\bin\g++.exe"  set "GXX=%%D:\mingw64\mingw64\bin\g++.exe"
    )
)

if not exist "%GXX%" (
    echo ERRO: g++ do MinGW64 nao encontrado. Instale o MSYS2: https://www.msys2.org/
    exit /b 1
)

echo Compilando download.cpp com:
echo    %GXX%
echo.

for %%I in ("%GXX%") do set "GXX_DIR=%%~dpI"
set "PATH=%GXX_DIR%;%PATH%"

"%GXX%" -std=c++17 -O2 -Wall -o "%~dp0download.exe" "%~dp0download.cpp" -lcurl -lsqlite3 -pthread

if errorlevel 1 (
    echo.
    echo Falha na compilacao.
    exit /b 1
)

echo.
echo OK: download.exe gerado.
echo Uso: download.exe [caminho_do_ini]   ^(padrao: download.ini^)
endlocal

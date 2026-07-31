@echo off
setlocal EnableExtensions EnableDelayedExpansion

title Auto Bash Launcher (All Drives)
cd /d "%~dp0"
pushd "%~dp0"

set "BASH_EXE="

echo.
echo Procurando bash.exe em todos os discos...
echo.

:: PATH
for %%I in (bash.exe) do (
    if not "%%~$PATH:I"=="" (
        set "CANDIDATE=%%~$PATH:I"

        echo !CANDIDATE! | findstr /I "System32\\bash.exe" >nul
        if errorlevel 1 (
            set "BASH_EXE=!CANDIDATE!"
            goto found
        )
    )
)

::ESCANEAR DISCOS
for %%D in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (

    if exist "%%D:\msys64\usr\bin\bash.exe" (
        set "BASH_EXE=%%D:\msys64\usr\bin\bash.exe"
        goto found
    )

    if exist "%%D:\mingw64\usr\bin\bash.exe" (
        set "BASH_EXE=%%D:\mingw64\usr\bin\bash.exe"
        goto found
    )

    if exist "%%D:\Program Files\Git\bin\bash.exe" (
        set "BASH_EXE=%%D:\Program Files\Git\bin\bash.exe"
        goto found
    )

    if exist "%%D:\Program Files (x86)\Git\bin\bash.exe" (
        set "BASH_EXE=%%D:\Program Files (x86)\Git\bin\bash.exe"
        goto found
    )
)

:: 3. WSL (IGNORADO)
if exist "C:\Windows\System32\bash.exe" (
    echo.
    echo AVISO: WSL detectado (System32\bash.exe) sera ignorado.
    echo Ele pode causar erro com caminhos tipo E:\ ou D:\.
)

:: RESULTADO FINAL
:found

if not defined BASH_EXE (
    echo.
    echo ERRO: Nenhum bash.exe encontrado em nenhum disco.
    echo.
    echo Instale uma opcao:
    echo - MSYS2: https://www.msys2.org/
    echo - Git Bash: https://git-scm.com/
    echo.
    pause
    exit /b 1
)

echo.
echo Bash encontrado:
echo %BASH_EXE%
echo.

:: CONFIGURACAO DO AMBIENTE
set MSYSTEM=MINGW64
set MSYS2_PATH_TYPE=inherit

echo Iniciando shell na pasta atual...
echo.

:: EXECUCAO
"%BASH_EXE%" --login -i -c "cd \"$(cygpath '%CD%')\" && exec bash"

if errorlevel 1 (
    echo.
    echo Bash terminou com erro.
    pause
)

endlocal
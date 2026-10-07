@echo off
setlocal EnableExtensions EnableDelayedExpansion

title Auto Bash Launcher (All Drives)
cd /d "%~dp0"
pushd "%~dp0"

set "BASH_EXE="
set "CONFIG_FILE=%~dp0config.ini"

:: 1. TENTA USAR O config.ini SE EXISTIR
if exist "%CONFIG_FILE%" (
    echo Lendo config.ini...
    echo    %CONFIG_FILE%
    echo.
    for /f "usebackq tokens=1,* delims==" %%A in ("%CONFIG_FILE%") do (
        set "KEY=%%A"
        set "VAL=%%B"
        REM remove espacos da chave
        set "KEY=!KEY: =!"
        if /I "!KEY!"=="bash" (
            set "BASH_EXE=!VAL!"
            REM remove aspas caso o usuario tenha salvo com aspas
            set BASH_EXE=!BASH_EXE:"=!
            REM remove espacos iniciais do valor
            for /f "tokens=*" %%V in ("!BASH_EXE!") do set "BASH_EXE=%%V"
        )
    )
    REM Compatibilidade: config.ini com apenas caminho puro sem "bash="
    if not defined BASH_EXE (
        for /f "usebackq delims=" %%L in ("%CONFIG_FILE%") do (
            if not defined BASH_EXE (
                set "LINE=%%L"
                if defined LINE (
                    echo !LINE! | findstr /I /C:"bash.exe" >nul
                    if not errorlevel 1 (
                        set BASH_EXE=!LINE:"=!
                    )
                )
            )
        )
    )

    if defined BASH_EXE (
        if exist "!BASH_EXE!" (
            echo Bash carregado do config.ini:
            echo !BASH_EXE!
            echo.
            goto run
        ) else (
            echo AVISO: caminho salvo no config.ini nao existe mais:
            echo    !BASH_EXE!
            echo Procurando novamente...
            echo.
            set "BASH_EXE="
        )
    ) else (
        echo AVISO: config.ini existe mas nao tem entrada "bash=" valida.
        echo Procurando novamente...
        echo.
    )
)

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
echo !BASH_EXE!
echo.

:: 2. SALVA (OU ATUALIZA) O config.ini COM O CAMINHO ENCONTRADO
(
    echo [mingw]
    echo bash=!BASH_EXE!
) > "%CONFIG_FILE%"
echo Caminho salvo em:
echo    %CONFIG_FILE%
echo.

:run

:: CONFIGURACAO DO AMBIENTE
set MSYSTEM=MINGW64
set MSYS2_PATH_TYPE=inherit

echo Iniciando shell na pasta atual...
echo.

:: EXECUCAO
"!BASH_EXE!" --login -i -c "cd \"$(cygpath '%CD%')\" && exec bash"

if errorlevel 1 (
    echo.
    echo Bash terminou com erro.
    pause
)

endlocal
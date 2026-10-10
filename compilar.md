# Compilar

Os dois executáveis (`clima_viewer.exe` e `download.exe`) são gerados pelo mesmo `CMakeLists.txt`, na raiz do projeto.

## Pré-requisitos (MSYS2)

No shell MSYS2 MinGW64 (use `abrir_mingw.bat`), instale o que falta:

```sh
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-base mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-nlohmann-json
```

## Configurar

No shell MinGW64 do MSYS2, na raiz do projeto:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Alternativa sem Ninja:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
```

## Compilar

```sh
cmake --build build
```

Isso gera:

- `build/clima_viewer.exe` — visualizador Qt (lê `clima.db` do diretório atual)
- `build/download.exe` — downloader (usa `download.ini` do diretório atual)

## Executar

Clima viewer:

```sh
./build/clima_viewer.exe
```

Downloader (precisa de `download.ini` na raiz do projeto):

```sh
./build/download.exe ./download.ini
```

Test (precisa de `download.ini` na raiz do projeto):

```sh
./build/clima_auditor.exe
```

No PowerShell/VS Code (raiz do projeto):

```powershell
.\build\download.exe .\download.ini
.\build\clima_viewer.exe   # execute a partir de app\ para achar clima.db
```

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

### Formas de execução do Downloader (`download.exe`):

O `download.exe` é totalmente universal e controlado pelo `download.ini` ou arquivo `.env`. Ele suporta baixar datasets salvando tanto em banco de dados SQLite (`tipo_saida = db`) quanto diretamente em arquivos organizados por pasta (`tipo_saida = arquivo`, como no caso de dados **ERA5** da Copernicus CDS API).

#### 1. Executar todos os datasets do `download.ini`
```cmd
./download.exe
```
ou especificando o arquivo `.ini`:
```cmd
./download.exe download.ini
```

#### 2. Executar apenas uma seção/dataset específico (ex: `[dataset:era5]`)
Você pode passar diretamente o nome da seção do dataset (com ou sem o prefixo `dataset:`):
```cmd
./download.exe era5
```
```cmd
./download.exe dataset:era5
```
Com o caminho do arquivo `.ini` explícito:
```cmd
./download.exe download.ini era5
```

#### 3. Usar flags de seleção de dataset (`-d`, `--dataset`, `-s`, `--secao`)
```cmd
./download.exe -d era5
```
```cmd
./download.exe download.ini --dataset era5
```

#### 4. Baixar múltiplos datasets específicos
Você pode especificar múltiplos datasets separados por vírgula ou por argumentos distintos:
```cmd
./download.exe era5,clima_diario
```
```cmd
./download.exe download.ini era5 clima_diario
```
```cmd
./download.exe -d era5 -d clima_diario
```

#### 5. Modos de Saída (Definidos no `download.ini`)
- **`tipo_saida = db`** (padrão): Descobre dinamicamente os campos JSON e popula tabelas no banco de dados SQLite (`clima.db`).
- **`tipo_saida = arquivo`** (ex: `[dataset:era5]`): Baixa diretamente os arquivos (GRIB / NetCDF) para a pasta informada (`pasta_saida = dados_era5`), sem criar tabelas `.db`.

Test (precisa de `download.ini` na raiz do projeto):

```sh
./build/clima_auditor.exe
```

No PowerShell/VS Code (raiz do projeto):

```powershell
.\download.exe era5
.\build\clima_viewer.exe   # execute a partir de app\ para achar clima.db
```


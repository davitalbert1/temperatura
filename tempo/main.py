import requests
import sqlite3
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta

# CONFIG
LOCAL = "Joinville"
DATAS = ["2020-05-01", "2024-05-02"]  # ou None
DB_NAME = "clima.db"

# GEOLOCALIZAÇÃO
def geocodificar(local):
    url = "https://geocoding-api.open-meteo.com/v1/search"
    params = {
        "name": local,
        "count": 1,
        "language": "pt",
        "format": "json"
    }

    r = requests.get(url, params=params)
    data = r.json()

    if "results" not in data: raise Exception("Local não encontrado")

    resultado = data["results"][0]
    return resultado["latitude"], resultado["longitude"]

# BANCO
def criar_banco():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute("PRAGMA journal_mode=WAL;")
    cursor.execute("PRAGMA synchronous=NORMAL;")
    cursor.execute("PRAGMA foreign_keys=ON;")
    cursor.execute("PRAGMA busy_timeout=30000;")

    # HOURLY
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_hourly (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            local TEXT,
            data_hora TEXT UNIQUE,

            temperatura REAL,
            umidade REAL,
            sensacao REAL,

            precipitacao REAL,
            chuva REAL,
            pancadas REAL,
            neve REAL,

            ponto_orvalho REAL,
            pressao_msl REAL,
            pressao_superficie REAL,

            nuvens REAL,
            radiacao REAL,
            evapotranspiracao REAL,
            uv REAL
        )
    """)

    # DAILY
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_daily (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            local TEXT,
            data TEXT UNIQUE,
            nascer_sol TEXT,
            por_sol TEXT
        )
    """)

    conn.commit()
    conn.close()

def dividir_intervalo(inicio, fim, dias_por_lote=7):
    inicio = datetime.fromisoformat(inicio)
    fim = datetime.fromisoformat(fim)

    atual = inicio
    intervalos = []

    while atual <= fim:
        final_lote = min(atual + timedelta(days=dias_por_lote - 1), fim)
        intervalos.append((
            atual.strftime("%Y-%m-%d"),
            final_lote.strftime("%Y-%m-%d")
        ))
        atual = final_lote + timedelta(days=1)

    return intervalos

def salvar_hourly(local, dados, batch_size=1):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    registros = []
    total = len(dados["time"])
    inseridos = 0

    for i in range(total):
        registros.append((
            local,
            dados["time"][i],

            dados.get("temperature_2m", [None])[i],
            dados.get("relative_humidity_2m", [None])[i],
            dados.get("apparent_temperature", [None])[i],

            dados.get("precipitation", [None])[i],
            dados.get("rain", [None])[i],
            dados.get("showers", [None])[i],
            dados.get("snowfall", [None])[i],

            dados.get("dewpoint_2m", [None])[i],
            dados.get("pressure_msl", [None])[i],
            dados.get("surface_pressure", [None])[i],

            dados.get("cloudcover", [None])[i],
            dados.get("shortwave_radiation", [None])[i],
            dados.get("et0_fao_evapotranspiration", [None])[i],
            dados.get("uv_index", [None])[i]
        ))

        if len(registros) >= batch_size:
            cursor.executemany("""
                INSERT OR IGNORE INTO clima_hourly VALUES (
                    NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
                )
            """, registros)

            conn.commit()

            inseridos += len(registros)
            print(f"[BATCH] Inseridos {inseridos}/{total}")

            registros.clear()

    # último lote
    if registros:
        cursor.executemany("""
            INSERT OR IGNORE INTO clima_hourly VALUES (
                NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
            )
        """, registros)

        conn.commit()

        inseridos += len(registros)
        print(f"[FINAL] Inseridos {inseridos}/{total}")

    conn.close()

def salvar_daily(local, dados):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    for i in range(len(dados["time"])):
        cursor.execute("""
            INSERT OR IGNORE INTO clima_daily VALUES (
                NULL, ?, ?, ?, ?
            )
        """, (
            local,
            dados["time"][i],
            dados.get("sunrise", [None])[i],
            dados.get("sunset", [None])[i]
        ))

    conn.commit()
    conn.close()

def processar_em_lotes(lat, lon, inicio, fim, lote_threads=3):
    intervalos = dividir_intervalo(inicio, fim, dias_por_lote=7)

    print(f"Total de intervalos: {len(intervalos)}")

    for i in range(0, len(intervalos), lote_threads):
        grupo = intervalos[i:i + lote_threads]

        print(f"\n[LOTE] {i} até {i + len(grupo)}")

        resultados = []

        with ThreadPoolExecutor(max_workers=lote_threads) as executor:
            futures = [
                executor.submit(obter_historico, lat, lon, ini, fim)
                for ini, fim in grupo
            ]

            for future in as_completed(futures):
                try:
                    resultados.append(future.result())
                    print("[OK] resposta recebida")
                except Exception as e:
                    print("Erro:", e)

        for json_data in resultados:
            if "hourly" in json_data: salvar_hourly(LOCAL, json_data["hourly"])
            if "daily" in json_data: salvar_daily(LOCAL, json_data["daily"])

        print("[COMMIT LOTE] concluído")

# BUSCAR HISTÓRICO
def obter_historico(lat, lon, data_inicio, data_fim):
    url = "https://archive-api.open-meteo.com/v1/archive"

    params = {
        "latitude": lat,
        "longitude": lon,
        "start_date": data_inicio,
        "end_date": data_fim,
        "hourly": ",".join([
            "temperature_2m",
            "relative_humidity_2m",
            "apparent_temperature",
            "precipitation",
            "rain",
            "showers",
            "snowfall",
            "dewpoint_2m",
            "pressure_msl",
            "surface_pressure",
            "cloudcover",
            "shortwave_radiation",
            "et0_fao_evapotranspiration",
            "uv_index"
        ]),
        "daily": "sunrise,sunset",
        "timezone": "America/Sao_Paulo"
    }

    r = requests.get(url, params=params)
    r.raise_for_status()
    return r.json()

# NORMALIZAR DATAS
def normalizar_datas(datas):
    if datas is None:
        hoje = datetime.now().strftime("%Y-%m-%d")
        return hoje, hoje

    if isinstance(datas, str): return datas, datas

    return min(datas), max(datas)

# MAIN
def main():
    criar_banco()

    lat, lon = geocodificar(LOCAL)
    data_inicio, data_fim = normalizar_datas(DATAS)

    print("Buscando em paralelo...")

    processar_em_lotes(lat, lon, data_inicio, data_fim)

    print("✔ Dados salvos com sucesso")

if __name__ == "__main__":
    main()
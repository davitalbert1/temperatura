import os
import random
import sqlite3
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timedelta
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

DB_PATH = "clima.db"
MAX_SQL_DATES_POR_LOTE = 900
MAX_WORKERS = int(os.environ.get("OPENMETEO_WORKERS", "2"))
CHUNK_DIAS = int(os.environ.get("OPENMETEO_CHUNK_DIAS", "30"))
REQUESTS_PER_SECOND = float(os.environ.get("OPENMETEO_RPS", "4"))
MAX_RETRIES = int(os.environ.get("OPENMETEO_RETRIES", "8"))
HTTP_TIMEOUT = float(os.environ.get("OPENMETEO_TIMEOUT", "120"))
JITTER_INICIAL_MAX = float(os.environ.get("OPENMETEO_JITTER", "5.0"))
BACKOFF_INICIAL = float(os.environ.get("OPENMETEO_BACKOFF", "2.0"))
BACKOFF_MAXIMO = float(os.environ.get("OPENMETEO_BACKOFF_MAX", "60.0"))
MIN_RPS = float(os.environ.get("OPENMETEO_MIN_RPS", "0.5"))
COOLDOWN_429 = float(os.environ.get("OPENMETEO_COOLDOWN_429", "30.0"))
CIRCUIT_LIMIT_429 = int(os.environ.get("OPENMETEO_429_CIRCUIT_LIMIT", "3"))
ESPERA_MIN_429 = float(os.environ.get("OPENMETEO_429_ESPERA_MIN", "5.0"))

# Coordenadas geográficas [latitude, longitude].
COORDENADAS = [
    [-23.5505, -46.6333], # São Paulo, Brasil
    [-26.3044, -48.8456], # Joinville, Brasil
    [-25.4278, -49.2731], # Curitiba, Brasil
    [-28.7833, -51.6100], # Nova Prata, Brasil
    [-20.4697, -54.6201], # Campo Grande, Brasil
    [-3.1019, -60.0250], # Manaus, Brasil
    [-3.7250, -38.5236], # Fortaleza, Brasil
    [52.5200, 13.4050], # Berlim, Alemanha
    [35.6762, 139.6503], # Tóquio, Japão
    [40.7128, -74.0060], # Nova York, EUA
    [55.7558, 37.6173], # Moscou, Rússia
    [43.1155, 131.8855], # Vladivostok, Rússia
    [-33.4489, -70.6693], # Santiago, Chile
    [-54.8019, -68.3030], # Ushuaia, Argentina
    [-33.9249, 18.4241], # Cidade do Cabo, África do Sul
    [19.4326, -99.1332], # Cidade do México, México
    [61.2181, -149.9003], # Anchorage, EUA (Alasca)
]
FUSO_HORARIO = "America/Sao_Paulo"

PERIODO_DOWNLOAD = [
    "2000-01-01",
    "2024-01-05",
]

URL_FORECAST = "https://api.open-meteo.com/v1/forecast"
URL_ARCHIVE = "https://archive-api.open-meteo.com/v1/archive"
URL_AIR_QUALITY = "https://air-quality-api.open-meteo.com/v1/air-quality"
URL_POLLEN = "https://api.open-meteo.com/v1/pollen"

DAILY_PARAMS = [
    "weathercode",
    "temperature_2m_max",
    "temperature_2m_min",
    "apparent_temperature_max",
    "apparent_temperature_min",
    "relativehumidity_2m_max",
    "relativehumidity_2m_min",
    "precipitation_sum",
    "precipitation_probability_max",
    "rain_sum",
    "showers_sum",
    "snowfall_sum",
    "sunrise",
    "sunset",
    "moonrise",
    "moonset",
    "uv_index_max",
    "uv_index_clear_sky_max",
    "windspeed_10m_max",
    "winddirection_10m_dominant",
    "windgusts_10m_max",
    "pressure_msl_max",
    "pressure_msl_min",
    "surface_pressure_max",
    "surface_pressure_min",
]

HOURLY_PARAMS = [
    "temperature_2m",
    "apparent_temperature",
    "relativehumidity_2m",
    "dewpoint_2m",
    "precipitation",
    "precipitation_probability",
    "rain",
    "showers",
    "snowfall",
    "snow_depth",
    "uv_index",
    "uv_index_clear_sky",
    "windspeed_10m",
    "winddirection_10m",
    "windgusts_10m",
    "pressure_msl",
    "surface_pressure",
    "cloudcover",
    "cloudcover_low",
    "cloudcover_mid",
    "cloudcover_high",
    "visibility",
    "is_day",
    "weathercode",
]

DAILY_PARAMS_NAO_SUPORTADOS_NO_ARQUIVO = {
    "relativehumidity_2m_max",
    "relativehumidity_2m_min",
    "moonrise",
    "moonset",
}

AIR_QUALITY_HOURLY = [
    "pm10",
    "pm2_5",
    "carbon_monoxide",
    "nitrogen_dioxide",
    "sulphur_dioxide",
    "ozone",
    "dust",
    "formaldehyde",
]

DATA_MINIMA_QUALIDADE_AR = "2013-01-01"

POLLEN_DAILY = [
    "alder_pollen_mean",
    "birch_pollen_mean",
    "olive_pollen_mean",
    "ragweed_pollen_mean",
    "grass_pollen_mean",
]

# Infraestrutura de concorrência
local = threading.local()
write_lock = threading.Lock()
print_lock = threading.Lock()

# Token-bucket com adaptação automática: reduz a taxa ao receber 429
_rate_lock = threading.Lock()
_tokens = REQUESTS_PER_SECOND # tokens disponíveis
_rps_atual = REQUESTS_PER_SECOND  # taxa vigente (auto-ajustada)
_ultimo_refil = time.time()
_429_consecutivos = 0
_cooldown_ate = 0.0 # circuit-breaker global (epoch)

def _proxima_espera():
    global _tokens, _ultimo_refil
    agora = time.time()

    # Circuit-breaker: se estamos em cooldown global, todo mundo espera.
    if agora < _cooldown_ate:
        return _cooldown_ate - agora

    # Refil contínuo de tokens (1 token = 1 requisição permitida).
    _tokens = min(_rps_atual, _tokens + (agora - _ultimo_refil) * _rps_atual)
    _ultimo_refil = agora

    if _tokens >= 1.0:
        _tokens -= 1.0
        return 0.0

    return (1.0 - _tokens) / _rps_atual

def _esperar_rate_limit():
    with _rate_lock: espera = _proxima_espera()
    if espera > 0: time.sleep(espera)

def _reduzir_taxa_429():
    global _rps_atual, _429_consecutivos, _cooldown_ate
    with _rate_lock:
        _429_consecutivos += 1
        _rps_atual = max(MIN_RPS, _rps_atual * 0.5)
        _tokens = min(_tokens, _rps_atual)
        if _429_consecutivos >= CIRCUIT_LIMIT_429:
            # Dispara cooldown global para a API se recuperar.
            _cooldown_ate = time.time() + COOLDOWN_429
            log(f"  [429] Muitas respostas 429 seguidas. "
                f"Ativando pausa global de {COOLDOWN_429:.0f}s "
                f"(taxa reduzida para {_rps_atual:.2f} req/s).")
            _429_consecutivos = 0

def _restaurar_taxa():
    global _rps_atual, _429_consecutivos
    with _rate_lock:
        _429_consecutivos = 0
        if _rps_atual < REQUESTS_PER_SECOND:
            # Sobe 10% por sucesso, até o máximo configurado.
            _rps_atual = min(REQUESTS_PER_SECOND, _rps_atual * 1.1)

def _esperar_cooldown_global():
    """Se estivermos em cooldown global, dorme até o fim dele."""
    with _rate_lock:
        espera = _cooldown_ate - time.time()
    if espera > 0:
        log(f"  [PAUSA GLOBAL] Aguardando {espera:.1f}s (cooldown 429)...")
        time.sleep(espera)

def get_connection():
    if not hasattr(local, "conn"):
        conn = sqlite3.connect(DB_PATH, timeout=60)
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute("PRAGMA busy_timeout=60000")
        local.conn = conn
    return local.conn

def get_session():
    if not hasattr(local, "session"):
        session = requests.Session()
        retry = Retry(
            total=3, connect=3, read=3, status=0,
            backoff_factor=0.5, status_forcelist=[],
            respect_retry_after_header=False,
        )
        adapter = HTTPAdapter(max_retries=retry, pool_connections=MAX_WORKERS + 2, pool_maxsize=MAX_WORKERS + 2)
        session.mount("https://", adapter)
        session.mount("http://", adapter)
        local.session = session
    return local.session

def log(msg):
    with print_lock: print(msg, flush=True)

def init_database():
    conn = get_connection()
    cursor = conn.cursor()

    # Migração
    cursor.execute("PRAGMA table_info(clima_horario)")
    colunas_horario = [col[1] for col in cursor.fetchall()]
    if colunas_horario and "latitude" not in colunas_horario:
        log("[DB] Esquema antigo detectado. Recriando tabelas...")
        cursor.execute("DROP TABLE IF EXISTS clima_diario")
        cursor.execute("DROP TABLE IF EXISTS clima_horario")
        cursor.execute("DROP TABLE IF EXISTS qualidade_ar")
        cursor.execute("DROP TABLE IF EXISTS polen")

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_diario (
            data TEXT NOT NULL, latitude REAL NOT NULL, longitude REAL NOT NULL,
            timezone TEXT, weathercode INTEGER, temperatura_max REAL,
            temperatura_min REAL, sensacao_termica_max REAL, sensacao_termica_min REAL,
            umidade_max REAL, umidade_min REAL, precipitacao_total REAL,
            probabilidade_chuva REAL, chuva_total REAL, aguas_claras_total REAL,
            neve_total REAL, nascer_sol TEXT, por_sol TEXT,
            nascer_lua TEXT, por_lua TEXT, uv_max REAL, uv_clear_sky_max REAL,
            vento_max REAL, direcao_vento REAL, rajadas_vento REAL,
            pressao_max REAL, pressao_min REAL, pressao_superficie_max REAL,
            pressao_superficie_min REAL, created_at TEXT,
            PRIMARY KEY (data, latitude, longitude)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_horario (
            id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT NOT NULL, hora TEXT NOT NULL,
            latitude REAL NOT NULL, longitude REAL NOT NULL, temperatura REAL,
            sensacao_termica REAL, umidade REAL, ponto_orvalho REAL,
            precipitacao REAL, probabilidade_chuva REAL, chuva REAL,
            aguas_claras REAL, neve REAL, profundidade_neve REAL,
            uv_index REAL, uv_index_clear_sky REAL, vento REAL,
            direcao_vento REAL, rajadas_vento REAL, pressao REAL,
            pressao_superficie REAL, cobertura_nuvens REAL,
            cobertura_nuvens_baixa REAL, cobertura_nuvens_media REAL,
            cobertura_nuvens_alta REAL, visibilidade REAL, is_day INTEGER,
            weathercode INTEGER, created_at TEXT,
            UNIQUE (data, hora, latitude, longitude)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS qualidade_ar (
            id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT NOT NULL, hora TEXT NOT NULL,
            latitude REAL NOT NULL, longitude REAL NOT NULL, pm10 REAL, pm2_5 REAL,
            monoxido_carbono REAL, nitrogenio_dioxide REAL, enxofre_dioxide REAL,
            ozonio REAL, aerosois REAL, poeira REAL, amonio REAL, radon REAL,
            formaldeido REAL, mercurio REAL, created_at TEXT,
            UNIQUE (data, hora, latitude, longitude)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS polen (
            data TEXT NOT NULL, latitude REAL NOT NULL, longitude REAL NOT NULL,
            alder_pollen_mean REAL, birch_pollen_mean REAL, olive_pollen_mean REAL,
            ragweed_pollen_mean REAL, grass_pollen_mean REAL, created_at TEXT,
            PRIMARY KEY (data, latitude, longitude)
        )
    """)

    # Cria índices para otimizar consultas de verificação
    log("[DB] Criando índices para otimização...")
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_clima_diario_loc ON clima_diario(latitude, longitude, data)")
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_clima_horario_loc ON clima_horario(latitude, longitude, data)")
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_qualidade_ar_loc ON qualidade_ar(latitude, longitude, data)")
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_polen_loc ON polen(latitude, longitude, data)")

    conn.commit()
    log("[DB] Banco de dados inicializado com sucesso.")

def carregar_existentes(datas_necessarias, coordenadas):
    conn = get_connection()
    cursor = conn.cursor()

    existentes = {
        "diario": {},
        "horario": {},
        "qualidade_ar": {},
        "polen": {},
    }

    # Para cada tabela, carrega apenas as combinações (lat, lon, data) que vamos precisar
    tabelas = {
        "diario": "clima_diario",
        "horario": "clima_horario",
        "qualidade_ar": "qualidade_ar",
        "polen": "polen",
    }

    for tipo, tabela in tabelas.items():
        # Filtra datas por tipo
        if tipo == "qualidade_ar":
            datas_filtradas = [d for d in datas_necessarias if d >= DATA_MINIMA_QUALIDADE_AR]
        elif tipo == "polen":
            hoje = datetime.now().date()
            datas_filtradas = [d for d in datas_necessarias
                             if datetime.strptime(d, "%Y-%m-%d").date() >= hoje]
        else:
            datas_filtradas = datas_necessarias

        if not datas_filtradas:
            continue

        # Carrega em lotes por coordenada, quebrando as datas em blocos de
        # até MAX_SQL_DATES_POR_LOTE (evita o limite de variáveis do SQLite).
        for lat, lon in coordenadas:
            chave_base = (float(lat), float(lon))
            if chave_base not in existentes[tipo]:
                existentes[tipo][chave_base] = set()

            for i in range(0, len(datas_filtradas), MAX_SQL_DATES_POR_LOTE):
                lote = datas_filtradas[i:i + MAX_SQL_DATES_POR_LOTE]
                placeholders = ",".join(["?" for _ in lote])
                query = f"""
                    SELECT data FROM {tabela}
                    WHERE latitude = ? AND longitude = ? AND data IN ({placeholders})
                """
                params = [lat, lon] + lote
                cursor.execute(query, params)
                existentes[tipo][chave_base].update(row[0] for row in cursor.fetchall())

    return existentes

# FUNÇÕES AUXILIARES
def get_url_clima(data):
    try:
        data_dt = datetime.strptime(data, "%Y-%m-%d").date()
        hoje = datetime.now().date()
        if data_dt < hoje: return URL_ARCHIVE
        return URL_FORECAST
    except ValueError:
        return URL_FORECAST

def _get_valor(dados, chave, indice):
    lista = dados.get(chave)
    if not lista or indice >= len(lista): return None
    return lista[indice]

def baixar_com_retry(url, params, session, max_tentativas=MAX_RETRIES):
    for tentativa in range(1, max_tentativas + 1):
        # Se um cooldown global estiver ativo, todo mundo espera (circuit-breaker).
        _esperar_cooldown_global()
        _esperar_rate_limit()
        try:
            response = session.get(url, params=params, timeout=HTTP_TIMEOUT)
            response.raise_for_status()
            _restaurar_taxa()
            return response.json()
        except requests.exceptions.HTTPError as e:
            status = getattr(e.response, "status_code", None)
            if status == 429:
                # Reduz a taxa global e dispara cooldown se necessário.
                _reduzir_taxa_429()
                retry_after = None
                try:
                    retry_after = float(e.response.headers.get("Retry-After", ""))
                except (TypeError, ValueError):
                    retry_after = None
                if retry_after is None:
                    retry_after = min(
                        BACKOFF_MAXIMO,
                        (BACKOFF_INICIAL * (2 ** (tentativa - 1))) * random.uniform(0.5, 1.5)
                    )
                # Garante um mínimo de espera para a API se recuperar.
                espera = max(ESPERA_MIN_429, retry_after)
                if tentativa == max_tentativas:
                    log(f"  [ERRO] 429 persistente após {max_tentativas} tentativas em {url}")
                    return None
                log(f"  [RETRY {tentativa}/{max_tentativas}] HTTP 429. Aguardando {espera:.1f}s...")
                time.sleep(espera)
            else:
                if tentativa == max_tentativas:
                    log(f"  [ERRO] Falha após {max_tentativas} tentativas em {url}: {e}")
                    return None
                espera = min(BACKOFF_MAXIMO, (BACKOFF_INICIAL * (2 ** (tentativa - 1))) * random.uniform(0.5, 1.5))
                log(f"  [RETRY {tentativa}/{max_tentativas}] HTTP {status}: {e}. Aguardando {espera:.1f}s...")
                time.sleep(espera)
        except (requests.exceptions.Timeout, requests.exceptions.ConnectionError) as e:
            if tentativa == max_tentativas:
                log(f"  [ERRO] Falha após {max_tentativas} tentativas em {url}: {e}")
                return None
            espera = min(BACKOFF_MAXIMO, (BACKOFF_INICIAL * (2 ** tentativa)) * random.uniform(0.5, 1.5))
            log(f"  [RETRY {tentativa}/{max_tentativas}] {type(e).__name__}. Aguardando {espera:.1f}s...")
            time.sleep(espera)
    return None

def agrupar_por_url(datas, chunk=CHUNK_DIAS):
    grupos = []
    for url in (URL_ARCHIVE, URL_FORECAST):
        lista = sorted(d for d in datas if get_url_clima(d) == url)
        for i in range(0, len(lista), chunk):
            grupos.append((url, lista[i:i + chunk]))
    return grupos

# PARSING
def parse_diario(daily, lat, lon, timezone, datas_filtro=None):
    filtro = set(datas_filtro) if datas_filtro else None
    times = daily.get("time", [])
    registros = []
    for i, data in enumerate(times):
        if filtro and data not in filtro: continue
        registros.append({
            "data": data, "latitude": lat, "longitude": lon, "timezone": timezone,
            "weathercode": _get_valor(daily, "weathercode", i),
            "temperatura_max": _get_valor(daily, "temperature_2m_max", i),
            "temperatura_min": _get_valor(daily, "temperature_2m_min", i),
            "sensacao_termica_max": _get_valor(daily, "apparent_temperature_max", i),
            "sensacao_termica_min": _get_valor(daily, "apparent_temperature_min", i),
            "umidade_max": _get_valor(daily, "relativehumidity_2m_max", i),
            "umidade_min": _get_valor(daily, "relativehumidity_2m_min", i),
            "precipitacao_total": _get_valor(daily, "precipitation_sum", i),
            "probabilidade_chuva": _get_valor(daily, "precipitation_probability_max", i),
            "chuva_total": _get_valor(daily, "rain_sum", i),
            "aguas_claras_total": _get_valor(daily, "showers_sum", i),
            "neve_total": _get_valor(daily, "snowfall_sum", i),
            "nascer_sol": _get_valor(daily, "sunrise", i),
            "por_sol": _get_valor(daily, "sunset", i),
            "nascer_lua": _get_valor(daily, "moonrise", i),
            "por_lua": _get_valor(daily, "moonset", i),
            "uv_max": _get_valor(daily, "uv_index_max", i),
            "uv_clear_sky_max": _get_valor(daily, "uv_index_clear_sky_max", i),
            "vento_max": _get_valor(daily, "windspeed_10m_max", i),
            "direcao_vento": _get_valor(daily, "winddirection_10m_dominant", i),
            "rajadas_vento": _get_valor(daily, "windgusts_10m_max", i),
            "pressao_max": _get_valor(daily, "pressure_msl_max", i),
            "pressao_min": _get_valor(daily, "pressure_msl_min", i),
            "pressao_superficie_max": _get_valor(daily, "surface_pressure_max", i),
            "pressao_superficie_min": _get_valor(daily, "surface_pressure_min", i),
        })
    return registros

def parse_horario(hourly, lat, lon, datas_filtro=None):
    filtro = set(datas_filtro) if datas_filtro else None
    times = hourly.get("time", [])
    registros = []
    for i, hora_str in enumerate(times):
        data = hora_str[:10]
        if filtro and data not in filtro: continue
        registros.append({
            "data": data, "hora": hora_str, "latitude": lat, "longitude": lon,
            "temperatura": _get_valor(hourly, "temperature_2m", i),
            "sensacao_termica": _get_valor(hourly, "apparent_temperature", i),
            "umidade": _get_valor(hourly, "relativehumidity_2m", i),
            "ponto_orvalho": _get_valor(hourly, "dewpoint_2m", i),
            "precipitacao": _get_valor(hourly, "precipitation", i),
            "probabilidade_chuva": _get_valor(hourly, "precipitation_probability", i),
            "chuva": _get_valor(hourly, "rain", i),
            "aguas_claras": _get_valor(hourly, "showers", i),
            "neve": _get_valor(hourly, "snowfall", i),
            "profundidade_neve": _get_valor(hourly, "snow_depth", i),
            "uv_index": _get_valor(hourly, "uv_index", i),
            "uv_index_clear_sky": _get_valor(hourly, "uv_index_clear_sky", i),
            "vento": _get_valor(hourly, "windspeed_10m", i),
            "direcao_vento": _get_valor(hourly, "winddirection_10m", i),
            "rajadas_vento": _get_valor(hourly, "windgusts_10m", i),
            "pressao": _get_valor(hourly, "pressure_msl", i),
            "pressao_superficie": _get_valor(hourly, "surface_pressure", i),
            "cobertura_nuvens": _get_valor(hourly, "cloudcover", i),
            "cobertura_nuvens_baixa": _get_valor(hourly, "cloudcover_low", i),
            "cobertura_nuvens_media": _get_valor(hourly, "cloudcover_mid", i),
            "cobertura_nuvens_alta": _get_valor(hourly, "cloudcover_high", i),
            "visibilidade": _get_valor(hourly, "visibility", i),
            "is_day": _get_valor(hourly, "is_day", i),
            "weathercode": _get_valor(hourly, "weathercode", i),
        })
    return registros

def parse_qualidade_ar(hourly, lat, lon, datas_filtro=None):
    filtro = set(datas_filtro) if datas_filtro else None
    times = hourly.get("time", [])
    registros = []
    for i, hora_str in enumerate(times):
        data = hora_str[:10]
        if filtro and data not in filtro: continue
        registros.append({
            "data": data, "hora": hora_str, "latitude": lat, "longitude": lon,
            "pm10": _get_valor(hourly, "pm10", i),
            "pm2_5": _get_valor(hourly, "pm2_5", i),
            "monoxido_carbono": _get_valor(hourly, "carbon_monoxide", i),
            "nitrogenio_dioxide": _get_valor(hourly, "nitrogen_dioxide", i),
            "enxofre_dioxide": _get_valor(hourly, "sulphur_dioxide", i),
            "ozonio": _get_valor(hourly, "ozone", i),
            "aerosois": _get_valor(hourly, "aerosol", i),
            "poeira": _get_valor(hourly, "dust", i),
            "amonio": _get_valor(hourly, "ammonium", i),
            "radon": _get_valor(hourly, "radon", i),
            "formaldeido": _get_valor(hourly, "formaldehyde", i),
            "mercurio": _get_valor(hourly, "mercury", i),
        })
    return registros

def parse_polen(daily, lat, lon, datas_filtro=None):
    filtro = set(datas_filtro) if datas_filtro else None
    times = daily.get("time", [])
    registros = []
    for i, data in enumerate(times):
        if filtro and data not in filtro: continue
        registros.append({
            "data": data, "latitude": lat, "longitude": lon,
            "alder_pollen_mean": _get_valor(daily, "alder_pollen_mean", i),
            "birch_pollen_mean": _get_valor(daily, "birch_pollen_mean", i),
            "olive_pollen_mean": _get_valor(daily, "olive_pollen_mean", i),
            "ragweed_pollen_mean": _get_valor(daily, "ragweed_pollen_mean", i),
            "grass_pollen_mean": _get_valor(daily, "grass_pollen_mean", i),
        })
    return registros

# ========== DOWNLOADS (mantidos iguais) ==========

def baixar_clima_bloco(datas, lat, lon, session):
    if not datas: return {"diario": [], "horario": []}
    diario, horario = [], []
    for url, grupo in agrupar_por_url(datas):
        inicio, fim = grupo[0], grupo[-1]
        if url == URL_ARCHIVE:
            daily_params = [p for p in DAILY_PARAMS if p not in DAILY_PARAMS_NAO_SUPORTADOS_NO_ARQUIVO]
        else:
            daily_params = DAILY_PARAMS

        params = {
            "latitude": lat, "longitude": lon,
            "start_date": inicio, "end_date": fim,
            "daily": ",".join(daily_params), "hourly": ",".join(HOURLY_PARAMS),
            "timezone": FUSO_HORARIO,
        }

        dados = baixar_com_retry(url, params, session)
        if not dados: continue

        if "daily" in dados and dados["daily"]:
            diario.extend(parse_diario(dados["daily"], lat, lon, dados.get("timezone"), grupo))
        if "hourly" in dados and dados["hourly"]:
            horario.extend(parse_horario(dados["hourly"], lat, lon, grupo))

    return {"diario": diario, "horario": horario}

def baixar_qualidade_ar_bloco(datas, lat, lon, session):
    datas = [d for d in datas if d >= DATA_MINIMA_QUALIDADE_AR]
    if not datas: return []
    inicio, fim = datas[0], datas[-1]
    params = {
        "latitude": lat, "longitude": lon,
        "start_date": inicio, "end_date": fim,
        "hourly": ",".join(AIR_QUALITY_HOURLY), "timezone": FUSO_HORARIO,
    }
    dados = baixar_com_retry(URL_AIR_QUALITY, params, session)
    if not dados or "hourly" not in dados: return []
    return parse_qualidade_ar(dados["hourly"], lat, lon, datas)

def baixar_polen_bloco(datas, lat, lon, session):
    hoje = datetime.now().date()
    datas = [d for d in datas if datetime.strptime(d, "%Y-%m-%d").date() >= hoje]
    if not datas: return []
    inicio, fim = datas[0], datas[-1]
    params = {
        "latitude": lat, "longitude": lon,
        "start_date": inicio, "end_date": fim,
        "daily": ",".join(POLLEN_DAILY), "timezone": FUSO_HORARIO,
    }
    dados = baixar_com_retry(URL_POLLEN, params, session)
    if not dados or "daily" not in dados: return []
    return parse_polen(dados["daily"], lat, lon, datas)

def salvar_dados_diarios(registros):
    if not registros: return
    conn = get_connection()
    cursor = conn.cursor()
    agora = datetime.now().isoformat()
    cursor.executemany("""
        INSERT OR REPLACE INTO clima_diario (
            data, latitude, longitude, timezone, weathercode,
            temperatura_max, temperatura_min, sensacao_termica_max, sensacao_termica_min,
            umidade_max, umidade_min, precipitacao_total, probabilidade_chuva,
            chuva_total, aguas_claras_total, neve_total, nascer_sol, por_sol,
            nascer_lua, por_lua, uv_max, uv_clear_sky_max, vento_max,
            direcao_vento, rajadas_vento, pressao_max, pressao_min,
            pressao_superficie_max, pressao_superficie_min, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, [
        (r["data"], r["latitude"], r["longitude"], r["timezone"],
         r["weathercode"], r["temperatura_max"], r["temperatura_min"],
         r["sensacao_termica_max"], r["sensacao_termica_min"],
         r["umidade_max"], r["umidade_min"], r["precipitacao_total"],
         r["probabilidade_chuva"], r["chuva_total"], r["aguas_claras_total"],
         r["neve_total"], r["nascer_sol"], r["por_sol"],
         r["nascer_lua"], r["por_lua"], r["uv_max"], r["uv_clear_sky_max"],
         r["vento_max"], r["direcao_vento"], r["rajadas_vento"],
         r["pressao_max"], r["pressao_min"], r["pressao_superficie_max"],
         r["pressao_superficie_min"], agora) for r in registros
    ])
    conn.commit()

def salvar_dados_horarios(registros):
    if not registros: return
    conn = get_connection()
    cursor = conn.cursor()
    agora = datetime.now().isoformat()
    cursor.executemany("""
        INSERT OR REPLACE INTO clima_horario (
            data, hora, latitude, longitude, temperatura, sensacao_termica, umidade,
            ponto_orvalho, precipitacao, probabilidade_chuva, chuva, aguas_claras,
            neve, profundidade_neve, uv_index, uv_index_clear_sky, vento,
            direcao_vento, rajadas_vento, pressao, pressao_superficie,
            cobertura_nuvens, cobertura_nuvens_baixa, cobertura_nuvens_media,
            cobertura_nuvens_alta, visibilidade, is_day, weathercode, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, [
        (h["data"], h["hora"], h["latitude"], h["longitude"],
         h["temperatura"], h["sensacao_termica"], h["umidade"], h["ponto_orvalho"],
         h["precipitacao"], h["probabilidade_chuva"], h["chuva"], h["aguas_claras"],
         h["neve"], h["profundidade_neve"], h["uv_index"], h["uv_index_clear_sky"],
         h["vento"], h["direcao_vento"], h["rajadas_vento"], h["pressao"],
         h["pressao_superficie"], h["cobertura_nuvens"], h["cobertura_nuvens_baixa"],
         h["cobertura_nuvens_media"], h["cobertura_nuvens_alta"], h["visibilidade"],
         h["is_day"], h["weathercode"], agora) for h in registros
    ])
    conn.commit()

def salvar_qualidade_ar(registros):
    if not registros: return
    conn = get_connection()
    cursor = conn.cursor()
    agora = datetime.now().isoformat()
    cursor.executemany("""
        INSERT OR REPLACE INTO qualidade_ar (
            data, hora, latitude, longitude, pm10, pm2_5, monoxido_carbono,
            nitrogenio_dioxide, enxofre_dioxide, ozonio, aerosois, poeira,
            amonio, radon, formaldeido, mercurio, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, [
        (d["data"], d["hora"], d["latitude"], d["longitude"],
         d["pm10"], d["pm2_5"], d["monoxido_carbono"], d["nitrogenio_dioxide"],
         d.get("enxofre_dioxide"), d["ozonio"], d.get("aerosois"), d["poeira"],
         d.get("amonio"), d.get("radon"), d["formaldeido"], d.get("mercurio"), agora)
        for d in registros
    ])
    conn.commit()

def salvar_dados_polen(registros):
    if not registros: return
    conn = get_connection()
    cursor = conn.cursor()
    agora = datetime.now().isoformat()
    cursor.executemany("""
        INSERT OR REPLACE INTO polen (
            data, latitude, longitude, alder_pollen_mean, birch_pollen_mean,
            olive_pollen_mean, ragweed_pollen_mean, grass_pollen_mean, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, [
        (p["data"], p["latitude"], p["longitude"],
         p["alder_pollen_mean"], p["birch_pollen_mean"], p["olive_pollen_mean"],
         p["ragweed_pollen_mean"], p["grass_pollen_mean"], agora) for p in registros
    ])
    conn.commit()

def expandir_periodo(periodo):
    if len(periodo) == 2:
        try:
            data_inicio = datetime.strptime(periodo[0], "%Y-%m-%d")
            data_fim = datetime.strptime(periodo[1], "%Y-%m-%d")
            datas = []
            data_atual = data_inicio
            while data_atual <= data_fim:
                datas.append(data_atual.strftime("%Y-%m-%d"))
                data_atual += timedelta(days=1)
            return datas
        except ValueError as e:
            print(f"[AVISO] Erro ao processar datas do período: {e}")
            return periodo
    return periodo

def processar_bloco(datas, lat, lon, existentes, stats):
    time.sleep(random.uniform(0.0, JITTER_INICIAL_MAX))

    chave_base = (float(lat), float(lon))

    # NOVA ABORDAGEM: Verificação otimizada usando dicionário por coordenada
    datas_diario = [d for d in datas if d not in existentes["diario"].get(chave_base, set())]
    datas_horario = [d for d in datas if d not in existentes["horario"].get(chave_base, set())]
    datas_ar = [d for d in datas if d >= DATA_MINIMA_QUALIDADE_AR and d not in existentes["qualidade_ar"].get(chave_base, set())]
    hoje = datetime.now().date()
    datas_polen = [
        d for d in datas
        if datetime.strptime(d, "%Y-%m-%d").date() >= hoje
        and d not in existentes["polen"].get(chave_base, set())
    ]

    stats["pulado_diario"] += len(datas) - len(datas_diario)
    stats["pulado_horario"] += len(datas) - len(datas_horario)
    stats["pulado_ar"] += len(datas) - len(datas_ar) - sum(1 for d in datas if d < DATA_MINIMA_QUALIDADE_AR)
    stats["pulado_polen"] += len(datas) - len(datas_polen) - sum(1 for d in datas if datetime.strptime(d, "%Y-%m-%d").date() < hoje)

    session = get_session()

    # Clima (diário + horário na MESMA requisição)
    # A API só é chamada se HOUVER datas ainda não salvas para qualquer um dos dois.
    # Ao salvar, filtramos novamente para gravar somente as datas realmente faltantes,
    # evitando sobrescrever (INSERT OR REPLACE) dados de componentes já salvos.
    if datas_diario or datas_horario:
        alvo = sorted(set(datas_diario + datas_horario))
        resultado = baixar_clima_bloco(alvo, lat, lon, session)
        if resultado["diario"]:
            set_diario = set(datas_diario)
            diario_faltante = [r for r in resultado["diario"] if r["data"] in set_diario]
            if diario_faltante:
                with write_lock: salvar_dados_diarios(diario_faltante)
                existentes["diario"].setdefault(chave_base, set()).update(r["data"] for r in diario_faltante)
                stats["baixado_diario"] += len(diario_faltante)
        if resultado["horario"]:
            set_horario = set(datas_horario)
            horario_faltante = [r for r in resultado["horario"] if r["data"] in set_horario]
            if horario_faltante:
                with write_lock: salvar_dados_horarios(horario_faltante)
                existentes["horario"].setdefault(chave_base, set()).update(r["data"] for r in horario_faltante)
                stats["baixado_horario"] += len(horario_faltante)

    # Qualidade do ar
    if datas_ar:
        resultado_ar = baixar_qualidade_ar_bloco(datas_ar, lat, lon, session)
        if resultado_ar:
            with write_lock: salvar_qualidade_ar(resultado_ar)
            existentes["qualidade_ar"].setdefault(chave_base, set()).update(r["data"] for r in resultado_ar)
            stats["baixado_ar"] += len(resultado_ar)

    # Pólen
    if datas_polen:
        resultado_polen = baixar_polen_bloco(datas_polen, lat, lon, session)
        if resultado_polen:
            with write_lock: salvar_dados_polen(resultado_polen)
            existentes["polen"].setdefault(chave_base, set()).update(r["data"] for r in resultado_polen)
            stats["baixado_polen"] += len(resultado_polen)

def main():
    datas = expandir_periodo(PERIODO_DOWNLOAD)
    print("=" * 60)
    print("DOWNLOAD DE DADOS CLIMÁTICOS")
    print("=" * 60)
    print(f"Fuso horário: {FUSO_HORARIO}")
    print(f"Banco de dados: {DB_PATH}")
    print(f"Período: {datas[0]} a {datas[-1]}")
    print(f"Total de datas: {len(datas)}")
    print(f"Total de localizações: {len(COORDENADAS)}")
    print(f"Threads (MAX_WORKERS): {MAX_WORKERS}")
    print(f"Dias por requisição (CHUNK_DIAS): {CHUNK_DIAS}")
    print(f"Rate-limit global (RPS): {REQUESTS_PER_SECOND}")
    print(f"Timeout HTTP: {HTTP_TIMEOUT}s")
    print(f"Retries máximos: {MAX_RETRIES}")
    print("=" * 60)

    init_database()

    print("\n[DB] Carregando registros existentes...")
    inicio_carregamento = time.time()
    existentes = carregar_existentes(datas, COORDENADAS)
    tempo_carregamento = time.time() - inicio_carregamento

    total_diario = sum(len(v) for v in existentes["diario"].values())
    total_horario = sum(len(v) for v in existentes["horario"].values())
    total_ar = sum(len(v) for v in existentes["qualidade_ar"].values())
    total_polen = sum(len(v) for v in existentes["polen"].values())

    print(f"[DB] Carregado em {tempo_carregamento:.2f}s")
    print(f"[DB] Itens já existentes -> diário: {total_diario}, "
          f"horário: {total_horario}, "
          f"qualidade do ar: {total_ar}, "
          f"pólen: {total_polen}")

    # Cria as tarefas
    tarefas = []
    for lat, lon in COORDENADAS:
        for i in range(0, len(datas), CHUNK_DIAS):
            tarefas.append((datas[i:i + CHUNK_DIAS], lat, lon))

    stats = {
        "baixado_diario": 0,
        "baixado_horario": 0,
        "baixado_ar": 0,
        "baixado_polen": 0,
        "pulado_diario": 0,
        "pulado_horario": 0,
        "pulado_ar": 0,
        "pulado_polen": 0,
    }
    stats_lock = threading.Lock()
    concluidos = 0
    total = len(tarefas)
    inicio = time.time()

    def executar(tarefa):
        nonlocal concluidos
        datas_bloco, lat, lon = tarefa
        processar_bloco(datas_bloco, lat, lon, existentes, stats)
        with stats_lock:
            concluidos += 1
            pct = concluidos / total * 100
            decorrido = time.time() - inicio
            if concluidos % 5 == 0 or concluidos == total:
                print(f"\r[PROGRESSO] {concluidos}/{total} blocos ({pct:.1f}%) "
                      f"- {decorrido:.0f}s", end="", flush=True)

    print(f"\nIniciando {total} tarefas com {MAX_WORKERS} threads...\n")

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = [executor.submit(executar, t) for t in tarefas]
        for future in as_completed(futures): future.result()

    print()
    print("\n" + "=" * 60)
    print("DOWNLOAD CONCLUÍDO!")
    print("=" * 60)
    print(f"Tempo total: {time.time() - inicio:.1f}s")
    print(f"Tempo carregamento DB: {tempo_carregamento:.2f}s")
    print(f"Blocos processados: {concluidos}/{total}")
    print("\n--- RESUMO ---")
    print(f"  Diário: {stats['baixado_diario']} baixados | {stats['pulado_diario']} já existentes")
    print(f"  Horário: {stats['baixado_horario']} registros baixados | {stats['pulado_horario']} dias já existentes")
    print(f"  Qualidade do ar: {stats['baixado_ar']} registros baixados | {stats['pulado_ar']} dias já existentes")
    print(f"  Pólen: {stats['baixado_polen']} dias baixados | {stats['pulado_polen']} dias já existentes")
    print("=" * 60)

if __name__ == "__main__":
    main()
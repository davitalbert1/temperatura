import os
import random
import sqlite3
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from queue import Queue, Empty
from datetime import datetime, timedelta
from typing import Dict, List, Set, Tuple, Any, Optional
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

DB_PATH = "clima.db"
MAX_SQL_DATES_POR_LOTE = 900
DOWNLOAD_WORKERS = int(os.environ.get("OPENMETEO_DOWNLOAD_WORKERS", "5"))
PROCESS_WORKERS = int(os.environ.get("OPENMETEO_PROCESS_WORKERS", "2"))
CHUNK_DIAS = int(os.environ.get("OPENMETEO_CHUNK_DIAS", "30"))
REQUESTS_PER_SECOND = float(os.environ.get("OPENMETEO_RPS", "4"))
MAX_RETRIES = int(os.environ.get("OPENMETEO_RETRIES", "8"))
HTTP_TIMEOUT = float(os.environ.get("OPENMETEO_TIMEOUT", "120"))
JITTER_INICIAL_MAX = float(os.environ.get("OPENMETEO_JITTER", "5.0"))
BACKOFF_INICIAL = float(os.environ.get("OPENMETEO_BACKOFF", "2.0"))
BACKOFF_MAXIMO = float(os.environ.get("OPENMETEO_BACKOFF_MAX", "60.0"))
MIN_RPS = float(os.environ.get("OPENMETEO_MIN_RPS", "5"))
COOLDOWN_429 = float(os.environ.get("OPENMETEO_COOLDOWN_429", "30.0"))
CIRCUIT_LIMIT_429 = int(os.environ.get("OPENMETEO_429_CIRCUIT_LIMIT", "3"))
ESPERA_MIN_429 = float(os.environ.get("OPENMETEO_429_ESPERA_MIN", "5.0"))

# Coordenadas geográficas [latitude, longitude]
COORDENADAS = [
    [-23.5505, -46.6333], # São Paulo, Brasil
    [-26.3044, -48.8456], # Joinville, Brasil
    [-25.4278, -49.2731], # Curitiba, Brasil
    [-28.7833, -51.6100], # Guaporé, Brasil
    [-20.4697, -54.6201], # Campo Grande, Brasil
    [-3.1019, -60.0250], # Manaus, Brasil
    [-3.7250, -38.5236], # Fortaleza, Brasil
    [52.5200, 13.4050], # Berlim, Alemanha
    [35.6762, 139.6503], # Tóquio, Japão
    [40.7128, -74.0060], # Nova York, Estados Unidos
    [55.7558, 37.6173], # Moscou, Rússia
    [43.1155, 131.8855], # Vladivostok, Rússia
    [-33.4489, -70.6693], # Santiago, Chile
    [-54.8019, -68.3030], # Ushuaia, Argentina
    [-33.9249, 18.4241], # Cidade do Cabo, África do Sul
    [19.4326, -99.1332], # Cidade do México, México
    [61.2181, -149.9003], # Anchorage, Estados Unidos
]

FUSO_HORARIO = "America/Sao_Paulo"
PERIODO_DOWNLOAD = ["1940-01-01", "2025-12-31"]

# URLs das APIs
URL_FORECAST = "https://api.open-meteo.com/v1/forecast" # disponível: ~92 dias.
URL_ARCHIVE = "https://archive-api.open-meteo.com/v1/archive" # Com ERA5: desde 1940.
URL_AIR_QUALITY = "https://air-quality-api.open-meteo.com/v1/air-quality" # dados recentes com past_days.
URL_POLLEN = "https://api.open-meteo.com/v1/pollen" # - Sem histórico de longo prazo.

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

local = threading.local()
write_lock = threading.Lock()
print_lock = threading.Lock()

# Token-bucket com adaptação automática
_rate_lock = threading.Lock()
_tokens = REQUESTS_PER_SECOND
_rps_atual = REQUESTS_PER_SECOND
_ultimo_refil = time.time()
_429_consecutivos = 0
_cooldown_ate = 0.0

def _proxima_espera() -> float:
    global _tokens, _ultimo_refil
    agora = time.time()

    if agora < _cooldown_ate: return _cooldown_ate - agora

    _tokens = min(_rps_atual, _tokens + (agora - _ultimo_refil) * _rps_atual)
    _ultimo_refil = agora

    if _tokens >= 1.0:
        _tokens -= 1.0
        return 0.0

    return (1.0 - _tokens) / _rps_atual

def _esperar_rate_limit() -> None:
    with _rate_lock:
        espera = _proxima_espera()
    if espera > 0:
        time.sleep(espera)

def _reduzir_taxa_429() -> None:
    global _rps_atual, _429_consecutivos, _cooldown_ate, _tokens
    with _rate_lock:
        _429_consecutivos += 1
        _rps_atual = max(MIN_RPS, _rps_atual * 0.5)
        _tokens = min(_tokens, _rps_atual)
        if _429_consecutivos >= CIRCUIT_LIMIT_429:
            _cooldown_ate = time.time() + COOLDOWN_429
            log(f"  [429] Muitas respostas 429. Pausa global de {COOLDOWN_429:.0f}s "
                f"(taxa: {_rps_atual:.2f} req/s)")
            _429_consecutivos = 0

def _restaurar_taxa() -> None:
    global _rps_atual, _429_consecutivos
    with _rate_lock:
        _429_consecutivos = 0
        if _rps_atual < REQUESTS_PER_SECOND:
            _rps_atual = min(REQUESTS_PER_SECOND, _rps_atual * 1.1)

def _esperar_cooldown_global() -> None:
    with _rate_lock:
        espera = _cooldown_ate - time.time()
    if espera > 0:
        log(f"  [PAUSA GLOBAL] Aguardando {espera:.1f}s (cooldown 429)...")
        time.sleep(espera)

def get_connection() -> sqlite3.Connection:
    if not hasattr(local, "conn"):
        conn = sqlite3.connect(DB_PATH, timeout=60)
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute("PRAGMA busy_timeout=60000")
        local.conn = conn
    return local.conn

def get_session() -> requests.Session:
    if not hasattr(local, "session"):
        session = requests.Session()
        retry = Retry(
            total=3, connect=3, read=3, status=0,
            backoff_factor=0.5, status_forcelist=[],
            respect_retry_after_header=False,
        )
        adapter = HTTPAdapter(
            max_retries=retry,
            pool_connections=DOWNLOAD_WORKERS + 2,
            pool_maxsize=DOWNLOAD_WORKERS + 2
        )
        session.mount("https://", adapter)
        session.mount("http://", adapter)
        local.session = session
    return local.session

def log(msg: str) -> None:
    with print_lock: print(msg, flush=True)

def get_url_clima(data: str) -> str:
    try:
        data_dt = datetime.strptime(data, "%Y-%m-%d").date()
        return URL_ARCHIVE if data_dt < datetime.now().date() else URL_FORECAST
    except ValueError:
        return URL_FORECAST

def _get_valor(dados: Dict, chave: str, indice: int) -> Any:
    lista = dados.get(chave)
    if not lista or indice >= len(lista):
        return None
    return lista[indice]

def expandir_periodo(periodo: List[str]) -> List[str]:
    if len(periodo) == 2:
        try:
            inicio = datetime.strptime(periodo[0], "%Y-%m-%d")
            fim = datetime.strptime(periodo[1], "%Y-%m-%d")
            datas = []
            atual = inicio
            while atual <= fim:
                datas.append(atual.strftime("%Y-%m-%d"))
                atual += timedelta(days=1)
            return datas
        except ValueError as e:
            log(f"[AVISO] Erro ao processar datas: {e}")
    return periodo

def init_database() -> None:
    conn = get_connection()
    cursor = conn.cursor()

    # Migração: detecta schema antigo
    cursor.execute("PRAGMA table_info(clima_horario)")
    colunas = [col[1] for col in cursor.fetchall()]
    if colunas and "latitude" not in colunas:
        log("[DB] Schema antigo detectado. Recriando tabelas...")
        for tabela in ["clima_diario", "clima_horario", "qualidade_ar", "polen"]:
            cursor.execute(f"DROP TABLE IF EXISTS {tabela}")

    # Cria tabelas
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_diario (
            data TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            timezone TEXT,
            weathercode INTEGER,
            temperatura_max REAL,
            temperatura_min REAL,
            sensacao_termica_max REAL,
            sensacao_termica_min REAL,
            umidade_max REAL,
            umidade_min REAL,
            precipitacao_total REAL,
            probabilidade_chuva REAL,
            chuva_total REAL,
            aguas_claras_total REAL,
            neve_total REAL,
            nascer_sol TEXT,
            por_sol TEXT,
            uv_max REAL,
            uv_clear_sky_max REAL,
            vento_max REAL,
            direcao_vento REAL,
            rajadas_vento REAL,
            pressao_max REAL,
            pressao_min REAL,
            pressao_superficie_max REAL,
            pressao_superficie_min REAL,
            created_at TEXT, PRIMARY KEY (data, latitude, longitude)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_horario (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            data TEXT NOT NULL,
            hora TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            temperatura REAL,
            sensacao_termica REAL,
            umidade REAL,
            ponto_orvalho REAL,
            precipitacao REAL,
            probabilidade_chuva REAL,
            chuva REAL,
            aguas_claras REAL,
            neve REAL,
            profundidade_neve REAL,
            uv_index REAL,
            uv_index_clear_sky REAL,
            vento REAL,
            direcao_vento REAL,
            rajadas_vento REAL,
            pressao REAL,
            pressao_superficie REAL,
            cobertura_nuvens REAL,
            cobertura_nuvens_baixa REAL,
            cobertura_nuvens_media REAL,
            cobertura_nuvens_alta REAL,
            visibilidade REAL,
            is_day INTEGER,
            weathercode INTEGER,
            created_at TEXT,
            UNIQUE (data, hora, latitude, longitude)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS qualidade_ar (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            data TEXT NOT NULL,
            hora TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            pm10 REAL, pm2_5 REAL,
            monoxido_carbono REAL,
            nitrogenio_dioxide REAL,
            enxofre_dioxide REAL,
            ozonio REAL,
            aerosois REAL,
            poeira REAL,
            formaldeido REAL,
            created_at TEXT, UNIQUE (data, hora, latitude, longitude)
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS polen (
            data TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            alder_pollen_mean REAL,
            birch_pollen_mean REAL,
            olive_pollen_mean REAL,
            ragweed_pollen_mean REAL,
            grass_pollen_mean REAL,
            created_at TEXT,
            PRIMARY KEY (data, latitude, longitude)
        )
    """)

    # Cria índices
    log("[DB] Criando índices...")
    for tabela, coluna in [
        ("clima_diario", "latitude, longitude, data"),
        ("clima_horario", "latitude, longitude, data"),
        ("qualidade_ar", "latitude, longitude, data"),
        ("polen", "latitude, longitude, data"),
    ]:
        cursor.execute(f"CREATE INDEX IF NOT EXISTS idx_{tabela}_loc ON {tabela}({coluna})")
    # Índices por data: aceleram a carga de registros existentes (filtro por intervalo de datas)
    for tabela in ["clima_diario", "clima_horario", "qualidade_ar", "polen"]:
        cursor.execute(f"CREATE INDEX IF NOT EXISTS idx_{tabela}_data ON {tabela}(data)")

    conn.commit()
    log("[DB] Banco inicializado.")

def carregar_existentes(datas: List[str], coordenadas: List) -> Dict[str, Dict]:
    conn = get_connection()
    cursor = conn.cursor()
    existentes = {"diario": {}, "horario": {}, "qualidade_ar": {}, "polen": {}}

    tabelas = {
        "diario": "clima_diario",
        "horario": "clima_horario",
        "qualidade_ar": "qualidade_ar",
        "polen": "polen",
    }

    # Conjunto de coordenadas de interesse (como floats, para casar com a chave usada no resto do código)
    coords_alvo = {(float(lat), float(lon)) for lat, lon in coordenadas}

    for tipo, tabela in tabelas.items():
        # Filtra datas por tipo
        if tipo == "qualidade_ar":
            datas_filtradas = [d for d in datas if d >= DATA_MINIMA_QUALIDADE_AR]
        elif tipo == "polen":
            hoje = datetime.now().date()
            datas_filtradas = [d for d in datas if datetime.strptime(d, "%Y-%m-%d").date() >= hoje]
        else:
            datas_filtradas = datas

        if not datas_filtradas: continue

        datas_set = set(datas_filtradas)
        mapa = existentes[tipo]
        for lat, lon in coords_alvo: mapa[(lat, lon)] = set()

        # 1 única consulta por tabela: apenas data, latitude e longitude no intervalo do período
        cursor.execute(
            f"SELECT latitude, longitude, data FROM {tabela} WHERE data BETWEEN ? AND ?",
            (datas_filtradas[0], datas_filtradas[-1]),
        )
        for lat, lon, data in cursor:
            if data not in datas_set: continue
            chave = (float(lat), float(lon))
            if chave not in coords_alvo: continue
            mapa[chave].add(data)

    return existentes

# PARSING GENÉRICO
def parse_registros(dados: Dict, lat: float, lon: float, tipo: str, datas_filtro: Optional[Set[str]] = None) -> List[Dict]:
    filtro = datas_filtro
    times = dados.get("time", [])
    registros = []

    # Mapeamento de campos por tipo
    mapeamentos = {
        "diario": {
            "data": ("time", lambda i: times[i]),
            "timezone": ("timezone", lambda _: dados.get("timezone")),
            "weathercode": ("weathercode", _get_valor),
            "temperatura_max": ("temperature_2m_max", _get_valor),
            "temperatura_min": ("temperature_2m_min", _get_valor),
            "sensacao_termica_max": ("apparent_temperature_max", _get_valor),
            "sensacao_termica_min": ("apparent_temperature_min", _get_valor),
            "umidade_max": ("relativehumidity_2m_max", _get_valor),
            "umidade_min": ("relativehumidity_2m_min", _get_valor),
            "precipitacao_total": ("precipitation_sum", _get_valor),
            "probabilidade_chuva": ("precipitation_probability_max", _get_valor),
            "chuva_total": ("rain_sum", _get_valor),
            "aguas_claras_total": ("showers_sum", _get_valor),
            "neve_total": ("snowfall_sum", _get_valor),
            "nascer_sol": ("sunrise", _get_valor),
            "por_sol": ("sunset", _get_valor),
            "uv_max": ("uv_index_max", _get_valor),
            "uv_clear_sky_max": ("uv_index_clear_sky_max", _get_valor),
            "vento_max": ("windspeed_10m_max", _get_valor),
            "direcao_vento": ("winddirection_10m_dominant", _get_valor),
            "rajadas_vento": ("windgusts_10m_max", _get_valor),
            "pressao_max": ("pressure_msl_max", _get_valor),
            "pressao_min": ("pressure_msl_min", _get_valor),
            "pressao_superficie_max": ("surface_pressure_max", _get_valor),
            "pressao_superficie_min": ("surface_pressure_min", _get_valor),
        },
        "horario": {
            "data": ("time", lambda i: times[i][:10]),
            "hora": ("time", lambda i: times[i]),
            "temperatura": ("temperature_2m", _get_valor),
            "sensacao_termica": ("apparent_temperature", _get_valor),
            "umidade": ("relativehumidity_2m", _get_valor),
            "ponto_orvalho": ("dewpoint_2m", _get_valor),
            "precipitacao": ("precipitation", _get_valor),
            "probabilidade_chuva": ("precipitation_probability", _get_valor),
            "chuva": ("rain", _get_valor),
            "aguas_claras": ("showers", _get_valor),
            "neve": ("snowfall", _get_valor),
            "profundidade_neve": ("snow_depth", _get_valor),
            "uv_index": ("uv_index", _get_valor),
            "uv_index_clear_sky": ("uv_index_clear_sky", _get_valor),
            "vento": ("windspeed_10m", _get_valor),
            "direcao_vento": ("winddirection_10m", _get_valor),
            "rajadas_vento": ("windgusts_10m", _get_valor),
            "pressao": ("pressure_msl", _get_valor),
            "pressao_superficie": ("surface_pressure", _get_valor),
            "cobertura_nuvens": ("cloudcover", _get_valor),
            "cobertura_nuvens_baixa": ("cloudcover_low", _get_valor),
            "cobertura_nuvens_media": ("cloudcover_mid", _get_valor),
            "cobertura_nuvens_alta": ("cloudcover_high", _get_valor),
            "visibilidade": ("visibility", _get_valor),
            "is_day": ("is_day", _get_valor),
            "weathercode": ("weathercode", _get_valor),
        },
        "qualidade_ar": {
            "data": ("time", lambda i: times[i][:10]),
            "hora": ("time", lambda i: times[i]),
            "pm10": ("pm10", _get_valor),
            "pm2_5": ("pm2_5", _get_valor),
            "monoxido_carbono": ("carbon_monoxide", _get_valor),
            "nitrogenio_dioxide": ("nitrogen_dioxide", _get_valor),
            "enxofre_dioxide": ("sulphur_dioxide", _get_valor),
            "ozonio": ("ozone", _get_valor),
            "aerosois": ("aerosol", _get_valor),
            "poeira": ("dust", _get_valor),
            "formaldeido": ("formaldehyde", _get_valor),
        },
        "polen": {
            "data": ("time", lambda i: times[i]),
            "alder_pollen_mean": ("alder_pollen_mean", _get_valor),
            "birch_pollen_mean": ("birch_pollen_mean", _get_valor),
            "olive_pollen_mean": ("olive_pollen_mean", _get_valor),
            "ragweed_pollen_mean": ("ragweed_pollen_mean", _get_valor),
            "grass_pollen_mean": ("grass_pollen_mean", _get_valor),
        },
    }

    mapeamento = mapeamentos.get(tipo, {})
    if not mapeamento: return []

    for i, _ in enumerate(times):
        data = times[i][:10] if len(times[i]) > 10 else times[i]
        if filtro and data not in filtro: continue

        registro = {"latitude": lat, "longitude": lon}
        for campo, (chave, func) in mapeamento.items():
            if campo in ["data", "hora", "timezone"]:
                valor = func(i) if callable(func) else dados.get(chave)
            else:
                valor = func(dados, chave, i)
            registro[campo] = valor

        registros.append(registro)

    return registros

def baixar_com_retry(url: str, params: Dict, session: requests.Session,
                     max_tentativas: int = MAX_RETRIES) -> Optional[Dict]:
    for tentativa in range(1, max_tentativas + 1):
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
                _reduzir_taxa_429()
                retry_after = _extrair_retry_after(e.response)
                espera = max(ESPERA_MIN_429, retry_after or _backoff_exponencial(tentativa))
                if tentativa == max_tentativas:
                    log(f"  [ERRO] 429 persistente após {max_tentativas} tentativas")
                    return None
                log(f"  [RETRY {tentativa}/{max_tentativas}] HTTP 429. Aguardando {espera:.1f}s...")
                time.sleep(espera)
            else:
                if tentativa == max_tentativas:
                    log(f"  [ERRO] Falha após {max_tentativas} tentativas: {e}")
                    return None
                espera = _backoff_exponencial(tentativa)
                log(f"  [RETRY {tentativa}/{max_tentativas}] HTTP {status}: {e}. Aguardando {espera:.1f}s...")
                time.sleep(espera)
        except (requests.exceptions.Timeout, requests.exceptions.ConnectionError) as e:
            if tentativa == max_tentativas:
                log(f"  [ERRO] Falha após {max_tentativas} tentativas: {e}")
                return None
            espera = _backoff_exponencial(tentativa)
            log(f"  [RETRY {tentativa}/{max_tentativas}] {type(e).__name__}. Aguardando {espera:.1f}s...")
            time.sleep(espera)

    return None

def _extrair_retry_after(response: requests.Response) -> Optional[float]:
    try: return float(response.headers.get("Retry-After", ""))
    except (TypeError, ValueError): return None

def _backoff_exponencial(tentativa: int) -> float:
    return min(BACKOFF_MAXIMO, (BACKOFF_INICIAL * (2 ** (tentativa - 1))) * random.uniform(0.5, 1.5))

def agrupar_por_url(datas: List[str], chunk: int = CHUNK_DIAS) -> List[Tuple[str, List[str]]]:
    grupos = []
    for url in (URL_ARCHIVE, URL_FORECAST):
        lista = sorted(d for d in datas if get_url_clima(d) == url)
        for i in range(0, len(lista), chunk): grupos.append((url, lista[i:i + chunk]))
    return grupos

def baixar_clima_bloco(datas: List[str], lat: float, lon: float,
                       session: requests.Session) -> Dict[str, List]:
    if not datas:
        return {"diario": [], "horario": []}

    diario, horario = [], []
    for url, grupo in agrupar_por_url(datas):
        inicio, fim = grupo[0], grupo[-1]
        daily_params = ([p for p in DAILY_PARAMS if p not in DAILY_PARAMS_NAO_SUPORTADOS_NO_ARQUIVO]
                       if url == URL_ARCHIVE else DAILY_PARAMS)

        params = {
            "latitude": lat, "longitude": lon,
            "start_date": inicio, "end_date": fim,
            "daily": ",".join(daily_params),
            "hourly": ",".join(HOURLY_PARAMS),
            "timezone": FUSO_HORARIO,
        }

        dados = baixar_com_retry(url, params, session)
        if not dados:
            continue

        if "daily" in dados and dados["daily"]:
            diario.extend(parse_registros(dados["daily"], lat, lon, "diario", set(grupo)))
        if "hourly" in dados and dados["hourly"]:
            horario.extend(parse_registros(dados["hourly"], lat, lon, "horario", set(grupo)))

    return {"diario": diario, "horario": horario}

def baixar_qualidade_ar_bloco(datas: List[str], lat: float, lon: float,
                               session: requests.Session) -> List[Dict]:
    datas = [d for d in datas if d >= DATA_MINIMA_QUALIDADE_AR]
    if not datas: return []

    params = {
        "latitude": lat, "longitude": lon,
        "start_date": datas[0], "end_date": datas[-1],
        "hourly": ",".join(AIR_QUALITY_HOURLY),
        "timezone": FUSO_HORARIO,
    }

    dados = baixar_com_retry(URL_AIR_QUALITY, params, session)
    if not dados or "hourly" not in dados:
        return []

    return parse_registros(dados["hourly"], lat, lon, "qualidade_ar", set(datas))

def baixar_polen_bloco(datas: List[str], lat: float, lon: float,
                       session: requests.Session) -> List[Dict]:
    hoje = datetime.now().date()
    datas = [d for d in datas if datetime.strptime(d, "%Y-%m-%d").date() >= hoje]
    if not datas: return []

    params = {
        "latitude": lat, "longitude": lon,
        "start_date": datas[0], "end_date": datas[-1],
        "daily": ",".join(POLLEN_DAILY),
        "timezone": FUSO_HORARIO,
    }

    dados = baixar_com_retry(URL_POLLEN, params, session)
    if not dados or "daily" not in dados: return []

    return parse_registros(dados["daily"], lat, lon, "polen", set(datas))

def salvar_dados(tabela: str, registros: List[Dict], campos: List[str]) -> None:
    if not registros: return

    conn = get_connection()
    cursor = conn.cursor()
    agora = datetime.now().isoformat()

    placeholders = ",".join(["?"] * (len(campos) + 1))
    campos_sql = ",".join(campos)
    query = f"INSERT OR REPLACE INTO {tabela} ({campos_sql}, created_at) VALUES ({placeholders})"

    valores = []
    for r in registros:
        linha = [r.get(c) for c in campos]
        linha.append(agora)
        valores.append(linha)

    cursor.executemany(query, valores)
    conn.commit()

# Definições de schema para cada tabela
SCHEMAS = {
    "clima_diario": [
        "data",
        "latitude",
        "longitude",
        "timezone",
        "weathercode",
        "temperatura_max",
        "temperatura_min",
        "sensacao_termica_max",
        "sensacao_termica_min",
        "umidade_max",
        "umidade_min",
        "precipitacao_total",
        "probabilidade_chuva",
        "chuva_total",
        "aguas_claras_total",
        "neve_total",
        "nascer_sol",
        "por_sol",
        "uv_max",
        "uv_clear_sky_max",
        "vento_max",
        "direcao_vento",
        "rajadas_vento",
        "pressao_max",
        "pressao_min",
        "pressao_superficie_max",
        "pressao_superficie_min",
    ],
    "clima_horario": [
        "data",
        "hora",
        "latitude",
        "longitude",
        "temperatura",
        "sensacao_termica",
        "umidade",
        "ponto_orvalho",
        "precipitacao",
        "probabilidade_chuva",
        "chuva",
        "aguas_claras",
        "neve",
        "profundidade_neve",
        "uv_index",
        "uv_index_clear_sky",
        "vento",
        "direcao_vento",
        "rajadas_vento",
        "pressao",
        "pressao_superficie",
        "cobertura_nuvens",
        "cobertura_nuvens_baixa",
        "cobertura_nuvens_media",
        "cobertura_nuvens_alta",
        "visibilidade",
        "is_day",
        "weathercode",
    ],
    "qualidade_ar": [
        "data",
        "hora",
        "latitude",
        "longitude",
        "pm10",
        "pm2_5",
        "monoxido_carbono",
        "nitrogenio_dioxide",
        "enxofre_dioxide",
        "ozonio",
        "aerosois",
        "poeira",
        "formaldeido",
    ],
    "polen": [
        "data",
        "latitude",
        "longitude",
        "alder_pollen_mean",
        "birch_pollen_mean",
        "olive_pollen_mean",
        "ragweed_pollen_mean",
        "grass_pollen_mean",
    ],
}

def baixar_bloco(datas: List[str], lat: float, lon: float, existentes: Dict) -> Optional[Dict]:
    time.sleep(random.uniform(0.0, JITTER_INICIAL_MAX))
    chave_base = (float(lat), float(lon))

    # Filtra datas faltantes por tipo
    datas_diario = _filtrar_faltantes(datas, existentes["diario"].get(chave_base, set()))
    datas_horario = _filtrar_faltantes(datas, existentes["horario"].get(chave_base, set()))
    datas_ar = [d for d in datas if d >= DATA_MINIMA_QUALIDADE_AR
                and d not in existentes["qualidade_ar"].get(chave_base, set())]
    hoje = datetime.now().date()
    datas_polen = [d for d in datas
                   if datetime.strptime(d, "%Y-%m-%d").date() >= hoje
                   and d not in existentes["polen"].get(chave_base, set())]

    session = get_session()
    resultado = {
        "lat": lat,
        "lon": lon,
        "datas_diario": datas_diario,
        "datas_horario": datas_horario,
        "datas_ar": datas_ar,
        "datas_polen": datas_polen,
        "diario": [],
        "horario": [],
        "qualidade_ar": [],
        "polen": [],
    }

    # Download clima (diário + horário juntos)
    if datas_diario or datas_horario:
        alvo = sorted(set(datas_diario + datas_horario))
        clima = baixar_clima_bloco(alvo, lat, lon, session)
        resultado["diario"] = clima["diario"]
        resultado["horario"] = clima["horario"]

    # Download qualidade do ar
    if datas_ar: resultado["qualidade_ar"] = baixar_qualidade_ar_bloco(datas_ar, lat, lon, session)

    # Download pólen
    if datas_polen: resultado["polen"] = baixar_polen_bloco(datas_polen, lat, lon, session)

    return resultado

def processar_resultado(resultado: Dict, existentes: Dict, stats: Dict) -> None:
    lat, lon = resultado["lat"], resultado["lon"]
    chave_base = (float(lat), float(lon))

    # Atualiza estatísticas de datas puladas
    stats["pulado_diario"] += len(resultado["datas_diario"]) - len(resultado["diario"])
    stats["pulado_horario"] += len(resultado["datas_horario"]) - len(resultado["horario"])
    stats["pulado_ar"] += len(resultado["datas_ar"]) - len(resultado["qualidade_ar"])
    stats["pulado_polen"] += len(resultado["datas_polen"]) - len(resultado["polen"])

    # Insere clima diário
    if resultado["diario"]:
        with write_lock: salvar_dados("clima_diario", resultado["diario"], SCHEMAS["clima_diario"])
        existentes["diario"].setdefault(chave_base, set()).update(r["data"] for r in resultado["diario"])
        stats["baixado_diario"] += len(resultado["diario"])

    # Insere clima horário
    if resultado["horario"]:
        with write_lock: salvar_dados("clima_horario", resultado["horario"], SCHEMAS["clima_horario"])
        existentes["horario"].setdefault(chave_base, set()).update(r["data"] for r in resultado["horario"])
        stats["baixado_horario"] += len(resultado["horario"])

    # Insere qualidade do ar
    if resultado["qualidade_ar"]:
        with write_lock: salvar_dados("qualidade_ar", resultado["qualidade_ar"], SCHEMAS["qualidade_ar"])
        existentes["qualidade_ar"].setdefault(chave_base, set()).update(r["data"] for r in resultado["qualidade_ar"])
        stats["baixado_ar"] += len(resultado["qualidade_ar"])

    # Insere pólen
    if resultado["polen"]:
        with write_lock: salvar_dados("polen", resultado["polen"], SCHEMAS["polen"])
        existentes["polen"].setdefault(chave_base, set()).update(r["data"] for r in resultado["polen"])
        stats["baixado_polen"] += len(resultado["polen"])

def _filtrar_faltantes(datas: List[str], existentes: Set[str]) -> List[str]:
    return [d for d in datas if d not in existentes]

def _datas_esperadas(tipo: str, datas: List[str]) -> Set[str]:
    if tipo == "qualidade_ar": return {d for d in datas if d >= DATA_MINIMA_QUALIDADE_AR}
    if tipo == "polen":
        hoje = datetime.now().date()
        return {d for d in datas if datetime.strptime(d, "%Y-%m-%d").date() >= hoje}
    return set(datas)

def coordenada_completa(datas: List[str], lat: float, lon: float,
                        existentes: Dict) -> bool:
    chave = (float(lat), float(lon))
    for tipo in ("diario", "horario", "qualidade_ar", "polen"):
        esperadas = _datas_esperadas(tipo, datas)
        presentes = existentes[tipo].get(chave, set())
        if not esperadas.issubset(presentes): return False
    return True

def main() -> None:
    datas = expandir_periodo(PERIODO_DOWNLOAD)

    print("=" * 60)
    print("DOWNLOAD DE DADOS CLIMÁTICOS")
    print("=" * 60)
    print(f"Fuso horário: {FUSO_HORARIO}")
    print(f"Banco de dados: {DB_PATH}")
    print(f"Período: {datas[0]} a {datas[-1]}")
    print(f"Total de datas: {len(datas)}")
    print(f"Total de localizações: {len(COORDENADAS)}")
    print(f"Threads download: {DOWNLOAD_WORKERS} | Threads processamento: {PROCESS_WORKERS}")
    print(f"Dias por requisição: {CHUNK_DIAS}")
    print(f"Rate-limit: {REQUESTS_PER_SECOND} req/s | Timeout: {HTTP_TIMEOUT}s")
    print(f"Retries máximos: {MAX_RETRIES}")
    print("=" * 60)

    init_database()

    print("\n[DB] Carregando registros existentes...")
    inicio_carregamento = time.time()
    existentes = carregar_existentes(datas, COORDENADAS)
    tempo_carregamento = time.time() - inicio_carregamento

    totais = {tipo: sum(len(v) for v in existentes[tipo].values()) for tipo in existentes}
    print(f"[DB] Carregado em {tempo_carregamento:.2f}s")
    print(f"[DB] Existentes -> diário: {totais['diario']}, horário: {totais['horario']}, " f"qualidade_ar: {totais['qualidade_ar']}, polen: {totais['polen']}")
    print(f"[DB] Total de itens (data x coordenada) faltantes para processamento: {total_faltantes}")

    # Pula coordenadas que já possuem todas as datas do período e monta tarefas
    # somente com as datas que NÃO existem (faltantes) para cada coordenada
    coordenadas_ativas = []
    total_faltantes = 0
    for lat, lon in COORDENADAS:
        if coordenada_completa(datas, lat, lon, existentes):
            print(f"[DB] Coordenada ({lat}, {lon}) já possui todas as datas do período. Pulando...")
            continue
        # Apenas datas com pelo menos um tipo de dado faltante entram para processamento
        datas_faltantes = [
            d for d in datas
            if d not in existentes["diario"].get((float(lat), float(lon)), set())
            or d not in existentes["horario"].get((float(lat), float(lon)), set())
            or (d >= DATA_MINIMA_QUALIDADE_AR and d not in existentes["qualidade_ar"].get((float(lat), float(lon)), set()))
            or (datetime.strptime(d, "%Y-%m-%d").date() >= datetime.now().date()
                and d not in existentes["polen"].get((float(lat), float(lon)), set()))
        ]
        if datas_faltantes:
            coordenadas_ativas.append((datas_faltantes, lat, lon))
            total_faltantes += len(datas_faltantes)

    coordenadas_puladas = len(COORDENADAS) - len(coordenadas_ativas)
    if not coordenadas_ativas:
        print("[DB] Nenhuma coordenada com dados pendentes. Nada a fazer.")
        return

    # Cria tarefas (cada coordenada só recebe os blocos com datas faltantes)
    tarefas = []
    for datas_coord, lat, lon in coordenadas_ativas:
        for i in range(0, len(datas_coord), CHUNK_DIAS):
            tarefas.append((datas_coord[i:i + CHUNK_DIAS], lat, lon))

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

    # Fila para transferir resultados de download -> processamento
    fila_resultados: Queue = Queue(maxsize=DOWNLOAD_WORKERS * 2)

    # Contador de produtores terminados (thread-safe)
    produtores_terminados = [0]
    produtores_lock = threading.Lock()

    def produtor(tarefa):
        datas_bloco, lat, lon = tarefa
        resultado = baixar_bloco(datas_bloco, lat, lon, existentes)
        if resultado: fila_resultados.put(resultado)

    def consumidor():
        nonlocal concluidos
        while True:
            try:
                resultado = fila_resultados.get(timeout=1)
            except Empty:
                # Se todos os produtores terminaram e a fila está vazia, sair
                with produtores_lock: todos_terminados = produtores_terminados[0] >= total
                if todos_terminados and fila_resultados.empty(): break
                continue

            processar_resultado(resultado, existentes, stats)
            with stats_lock:
                concluidos += 1
                if concluidos % 5 == 0 or concluidos == total:
                    pct = concluidos / total * 100
                    decorrido = time.time() - inicio
                    print(f"\r[PROGRESSO] {concluidos}/{total} blocos ({pct:.1f}%) - {decorrido:.0f}s", end="", flush=True)

    def produtor_wrapper(tarefa):
        try:
            produtor(tarefa)
        finally:
            with produtores_lock: produtores_terminados[0] += 1

    print(f"\nIniciando {total} tarefas: {DOWNLOAD_WORKERS} threads de download, " f"{PROCESS_WORKERS} threads de processamento...\n")

    with ThreadPoolExecutor(max_workers=DOWNLOAD_WORKERS) as executor_download:
        with ThreadPoolExecutor(max_workers=PROCESS_WORKERS) as executor_process:
            # Inicia consumidores
            futures_process = [executor_process.submit(consumidor) for _ in range(PROCESS_WORKERS)]

            # Envia tarefas de download
            futures_download = [executor_download.submit(produtor_wrapper, t) for t in tarefas]
            # Espera que todos os downloads terminem
            for future in as_completed(futures_download): future.result()
            # Espera que os consumidores terminem
            for future in futures_process: future.result()

    print()
    print("\n" + "=" * 60)
    print("DOWNLOAD CONCLUÍDO!")
    print("=" * 60)
    print(f"Tempo total: {time.time() - inicio:.1f}s")
    print(f"Tempo carregamento DB: {tempo_carregamento:.2f}s")
    print(f"Blocos processados: {concluidos}/{total}")
    print(f"Coordenadas puladas (período completo): {coordenadas_puladas}")
    print("\n--- RESUMO ---")
    print(f"  Diário: {stats['baixado_diario']} baixados | {stats['pulado_diario']} já existentes")
    print(f"  Horário: {stats['baixado_horario']} registros | {stats['pulado_horario']} dias existentes")
    print(f"  Qualidade do ar: {stats['baixado_ar']} registros | {stats['pulado_ar']} dias existentes")
    print(f"  Pólen: {stats['baixado_polen']} dias | {stats['pulado_polen']} dias existentes")
    print("=" * 60)

if __name__ == "__main__":
    main()

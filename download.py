#!/usr/bin/env python3
"""
download.py - Download de informações climáticas via API gratuita (Open-Meteo)

API utilizada: Open-Meteo (https://open-meteo.com)
  - Não requer autenticação, chaves ou tokens
  - Fornece: temperatura, sensação térmica, umidade, precipitação,
    nascer/pôr do sol, nascer/pôr da lua, índice UV, polen, qualidade do ar

Dados são armazenados em um arquivo SQLite (.db).
Antes de baixar, verifica se os dados já existem no banco para evitar duplicatas.

Uso:
    python download.py
"""

import sqlite3
import requests
from datetime import datetime, timedelta
import time
import sys

# ============================================================================
# CONFIGURAÇÕES
# ============================================================================

# Caminho do arquivo de banco de dados SQLite
DB_PATH = "clima.db"

# Coordenadas geográficas (latitude, longitude)
# Exemplo: São Paulo, Brasil
LATITUDE = -23.5505
LONGITUDE = -46.6333

# Fuso horário (IANA timezone)
FUSO_HORARIO = "America/Sao_Paulo"

# ----------------------------------------------------------------------------
# ARRAY COM O PERÍODO DE DOWNLOAD
# Informe as datas (formato YYYY-MM-DD) que deseja baixar.
# O script fará o download dia por dia, verificando se já existe no .db.
# Exemplo: ["2024-01-01", "2024-01-02", "2024-01-03"]
# ----------------------------------------------------------------------------
PERIODO_DOWNLOAD = [
    "2024-01-01",
    "2024-01-02",
    "2024-01-03",
    "2024-01-04",
    "2024-01-05",
]

# Endpoints da API Open-Meteo (gratuita, sem autenticação)
URL_FORECAST = "https://api.open-meteo.com/v1/forecast"
URL_AIR_QUALITY = "https://api.open-meteo.com/v1/air-quality"
URL_POLLEN = "https://api.open-meteo.com/v1/pollen"

# Parâmetros que queremos baixar da API de previsão (forecast)
# - daily: dados diários (nascer/pôr do sol, lua, UV máximo, etc.)
# - hourly: dados por hora (temperatura, sensação térmica, umidade, chuva, UV)
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

# Parâmetros de qualidade do ar (air quality)
AIR_QUALITY_HOURLY = [
    "pm10",
    "pm2_5",
    "carbon_monoxide",
    "nitrogen_dioxide",
    "sulfur_dioxide",
    "ozone",
    "aerosol",
    "dust",
    "ammonium",
    "radon",
    "formaldehyde",
    "mercury",
]

# Parâmetros de polen (pollen API)
POLLEN_DAILY = [
    "alder_pollen_mean",
    "birch_pollen_mean",
    "olive_pollen_mean",
    "ragweed_pollen_mean",
    "grass_pollen_mean",
]


# ============================================================================
# FUNÇÕES DE BANCO DE DADOS
# ============================================================================

def get_connection():
    """Retorna uma conexão com o banco SQLite."""
    return sqlite3.connect(DB_PATH)


def init_database():
    """Cria as tabelas do banco de dados se não existirem."""
    conn = get_connection()
    cursor = conn.cursor()

    # Tabela de dados diários (resumo climático do dia)
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_diario (
            data TEXT PRIMARY KEY,
            latitude REAL,
            longitude REAL,
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
            nascer_lua TEXT,
            por_lua TEXT,
            uv_max REAL,
            uv_clear_sky_max REAL,
            vento_max REAL,
            direcao_vento REAL,
            rajadas_vento REAL,
            pressao_max REAL,
            pressao_min REAL,
            pressao_superficie_max REAL,
            pressao_superficie_min REAL,
            created_at TEXT
        )
    """)

    # Tabela de dados horários (detalhes por hora)
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS clima_horario (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            data TEXT,
            hora TEXT,
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
            created_at TEXT
        )
    """)

    # Tabela de qualidade do ar
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS qualidade_ar (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            data TEXT,
            hora TEXT,
            pm10 REAL,
            pm2_5 REAL,
            monoxido_carbono REAL,
            nitrogenio_dioxide REAL,
            enxofre_dioxide REAL,
            ozonio REAL,
            aerosois REAL,
            poeira REAL,
            amonio REAL,
            radon REAL,
            formaldeido REAL,
            mercurio REAL,
            created_at TEXT
        )
    """)

    # Tabela de polen
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS polen (
            data TEXT PRIMARY KEY,
            latitude REAL,
            longitude REAL,
            alder_pollen_mean REAL,
            birch_pollen_mean REAL,
            olive_pollen_mean REAL,
            ragweed_pollen_mean REAL,
            grass_pollen_mean REAL,
            created_at TEXT
        )
    """)

    conn.commit()
    conn.close()
    print("[DB] Banco de dados inicializado com sucesso.")


def data_diaria_existe(data):
    """Verifica se os dados diários de uma data já existem no banco."""
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute("SELECT 1 FROM clima_diario WHERE data = ?", (data,))
    result = cursor.fetchone()
    conn.close()
    return result is not None


def data_horaria_existe(data):
    """Verifica se os dados horários de uma data já existem no banco."""
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute("SELECT 1 FROM clima_horario WHERE data = ? LIMIT 1", (data,))
    result = cursor.fetchone()
    conn.close()
    return result is not None


def qualidade_ar_existe(data):
    """Verifica se os dados de qualidade do ar de uma data já existem no banco."""
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute("SELECT 1 FROM qualidade_ar WHERE data = ? LIMIT 1", (data,))
    result = cursor.fetchone()
    conn.close()
    return result is not None


def polen_existe(data):
    """Verifica se os dados de polen de uma data já existem no banco."""
    conn = get_connection()
    cursor = conn.cursor()
    cursor.execute("SELECT 1 FROM polen WHERE data = ?", (data,))
    result = cursor.fetchone()
    conn.close()
    return result is not None


# ============================================================================
# FUNÇÕES DE DOWNLOAD DA API
# ============================================================================

def baixar_dados_diarios(data):
    """
    Baixa dados diários da API Open-Meteo para uma data específica.
    Retorna um dicionário com os dados ou None em caso de erro.
    """
    params = {
        "latitude": LATITUDE,
        "longitude": LONGITUDE,
        "start_date": data,
        "end_date": data,
        "daily": ",".join(DAILY_PARAMS),
        "timezone": FUSO_HORARIO,
    }

    try:
        response = requests.get(URL_FORECAST, params=params, timeout=30)
        response.raise_for_status()
        dados = response.json()

        if "daily" not in dados or not dados["daily"]:
            print(f"  [AVISO] Nenhum dado diário retornado para {data}")
            return None

        daily = dados["daily"]
        # Como start_date == end_date, há apenas um elemento em cada lista
        idx = 0

        return {
            "data": data,
            "latitude": dados.get("latitude"),
            "longitude": dados.get("longitude"),
            "timezone": dados.get("timezone"),
            "weathercode": daily.get("weathercode", [None])[idx],
            "temperatura_max": daily.get("temperature_2m_max", [None])[idx],
            "temperatura_min": daily.get("temperature_2m_min", [None])[idx],
            "sensacao_termica_max": daily.get("apparent_temperature_max", [None])[idx],
            "sensacao_termica_min": daily.get("apparent_temperature_min", [None])[idx],
            "umidade_max": daily.get("relativehumidity_2m_max", [None])[idx],
            "umidade_min": daily.get("relativehumidity_2m_min", [None])[idx],
            "precipitacao_total": daily.get("precipitation_sum", [None])[idx],
            "probabilidade_chuva": daily.get("precipitation_probability_max", [None])[idx],
            "chuva_total": daily.get("rain_sum", [None])[idx],
            "aguas_claras_total": daily.get("showers_sum", [None])[idx],
            "neve_total": daily.get("snowfall_sum", [None])[idx],
            "nascer_sol": daily.get("sunrise", [None])[idx],
            "por_sol": daily.get("sunset", [None])[idx],
            "nascer_lua": daily.get("moonrise", [None])[idx],
            "por_lua": daily.get("moonset", [None])[idx],
            "uv_max": daily.get("uv_index_max", [None])[idx],
            "uv_clear_sky_max": daily.get("uv_index_clear_sky_max", [None])[idx],
            "vento_max": daily.get("windspeed_10m_max", [None])[idx],
            "direcao_vento": daily.get("winddirection_10m_dominant", [None])[idx],
            "rajadas_vento": daily.get("windgusts_10m_max", [None])[idx],
            "pressao_max": daily.get("pressure_msl_max", [None])[idx],
            "pressao_min": daily.get("pressure_msl_min", [None])[idx],
            "pressao_superficie_max": daily.get("surface_pressure_max", [None])[idx],
            "pressao_superficie_min": daily.get("surface_pressure_min", [None])[idx],
        }

    except requests.exceptions.RequestException as e:
        print(f"  [ERRO] Falha ao baixar dados diários para {data}: {e}")
        return None


def baixar_dados_horarios(data):
    """
    Baixa dados horários da API Open-Meteo para uma data específica.
    Retorna uma lista de dicionários (um por hora) ou None em caso de erro.
    """
    params = {
        "latitude": LATITUDE,
        "longitude": LONGITUDE,
        "start_date": data,
        "end_date": data,
        "hourly": ",".join(HOURLY_PARAMS),
        "timezone": FUSO_HORARIO,
    }

    try:
        response = requests.get(URL_FORECAST, params=params, timeout=30)
        response.raise_for_status()
        dados = response.json()

        if "hourly" not in dados or not dados["hourly"]:
            print(f"  [AVISO] Nenhum dado horário retornado para {data}")
            return None

        hourly = dados["hourly"]
        times = hourly.get("time", [])
        resultados = []

        for i, hora_str in enumerate(times):
            resultados.append({
                "data": data,
                "hora": hora_str,
                "temperatura": hourly.get("temperature_2m", [None])[i],
                "sensacao_termica": hourly.get("apparent_temperature", [None])[i],
                "umidade": hourly.get("relativehumidity_2m", [None])[i],
                "ponto_orvalho": hourly.get("dewpoint_2m", [None])[i],
                "precipitacao": hourly.get("precipitation", [None])[i],
                "probabilidade_chuva": hourly.get("precipitation_probability", [None])[i],
                "chuva": hourly.get("rain", [None])[i],
                "aguas_claras": hourly.get("showers", [None])[i],
                "neve": hourly.get("snowfall", [None])[i],
                "profundidade_neve": hourly.get("snow_depth", [None])[i],
                "uv_index": hourly.get("uv_index", [None])[i],
                "uv_index_clear_sky": hourly.get("uv_index_clear_sky", [None])[i],
                "vento": hourly.get("windspeed_10m", [None])[i],
                "direcao_vento": hourly.get("winddirection_10m", [None])[i],
                "rajadas_vento": hourly.get("windgusts_10m", [None])[i],
                "pressao": hourly.get("pressure_msl", [None])[i],
                "pressao_superficie": hourly.get("surface_pressure", [None])[i],
                "cobertura_nuvens": hourly.get("cloudcover", [None])[i],
                "cobertura_nuvens_baixa": hourly.get("cloudcover_low", [None])[i],
                "cobertura_nuvens_media": hourly.get("cloudcover_mid", [None])[i],
                "cobertura_nuvens_alta": hourly.get("cloudcover_high", [None])[i],
                "visibilidade": hourly.get("visibility", [None])[i],
                "is_day": hourly.get("is_day", [None])[i],
                "weathercode": hourly.get("weathercode", [None])[i],
            })

        return resultados

    except requests.exceptions.RequestException as e:
        print(f"  [ERRO] Falha ao baixar dados horários para {data}: {e}")
        return None


def baixar_qualidade_ar(data):
    """
    Baixa dados de qualidade do ar da API Open-Meteo para uma data específica.
    Retorna uma lista de dicionários (um por hora) ou None em caso de erro.
    """
    params = {
        "latitude": LATITUDE,
        "longitude": LONGITUDE,
        "start_date": data,
        "end_date": data,
        "hourly": ",".join(AIR_QUALITY_HOURLY),
        "timezone": FUSO_HORARIO,
    }

    try:
        response = requests.get(URL_AIR_QUALITY, params=params, timeout=30)
        response.raise_for_status()
        dados = response.json()

        if "hourly" not in dados or not dados["hourly"]:
            print(f"  [AVISO] Nenhum dado de qualidade do ar retornado para {data}")
            return None

        hourly = dados["hourly"]
        times = hourly.get("time", [])
        resultados = []

        for i, hora_str in enumerate(times):
            resultados.append({
                "data": data,
                "hora": hora_str,
                "pm10": hourly.get("pm10", [None])[i],
                "pm2_5": hourly.get("pm2_5", [None])[i],
                "monoxido_carbono": hourly.get("carbon_monoxide", [None])[i],
                "nitrogenio_dioxide": hourly.get("nitrogen_dioxide", [None])[i],
                "enxofre_dioxide": hourly.get("sulfur_dioxide", [None])[i],
                "ozonio": hourly.get("ozone", [None])[i],
                "aerosois": hourly.get("aerosol", [None])[i],
                "poeira": hourly.get("dust", [None])[i],
                "amonio": hourly.get("ammonium", [None])[i],
                "radon": hourly.get("radon", [None])[i],
                "formaldeido": hourly.get("formaldehyde", [None])[i],
                "mercurio": hourly.get("mercury", [None])[i],
            })

        return resultados

    except requests.exceptions.RequestException as e:
        print(f"  [ERRO] Falha ao baixar qualidade do ar para {data}: {e}")
        return None


def baixar_dados_polen(data):
    """
    Baixa dados de polen da API Open-Meteo para uma data específica.
    Retorna um dicionário com os dados ou None em caso de erro.
    """
    params = {
        "latitude": LATITUDE,
        "longitude": LONGITUDE,
        "start_date": data,
        "end_date": data,
        "daily": ",".join(POLLEN_DAILY),
        "timezone": FUSO_HORARIO,
    }

    try:
        response = requests.get(URL_POLLEN, params=params, timeout=30)
        response.raise_for_status()
        dados = response.json()

        if "daily" not in dados or not dados["daily"]:
            print(f"  [AVISO] Nenhum dado de polen retornado para {data}")
            return None

        daily = dados["daily"]
        idx = 0

        return {
            "data": data,
            "latitude": dados.get("latitude"),
            "longitude": dados.get("longitude"),
            "alder_pollen_mean": daily.get("alder_pollen_mean", [None])[idx],
            "birch_pollen_mean": daily.get("birch_pollen_mean", [None])[idx],
            "olive_pollen_mean": daily.get("olive_pollen_mean", [None])[idx],
            "ragweed_pollen_mean": daily.get("ragweed_pollen_mean", [None])[idx],
            "grass_pollen_mean": daily.get("grass_pollen_mean", [None])[idx],
        }

    except requests.exceptions.RequestException as e:
        print(f"  [ERRO] Falha ao baixar polen para {data}: {e}")
        return None


# ============================================================================
# FUNÇÕES DE SALVAMENTO NO BANCO
# ============================================================================

def salvar_dados_diarios(dados):
    """Salva os dados diários no banco de dados."""
    conn = get_connection()
    cursor = conn.cursor()

    cursor.execute("""
        INSERT OR REPLACE INTO clima_diario (
            data, latitude, longitude, timezone, weathercode,
            temperatura_max, temperatura_min,
            sensacao_termica_max, sensacao_termica_min,
            umidade_max, umidade_min,
            precipitacao_total, probabilidade_chuva,
            chuva_total, aguas_claras_total, neve_total,
            nascer_sol, por_sol, nascer_lua, por_lua,
            uv_max, uv_clear_sky_max,
            vento_max, direcao_vento, rajadas_vento,
            pressao_max, pressao_min,
            pressao_superficie_max, pressao_superficie_min,
            created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        dados["data"], dados["latitude"], dados["longitude"], dados["timezone"],
        dados["weathercode"], dados["temperatura_max"], dados["temperatura_min"],
        dados["sensacao_termica_max"], dados["sensacao_termica_min"],
        dados["umidade_max"], dados["umidade_min"],
        dados["precipitacao_total"], dados["probabilidade_chuva"],
        dados["chuva_total"], dados["aguas_claras_total"], dados["neve_total"],
        dados["nascer_sol"], dados["por_sol"], dados["nascer_lua"], dados["por_lua"],
        dados["uv_max"], dados["uv_clear_sky_max"],
        dados["vento_max"], dados["direcao_vento"], dados["rajadas_vento"],
        dados["pressao_max"], dados["pressao_min"],
        dados["pressao_superficie_max"], dados["pressao_superficie_min"],
        datetime.now().isoformat()
    ))

    conn.commit()
    conn.close()
    print(f"  [OK] Dados diários salvos para {dados['data']}")


def salvar_dados_horarios(lista_horarios):
    """Salva os dados horários no banco de dados."""
    conn = get_connection()
    cursor = conn.cursor()

    for h in lista_horarios:
        cursor.execute("""
            INSERT OR REPLACE INTO clima_horario (
                data, hora, temperatura, sensacao_termica, umidade,
                ponto_orvalho, precipitacao, probabilidade_chuva,
                chuva, aguas_claras, neve, profundidade_neve,
                uv_index, uv_index_clear_sky,
                vento, direcao_vento, rajadas_vento,
                pressao, pressao_superficie,
                cobertura_nuvens, cobertura_nuvens_baixa,
                cobertura_nuvens_media, cobertura_nuvens_alta,
                visibilidade, is_day, weathercode, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            h["data"], h["hora"], h["temperatura"], h["sensacao_termica"],
            h["umidade"], h["ponto_orvalho"], h["precipitacao"],
            h["probabilidade_chuva"], h["chuva"], h["aguas_claras"],
            h["neve"], h["profundidade_neve"], h["uv_index"],
            h["uv_index_clear_sky"], h["vento"], h["direcao_vento"],
            h["rajadas_vento"], h["pressao"], h["pressao_superficie"],
            h["cobertura_nuvens"], h["cobertura_nuvens_baixa"],
            h["cobertura_nuvens_media"], h["cobertura_nuvens_alta"],
            h["visibilidade"], h["is_day"], h["weathercode"],
            datetime.now().isoformat()
        ))

    conn.commit()
    conn.close()
    print(f"  [OK] {len(lista_horarios)} registros horários salvos para {lista_horarios[0]['data']}")


def salvar_qualidade_ar(lista_dados):
    """Salva os dados de qualidade do ar no banco de dados."""
    conn = get_connection()
    cursor = conn.cursor()

    for d in lista_dados:
        cursor.execute("""
            INSERT OR REPLACE INTO qualidade_ar (
                data, hora, pm10, pm2_5, monoxido_carbono,
                nitrogenio_dioxide, enxofre_dioxide, ozonio,
                aerosois, poeira, amonio, radon,
                formaldeido, mercurio, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            d["data"], d["hora"], d["pm10"], d["pm2_5"],
            d["monoxido_carbono"], d["nitrogenio_dioxide"],
            d["enxofre_dioxide"], d["ozonio"], d["aerosois"],
            d["poeira"], d["amonio"], d["radon"],
            d["formaldeido"], d["mercurio"],
            datetime.now().isoformat()
        ))

    conn.commit()
    conn.close()
    print(f"  [OK] {len(lista_dados)} registros de qualidade do ar salvos para {lista_dados[0]['data']}")


def salvar_dados_polen(dados):
    """Salva os dados de polen no banco de dados."""
    conn = get_connection()
    cursor = conn.cursor()

    cursor.execute("""
        INSERT OR REPLACE INTO polen (
            data, latitude, longitude,
            alder_pollen_mean, birch_pollen_mean,
            olive_pollen_mean, ragweed_pollen_mean,
            grass_pollen_mean, created_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        dados["data"], dados["latitude"], dados["longitude"],
        dados["alder_pollen_mean"], dados["birch_pollen_mean"],
        dados["olive_pollen_mean"], dados["ragweed_pollen_mean"],
        dados["grass_pollen_mean"],
        datetime.now().isoformat()
    ))

    conn.commit()
    conn.close()
    print(f"  [OK] Dados de polen salvos para {dados['data']}")


# ============================================================================
# FUNÇÃO PRINCIPAL
# ============================================================================

def processar_data(data):
    """
    Processa uma única data: verifica se já existe no banco,
    e se não existir, baixa e salva todos os dados.
    """
    print(f"\n{'='*60}")
    print(f"Processando: {data}")
    print(f"{'='*60}")

    # --- Dados diários ---
    if data_diaria_existe(data):
        print(f"  [PULADO] Dados diários já existem para {data}")
    else:
        print(f"  Baixando dados diários para {data}...")
        dados_diarios = baixar_dados_diarios(data)
        if dados_diarios:
            salvar_dados_diarios(dados_diarios)
        else:
            print(f"  [FALHA] Não foi possível baixar dados diários para {data}")

    # --- Dados horários ---
    if data_horaria_existe(data):
        print(f"  [PULADO] Dados horários já existem para {data}")
    else:
        print(f"  Baixando dados horários para {data}...")
        dados_horarios = baixar_dados_horarios(data)
        if dados_horarios:
            salvar_dados_horarios(dados_horarios)
        else:
            print(f"  [FALHA] Não foi possível baixar dados horários para {data}")

    # --- Qualidade do ar ---
    if qualidade_ar_existe(data):
        print(f"  [PULADO] Dados de qualidade do ar já existem para {data}")
    else:
        print(f"  Baixando qualidade do ar para {data}...")
        dados_ar = baixar_qualidade_ar(data)
        if dados_ar:
            salvar_qualidade_ar(dados_ar)
        else:
            print(f"  [FALHA] Não foi possível baixar qualidade do ar para {data}")

    # --- Polen ---
    if polen_existe(data):
        print(f"  [PULADO] Dados de polen já existem para {data}")
    else:
        print(f"  Baixando dados de polen para {data}...")
        dados_polen = baixar_dados_polen(data)
        if dados_polen:
            salvar_dados_polen(dados_polen)
        else:
            print(f"  [FALHA] Não foi possível baixar polen para {data}")

    # Pequena pausa entre requisições para não sobrecarregar a API
    time.sleep(1)


def main():
    """Função principal - executa o download para todas as datas do período."""
    print("=" * 60)
    print("DOWNLOAD DE DADOS CLIMÁTICOS - Open-Meteo API (gratuita)")
    print("=" * 60)
    print(f"Localização: {LATITUDE}, {LONGITUDE}")
    print(f"Fuso horário: {FUSO_HORARIO}")
    print(f"Banco de dados: {DB_PATH}")
    print(f"Período: {PERIODO_DOWNLOAD[0]} a {PERIODO_DOWNLOAD[-1]}")
    print(f"Total de datas: {len(PERIODO_DOWNLOAD)}")
    print("=" * 60)

    # Inicializa o banco de dados
    init_database()

    # Processa cada data do array PERIODO_DOWNLOAD (dia por dia)
    total = len(PERIODO_DOWNLOAD)
    for i, data in enumerate(PERIODO_DOWNLOAD, 1):
        print(f"\n[{i}/{total}] ", end="")
        processar_data(data)

    print("\n" + "=" * 60)
    print("DOWNLOAD CONCLUÍDO!")
    print("=" * 60)


if __name__ == "__main__":
    main()

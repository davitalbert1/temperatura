/*
 * download.cpp - Porta C++ de download.py
 *
 * Download de dados climaticos (Open-Meteo) para SQLite, com rate-limit
 * adaptativo (token bucket), retry com backoff exponencial, circuit breaker
 * para HTTP 429 e pipeline de threads produtoras -> fila -> consumidoras.
 *
 * Compilacao (MSYS2/MinGW64):
 *   g++ -std=c++17 -O2 -Wall download.cpp -o download.exe -lcurl -lsqlite3
 *   (ou use compilar_download.bat)
 *
 * Execucao:
 *   download.exe [caminho_do_ini]   (padrao: download.ini)
 *
 * Bibliotecas: libcurl (HTTP), nlohmann/json (JSON), sqlite3 (banco).
 */

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using json = nlohmann::json;

// Utilidades de texto / arquivos .ini
static std::string cortar(const std::string& s) {
    const char* espacos = " \t\r\n";
    size_t i = s.find_first_not_of(espacos);
    if (i == std::string::npos) return "";
    size_t f = s.find_last_not_of(espacos);
    return s.substr(i, f - i + 1);
}

static std::string sem_comentario(const std::string& s) {
    size_t p = s.find_first_of(";#");
    return p == std::string::npos ? s : s.substr(0, p);
}

static std::vector<std::string> dividir(const std::string& s, char sep) {
    std::vector<std::string> partes;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) partes.push_back(cortar(item));
    return partes;
}

static std::string juntar(const std::vector<std::string>& itens, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < itens.size(); ++i) {
        if (i) out += sep;
        out += itens[i];
    }
    return out;
}

// Menor representacao round-trip de um double (igual a str() do Python)
static std::string num_str(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
    std::ostringstream oss;
    oss.precision(std::numeric_limits<double>::max_digits10);
    oss << v;
    return oss.str();
}

struct Ini {
    std::map<std::string, std::map<std::string, std::string>> valores;
    std::map<std::string, std::vector<std::string>> listas;

    bool obter(const std::string& sec, const std::string& chave, std::string& saida) const {
        auto it = valores.find(sec);
        if (it == valores.end()) return false;
        auto it2 = it->second.find(chave);
        if (it2 == it->second.end()) return false;
        saida = it2->second;
        return true;
    }
    std::vector<std::string> lista(const std::string& sec) const {
        auto it = listas.find(sec);
        if (it == listas.end()) return {};
        return it->second;
    }
};

static bool eh_secao_lista(const std::string& s) {
    return s == "coordenadas" || s == "daily_params" || s == "hourly_params" ||
           s == "daily_params_nao_suportados" || s == "air_quality_hourly" ||
           s == "pollen_daily";
}

static Ini ler_ini(const std::string& caminho) {
    Ini ini;
    std::ifstream arquivo(caminho);
    if (!arquivo) {
        std::cerr << "[AVISO] Arquivo de configuracao nao encontrado: " << caminho
                  << " (usando valores padrao)\n";
        return ini;
    }
    std::string linha, secao;
    while (std::getline(arquivo, linha)) {
        linha = cortar(sem_comentario(linha));
        if (linha.empty()) continue;
        if (linha.front() == '[' && linha.back() == ']') {
            secao = cortar(linha.substr(1, linha.size() - 2));
            continue;
        }
        if (secao.empty()) continue;
        if (secao == "coordenadas") {
            if (!linha.empty()) ini.listas[secao].push_back(linha);
            continue;
        }
        if (eh_secao_lista(secao)) {
            for (auto& token : dividir(linha, ','))
                if (!token.empty()) ini.listas[secao].push_back(token);
            continue;
        }
        size_t igual = linha.find('=');
        if (igual == std::string::npos) continue;
        std::string chave = cortar(linha.substr(0, igual));
        std::string valor = cortar(linha.substr(igual + 1));
        if (!chave.empty()) ini.valores[secao][chave] = valor;
    }
    return ini;
}

// Variaveis de configuracao (todas vindas do .ini; env var sobrescreve)
static std::string env_texto(const char* nome, const std::string& padrao) {
    const char* v = std::getenv(nome);
    return v ? std::string(v) : padrao;
}

static int env_int(const char* nome, int padrao) {
    const char* v = std::getenv(nome);
    if (!v || !*v) return padrao;
    try {
        return std::stoi(v);
    } catch (...) {
        return padrao;
    }
}

static double env_double(const char* nome, double padrao) {
    const char* v = std::getenv(nome);
    if (!v || !*v) return padrao;
    try {
        return std::stod(v);
    } catch (...) {
        return padrao;
    }
}

static int para_int(const std::string& s, int padrao) {
    try {
        return std::stoi(cortar(s));
    } catch (...) {
        return padrao;
    }
}

static double para_double(const std::string& s, double padrao) {
    try {
        return std::stod(cortar(s));
    } catch (...) {
        return padrao;
    }
}

struct Config {
    // [geral] - espelham as variaveis do download.py
    std::string db_path = "clima.db";
    int max_sql_dates_por_lote = 900;
    int download_workers = 5;
    int process_workers = 2;
    int chunk_dias = 30;
    double requests_per_second = 4.0;
    int max_retries = 8;
    double http_timeout = 120.0;
    double jitter_inicial_max = 5.0;
    double backoff_inicial = 2.0;
    double backoff_maximo = 60.0;
    double min_rps = 5.0;
    double cooldown_429 = 30.0;
    int circuit_limit_429 = 3;
    double espera_min_429 = 5.0;
    std::string fuso_horario = "America/Sao_Paulo";
    std::string periodo_inicio = "1940-01-01";
    std::string periodo_fim = "2025-12-31";
    std::string data_minima_qualidade_ar = "2013-01-01";

    // [urls]
    std::string url_forecast = "https://api.open-meteo.com/v1/forecast";
    std::string url_archive = "https://archive-api.open-meteo.com/v1/archive";
    std::string url_air_quality = "https://air-quality-api.open-meteo.com/v1/air-quality";
    std::string url_pollen = "https://api.open-meteo.com/v1/pollen";

    // [coordenadas] - padrao: as mesmas do download.py
    std::vector<std::pair<double, double>> coordenadas = {
        {-23.5505, -46.6333}, // Sao Paulo, Brasil
        {-26.3044, -48.8456}, // Joinville, Brasil
        {-25.4278, -49.2731}, // Curitiba, Brasil
        {-28.7833, -51.6100}, // Guapore, Brasil
        {-20.4697, -54.6201}, // Campo Grande, Brasil
        {-3.1019, -60.0250}, // Manaus, Brasil
        {-3.7250, -38.5236}, // Fortaleza, Brasil
        {52.5200, 13.4050}, // Berlim, Alemanha
        {35.6762, 139.6503}, // Tquio, Japao
        {40.7128, -74.0060}, // Nova York, Estados Unidos
        {55.7558, 37.6173}, // Moscou, Russia
        {43.1155, 131.8855}, // Vladivostok, Russia
        {-33.4489, -70.6693}, // Santiago, Chile
        {-54.8019, -68.3030}, // Ushuaia, Argentina
        {-33.9249, 18.4241}, // Cidade do Cabo, Africa do Sul
        {19.4326, -99.1332}, // Cidade do Mexico, Mexico
        {61.2181, -149.9003}, // Anchorage, Estados Unidos
    };

    // listas de parametros da API
    std::vector<std::string> daily_params = {
        "weathercode", "temperature_2m_max", "temperature_2m_min",
        "apparent_temperature_max", "apparent_temperature_min",
        "relativehumidity_2m_max", "relativehumidity_2m_min", "precipitation_sum",
        "precipitation_probability_max", "rain_sum", "showers_sum", "snowfall_sum",
        "sunrise", "sunset", "uv_index_max", "uv_index_clear_sky_max",
        "windspeed_10m_max", "winddirection_10m_dominant", "windgusts_10m_max",
        "pressure_msl_max", "pressure_msl_min", "surface_pressure_max",
        "surface_pressure_min",
    };
    std::vector<std::string> hourly_params = {
        "temperature_2m", "apparent_temperature", "relativehumidity_2m", "dewpoint_2m",
        "precipitation", "precipitation_probability", "rain", "showers", "snowfall",
        "snow_depth", "uv_index", "uv_index_clear_sky", "windspeed_10m",
        "winddirection_10m", "windgusts_10m", "pressure_msl", "surface_pressure",
        "cloudcover", "cloudcover_low", "cloudcover_mid", "cloudcover_high",
        "visibility", "is_day", "weathercode",
    };
    std::set<std::string> daily_nao_suportados = {
        "relativehumidity_2m_max", "relativehumidity_2m_min", "moonrise", "moonset",
    };
    std::vector<std::string> air_quality_hourly = {
        "pm10", "pm2_5", "carbon_monoxide", "nitrogen_dioxide", "sulphur_dioxide",
        "ozone", "dust", "formaldehyde",
    };
    std::vector<std::string> pollen_daily = {
        "alder_pollen_mean", "birch_pollen_mean", "olive_pollen_mean",
        "ragweed_pollen_mean", "grass_pollen_mean",
    };
};

Config CFG;

static void carregar_config(const std::string& caminho) {
    Ini ini = ler_ini(caminho);
    std::string v;

    auto le_texto = [&](const char* sec, const char* chave, std::string& alvo) {
        if (ini.obter(sec, chave, v)) alvo = v;
    };
    auto le_int = [&](const char* chave, const char* env, int& alvo) {
        if (ini.obter("geral", chave, v)) alvo = para_int(v, alvo);
        if (env) alvo = env_int(env, alvo);
    };
    auto le_dbl = [&](const char* chave, const char* env, double& alvo) {
        if (ini.obter("geral", chave, v)) alvo = para_double(v, alvo);
        if (env) alvo = env_double(env, alvo);
    };

    le_texto("geral", "db_path", CFG.db_path);
    le_int("max_sql_dates_por_lote", nullptr, CFG.max_sql_dates_por_lote);
    le_int("download_workers", "OPENMETEO_DOWNLOAD_WORKERS", CFG.download_workers);
    le_int("process_workers", "OPENMETEO_PROCESS_WORKERS", CFG.process_workers);
    le_int("chunk_dias", "OPENMETEO_CHUNK_DIAS", CFG.chunk_dias);
    le_dbl("requests_per_second", "OPENMETEO_RPS", CFG.requests_per_second);
    le_int("max_retries", "OPENMETEO_RETRIES", CFG.max_retries);
    le_dbl("http_timeout", "OPENMETEO_TIMEOUT", CFG.http_timeout);
    le_dbl("jitter_inicial_max", "OPENMETEO_JITTER", CFG.jitter_inicial_max);
    le_dbl("backoff_inicial", "OPENMETEO_BACKOFF", CFG.backoff_inicial);
    le_dbl("backoff_maximo", "OPENMETEO_BACKOFF_MAX", CFG.backoff_maximo);
    le_dbl("min_rps", "OPENMETEO_MIN_RPS", CFG.min_rps);
    le_dbl("cooldown_429", "OPENMETEO_COOLDOWN_429", CFG.cooldown_429);
    le_int("circuit_limit_429", "OPENMETEO_429_CIRCUIT_LIMIT", CFG.circuit_limit_429);
    le_dbl("espera_min_429", "OPENMETEO_429_ESPERA_MIN", CFG.espera_min_429);
    le_texto("geral", "fuso_horario", CFG.fuso_horario);
    le_texto("geral", "periodo_inicio", CFG.periodo_inicio);
    le_texto("geral", "periodo_fim", CFG.periodo_fim);
    le_texto("geral", "data_minima_qualidade_ar", CFG.data_minima_qualidade_ar);

    le_texto("urls", "forecast", CFG.url_forecast);
    le_texto("urls", "archive", CFG.url_archive);
    le_texto("urls", "air_quality", CFG.url_air_quality);
    le_texto("urls", "pollen", CFG.url_pollen);

    auto coord_linhas = ini.lista("coordenadas");
    if (!coord_linhas.empty()) {
        std::vector<std::pair<double, double>> coords;
        for (const auto& l : coord_linhas) {
            auto partes = dividir(l, ',');
            if (partes.size() < 2) {
                std::cerr << "[AVISO] Coordenada invalida ignorada: " << l << "\n";
                continue;
            }
            try {
                coords.emplace_back(std::stod(partes[0]), std::stod(partes[1]));
            } catch (...) {
                std::cerr << "[AVISO] Coordenada invalida ignorada: " << l << "\n";
            }
        }
        if (!coords.empty()) CFG.coordenadas = coords;
    }

    auto le_lista = [&](const char* sec, std::vector<std::string>& alvo) {
        auto l = ini.lista(sec);
        if (!l.empty()) alvo = l;
    };
    le_lista("daily_params", CFG.daily_params);
    le_lista("hourly_params", CFG.hourly_params);
    le_lista("air_quality_hourly", CFG.air_quality_hourly);
    le_lista("pollen_daily", CFG.pollen_daily);
    auto ns = ini.lista("daily_params_nao_suportados");
    if (!ns.empty()) CFG.daily_nao_suportados = std::set<std::string>(ns.begin(), ns.end());

    if (CFG.download_workers < 1) CFG.download_workers = 1;
    if (CFG.process_workers < 1) CFG.process_workers = 1;
    if (CFG.chunk_dias < 1) CFG.chunk_dias = 1;
    if (CFG.max_sql_dates_por_lote < 1) CFG.max_sql_dates_por_lote = 1;
    if (CFG.requests_per_second <= 0.0) CFG.requests_per_second = 1.0;
    if (CFG.max_retries < 1) CFG.max_retries = 1;
}

// Log + utilidades de data
static std::mutex g_print_mtx;
static std::mutex g_write_mtx; // serializa gravacoes no banco (equivale write_lock)

static void log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_print_mtx);
    std::cout << msg << std::endl;
}

struct Data {
    int ano = 0, mes = 0, dia = 0;
};

static Data para_data(const std::string& s) {
    Data d;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &d.ano, &d.mes, &d.dia) != 3 || d.mes < 1 ||
        d.mes > 12 || d.dia < 1 || d.dia > 31)
        throw std::runtime_error("data invalida: " + s);
    return d;
}

// Algoritmo de Howard Hinnant (days_from_civil / civil_from_days)
static long dias_de_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

static Data civil_de_dias(long z) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = static_cast<long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    Data r;
    r.ano = static_cast<int>(y + (m <= 2));
    r.mes = static_cast<int>(m);
    r.dia = static_cast<int>(d);
    return r;
}

static std::string data_texto(const Data& d) {
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", d.ano, d.mes, d.dia);
    return b;
}

static std::tm agora_local() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    #ifdef _WIN32
        localtime_s(&tmv, &t);
    #else
        localtime_r(&t, &tmv);
    #endif
    return tmv;
}

// Data local no formato YYYY-MM-DD (compara lexicograficamente com as datas)
static std::string hoje() {
    std::tm tmv = agora_local();
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return b;
}

static std::string agora_iso() {
    std::tm tmv = agora_local();
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}

static std::vector<std::string> expandir_periodo(const std::string& inicio, const std::string& fim) {
    try {
        Data a = para_data(inicio), b = para_data(fim);
        long ia = dias_de_civil(a.ano, a.mes, a.dia);
        long ib = dias_de_civil(b.ano, b.mes, b.dia);
        std::vector<std::string> datas;
        for (long i = ia; i <= ib; ++i) datas.push_back(data_texto(civil_de_dias(i)));
        return datas;
    } catch (const std::exception& e) {
        log(std::string("[AVISO] Erro ao processar datas: ") + e.what());
        return {inicio, fim};
    }
}

// Rate-limit (token bucket adaptativo) + backoff
static std::mutex g_rate_mtx;
static double g_tokens = 0.0;
static double g_rps_atual = 0.0;
static std::chrono::steady_clock::time_point g_ultimo_refil;
static std::chrono::steady_clock::time_point g_cooldown_ate;
static int g_429_consecutivos = 0;

static void iniciar_rate_limit() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_tokens = CFG.requests_per_second;
    g_rps_atual = CFG.requests_per_second;
    g_ultimo_refil = std::chrono::steady_clock::now();
    g_cooldown_ate = std::chrono::steady_clock::time_point{};
    g_429_consecutivos = 0;
}

// Gerador por thread (thread-safe por construcao)
static std::mt19937& rng() {
    static thread_local std::mt19937 gen{
        static_cast<std::mt19937::result_type>(std::random_device{}() ^
                                               std::hash<std::thread::id>{}(
                                                   std::this_thread::get_id()))};
    return gen;
}

static void dormir(double segundos) {
    if (segundos > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(segundos));
}

// Deve ser chamada com g_rate_mtx segurado
static double proxima_espera() {
    auto agora = std::chrono::steady_clock::now();
    if (agora < g_cooldown_ate)
        return std::chrono::duration<double>(g_cooldown_ate - agora).count();

    double desde = std::chrono::duration<double>(agora - g_ultimo_refil).count();
    g_tokens = std::min(g_rps_atual, g_tokens + desde * g_rps_atual);
    g_ultimo_refil = agora;

    if (g_tokens >= 1.0) {
        g_tokens -= 1.0;
        return 0.0;
    }
    return (1.0 - g_tokens) / g_rps_atual;
}

static void esperar_rate_limit() {
    double espera;
    {
        std::lock_guard<std::mutex> lk(g_rate_mtx);
        espera = proxima_espera();
    }
    dormir(espera);
}

static void reduzir_taxa_429() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_429_consecutivos++;
    g_rps_atual = std::max(CFG.min_rps, g_rps_atual * 0.5);
    g_tokens = std::min(g_tokens, g_rps_atual);
    if (g_429_consecutivos >= CFG.circuit_limit_429) {
        g_cooldown_ate = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(CFG.cooldown_429));
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "  [429] Muitas respostas 429. Pausa global de %.0fs "
                      "(taxa: %.2f req/s)",
                      CFG.cooldown_429, g_rps_atual);
        log(buf);
        g_429_consecutivos = 0;
    }
}

static void restaurar_taxa() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_429_consecutivos = 0;
    if (g_rps_atual < CFG.requests_per_second)
        g_rps_atual = std::min(CFG.requests_per_second, g_rps_atual * 1.1);
}

static void esperar_cooldown_global() {
    double espera;
    {
        std::lock_guard<std::mutex> lk(g_rate_mtx);
        espera = std::chrono::duration<double>(g_cooldown_ate - std::chrono::steady_clock::now()).count();
    }
    if (espera > 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  [PAUSA GLOBAL] Aguardando %.1fs (cooldown 429)...", espera);
        log(buf);
        dormir(espera);
    }
}

static double backoff_exponencial(int tentativa) {
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    double bruto = CFG.backoff_inicial * std::pow(2.0, tentativa - 1);
    return std::min(CFG.backoff_maximo, bruto * dist(rng()));
}

// Banco de dados (SQLite) - uma conexao por thread, como no Python
static void executar_sql(sqlite3* conn, const char* sql) {
    char* erro = nullptr;
    if (sqlite3_exec(conn, sql, nullptr, nullptr, &erro) != SQLITE_OK) {
        std::string msg = erro ? erro : "erro desconhecido";
        sqlite3_free(erro);
        throw std::runtime_error("SQLite: " + msg);
    }
}

static sqlite3* get_connection() {
    static thread_local sqlite3* conn = nullptr;
    if (!conn) {
        if (sqlite3_open(CFG.db_path.c_str(), &conn) != SQLITE_OK) {
            std::string msg = conn ? sqlite3_errmsg(conn) : "falha ao abrir";
            if (conn) sqlite3_close(conn);
            conn = nullptr;
            throw std::runtime_error("SQLite: nao foi possivel abrir " + CFG.db_path + ": " + msg);
        }
        executar_sql(conn, "PRAGMA journal_mode=WAL");
        executar_sql(conn, "PRAGMA synchronous=NORMAL");
        executar_sql(conn, "PRAGMA busy_timeout=60000");
    }
    return conn;
}

static void init_database() {
    sqlite3* conn = get_connection();

    // Migracao: detecta schema antigo (tabela sem coluna latitude)
    std::vector<std::string> colunas;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, "PRAGMA table_info(clima_horario)", -1, &stmt, nullptr) ==
        SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* nome = sqlite3_column_text(stmt, 1);
            if (nome) colunas.emplace_back(reinterpret_cast<const char*>(nome));
        }
    }
    sqlite3_finalize(stmt);

    if (!colunas.empty() &&
        std::find(colunas.begin(), colunas.end(), "latitude") == colunas.end()) {
        log("[DB] Schema antigo detectado. Recriando tabelas...");
        for (const char* tabela : {"clima_diario", "clima_horario", "qualidade_ar", "polen"}) {
            executar_sql(conn, (std::string("DROP TABLE IF EXISTS ") + tabela).c_str());
        }
    }

    executar_sql(conn, R"SQL(
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
    )SQL");

    executar_sql(conn, R"SQL(
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
    )SQL");

    executar_sql(conn, R"SQL(
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
    )SQL");

    executar_sql(conn, R"SQL(
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
    )SQL");

    // Indices
    log("[DB] Criando indices...");
    const std::vector<std::pair<std::string, std::string>> idx_loc = {
        {"clima_diario", "latitude, longitude, data"},
        {"clima_horario", "latitude, longitude, data"},
        {"qualidade_ar", "latitude, longitude, data"},
        {"polen", "latitude, longitude, data"},
    };
    for (const auto& [tabela, coluna] : idx_loc) {
        executar_sql(conn, ("CREATE INDEX IF NOT EXISTS idx_" + tabela + "_loc ON " +
                            tabela + "(" + coluna + ")")
                               .c_str());
    }
    for (const char* tabela : {"clima_diario", "clima_horario", "qualidade_ar", "polen"}) {
        executar_sql(conn, ("CREATE INDEX IF NOT EXISTS idx_" + std::string(tabela) +
                            "_data ON " + tabela + "(data)")
                               .c_str());
    }

    log("[DB] Banco inicializado.");
}

// Registros existentes no banco (carregados uma unica vez)
using Chave = std::pair<double, double>;
using ConjuntoDatas = std::set<std::string>;

struct Existentes {
    // tipo -> (latitude, longitude) -> datas ja presentes
    std::map<std::string, std::map<Chave, ConjuntoDatas>> dados;
};

// Produtores fazem leitura compartilhada; consumidores fazem escrita exclusiva
static std::shared_mutex g_mtx_existentes;

static const ConjuntoDatas& datas_vazias() {
    static const ConjuntoDatas vazio;
    return vazio;
}

static const ConjuntoDatas& presentes(const Existentes& e, const std::string& tipo,
                                      const Chave& chave) {
    auto it_t = e.dados.find(tipo);
    if (it_t == e.dados.end()) return datas_vazias();
    auto it = it_t->second.find(chave);
    if (it == it_t->second.end()) return datas_vazias();
    return it->second;
}

static std::vector<std::string> filtrar_faltantes(const std::vector<std::string>& datas,
                                                  const ConjuntoDatas& existentes) {
    std::vector<std::string> out;
    out.reserve(datas.size());
    for (const auto& d : datas) {
        if (!existentes.count(d)) out.push_back(d);
    }
    return out;
}

// Espelha _datas_esperadas() do download.py
static ConjuntoDatas datas_esperadas(const std::string& tipo,
                                     const std::vector<std::string>& datas) {
    ConjuntoDatas out;
    if (tipo == "qualidade_ar") {
        for (const auto& d : datas) {
            if (d >= CFG.data_minima_qualidade_ar) out.insert(d);
        }
    } else if (tipo == "polen") {
        std::string h = hoje();
        for (const auto& d : datas) {
            if (d >= h) out.insert(d);
        }
    } else {
        out.insert(datas.begin(), datas.end());
    }
    return out;
}

static Existentes carregar_existentes(const std::vector<std::string>& datas,
                                      const std::vector<std::pair<double, double>>& coordenadas) {
    sqlite3* conn = get_connection();
    Existentes existentes;
    const std::map<std::string, std::string> tabelas = {
        {"diario", "clima_diario"},
        {"horario", "clima_horario"},
        {"qualidade_ar", "qualidade_ar"},
        {"polen", "polen"},
    };

    std::set<Chave> coords_alvo;
    for (const auto& c : coordenadas) coords_alvo.insert(c);

    for (const auto& [tipo, tabela] : tabelas) {
        // Filtra datas por tipo
        std::vector<std::string> datas_filtradas;
        if (tipo == "qualidade_ar") {
            for (const auto& d : datas) {
                if (d >= CFG.data_minima_qualidade_ar) datas_filtradas.push_back(d);
            }
        } else if (tipo == "polen") {
            std::string h = hoje();
            for (const auto& d : datas) {
                if (d >= h) datas_filtradas.push_back(d);
            }
        } else {
            datas_filtradas = datas;
        }
        if (datas_filtradas.empty()) continue;

        ConjuntoDatas datas_set(datas_filtradas.begin(), datas_filtradas.end());
        auto& mapa = existentes.dados[tipo];
        for (const auto& c : coords_alvo) mapa[c] = ConjuntoDatas();

        // 1 unica consulta por tabela: apenas data, latitude e longitude
        std::string sql = "SELECT latitude, longitude, data FROM " + tabela +
                          " WHERE data BETWEEN ? AND ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::string err = sqlite3_errmsg(conn);
            sqlite3_finalize(stmt);
            throw std::runtime_error("SQLite: " + err);
        }
        sqlite3_bind_text(stmt, 1, datas_filtradas.front().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, datas_filtradas.back().c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            double lat = sqlite3_column_double(stmt, 0);
            double lon = sqlite3_column_double(stmt, 1);
            const unsigned char* txt = sqlite3_column_text(stmt, 2);
            if (!txt) continue;
            std::string data(reinterpret_cast<const char*>(txt));
            if (!datas_set.count(data)) continue;
            Chave chave{lat, lon};
            if (!coords_alvo.count(chave)) continue;
            mapa[chave].insert(data);
        }
        sqlite3_finalize(stmt);
    }
    return existentes;
}

// Parsing generico de registros JSON -> colunas do banco
using Valor = std::variant<std::monostate, double, std::string>;
using Registro = std::map<std::string, Valor>;

struct CampoMapa {
    const char* campo; // coluna no banco
    const char* chave; // chave no JSON
};

// Campos especiais ("data", "hora", "timezone") sao tratados em parse_registros;
// nos demais, chave == campo (com as excecoes listadas abaixo)
static const std::vector<CampoMapa> MAPA_DIARIO = {
    {"data", "time"},
    {"timezone", "timezone"},
    {"weathercode", "weathercode"},
    {"temperatura_max", "temperature_2m_max"},
    {"temperatura_min", "temperature_2m_min"},
    {"sensacao_termica_max", "apparent_temperature_max"},
    {"sensacao_termica_min", "apparent_temperature_min"},
    {"umidade_max", "relativehumidity_2m_max"},
    {"umidade_min", "relativehumidity_2m_min"},
    {"precipitacao_total", "precipitation_sum"},
    {"probabilidade_chuva", "precipitation_probability_max"},
    {"chuva_total", "rain_sum"},
    {"aguas_claras_total", "showers_sum"},
    {"neve_total", "snowfall_sum"},
    {"nascer_sol", "sunrise"},
    {"por_sol", "sunset"},
    {"uv_max", "uv_index_max"},
    {"uv_clear_sky_max", "uv_index_clear_sky_max"},
    {"vento_max", "windspeed_10m_max"},
    {"direcao_vento", "winddirection_10m_dominant"},
    {"rajadas_vento", "windgusts_10m_max"},
    {"pressao_max", "pressure_msl_max"},
    {"pressao_min", "pressure_msl_min"},
    {"pressao_superficie_max", "surface_pressure_max"},
    {"pressao_superficie_min", "surface_pressure_min"},
};

static const std::vector<CampoMapa> MAPA_HORARIO = {
    {"data", "time"},
    {"hora", "time"},
    {"temperatura", "temperature_2m"},
    {"sensacao_termica", "apparent_temperature"},
    {"umidade", "relativehumidity_2m"},
    {"ponto_orvalho", "dewpoint_2m"},
    {"precipitacao", "precipitation"},
    {"probabilidade_chuva", "precipitation_probability"},
    {"chuva", "rain"},
    {"aguas_claras", "showers"},
    {"neve", "snowfall"},
    {"profundidade_neve", "snow_depth"},
    {"uv_index", "uv_index"},
    {"uv_index_clear_sky", "uv_index_clear_sky"},
    {"vento", "windspeed_10m"},
    {"direcao_vento", "winddirection_10m"},
    {"rajadas_vento", "windgusts_10m"},
    {"pressao", "pressure_msl"},
    {"pressao_superficie", "surface_pressure"},
    {"cobertura_nuvens", "cloudcover"},
    {"cobertura_nuvens_baixa", "cloudcover_low"},
    {"cobertura_nuvens_media", "cloudcover_mid"},
    {"cobertura_nuvens_alta", "cloudcover_high"},
    {"visibilidade", "visibility"},
    {"is_day", "is_day"},
    {"weathercode", "weathercode"},
};

static const std::vector<CampoMapa> MAPA_QUALIDADE_AR = {
    {"data", "time"},
    {"hora", "time"},
    {"pm10", "pm10"},
    {"pm2_5", "pm2_5"},
    {"monoxido_carbono", "carbon_monoxide"},
    {"nitrogenio_dioxide", "nitrogen_dioxide"},
    {"enxofre_dioxide", "sulphur_dioxide"},
    {"ozonio", "ozone"},
    {"aerosois", "aerosol"},
    {"poeira", "dust"},
    {"formaldeido", "formaldehyde"},
};

static const std::vector<CampoMapa> MAPA_POLEN = {
    {"data", "time"},
    {"alder_pollen_mean", "alder_pollen_mean"},
    {"birch_pollen_mean", "birch_pollen_mean"},
    {"olive_pollen_mean", "olive_pollen_mean"},
    {"ragweed_pollen_mean", "ragweed_pollen_mean"},
    {"grass_pollen_mean", "grass_pollen_mean"},
};

static const std::vector<CampoMapa>* mapa_do_tipo(const std::string& tipo) {
    if (tipo == "diario") return &MAPA_DIARIO;
    if (tipo == "horario") return &MAPA_HORARIO;
    if (tipo == "qualidade_ar") return &MAPA_QUALIDADE_AR;
    if (tipo == "polen") return &MAPA_POLEN;
    return nullptr;
}

// Equivalente a _get_valor() do download.py
static Valor pegar(const json& dados, const std::string& chave, size_t i) {
    if (!dados.is_object()) return Valor{};
    auto it = dados.find(chave);
    if (it == dados.end() || !it->is_array() || i >= it->size()) return Valor{};
    const json& el = (*it)[i];
    if (el.is_number()) return Valor{el.get<double>()};
    if (el.is_boolean()) return Valor{el.get<bool>() ? 1.0 : 0.0};
    if (el.is_string()) return Valor{el.get<std::string>()};
    return Valor{};
}

static std::vector<Registro> parse_registros(const json& dados, double lat, double lon,
                                             const std::string& tipo,
                                             const std::set<std::string>* filtro) {
    const auto* mapa = mapa_do_tipo(tipo);
    if (!mapa || !dados.is_object()) return {};

    std::vector<std::string> times;
    if (dados.contains("time") && dados["time"].is_array()) {
        for (const auto& t : dados["time"]) {
            if (t.is_string()) times.push_back(t.get<std::string>());
        }
    }

    std::vector<Registro> registros;
    registros.reserve(times.size());
    for (size_t i = 0; i < times.size(); ++i) {
        const std::string& t = times[i];
        std::string data = t.substr(0, std::min<size_t>(10, t.size()));
        if (filtro && !filtro->count(data)) continue;

        Registro reg;
        reg["latitude"] = lat;
        reg["longitude"] = lon;
        for (const auto& cm : *mapa) {
            if (std::strcmp(cm.campo, "data") == 0) {
                reg[cm.campo] = data;
            } else if (std::strcmp(cm.campo, "hora") == 0) {
                reg[cm.campo] = t;
            } else if (std::strcmp(cm.campo, "timezone") == 0) {
                if (dados.contains("timezone") && dados["timezone"].is_string())
                    reg[cm.campo] = dados["timezone"].get<std::string>();
                else
                    reg[cm.campo] = Valor{};
            } else {
                reg[cm.campo] = pegar(dados, cm.chave, i);
            }
        }
        registros.push_back(std::move(reg));
    }
    return registros;
}

// HTTP (libcurl) + retry/backoff
struct RespostaHttp {
    bool erro_rede = false; // timeout/conexao (equivale Timeout/ConnectionError)
    std::string erro_mensagem; // descricao do erro de rede
    long status = 0; // HTTP status (0 se erro de rede)
    std::string corpo;
    double retry_after = -1.0; // header Retry-After, se presente
};

static CURL* curl_da_thread() {
    static thread_local CURL* handle = curl_easy_init();
    return handle;
}

static size_t escrever_corpo(char* ptr, size_t tam, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, tam * nmemb);
    return tam * nmemb;
}

static size_t ler_header(char* buffer, size_t tam, size_t n, void* ud) {
    std::string linha(buffer, tam * n);
    std::string baixo = linha;
    std::transform(baixo.begin(), baixo.end(), baixo.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (baixo.rfind("retry-after:", 0) == 0) {
        std::string valor = cortar(linha.substr(12));
        try {
            static_cast<RespostaHttp*>(ud)->retry_after = std::stod(valor);
        } catch (...) {}
    }
    return tam * n;
}

static std::string escapar(const std::string& s) {
    CURL* c = curl_da_thread();
    if (!c) return s;
    char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string r = e ? e : s;
    if (e) curl_free(e);
    return r;
}

static std::string montar_url(const std::string& base,
                              const std::map<std::string, std::string>& params) {
    std::string url = base;
    bool primeiro = true;
    for (const auto& [k, v] : params) {
        url += (primeiro ? "?" : "&");
        primeiro = false;
        url += escapar(k) + "=" + escapar(v);
    }
    return url;
}

static RespostaHttp http_get(const std::string& url) {
    RespostaHttp r;
    CURL* curl = curl_da_thread();
    if (!curl) {
        r.erro_rede = true;
        r.erro_mensagem = "falha ao iniciar curl";
        return r;
    }
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escrever_corpo);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.corpo);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ler_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(CFG.http_timeout * 1000.0));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // gzip automatico

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.erro_rede = true;
        r.erro_mensagem = curl_easy_strerror(rc);
        r.corpo.clear();
        return r;
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    r.status = status;
    return r;
}

static std::optional<json> baixar_com_retry(const std::string& url,
                                            const std::map<std::string, std::string>& params,
                                            int max_tentativas) {
    std::string url_completa = montar_url(url, params);
    for (int tentativa = 1; tentativa <= max_tentativas; ++tentativa) {
        esperar_cooldown_global();
        esperar_rate_limit();

        RespostaHttp r = http_get(url_completa);

        if (!r.erro_rede && r.status < 400) {
            try {
                restaurar_taxa();
                return json::parse(r.corpo);
            } catch (const std::exception& e) {
                if (tentativa == max_tentativas) {
                    log("  [ERRO] Resposta sem JSON valido apos " +
                        std::to_string(max_tentativas) + " tentativas: " + e.what());
                    return std::nullopt;
                }
                double espera = backoff_exponencial(tentativa);
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "  [RETRY %d/%d] Resposta sem JSON valido. Aguardando %.1fs...",
                              tentativa, max_tentativas, espera);
                log(buf);
                dormir(espera);
                continue;
            }
        }

        if (!r.erro_rede && r.status == 429) {
            reduzir_taxa_429();
            double espera =
                std::max(CFG.espera_min_429,
                         r.retry_after > 0 ? r.retry_after : backoff_exponencial(tentativa));
            if (tentativa == max_tentativas) {
                log("  [ERRO] 429 persistente apos " + std::to_string(max_tentativas) +
                    " tentativas");
                return std::nullopt;
            }
            char buf[200];
            std::snprintf(buf, sizeof(buf), "  [RETRY %d/%d] HTTP 429. Aguardando %.1fs...",
                          tentativa, max_tentativas, espera);
            log(buf);
            dormir(espera);
        } else if (!r.erro_rede) {
            if (tentativa == max_tentativas) {
                log("  [ERRO] Falha apos " + std::to_string(max_tentativas) +
                    " tentativas: HTTP " + std::to_string(r.status));
                return std::nullopt;
            }
            double espera = backoff_exponencial(tentativa);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "  [RETRY %d/%d] HTTP %ld. Aguardando %.1fs...",
                          tentativa, max_tentativas, r.status, espera);
            log(buf);
            dormir(espera);
        } else {
            if (tentativa == max_tentativas) {
                log("  [ERRO] Falha apos " + std::to_string(max_tentativas) +
                    " tentativas: " + r.erro_mensagem);
                return std::nullopt;
            }
            double espera = backoff_exponencial(tentativa);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "  [RETRY %d/%d] %s. Aguardando %.1fs...",
                          tentativa, max_tentativas, r.erro_mensagem.c_str(), espera);
            log(buf);
            dormir(espera);
        }
    }
    return std::nullopt;
}

// Download de blocos de dados
static std::string get_url_clima(const std::string& data) {
    // YYYY-MM-DD compara lexicograficamente; datas invalidas vao para o forecast
    try {
        para_data(data);
    } catch (...) {
        return CFG.url_forecast;
    }
    return data < hoje() ? CFG.url_archive : CFG.url_forecast;
}

// Agrupa datas por URL (archive x forecast) em blocos de CFG.chunk_dias
static std::vector<std::pair<std::string, std::vector<std::string>>>
agrupar_por_url(const std::vector<std::string>& datas) {
    std::vector<std::pair<std::string, std::vector<std::string>>> grupos;
    for (const std::string& url : {CFG.url_archive, CFG.url_forecast}) {
        std::vector<std::string> lista;
        for (const auto& d : datas) {
            if (get_url_clima(d) == url) lista.push_back(d);
        }
        std::sort(lista.begin(), lista.end());
        for (size_t i = 0; i < lista.size(); i += CFG.chunk_dias) {
            size_t f = std::min(i + static_cast<size_t>(CFG.chunk_dias), lista.size());
            grupos.emplace_back(url, std::vector<std::string>(lista.begin() + i, lista.begin() + f));
        }
    }
    return grupos;
}

struct ResultadoClima {
    std::vector<Registro> diario;
    std::vector<Registro> horario;
};

static ResultadoClima baixar_clima_bloco(const std::vector<std::string>& datas, double lat,
                                         double lon) {
    ResultadoClima out;
    if (datas.empty()) return out;

    for (const auto& [url, grupo] : agrupar_por_url(datas)) {
        // No archive, alguns params diarios nao sao suportados
        std::vector<std::string> daily_params;
        for (const auto& p : CFG.daily_params) {
            if (url == CFG.url_archive && CFG.daily_nao_suportados.count(p)) continue;
            daily_params.push_back(p);
        }

        std::map<std::string, std::string> params = {
            {"latitude", num_str(lat)},
            {"longitude", num_str(lon)},
            {"start_date", grupo.front()},
            {"end_date", grupo.back()},
            {"daily", juntar(daily_params, ",")},
            {"hourly", juntar(CFG.hourly_params, ",")},
            {"timezone", CFG.fuso_horario},
        };

        auto dados = baixar_com_retry(url, params, CFG.max_retries);
        if (!dados) continue;

        std::set<std::string> filtro(grupo.begin(), grupo.end());
        if (dados->contains("daily") && (*dados)["daily"].is_object()) {
            auto regs = parse_registros((*dados)["daily"], lat, lon, "diario", &filtro);
            out.diario.insert(out.diario.end(), std::make_move_iterator(regs.begin()),
                              std::make_move_iterator(regs.end()));
        }
        if (dados->contains("hourly") && (*dados)["hourly"].is_object()) {
            auto regs = parse_registros((*dados)["hourly"], lat, lon, "horario", &filtro);
            out.horario.insert(out.horario.end(), std::make_move_iterator(regs.begin()),
                               std::make_move_iterator(regs.end()));
        }
    }
    return out;
}

static std::vector<Registro> baixar_qualidade_ar_bloco(const std::vector<std::string>& datas,
                                                       double lat, double lon) {
    std::vector<std::string> alvo;
    for (const auto& d : datas)
        if (d >= CFG.data_minima_qualidade_ar) alvo.push_back(d);
    if (alvo.empty()) return {};

    std::map<std::string, std::string> params = {
        {"latitude", num_str(lat)},
        {"longitude", num_str(lon)},
        {"start_date", alvo.front()},
        {"end_date", alvo.back()},
        {"hourly", juntar(CFG.air_quality_hourly, ",")},
        {"timezone", CFG.fuso_horario},
    };

    auto dados = baixar_com_retry(CFG.url_air_quality, params, CFG.max_retries);
    if (!dados || !dados->contains("hourly") || !(*dados)["hourly"].is_object()) return {};

    std::set<std::string> filtro(alvo.begin(), alvo.end());
    return parse_registros((*dados)["hourly"], lat, lon, "qualidade_ar", &filtro);
}

static std::vector<Registro> baixar_polen_bloco(const std::vector<std::string>& datas,
                                                double lat, double lon) {
    std::string h = hoje();
    std::vector<std::string> alvo;
    for (const auto& d : datas)
        if (d >= h) alvo.push_back(d);
    if (alvo.empty()) return {};

    std::map<std::string, std::string> params = {
        {"latitude", num_str(lat)},
        {"longitude", num_str(lon)},
        {"start_date", alvo.front()},
        {"end_date", alvo.back()},
        {"daily", juntar(CFG.pollen_daily, ",")},
        {"timezone", CFG.fuso_horario},
    };

    auto dados = baixar_com_retry(CFG.url_pollen, params, CFG.max_retries);
    if (!dados || !dados->contains("daily") || !(*dados)["daily"].is_object()) return {};

    std::set<std::string> filtro(alvo.begin(), alvo.end());
    return parse_registros((*dados)["daily"], lat, lon, "polen", &filtro);
}

// Gravacao no banco
// Definicoes de schema (colunas) para cada tabela - iguais as do download.py
static const std::map<std::string, std::vector<std::string>> SCHEMAS = {
    {"clima_diario",
     {"data", "latitude", "longitude", "timezone", "weathercode", "temperatura_max",
      "temperatura_min", "sensacao_termica_max", "sensacao_termica_min", "umidade_max",
      "umidade_min", "precipitacao_total", "probabilidade_chuva", "chuva_total",
      "aguas_claras_total", "neve_total", "nascer_sol", "por_sol", "uv_max",
      "uv_clear_sky_max", "vento_max", "direcao_vento", "rajadas_vento", "pressao_max",
      "pressao_min", "pressao_superficie_max", "pressao_superficie_min"}},
    {"clima_horario",
     {"data", "hora", "latitude", "longitude", "temperatura", "sensacao_termica", "umidade",
      "ponto_orvalho", "precipitacao", "probabilidade_chuva", "chuva", "aguas_claras",
      "neve", "profundidade_neve", "uv_index", "uv_index_clear_sky", "vento",
      "direcao_vento", "rajadas_vento", "pressao", "pressao_superficie", "cobertura_nuvens",
      "cobertura_nuvens_baixa", "cobertura_nuvens_media", "cobertura_nuvens_alta",
      "visibilidade", "is_day", "weathercode"}},
    {"qualidade_ar",
     {"data", "hora", "latitude", "longitude", "pm10", "pm2_5", "monoxido_carbono",
      "nitrogenio_dioxide", "enxofre_dioxide", "ozonio", "aerosois", "poeira",
      "formaldeido"}},
    {"polen",
     {"data", "latitude", "longitude", "alder_pollen_mean", "birch_pollen_mean",
      "olive_pollen_mean", "ragweed_pollen_mean", "grass_pollen_mean"}},
};

static void salvar_dados(const std::string& tabela, const std::vector<Registro>& registros,
                         const std::vector<std::string>& campos) {
    if (registros.empty()) return;
    sqlite3* conn = get_connection();

    std::vector<std::string> marcadores(campos.size() + 1, "?");
    std::string sql = "INSERT OR REPLACE INTO " + tabela + " (" + juntar(campos, ",") +
                      ", created_at) VALUES (" + juntar(marcadores, ",") + ")";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(conn);
        sqlite3_finalize(stmt);
        throw std::runtime_error("SQLite: " + err);
    }

    const std::string agora = agora_iso();
    executar_sql(conn, "BEGIN");
    size_t desde_commit = 0;
    for (const auto& r : registros) {
        int idx = 1;
        for (const auto& c : campos) {
            auto it = r.find(c);
            if (it == r.end() || std::holds_alternative<std::monostate>(it->second)) {
                sqlite3_bind_null(stmt, idx);
            } else if (std::holds_alternative<double>(it->second)) {
                sqlite3_bind_double(stmt, idx, std::get<double>(it->second));
            } else {
                const std::string& s = std::get<std::string>(it->second);
                sqlite3_bind_text(stmt, idx, s.c_str(), static_cast<int>(s.size()),
                                  SQLITE_TRANSIENT);
            }
            ++idx;
        }
        sqlite3_bind_text(stmt, idx, agora.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            log("[ERRO] Falha ao inserir em " + tabela + ": " + sqlite3_errmsg(conn));
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        ++desde_commit;

        // Lote de gravacao limitado por max_sql_dates_por_lote
        if (desde_commit >= static_cast<size_t>(CFG.max_sql_dates_por_lote)) {
            executar_sql(conn, "COMMIT");
            executar_sql(conn, "BEGIN");
            desde_commit = 0;
        }
    }
    executar_sql(conn, "COMMIT");
    sqlite3_finalize(stmt);
}

// Orquestracao: baixar bloco -> fila -> processar (gravar)
struct Resultado {
    double lat = 0.0, lon = 0.0;
    std::vector<std::string> datas_diario, datas_horario, datas_ar, datas_polen;
    std::vector<Registro> diario, horario, qualidade_ar, polen;
};

static std::optional<Resultado> baixar_bloco(const std::vector<std::string>& datas, double lat,
                                             double lon, const Existentes& existentes) {
    // Jitter inicial aleatorio (igual ao Python)
    std::uniform_real_distribution<double> jitter(0.0, CFG.jitter_inicial_max);
    dormir(jitter(rng()));

    Chave chave{lat, lon};
    std::vector<std::string> datas_diario, datas_horario, datas_ar, datas_polen;
    {
        // Leitura compartilhada enquanto os consumidores podem gravar
        std::shared_lock lk(g_mtx_existentes);
        datas_diario = filtrar_faltantes(datas, presentes(existentes, "diario", chave));
        datas_horario = filtrar_faltantes(datas, presentes(existentes, "horario", chave));
        for (const auto& d : datas) {
            if (d >= CFG.data_minima_qualidade_ar &&
                !presentes(existentes, "qualidade_ar", chave).count(d))
                datas_ar.push_back(d);
            if (d >= hoje() && !presentes(existentes, "polen", chave).count(d))
                datas_polen.push_back(d);
        }
    }

    Resultado res;
    res.lat = lat;
    res.lon = lon;
    res.datas_diario = datas_diario;
    res.datas_horario = datas_horario;
    res.datas_ar = datas_ar;
    res.datas_polen = datas_polen;

    // Download de clima (diario + horario juntos)
    if (!datas_diario.empty() || !datas_horario.empty()) {
        std::vector<std::string> alvo = datas_diario;
        alvo.insert(alvo.end(), datas_horario.begin(), datas_horario.end());
        std::sort(alvo.begin(), alvo.end());
        alvo.erase(std::unique(alvo.begin(), alvo.end()), alvo.end());
        auto clima = baixar_clima_bloco(alvo, lat, lon);
        res.diario = std::move(clima.diario);
        res.horario = std::move(clima.horario);
    }

    if (!datas_ar.empty()) res.qualidade_ar = baixar_qualidade_ar_bloco(datas_ar, lat, lon);
    if (!datas_polen.empty()) res.polen = baixar_polen_bloco(datas_polen, lat, lon);

    return res;
}

struct Stats {
    std::atomic<long long> baixado_diario{0}, baixado_horario{0}, baixado_ar{0},
        baixado_polen{0};
    std::atomic<long long> pulado_diario{0}, pulado_horario{0}, pulado_ar{0},
        pulado_polen{0};
};

static void registrar_datas(Existentes& existentes, const std::string& tipo,
                            const Chave& chave, const std::vector<Registro>& regs) {
    auto& conjunto = existentes.dados[tipo][chave];
    for (const auto& r : regs) {
        auto it = r.find("data");
        if (it != r.end() && std::holds_alternative<std::string>(it->second))
            conjunto.insert(std::get<std::string>(it->second));
    }
}

static void processar_resultado(const Resultado& res, Existentes& existentes, Stats& stats) {
    Chave chave{res.lat, res.lon};

    stats.pulado_diario += static_cast<long long>(res.datas_diario.size()) -
                           static_cast<long long>(res.diario.size());
    stats.pulado_horario += static_cast<long long>(res.datas_horario.size()) -
                            static_cast<long long>(res.horario.size());
    stats.pulado_ar += static_cast<long long>(res.datas_ar.size()) -
                       static_cast<long long>(res.qualidade_ar.size());
    stats.pulado_polen += static_cast<long long>(res.datas_polen.size()) -
                          static_cast<long long>(res.polen.size());

    struct Lote {
        const char* tabela;
        const std::vector<Registro>* regs;
        const char* tipo;
        std::atomic<long long>* contador;
    };
    const Lote lotes[] = {
        {"clima_diario", &res.diario, "diario", &stats.baixado_diario},
        {"clima_horario", &res.horario, "horario", &stats.baixado_horario},
        {"qualidade_ar", &res.qualidade_ar, "qualidade_ar", &stats.baixado_ar},
        {"polen", &res.polen, "polen", &stats.baixado_polen},
    };

    for (const auto& l : lotes) {
        if (l.regs->empty()) continue;
        {
            // Uma gravacao por vez (equivale ao write_lock do Python)
            std::lock_guard<std::mutex> lk(g_write_mtx);
            salvar_dados(l.tabela, *l.regs, SCHEMAS.at(l.tabela));
        }
        {
            std::unique_lock<std::shared_mutex> lk(g_mtx_existentes);
            registrar_datas(existentes, l.tipo, chave, *l.regs);
        }
        *l.contador += static_cast<long long>(l.regs->size());
    }
}

// Espelha coordenada_completa() do download.py
static bool coordenada_completa(const std::vector<std::string>& datas, double lat, double lon,
                                const Existentes& existentes) {
    Chave chave{lat, lon};
    for (const char* tipo : {"diario", "horario", "qualidade_ar", "polen"}) {
        auto esperadas = datas_esperadas(tipo, datas);
        const auto& p = presentes(existentes, tipo, chave);
        for (const auto& d : esperadas){
            if (!p.count(d)) return false;
        }
    }
    return true;
}

// Fila limitada (download -> processamento)
template <typename T>
class Fila {
public:
    explicit Fila(size_t capacidade) : cap_(capacidade) {}

    void put(T&& valor) {
        std::unique_lock<std::mutex> lk(m_);
        cv_cheia_.wait(lk, [&] { return q_.size() < cap_; });
        q_.push(std::move(valor));
        cv_vazia_.notify_one();
    }

    // Retorna false quando a fila foi fechada e esvaziada
    bool get(T& saida) {
        std::unique_lock<std::mutex> lk(m_);
        cv_vazia_.wait(lk, [&] { return !q_.empty() || fechada_; });
        if (q_.empty()) return false;
        saida = std::move(q_.front());
        q_.pop();
        cv_cheia_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(m_);
        fechada_ = true;
        cv_vazia_.notify_all();
        cv_cheia_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_vazia_, cv_cheia_;
    std::queue<T> q_;
    size_t cap_;
    bool fechada_ = false;
};

int main(int argc, char** argv) {
    std::string caminho_ini = argc > 1 ? argv[1] : "download.ini";
    carregar_config(caminho_ini);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    iniciar_rate_limit();

    try {
        std::vector<std::string> datas = expandir_periodo(CFG.periodo_inicio, CFG.periodo_fim);
        if (datas.empty()) {
            std::cerr << "[ERRO] Periodo invalido (inicio > fim) em " << caminho_ini << "\n";
            curl_global_cleanup();
            return 1;
        }

        std::cout << "============================================================\n"
                  << "DOWNLOAD DE DADOS CLIMATICOS\n"
                  << "============================================================\n"
                  << "Configuracao: " << caminho_ini << "\n"
                  << "Fuso horario: " << CFG.fuso_horario << "\n"
                  << "Banco de dados: " << CFG.db_path << "\n"
                  << "Periodo: " << datas.front() << " a " << datas.back() << "\n"
                  << "Total de datas: " << datas.size() << "\n"
                  << "Total de localizacoes: " << CFG.coordenadas.size() << "\n"
                  << "Threads download: " << CFG.download_workers
                  << " | Threads processamento: " << CFG.process_workers << "\n"
                  << "Dias por requisicao: " << CFG.chunk_dias << "\n"
                  << "Rate-limit: " << CFG.requests_per_second << " req/s | Timeout: "
                  << CFG.http_timeout << "s\n"
                  << "Retries maximos: " << CFG.max_retries << "\n"
                  << "============================================================\n";

        init_database();

        std::cout << "\n[DB] Carregando registros existentes...\n";
        auto inicio_carregamento = std::chrono::steady_clock::now();
        Existentes existentes = carregar_existentes(datas, CFG.coordenadas);
        double tempo_carregamento =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          inicio_carregamento)
                .count();

        const std::map<std::string, size_t> ordem = {
            {"diario", 0}, {"horario", 1}, {"qualidade_ar", 2}, {"polen", 3}};
        long long totais[4] = {0, 0, 0, 0};
        for (const auto& [tipo, mapa] : existentes.dados) {
            for (const auto& [chave, conjunto] : mapa) {
                (void)chave;
                totais[ordem.at(tipo)] += static_cast<long long>(conjunto.size());
            }
        }
        std::cout << "[DB] Carregado em " << tempo_carregamento << "s\n"
                  << "[DB] Existentes -> diario: " << totais[0]
                  << ", horario: " << totais[1] << ", qualidade_ar: " << totais[2]
                  << ", polen: " << totais[3] << "\n";

        // Pula coordenadas completas e monta tarefas apenas com datas faltantes
        std::vector<std::pair<std::vector<std::string>, std::pair<double, double>>>
            coordenadas_ativas;
        long long total_faltantes = 0;
        for (const auto& [lat, lon] : CFG.coordenadas) {
            if (coordenada_completa(datas, lat, lon, existentes)) {
                std::cout << "[DB] Coordenada (" << lat << ", " << lon
                          << ") ja possui todas as datas do periodo. Pulando...\n";
                continue;
            }
            Chave chave{lat, lon};
            std::vector<std::string> datas_faltantes;
            {
                std::shared_lock lk(g_mtx_existentes);
                for (const auto& d : datas) {
                    bool falta = false;
                    if (!presentes(existentes, "diario", chave).count(d)) falta = true;
                    else if (!presentes(existentes, "horario", chave).count(d)) falta = true;
                    else if (d >= CFG.data_minima_qualidade_ar && !presentes(existentes, "qualidade_ar", chave).count(d)) falta = true;
                    else if (d >= hoje() && !presentes(existentes, "polen", chave).count(d)) falta = true;
                    if (falta) datas_faltantes.push_back(d);
                }
            }
            if (!datas_faltantes.empty()) {
                total_faltantes += static_cast<long long>(datas_faltantes.size());
                coordenadas_ativas.emplace_back(std::move(datas_faltantes),
                                                std::make_pair(lat, lon));
            }
        }

        std::cout << "[DB] Total de itens (data x coordenada) faltantes para processamento: "
                  << total_faltantes << "\n";
        size_t coordenadas_puladas = CFG.coordenadas.size() - coordenadas_ativas.size();
        if (coordenadas_ativas.empty()) {
            std::cout << "[DB] Nenhuma coordenada com dados pendentes. Nada a fazer.\n";
            curl_global_cleanup();
            return 0;
        }

        // Cada coordenada gera blocos de datas (CHUNK_DIAS)
        struct Tarefa {
            std::vector<std::string> datas;
            double lat, lon;
        };
        std::vector<Tarefa> tarefas;
        for (auto& [datas_coord, coord] : coordenadas_ativas) {
            for (size_t i = 0; i < datas_coord.size(); i += CFG.chunk_dias) {
                size_t f = std::min(i + static_cast<size_t>(CFG.chunk_dias), datas_coord.size());
                tarefas.push_back({std::vector<std::string>(datas_coord.begin() + i,
                                                            datas_coord.begin() + f),
                                   coord.first, coord.second});
            }
        }

        Stats stats;
        std::atomic<long long> concluidos = 0;
        const long long total = static_cast<long long>(tarefas.size());
        auto inicio = std::chrono::steady_clock::now();

        // Fila para transferir resultados de download -> processamento
        Fila<Resultado> fila(static_cast<size_t>(CFG.download_workers) * 2);
        std::atomic<size_t> indice_tarefa{0};
        std::atomic<long long> tarefas_concluidas{0};

        std::cout << "\nIniciando " << total << " tarefas: " << CFG.download_workers
                  << " threads de download, " << CFG.process_workers
                  << " threads de processamento...\n\n";

        // Consumidores (gravacao no banco)
        std::vector<std::thread> consumidores;
        for (int i = 0; i < CFG.process_workers; ++i) {
            consumidores.emplace_back([&] {
                try {
                    Resultado r;
                    while (fila.get(r)) {
                        processar_resultado(r, existentes, stats);
                        long long c = ++concluidos;
                        if (c % 5 == 0 || c == total) {
                            double pct = 100.0 * static_cast<double>(c) / static_cast<double>(total);
                            double decorrido =
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - inicio)
                                    .count();
                            char buf[160];
                            std::snprintf(buf, sizeof(buf),
                                          "\r[PROGRESSO] %lld/%lld blocos (%.1f%%) - %.0fs",
                                          c, total, pct, decorrido);
                            std::lock_guard<std::mutex> lk(g_print_mtx);
                            std::cout << buf << std::flush;
                        }
                    }
                } catch (const std::exception& e) {
                    log(std::string("[ERRO] Consumidor: ") + e.what());
                }
            });
        }

        // Produtores (download)
        std::vector<std::thread> produtores;
        for (int i = 0; i < CFG.download_workers; ++i) {
            produtores.emplace_back([&] {
                try {
                    while (true) {
                        size_t idx = indice_tarefa.fetch_add(1);
                        if (idx >= tarefas.size()) break;
                        const Tarefa& t = tarefas[idx];
                        auto resultado = baixar_bloco(t.datas, t.lat, t.lon, existentes);
                        if (resultado) fila.put(std::move(*resultado));
                        if (tarefas_concluidas.fetch_add(1) + 1 >= total) fila.close();
                    }
                } catch (const std::exception& e) {
                    log(std::string("[ERRO] Produtor: ") + e.what());
                    // Garante o encerramento dos consumidores mesmo com falha
                    if (tarefas_concluidas.fetch_add(1) + 1 >= total) fila.close();
                }
            });
        }

        for (auto& t : produtores) t.join();
        for (auto& t : consumidores) t.join();

        double tempo_total =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
        std::cout << "\n\n============================================================\n"
                  << "DOWNLOAD CONCLUIDO!\n"
                  << "============================================================\n"
                  << "Tempo total: " << tempo_total << "s\n"
                  << "Tempo carregamento DB: " << tempo_carregamento << "s\n"
                  << "Blocos processados: " << concluidos.load() << "/" << total << "\n"
                  << "Coordenadas puladas (periodo completo): " << coordenadas_puladas
                  << "\n"
                  << "\n--- RESUMO ---\n"
                  << "Diario: " << stats.baixado_diario.load() << " baixados | "
                  << stats.pulado_diario.load() << " ja existentes\n"
                  << "Horario: " << stats.baixado_horario.load() << " registros | "
                  << stats.pulado_horario.load() << " dias existentes\n"
                  << "Qualidade do ar: " << stats.baixado_ar.load() << " registros | "
                  << stats.pulado_ar.load() << " dias existentes\n"
                  << "Polen: " << stats.baixado_polen.load() << " dias | "
                  << stats.pulado_polen.load() << " dias existentes\n"
                  << "============================================================\n";
    } catch (const std::exception& e) {
        std::cerr << "\n[ERRO] " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}

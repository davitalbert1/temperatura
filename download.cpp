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
#include <deque>
#include <cstdio>
#include <cstdlib>
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

// Utilidades of text / files .ini
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

// Under age acting round-trip of the double (equal the str() of Python)
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
    std::vector<std::string> ordem_secoes;

    bool obter(const std::string& sec, const std::string& chave, std::string& saida) const {
        auto it = valores.find(sec);
        if (it == valores.end()) return false;
        auto it2 = it->second.find(chave);
        if (it2 == it->second.end()) return false;
        saida = it2->second;
        return true;
    }
    std::string obter_ou(const std::string& sec, const std::string& chave,
                         const std::string& padrao = "") const {
        std::string v;
        return obter(sec, chave, v) ? v : padrao;
    }
    std::vector<std::string> lista(const std::string& sec) const {
        auto it = listas.find(sec);
        if (it == listas.end()) return {};
        return it->second;
    }
    bool tem_secao(const std::string& sec) const {
        return valores.count(sec) || listas.count(sec);
    }
};

static bool eh_secao_dataset(const std::string& s) {
    return s.rfind("dataset:", 0) == 0;
}

static Ini ler_ini(const std::string& caminho) {
    Ini ini;
    std::ifstream arquivo(caminho);
    if (!arquivo) {
        std::cerr << "[WARNING] File of configuracao not found: " << caminho
                  << " (usando values padrao)\n";
        return ini;
    }
    auto registrar_secao = [&](const std::string& secao) {
        if (!secao.empty() &&
            std::find(ini.ordem_secoes.begin(), ini.ordem_secoes.end(), secao) ==
                ini.ordem_secoes.end())
            ini.ordem_secoes.push_back(secao);
    };
    std::string linha, secao;
    while (std::getline(arquivo, linha)) {
        linha = cortar(sem_comentario(linha));
        if (linha.empty()) continue;
        if (linha.front() == '[' && linha.back() == ']') {
            secao = cortar(linha.substr(1, linha.size() - 2));
            registrar_secao(secao);
            continue;
        }
        if (secao.empty()) continue;
        registrar_secao(secao);
        // Secoes of dataset: aceitam the "key = value" how much linhas of
        // list ("vars = the, b" or simplesmente "the, b" / "coluna.x = ...").
        if (eh_secao_dataset(secao)) {
            size_t igual = linha.find('=');
            if (igual == std::string::npos) {
                // Thread solta: trata how item of list generica of section.
                for (auto& token : dividir(linha, ',')) {
                    if (token.empty() || token.find('=') != std::string::npos) continue;
                    ini.listas[secao].push_back(token);
                }
                continue;
            }
            std::string chave = cortar(linha.substr(0, igual));
            std::string valor = cortar(linha.substr(igual + 1));
            if (chave.empty()) continue;
            if (chave == "vars" || chave == "colunas" || chave == "params_excluir" ||
                chave == "lista_vars" || chave == "colunas_extra") {
                for (auto& token : dividir(valor, ',')) {
                    if (token.empty()) continue;
                    // "col = api:tipo" dentro of vars also and aceito.
                    size_t dp = token.find(':');
                    std::string nome = cortar(dp == std::string::npos ? token
                                                                      : token.substr(0, dp));
                    if (!nome.empty()) ini.listas[secao].push_back(token);
                }
                ini.valores[secao][chave] = valor;
                continue;
            }
            ini.valores[secao][chave] = valor;
            continue;
        }
        if (secao == "coordenadas") {
            if (!linha.empty()) ini.listas[secao].push_back(linha);
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

// Variaveis of configuracao (all vindas of .ini; env var sobrescreve)
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

static bool para_bool(const std::string& s, bool padrao) {
    std::string v = cortar(s);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "1" || v == "true" || v == "sim" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "nao" || v == "não" || v == "no" || v == "off" ||
        v == "desligado")
        return false;
    return padrao;
}

// Name of coluna/tabela insurance to SQLite: minusculas, [a-z0-9_], without
// accent, without espacos. "pm2.5 (µg/m³)" -> "pm2_5_ug_m3".
static std::string normalizar_nome(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-' || c == '.' || c == ' ' || c == '/')
            out += '_';
        // ignora besides simbolos/acentos
    }
    // colapsa underscores
    std::string limpo;
    bool ultimo_sep = true; // evita '_' inicial
    for (char c : out) {
        bool sep = (c == '_');
        if (sep && ultimo_sep) continue;
        limpo += c;
        ultimo_sep = sep;
    }
    while (!limpo.empty() && limpo.back() == '_') limpo.pop_back();
    if (limpo.empty()) limpo = "col";
    if (std::isdigit(static_cast<unsigned char>(limpo.front()))) limpo = "c_" + limpo;
    return limpo;
}

// "column = api[:tipo]" -> {column normalizada, variable of api, type}
// Without ":" the type and inferido of date (car). Tipos aceitos:
// current | integer | text (the for the rest: int, float, double, str, string, date, hour).
// Colunas geridas hair decent engine: never vem of date of API and never
// podem creature sobrescritas (the of data/hora usa the nomes configurados).
static bool coluna_reservada(const std::string& c) {
    return c == "data" || c == "hora" || c == "timezone" || c == "latitude" ||
           c == "longitude" || c == "created_at";
}

static std::string normalizar_tipo(const std::string& t) {
    std::string v = cortar(t);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v.empty() || v == "auto") return "auto";
    if (v == "int" || v == "integer" || v == "bool" || v == "boolean") return "integer";
    if (v == "real" || v == "float" || v == "double" || v == "number" || v == "numeric")
        return "real";
    return "text";
}

// Divide "col = api:tipo" in partes. Retorna false if empty.
static bool separar_def_coluna(const std::string& def, std::string& coluna,
                               std::string& variavel_api, std::string& tipo) {
    std::string d = cortar(def);
    if (d.empty()) return false;
    size_t igual = d.find('=');
    std::string esq, dir;
    if (igual == std::string::npos) {
        esq = d;
        dir = d;
    } else {
        esq = cortar(d.substr(0, igual));
        dir = cortar(d.substr(igual + 1));
        if (esq.empty()) esq = dir;
        if (dir.empty()) dir = esq;
    }
    // dir pode creature "variavel:tipo"
    size_t dp = dir.find(':');
    std::string var = cortar(dp == std::string::npos ? dir : dir.substr(0, dp));
    std::string tp = dp == std::string::npos ? "auto" : cortar(dir.substr(dp + 1));
    if (var.empty()) var = esq;
    coluna = normalizar_nome(esq);
    variavel_api = var.empty() ? coluna : var;
    tipo = normalizar_tipo(tp);
    return !coluna.empty();
}

struct ColunaCfg {
    std::string coluna;       // name of column in database (normalizado)
    std::string variavel_api; // name of variable in API/JSON
    std::string tipo;         // "auto" | "real" | "integer" | "text"
};

// Dataset general: the API (or outline her) -> the table of the database.
// THE few datasets podem aim to the SAME database (tabelas diferentes or
// until the same table -> merge by key). Each dataset also pode aim
// to the database OTHER (db decent).
struct DatasetCfg {
    std::string id;              // name of section without "dataset:" (ex: "clima_diario")
    std::string db;              // file sqlite (empty = db norm of [general])
    std::string tabela;          // table target (norm = id normalizado)
    std::string url;             // URL element of API
    std::string url_passado;     // URL alternate p/ datas < today (ex: archive)
    std::string url_futuro;      // URL alternate p/ datas >= today (ex: forecast)
    std::string bloco = "daily"; // block JSON with the date ("daily"|"hourly"|...)
    std::string params_chave = "daily"; // name of parameter of query (daily=...&hourly=...)
    std::vector<std::string> vars;      // variaveis pedidas the API
    std::set<std::string> vars_excluir; // variaveis removidas when usa url_passado
    std::vector<ColunaCfg> colunas;     // overrides opcionais (column.*): renomeia/tipa
    std::string data_min;               // exclusively baixa datas >= data_min ("", date or "today")
    std::string data_max;               // exclusively baixa datas <= data_max ("", date or "today")
    std::string granularidade = "dia";  // "day" (time=YYYY-MM-DD) | "hour" (team absolute)
    bool salvar_hora = false;           // cria column "hour" with the team absolute
    bool salvar_timezone = false;       // cria column "timezone"
    std::string chave_data = "data";    // column of date ("" = without column of date)
    std::string chave_hora = "hour";    // column of hour ("" = without column of hour)
    std::string api_keys;               // chaves "k1 | k2" (empty = herda [general] api_keys)
};

struct Config {
    // [general] - espelham the variaveis of download.py
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
    double min_rps = 0.5;
    double cooldown_429 = 30.0;
    int circuit_limit_429 = 3;
    double espera_min_429 = 5.0;
    // Budget of chamadas: janelas deslizantes in unidades fracionarias
    // (FAQ Open-Meteo: cost by request = max(1, vars/10, dias*1.5/14)).
    // Limites oficiais of tier gratuito: 600/min, 5000/h, 10000/dia.
    // 0 desliga the window. Without key of API all the URLs compartilham the
    // same budget ("anonymous"); with api_keys each key tem the his.
    double limite_minuto = 600.0;
    double limite_hora = 5000.0;
    double limite_dia = 10000.0;
    double custo_vars_ref = 10.0;  // cost = max(1, vars/custo_vars_ref, ...)
    double custo_dias_ref = 14.0;  // max(..., dias*custo_dias_fator/custo_dias_ref)
    double custo_dias_fator = 1.5;
    double cooldown_quota = 900.0; // break global p/ 429 of quota (daily/hourly)
    std::string api_keys; // "k1 | k2" - rotacionadas by request
    std::string api_key_param = "apikey"; // name of parameter of key in URL
    std::string fuso_horario = "America/Sao_Paulo";
    std::string periodo_inicio = "1940-01-01";
    std::string periodo_fim = "2025-12-31";
    // Datasets genericos: each [dataset:<id>] vira the table in the database.
    // THE few datasets podem share the same .db (tabelas iguais or
    // diferentes -> merge by key) and the dataset pode give birth db decent.
    std::vector<DatasetCfg> datasets;

    // [coordenadas] - norm: the mesmas of download.py
    std::vector<std::pair<double, double>> coordenadas = {
        {-23.5505, -46.6333}, // Healthy Paulo, Brazil
        {-26.3044, -48.8456}, // Joinville, Brazil
        {-25.4278, -49.2731}, // Curitiba, Brazil
        {-28.7833, -51.6100}, // Guapore, Brazil
        {-20.4697, -54.6201}, // Country Large, Brazil
        {-3.1019, -60.0250}, // Manaus, Brazil
        {-3.7250, -38.5236}, // Fortification, Brazil
        {52.5200, 13.4050}, // Berlin, Germany
        {35.6762, 139.6503}, // Tquio, Japan
        {40.7128, -74.0060}, // New York, Estados Unidos
        {55.7558, 37.6173}, // Moscou, Russia
        {43.1155, 131.8855}, // Vladivostok, Russia
        {-33.4489, -70.6693}, // Santiago, Chile
        {-54.8019, -68.3030}, // Ushuaia, Argentina
        {-33.9249, 18.4241}, // City state of Cable, Africa of South
        {19.4326, -99.1332}, // City state of Mexico, Mexico
        {61.2181, -149.9003}, // Anchorage, Estados Unidos
    };

};

Config CFG;

static std::vector<DatasetCfg> montar_datasets_genericos(const Ini& ini) {
    std::vector<DatasetCfg> out;
    for (const auto& secao : ini.ordem_secoes) {
        if (!eh_secao_dataset(secao)) continue;
        DatasetCfg d;
        d.id = secao.substr(8);
        d.db = ini.obter_ou(secao, "db");
        std::string tab = ini.obter_ou(secao, "tabela", d.id);
        d.tabela = normalizar_nome(tab.empty() ? d.id : tab);
        d.url = cortar(ini.obter_ou(secao, "url"));
        d.url_passado = cortar(ini.obter_ou(secao, "url_passado"));
        d.url_futuro = cortar(ini.obter_ou(secao, "url_futuro"));
        d.api_keys = cortar(ini.obter_ou(secao, "api_keys"));
        // Aceita tanto "bloco" (pt, usado no download.ini) quanto "block" (en).
        d.bloco = cortar(ini.obter_ou(secao, "bloco"));
        if (d.bloco.empty()) d.bloco = cortar(ini.obter_ou(secao, "block", "daily"));
        if (d.bloco.empty()) d.bloco = "daily";
        d.params_chave = cortar(ini.obter_ou(secao, "params_chave", d.bloco));
        if (d.params_chave.empty()) d.params_chave = d.bloco;
        // 1) Overrides opcionais coluna.<nome> = <var_api>[:tipo]: renomeiam/
        //    tipam the column descoberta automatically (not healthy obrigatorios)
        //    and garantem that the variable seja pedida the API.
        auto add_coluna = [&](const std::string& c, const std::string& v,
                              const std::string& tp) {
            for (auto& cc : d.colunas) {
                if (cc.coluna == c) {
                    cc.variavel_api = v;
                    cc.tipo = tp;
                    return;
                }
            }
            d.colunas.push_back(ColunaCfg{c, v, tp});
        };
        auto add_var = [&](const std::string& v) {
            if (v.empty() || v == "time" || v == "timezone") return;
            if (std::find(d.vars.begin(), d.vars.end(), v) == d.vars.end()) d.vars.push_back(v);
        };
        auto it_sec = ini.valores.find(secao);
        if (it_sec != ini.valores.end()) {
            std::map<std::string, std::string> defs; // ordenada -> order stable
            for (const auto& kv : it_sec->second) {
                // Aceita "coluna." (pt, doc do download.ini) e "column." (en).
                std::string resto;
                if (kv.first.rfind("coluna.", 0) == 0)
                    resto = kv.first.substr(7);
                else if (kv.first.rfind("column.", 0) == 0)
                    resto = kv.first.substr(7);
                else
                    continue;
                defs[resto] = kv.second;
            }
            for (const auto& kv : defs) {
                std::string c, v, tp;
                if (!separar_def_coluna(kv.first + "=" + kv.second, c, v, tp)) continue;
                if (coluna_reservada(c)) continue;
                add_coluna(c, v, tp);
                add_var(v);
            }
        }
        // 2) vars / linhas soltas: only pedem variaveis the API. Column exclusively and
        //    declarada when there is type formal ("<var>:<tipo>"); without that the
        //    column and descoberta automatically of response JSON (runtime),
        //    with the type inferido of date in CREATE TABLE / ADD COLUMN.
        std::vector<std::string> crus;
        for (const auto& t : dividir(ini.obter_ou(secao, "vars"), ',')) crus.push_back(t);
        for (const auto& t : ini.lista(secao)) crus.push_back(t);
        for (const auto& t : dividir(ini.obter_ou(secao, "colunas"), ',')) crus.push_back(t);
        for (const auto& t : crus) {
            std::string c, v, tp;
            if (!separar_def_coluna(t, c, v, tp)) continue;
            if (coluna_reservada(c)) continue;
            add_var(v);
            bool explicito = t.find('=') != std::string::npos ||
                            t.find(':') != std::string::npos;
            if (explicito) add_coluna(c, v, tp);
        }
        // 3) vars_excluir and besides chaves of filter.
        for (const auto& t : dividir(ini.obter_ou(secao, "vars_excluir"), ',')) {
            if (!t.empty()) d.vars_excluir.insert(t);
        }
        d.data_min = cortar(ini.obter_ou(secao, "data_min"));
        d.data_max = cortar(ini.obter_ou(secao, "data_max"));
        std::string gran = cortar(ini.obter_ou(secao, "granularidade", "dia"));
        std::transform(gran.begin(), gran.end(), gran.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        d.granularidade = (gran == "hora" || gran == "horario" || gran == "hourly") ? "hour" : "dia";
        d.salvar_hora = para_bool(ini.obter_ou(secao, "salvar_hora", d.granularidade == "hour" ? "1" : "0"), d.granularidade == "hour");
        d.salvar_timezone = para_bool(ini.obter_ou(secao, "salvar_timezone", "0"), false);
        std::string cd = cortar(ini.obter_ou(secao, "chave_data", "data"));
        d.chave_data = cd.empty() ? "" : normalizar_nome(cd);
        std::string ch = cortar(ini.obter_ou(secao, "chave_hora", "hour"));
        d.chave_hora = ch.empty() ? "" : normalizar_nome(ch);
        if (d.url.empty()) {
            std::cerr << "[WARNING] Dataset '" << d.id << "' ignorado (url vazia).\n";
            continue;
        }
        if (d.vars.empty())
            std::cerr << "[WARNING] Dataset '" << d.id
                      << "': without 'vars' - the API decide the that retorna.\n";
        out.push_back(std::move(d));
    }
    return out;
}

static void carregar_config(const std::string& caminho) {
    Ini ini = ler_ini(caminho);
    std::string v;

    auto le_texto = [&](const char* sec, const char* chave, std::string& alvo) {
        // Aceita tanto [geral] (pt) quanto [general] (en).
        if (ini.obter(sec, chave, v)) alvo = v;
        if (std::string(sec) == "geral" && ini.obter("general", chave, v)) alvo = v;
        if (std::string(sec) == "general" && ini.obter("geral", chave, v)) alvo = v;
    };
    auto le_int = [&](const char* chave, const char* env, int& alvo) {
        if (ini.obter("geral", chave, v)) alvo = para_int(v, alvo);
        if (ini.obter("general", chave, v)) alvo = para_int(v, alvo);
        if (env) alvo = env_int(env, alvo);
    };
    auto le_dbl = [&](const char* chave, const char* env, double& alvo) {
        if (ini.obter("geral", chave, v)) alvo = para_double(v, alvo);
        if (ini.obter("general", chave, v)) alvo = para_double(v, alvo);
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
    le_dbl("limite_minuto", "OPENMETEO_LIMITE_MINUTO", CFG.limite_minuto);
    le_dbl("limite_hora", "OPENMETEO_LIMITE_HORA", CFG.limite_hora);
    le_dbl("limite_dia", "OPENMETEO_LIMITE_DIA", CFG.limite_dia);
    le_dbl("custo_vars_ref", nullptr, CFG.custo_vars_ref);
    le_dbl("custo_dias_ref", nullptr, CFG.custo_dias_ref);
    le_dbl("custo_dias_fator", nullptr, CFG.custo_dias_fator);
    le_dbl("cooldown_quota", "OPENMETEO_COOLDOWN_QUOTA", CFG.cooldown_quota);
    le_texto("general", "api_keys", CFG.api_keys);
    le_texto("general", "api_key_param", CFG.api_key_param);
    le_texto("general", "fuso_horario", CFG.fuso_horario);
    le_texto("general", "periodo_inicio", CFG.periodo_inicio);
    le_texto("general", "periodo_fim", CFG.periodo_fim);

    auto coord_linhas = ini.lista("coordenadas");
    if (!coord_linhas.empty()) {
        std::vector<std::pair<double, double>> coords;
        for (const auto& l : coord_linhas) {
            auto partes = dividir(l, ',');
            if (partes.size() < 2) {
                std::cerr << "[WARNING] Coordenada invalida ignorada: " << l << "\n";
                continue;
            }
            try {
                coords.emplace_back(std::stod(partes[0]), std::stod(partes[1]));
            } catch (...) {
                std::cerr << "[WARNING] Coordenada invalida ignorada: " << l << "\n";
            }
        }
        if (!coords.empty()) CFG.coordenadas = coords;
    }

    if (CFG.download_workers < 1) CFG.download_workers = 1;
    if (CFG.process_workers < 1) CFG.process_workers = 1;
    if (CFG.chunk_dias < 1) CFG.chunk_dias = 1;
    if (CFG.max_sql_dates_por_lote < 1) CFG.max_sql_dates_por_lote = 1;
    if (CFG.requests_per_second <= 0.0) CFG.requests_per_second = 1.0;
    if (CFG.max_retries < 1) CFG.max_retries = 1;
    if (CFG.limite_minuto < 0.0) CFG.limite_minuto = 0.0;
    if (CFG.limite_hora < 0.0) CFG.limite_hora = 0.0;
    if (CFG.limite_dia < 0.0) CFG.limite_dia = 0.0;
    if (CFG.custo_vars_ref <= 0.0) CFG.custo_vars_ref = 10.0;
    if (CFG.custo_dias_ref <= 0.0) CFG.custo_dias_ref = 14.0;
    if (CFG.custo_dias_fator <= 0.0) CFG.custo_dias_fator = 1.5;
    if (CFG.cooldown_quota <= 0.0) CFG.cooldown_quota = 300.0;
    if (CFG.api_key_param.empty()) CFG.api_key_param = "apikey";
    // min_rps and the piso of percent: never adult that the propria percent (senao the
    // 429 AUMENTARIA the speed) neither <= 0.
    if (CFG.min_rps <= 0.0) CFG.min_rps = CFG.requests_per_second * 0.1;
    if (CFG.min_rps > CFG.requests_per_second) CFG.min_rps = CFG.requests_per_second;
    CFG.datasets = montar_datasets_genericos(ini);
    // db empty in dataset = db norm of [general]; api_keys empty = herda [general]
    for (auto& d : CFG.datasets) {
        if (d.db.empty()) d.db = CFG.db_path;
        if (d.api_keys.empty()) d.api_keys = CFG.api_keys;
    }
}

// Log + utilidades of date
static std::mutex g_print_mtx;
static std::mutex g_write_mtx; // serializa gravacoes in database (equivale write_lock)

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
        throw std::runtime_error("date invalida: " + s);
    return d;
}

// Algorithm of Howard Hinnant (days_from_civil / civil_from_days)
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

// Date place in formato YYYY-MM-DD (compara lexicograficamente with the datas)
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
        log(std::string("[WARNING] Error at go to court datas: ") + e.what());
        return {inicio, fim};
    }
}

// Rate-limit: budget in window deslizante (minuto/hora/dia, in unidades
// fracionarias of chamada) + espacamento global of envios by reserva of
// slot + cooldown of 429 compartilhado by all the threads.
static std::mutex g_rate_mtx;
static std::chrono::steady_clock::time_point g_proximo_slot; // next envio permitido
static double g_intervalo = 0.25; // segundos between envios (1 / rps current)
// Cooldown global absoluto (epoch steady): all the threads esperam until he
static std::chrono::steady_clock::time_point g_cooldown_fim;
static int g_429_consecutivos = 0;
// Cooldown by endpoint (rotacao of multiplas URLs "the | b")
static std::map<std::string, std::chrono::steady_clock::time_point> g_ep_cooldown;

// Budget by bevy of quota: key of API (each key = quota propria) or
// "anonymous" (tier gratuito without key: all the URLs compartilham the quota of IP).
struct Janela {
    std::deque<std::pair<std::chrono::steady_clock::time_point, double>> ev; // by ts
    double soma = 0.0;

    void limpar(std::chrono::steady_clock::time_point agora, double dur) {
        auto corte = agora - std::chrono::duration<double>(dur);
        while (!ev.empty() && !(ev.front().first > corte)) {
            soma -= ev.front().second;
            ev.pop_front();
        }
    }
    // Segundos necessarios to cabir `cost` (0 = already cabe). Not reserva.
    double espera(std::chrono::steady_clock::time_point agora, double dur,
                  double limite, double custo) const {
        if (limite <= 0.0 || soma + custo <= limite) return 0.0;
        if (ev.empty()) return 0.0; // cost > limit: deixa the request fare
        auto expira = ev.front().first + std::chrono::duration<double>(dur);
        double w = std::chrono::duration<double>(expira - agora).count();
        return w > 0.0 ? w + 0.05 : 0.05;
    }
    void reservar(std::chrono::steady_clock::time_point ts, double custo) {
        ev.emplace_back(ts, custo);
        soma += custo;
    }
};
struct Orcamento {
    Janela minuto, hora, dia;

    void limpar(const std::chrono::steady_clock::time_point& agora) {
        minuto.limpar(agora, 60.0);
        hora.limpar(agora, 3600.0);
        dia.limpar(agora, 86400.0);
    }
    double espera(std::chrono::steady_clock::time_point agora, double custo) const {
        return std::max({minuto.espera(agora, 60.0, CFG.limite_minuto, custo),
                         hora.espera(agora, 3600.0, CFG.limite_hora, custo),
                         dia.espera(agora, 86400.0, CFG.limite_dia, custo)});
    }
    void reservar(std::chrono::steady_clock::time_point ts, double custo) {
        minuto.reservar(ts, custo);
        hora.reservar(ts, custo);
        dia.reservar(ts, custo);
    }
};
static std::map<std::string, Orcamento> g_orcamento;

static void iniciar_rate_limit() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    auto agora = std::chrono::steady_clock::now();
    g_proximo_slot = agora;
    g_intervalo = 1.0 / CFG.requests_per_second;
    g_cooldown_fim = agora;
    g_429_consecutivos = 0;
    g_orcamento.clear();
    g_ep_cooldown.clear();
}

// Gerador by thread (thread-safe by building)
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

// Expectation until be able to send and RESERVA slot of envio + budget atomicamente
// (the two reservas in the exclusively lock impedem that threads disparem in rajada and
// that ultrapassem juntas the janelas of minuto/hora/dia).
static void esperar_envio(const std::string& grupo, double custo) {
    static std::atomic<long long> g_ult_log_orc{0};
    for (int volta = 0; volta < 5000; ++volta) {
        double espera = 0.0;
        bool reservado = false;
        bool por_orcamento = false;
        {
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            auto agora = std::chrono::steady_clock::now();
            // 1) cooldown global of 429 (compartilhado by all the threads)
            if (agora < g_cooldown_fim)
                espera = std::chrono::duration<double>(g_cooldown_fim - agora).count();
            // 2) janelas of budget (minuto/hora/dia)
            if (espera <= 0.0) {
                auto& orc = g_orcamento[grupo];
                orc.limpar(agora);
                espera = orc.espera(agora, custo);
                por_orcamento = espera > 0.0;
            }
            // 3) all livre: reserva slot + budget of the occasion
            if (espera <= 0.0) {
                auto& orc = g_orcamento[grupo];
                auto slot = std::max(agora, g_proximo_slot);
                std::uniform_real_distribution<double> jit(0.9, 1.1);
                g_proximo_slot = slot + std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(
                                         g_intervalo * jit(rng())));
                orc.reservar(slot, custo);
                espera = std::chrono::duration<double>(slot - agora).count();
                reservado = true;
            }
        }
        if (reservado) {
            dormir(espera);
            return;
        }
        // Log in short supply of expectation longa of budget (1 thread / 60s)
        if (por_orcamento && espera > 30.0) {
            long long seg = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            long long ult = g_ult_log_orc.load();
            if (seg - ult >= 60 && g_ult_log_orc.compare_exchange_strong(ult, seg)) {
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "  [BUDGET] Quota of API atingida (bevy %s). "
                              "Waiting %.0fs to liberar...",
                              grupo.c_str(), espera);
                log(buf);
            }
        }
        dormir(espera > 0.0 ? espera : 0.05);
    }
    log("  [WARNING] Budget not liberou after too many esperas; seguindo same so.");
}

// Registra the 429: fold the interval between envios (AIMD), bead to the
// circuit breaker and ativa the cooldown GLOBAL (epoch steady) compartilhado
// by all the threads. Before, each thread esperava by bead propria
// while the outras continuavam enviando - era the "hurricane" of 429.
// `endpoint` (URL element) entra in cooldown decent p/ rotacao "the | b".
static void tratar_429(double espera, const std::string& motivo,
                       const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    auto agora = std::chrono::steady_clock::now();
    // AIMD: fold the interval the each 429 (piso of lentidao = 1/min_rps)
    double intervalo_max = (CFG.min_rps > 0.0) ? 1.0 / CFG.min_rps : 1.0e9;
    g_intervalo = std::min(intervalo_max, g_intervalo * 2.0);
    // Bead exclusively 429s that chegam abroad of the cooldown already ativo (evita that 3
    // threads simultaneas inflacionem the contador and reativem the breaker).
    if (agora >= g_cooldown_fim) g_429_consecutivos++;
    std::string motivo_final = motivo;
    if (g_429_consecutivos >= CFG.circuit_limit_429) {
        espera = std::max(espera, CFG.cooldown_429);
        motivo_final = "circuit breaker";
        g_429_consecutivos = 0;
    }
    if (espera > 0.0) {
        auto novo_fim = agora + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(std::chrono::duration<double>(espera));
        if (novo_fim > g_cooldown_fim) {
            bool primeiro = agora >= g_cooldown_fim;
            g_cooldown_fim = novo_fim;
            // Log alone by event (not the thread by thread in expectation)
            if (primeiro || espera > 30.0) {
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                              "  [429] Break global of %.0fs (%s; interval %.2fs)",
                              espera, motivo_final.c_str(), g_intervalo);
                log(buf);
            }
        }
    }
    if (!endpoint.empty() && espera > 0.0) {
        auto fim_ep = agora + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(std::chrono::duration<double>(espera));
        auto& c = g_ep_cooldown[endpoint];
        if (fim_ep > c) c = fim_ep;
    }
}

// Success: reseta the contador of circuit breaker and recupera the percent aos
// poucos. NOT grate the cooldown ativo (bug old: any success in
// flight derrubava the break global and the access 429 recomecava in hour).
static void restaurar_taxa() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_429_consecutivos = 0;
    double intervalo_min = 1.0 / CFG.requests_per_second;
    if (g_intervalo > intervalo_min)
        g_intervalo = std::max(intervalo_min, g_intervalo * 0.85);
}

static double backoff_exponencial(int tentativa) {
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    double bruto = CFG.backoff_inicial * std::pow(2.0, tentativa - 1);
    return std::min(CFG.backoff_maximo, bruto * dist(rng()));
}

// Database of date (SQLite) - the connection by thread by file, therefore the same
// download pode engrave in the few .db at same team (multiplas APIs).
// Tipos of value usados hair engine general.
using Valor = std::variant<std::monostate, double, std::string>;
using Registro = std::map<std::string, Valor>;

static void executar_sql(sqlite3* conn, const char* sql) {
    char* erro = nullptr;
    if (sqlite3_exec(conn, sql, nullptr, nullptr, &erro) != SQLITE_OK) {
        std::string msg = erro ? erro : "error unknown";
        sqlite3_free(erro);
        throw std::runtime_error("SQLite: " + msg);
    }
}

static void executar_sql(sqlite3* conn, const std::string& sql) {
    executar_sql(conn, sql.c_str());
}

static sqlite3* get_connection_para(const std::string& db_path) {
    // Cache thread-local: path -> connection (the download pode use N bancos).
    static thread_local std::map<std::string, sqlite3*> conns;
    auto it = conns.find(db_path);
    if (it != conns.end() && it->second) return it->second;
    sqlite3* conn = nullptr;
    if (sqlite3_open(db_path.c_str(), &conn) != SQLITE_OK) {
        std::string msg = conn ? sqlite3_errmsg(conn) : "failure at open";
        if (conn) sqlite3_close(conn);
        throw std::runtime_error("SQLite: not foi possible open " + db_path + ": " + msg);
    }
    executar_sql(conn, "PRAGMA journal_mode=WAL");
    executar_sql(conn, "PRAGMA synchronous=NORMAL");
    executar_sql(conn, "PRAGMA busy_timeout=60000");
    conns[db_path] = conn;
    return conn;
}

static std::string resolver_data_limite(const std::string& v, const std::string& padrao) {
    std::string s = cortar(v);
    if (s.empty()) return padrao;
    std::string low = s;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (low == "hoje" || low == "today" || low == "now") return hoje();
    return s; // date YYYY-MM-DD compara lexicograficamente
}

static bool data_no_intervalo(const std::string& d, const DatasetCfg& ds) {
    std::string mn = resolver_data_limite(ds.data_min, "");
    std::string mx = resolver_data_limite(ds.data_max, "");
    if (!mn.empty() && d < mn) return false;
    if (!mx.empty() && d > mx) return false;
    return true;
}

// Infere the type SQLite of the column the start running of values observados.
// "auto" -> integer if all are integers, "real" if any decimal,
// text if houver any string/data. Type declarado in .ini prevalece.
static std::string inferir_tipo_coluna(const std::string& declarado,
                                       const std::vector<Registro>& regs,
                                       const std::string& coluna) {
    if (declarado == "integer") return "INTEGER";
    if (declarado == "real") return "REAL";
    if (declarado == "text") return "TEXT";
    bool viu_real = false, viu_texto = false, viu_int = false, viu_algo = false;
    size_t n = 0;
    for (const auto& r : regs) {
        if (n++ > 200) break;
        auto it = r.find(coluna);
        if (it == r.end() || std::holds_alternative<std::monostate>(it->second)) continue;
        viu_algo = true;
        if (std::holds_alternative<std::string>(it->second)) {
            viu_texto = true;
            break;
        }
        double v = std::get<double>(it->second);
        if (std::floor(v) == v && std::fabs(v) < 9e15)
            viu_int = true;
        else
            viu_real = true;
    }
    if (viu_texto) return "TEXT";
    if (viu_real) return "REAL";
    if (viu_int) return "INTEGER";
    if (viu_algo) return "REAL";
    if (coluna == "data" || coluna == "hora" || coluna == "timezone") return "TEXT";
    if (coluna == "latitude" || coluna == "longitude") return "REAL";
    return "REAL";
}

static std::vector<std::string> colunas_da_tabela(sqlite3* conn, const std::string& tabela) {
    std::vector<std::string> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "PRAGMA table_info(\"" + tabela + "\")";
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_finalize(st);
        return out;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* nome = sqlite3_column_text(st, 1);
        if (nome) out.emplace_back(reinterpret_cast<const char*>(nome));
    }
    sqlite3_finalize(st);
    return out;
}

// Cria the table of dataset automatically (or evolui with ADD COLUMN).
// Colunas: reservadas (data/hora/lat/lon) + descobertas of response of API
// (test) + overrides opcionais of .ini + created_at. The colunas already
// existentes healthy verificadas road PRAGMA table_info (exclusively ADD COLUMN the that absence)
// and the tipos vem inferidos of date when not declarados.
// Key UNIQUE: (data,lat,lon) p/ diary or (data,hora,lat,lon) p/ schedule.
static void garantir_tabela(const DatasetCfg& ds, const std::vector<Registro>& amostra) {
    sqlite3* conn = get_connection_para(ds.db);
    std::vector<std::string> ordem;
    if (!ds.chave_data.empty()) ordem.push_back(ds.chave_data);
    if (ds.salvar_hora && !ds.chave_hora.empty() && ds.chave_hora != ds.chave_data)
        ordem.push_back(ds.chave_hora);
    ordem.push_back("latitude");
    ordem.push_back("longitude");
    for (const auto& c : ds.colunas) {
        if (std::find(ordem.begin(), ordem.end(), c.coluna) == ordem.end())
            ordem.push_back(c.coluna);
    }
    // Colunas descobertas automatically in response of API: toda key of
    // account book that not seja reservada vira column (type inferido of date).
    for (const auto& r : amostra) {
        for (const auto& kv : r) {
            if (coluna_reservada(kv.first)) continue;
            if (std::find(ordem.begin(), ordem.end(), kv.first) == ordem.end())
                ordem.push_back(kv.first);
        }
    }
    if (ds.salvar_timezone &&
        std::find(ordem.begin(), ordem.end(), "timezone") == ordem.end())
        ordem.push_back("timezone");

    std::map<std::string, std::string> tipo_decl;
    for (const auto& c : ds.colunas) tipo_decl[c.coluna] = c.tipo;

    std::vector<std::string> existentes = colunas_da_tabela(conn, ds.tabela);
    if (existentes.empty()) {
        std::string sql = "CREATE TABLE IF NOT EXISTS \"" + ds.tabela + "\" (\n";
        bool prim = true;
        for (const auto& col : ordem) {
            std::string decl = "auto";
            auto it = tipo_decl.find(col);
            if (it != tipo_decl.end()) decl = it->second;
            sql += (prim ? "  " : ", ") + col + " " + inferir_tipo_coluna(decl, amostra, col) +
                   "\n";
            prim = false;
        }
        sql += ", created_at TEXT";
        // key of deduplicacao
        std::vector<std::string> chave;
        if (!ds.chave_data.empty()) chave.push_back(ds.chave_data);
        if (ds.salvar_hora && !ds.chave_hora.empty() && ds.chave_hora != ds.chave_data)
            chave.push_back(ds.chave_hora);
        chave.push_back("latitude");
        chave.push_back("longitude");
        sql += ",\n  UNIQUE (" + juntar(chave, ", ") + ")\n)";
        executar_sql(conn, sql);
        log("[DB] Table created: " + ds.db + "." + ds.tabela + " (" +
            std::to_string(ordem.size()) + " columns)");
    } else {
        for (const auto& col : ordem) {
            if (std::find(existentes.begin(), existentes.end(), col) == existentes.end()) {
                std::string decl = "auto";
                auto it = tipo_decl.find(col);
                if (it != tipo_decl.end()) decl = it->second;
                executar_sql(conn, "ALTER TABLE \"" + ds.tabela + "\" ADD COLUMN " + col +
                                       " " + inferir_tipo_coluna(decl, amostra, col));
                log("[DB] Column added: " + ds.tabela + "." + col);
            }
        }
        if (std::find(existentes.begin(), existentes.end(), "created_at") ==
            existentes.end())
            executar_sql(conn,
                         "ALTER TABLE \"" + ds.tabela + "\" ADD COLUMN created_at TEXT");
    }
    executar_sql(conn, "CREATE INDEX IF NOT EXISTS idx_" + ds.tabela +
                           "_loc ON \"" + ds.tabela + "\"(latitude, longitude, \"" +
                           (ds.chave_data.empty() ? "latitude" : ds.chave_data) + "\")");
    if (!ds.chave_data.empty())
        executar_sql(conn, "CREATE INDEX IF NOT EXISTS idx_" + ds.tabela + "_date ON \"" +
                               ds.tabela + "\"(\"" + ds.chave_data + "\")");
}

static void criar_tabelas_datasets() {
    // Tabelas genericas ([dataset:*]): cria (or evolui) each table in his
    // .db and cria files vazios antecipadamente to that multiplos .db
    // existam same before of first download.
    for (const auto& ds : CFG.datasets) {
        try {
            std::vector<Registro> vazio;
            garantir_tabela(ds, vazio);
        } catch (const std::exception& e) {
            log(std::string("[WARNING] ") + ds.db + "." + ds.tabela + ": " + e.what());
        }
    }
}

// Registros existentes in database (carregados the unica occasion)
using Chave = std::pair<double, double>;
using ConjuntoDatas = std::set<std::string>;

// Produtores fazem leitura compartilhada; consumidores fazem escrita exclusiva
static std::shared_mutex g_mtx_existentes;

static const ConjuntoDatas& datas_vazias() {
    static const ConjuntoDatas vazio;
    return vazio;
}

// Equivalent the _get_valor() of download.py
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

static std::optional<json> baixar_com_retry(const std::string& url,
                                            const std::map<std::string, std::string>& params,
                                            int max_tentativas, double custo,
                                            const std::string& chave_api);

// Parse general: block JSON -> registros. COLUNAS DESCOBERTAS
// AUTOMATICALLY: toda key array of block (besides "team") vira column with
// name normalizado; the type and inferido at criar/evoluir the table. Overrides
// opcionais of .ini (column.*) only renomeiam/tipam.
static std::vector<Registro> parse_dataset(const json& bloco_json, const json& raiz,
                                           double lat, double lon, const DatasetCfg& ds,
                                           const std::set<std::string>* filtro,
                                           bool forcar_hora = false) {
    if (!bloco_json.is_object()) return {};
    std::vector<std::string> times;
    auto it_t = bloco_json.find("time");
    if (it_t != bloco_json.end() && it_t->is_array()) {
        for (const auto& t : *it_t) {
            if (t.is_string()) times.push_back(t.get<std::string>());
        }
    }
    // Overrides opcionais: variable of API -> column renomeada/tipada
    std::map<std::string, ColunaCfg> overrides;
    for (const auto& c : ds.colunas) overrides[c.variavel_api] = c;
    std::vector<Registro> regs;
    regs.reserve(times.size());
    for (size_t i = 0; i < times.size(); ++i) {
        const std::string& t = times[i];
        std::string data = t.substr(0, std::min<size_t>(10, t.size()));
        if (filtro && !filtro->count(data)) continue;
        if (!data_no_intervalo(data, ds)) continue;
        Registro reg;
        reg["latitude"] = lat;
        reg["longitude"] = lon;
        if (!ds.chave_data.empty()) reg[ds.chave_data] = data;
        // Polen redirecionado vira hourly: grava "hora" mesmo que o dataset
        // esteja marcado como dia no .ini (senao 24 linhas/dia colapsam numa
        // so no UPSERT e quase tudo se perde).
        if ((ds.salvar_hora || forcar_hora) && !ds.chave_hora.empty())
            reg[ds.chave_hora] = t;
        if (ds.salvar_timezone) {
            auto itz = raiz.find("timezone");
            if (itz != raiz.end() && itz->is_string())
                reg["timezone"] = itz->get<std::string>();
            else if (bloco_json.contains("timezone") && bloco_json["timezone"].is_string())
                reg["timezone"] = bloco_json["timezone"].get<std::string>();
        }
        for (auto it = bloco_json.begin(); it != bloco_json.end(); ++it) {
            if (it.key() == "time" || !it.value().is_array()) continue;
            auto ov = overrides.find(it.key());
            std::string col =
                ov != overrides.end() ? ov->second.coluna : normalizar_nome(it.key());
            if (coluna_reservada(col) || col == ds.chave_data || col == ds.chave_hora)
                continue;
            reg[col] = pegar(bloco_json, it.key(), i);
        }
        regs.push_back(std::move(reg));
    }
    return regs;
}

static std::string url_do_dataset(const DatasetCfg& ds, const std::string& data) {
    // url_passado p/ datas < hoje (archive), url_futuro p/ datas >= hoje.
    // O archive aceita ate ~hoje mas o forecast REJEITA start_date antigo
    // com 400 — por isso a fronteira precisa ser exata e os blocos nunca
    // podem misturar passado+futuro. Datas "YYYY-MM-DD" zero-padded
    // comparam lexicograficamente = ordem cronologica, sem parsear.
    if (!ds.url_passado.empty() || !ds.url_futuro.empty()) {
        bool futuro = !(data < hoje());
        if (!futuro && !ds.url_passado.empty()) return ds.url_passado;
        if (futuro && !ds.url_futuro.empty()) return ds.url_futuro;
    }
    return ds.url;
}

// Rotacao round-robin between varias URLs of same country ("url = the | b"):
// espalha the job between endpoints and pula the that estao in cooldown of 429.
// If all estiverem resfriados, expectation the under age cooldown and tenta of new.
static std::string escolher_url(const std::string& lista) {
    auto cands = dividir(lista, '|');
    cands.erase(std::remove_if(cands.begin(), cands.end(), [](const std::string& s) {
        return s.empty();
    }), cands.end());
    if (cands.empty()) return lista;
    if (cands.size() == 1) return cands.front();
    static std::atomic<unsigned> g_rr_url{0};
    for (;;) {
        double espera = -1.0;
        {
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            auto agora = std::chrono::steady_clock::now();
            unsigned base = g_rr_url.fetch_add(1);
            for (size_t k = 0; k < cands.size(); ++k) {
                const std::string& u = cands[(base + k) % cands.size()];
                auto it = g_ep_cooldown.find(u);
                if (it == g_ep_cooldown.end() || it->second <= agora) return u;
                double rem = std::chrono::duration<double>(it->second - agora).count();
                if (espera < 0.0 || rem < espera) espera = rem;
            }
        }
        dormir((espera > 0.0 ? espera : 0.5) + 0.1);
    }
}

static std::vector<Registro> baixar_dataset_bloco(const DatasetCfg& ds,
                                                  const std::vector<std::string>& datas,
                                                  double lat, double lon) {
    std::vector<std::string> alvo;
    for (const auto& d : datas) {
        if (data_no_intervalo(d, ds)) alvo.push_back(d);
    }
    if (alvo.empty()) return {};
    std::sort(alvo.begin(), alvo.end());
    std::vector<Registro> todos;
    // Agrupa by URL (former x future) in blocos of chunk_dias.
    size_t i = 0;
    while (i < alvo.size()) {
        std::string lista = url_do_dataset(ds, alvo[i]);
        size_t j = i;
        while (j < alvo.size() && url_do_dataset(ds, alvo[j]) == lista &&
               (j - i) < static_cast<size_t>(CFG.chunk_dias))
            ++j;
        std::vector<std::string> grupo(alvo.begin() + i, alvo.begin() + j);
        std::string bloco_req = ds.bloco;
        std::string chave_req = ds.params_chave;
        std::vector<std::string> vars = ds.vars;
        // POLEN: o endpoint dedicado api.open-meteo.com/v1/pollen NAO existe
        // (retorna 404 "Not Found"). Polen mora no air-quality-api com bloco
        // hourly e nomes sem sufixo _mean (alder_pollen, nao alder_pollen_mean).
        bool eh_polen = (lista.find("pollen") != std::string::npos);
        if (eh_polen) {
            lista = "https://air-quality-api.open-meteo.com/v1/air-quality";
            bloco_req = "hourly";
            chave_req = "hourly";
            for (auto& v : vars) {
                const std::string suf = "_mean";
                if (v.size() > suf.size() &&
                    v.compare(v.size() - suf.size(), suf.size(), suf) == 0)
                    v = v.substr(0, v.size() - suf.size());
            }
        }
        if (lista == ds.url_passado && !ds.url_passado.empty() && !ds.vars_excluir.empty()) {
            vars.erase(std::remove_if(vars.begin(), vars.end(),
                                      [&](const std::string& v) {
                                          return ds.vars_excluir.count(v);
                                      }),
                       vars.end());
        }
        // Cost of request in unidades fracionarias of chamada (FAQ
        // Open-Meteo): max(1, vars/10, dias*1.5/14) - ex.: 30 dias x 24
        // vars ~ 3.2 calls. AND the that the budget min/hora/dia desconta.
        double custo = 1.0;
        if (!vars.empty())
            custo = std::max(custo,
                             static_cast<double>(vars.size()) / CFG.custo_vars_ref);
        try {
            Data da = para_data(grupo.front());
            Data db = para_data(grupo.back());
            long d1 = dias_de_civil(da.ano, da.mes, da.dia);
            long d2 = dias_de_civil(db.ano, db.mes, db.dia);
            custo = std::max(custo, (d2 - d1 + 1) * CFG.custo_dias_fator /
                                        CFG.custo_dias_ref);
        } catch (...) {
            custo = std::max(custo, static_cast<double>(grupo.size()) *
                                        CFG.custo_dias_fator / CFG.custo_dias_ref);
        }
        // Key of API rotacionada: each key tem budget decent, now
        // "api_keys = k1 | k2" fold the quota disponivel.
        std::string chave_api;
        if (!ds.api_keys.empty()) {
            auto chaves = dividir(ds.api_keys, '|');
            chaves.erase(std::remove_if(chaves.begin(), chaves.end(), [](const std::string& s) {
                return s.empty();
            }), chaves.end());
            if (!chaves.empty()) {
                static std::atomic<unsigned> g_rr_chave{0};
                chave_api = chaves[g_rr_chave.fetch_add(1) % chaves.size()];
            }
        }
        std::map<std::string, std::string> params = {
            {"latitude", num_str(lat)},
            {"longitude", num_str(lon)},
            {"start_date", grupo.front()},
            {"end_date", grupo.back()},
            {"timezone", CFG.fuso_horario},
        };
        if (!vars.empty()) params[chave_req] = juntar(vars, ",");
        if (!chave_api.empty()) params[CFG.api_key_param] = chave_api;
        auto dados = baixar_com_retry(escolher_url(lista), params, CFG.max_retries,
                                      custo, chave_api);
        if (dados && dados->contains(bloco_req) && (*dados)[bloco_req].is_object()) {
            std::set<std::string> filtro(grupo.begin(), grupo.end());
            auto regs = parse_dataset((*dados)[bloco_req], *dados, lat, lon, ds, &filtro,
                                    eh_polen);
            todos.insert(todos.end(), std::make_move_iterator(regs.begin()),
                         std::make_move_iterator(regs.end()));
        }
        i = j;
    }
    return todos;
}

// HTTP (libcurl) + retry/backoff
struct RespostaHttp {
    bool erro_rede = false; // timeout/conexao (equivale Timeout/ConnectionError)
    std::string erro_mensagem; // descricao of error of network
    long status = 0; // HTTP status (0 if error of network)
    std::string corpo;
    double retry_after = -1.0; // header Retry-After, if gift
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
    std::transform(baixo.begin(), baixo.end(), baixo.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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
        r.erro_mensagem = "failure at start curl";
        return r;
    }
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, escrever_corpo);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.corpo);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ler_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(CFG.http_timeout * 1000.0));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // gzip automatic

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
                                            int max_tentativas, double custo,
                                            const std::string& chave_api) {
    std::string url_completa = montar_url(url, params);
    // Bevy of budget: key of API (quota propria) or "anonymous" (quota of IP)
    std::string grupo = chave_api.empty() ? std::string("anonymous") : chave_api;
    for (int tentativa = 1; tentativa <= max_tentativas; ++tentativa) {
        esperar_envio(grupo, custo); // cooldown global + budget + slot

        RespostaHttp r = http_get(url_completa);

        if (!r.erro_rede && r.status < 400) {
            try {
                restaurar_taxa();
                return json::parse(r.corpo);
            } catch (const std::exception& e) {
                if (tentativa == max_tentativas) {
                    log("  [ERROR] Response without JSON valid after " +
                        std::to_string(max_tentativas) + " attempts: " + e.what());
                    return std::nullopt;
                }
                double espera = backoff_exponencial(tentativa);
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "  [RETRY %d/%d] Response without JSON valid. Waiting %.1fs...",
                              tentativa, max_tentativas, espera);
                log(buf);
                dormir(espera);
                continue;
            }
        }

        if (!r.erro_rede && r.status == 429) {
            // THE body of error diz which limit estourou (ex.: "Daily API
            // request limit exceeded") - quota exige break longa, not retry
            // fast with backoff crescente.
            std::string corpo = r.corpo.substr(0, 1000);
            std::transform(corpo.begin(), corpo.end(), corpo.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            bool quota = corpo.find("daily") != std::string::npos ||
                         corpo.find("per day") != std::string::npos ||
                         corpo.find("hour") != std::string::npos ||
                         corpo.find("minute") != std::string::npos ||
                         corpo.find("month") != std::string::npos;
            // Prioridade: Retry-After of server > break of quota > backoff
            double espera;
            if (r.retry_after > 0)
                espera = r.retry_after;
            else if (quota)
                espera = CFG.cooldown_quota;
            else
                espera = std::max(CFG.espera_min_429, backoff_exponencial(tentativa));
            // Cooldown GLOBAL (all the threads param juntas, log alone) +
            // cooldown of endpoint p/ rotacao of URLs. Not dorme here: the
            // proxima bend expectation the cooldown compartilhado (evita expectation
            // dupla and the hurricane of retries by thread).
            tratar_429(espera, quota ? "quota of API exceeded" : "HTTP 429", url);
            if (tentativa == max_tentativas) {
                log("  [ERROR] 429 persistent after " +
                    std::to_string(max_tentativas) + " attempts");
                return std::nullopt;
            }
            char buf[220];
            std::snprintf(buf, sizeof(buf),
                          "  [RETRY %d/%d] HTTP 429%s. Break global of %.0fs...",
                          tentativa, max_tentativas, quota ? " (quota)" : "",
                          espera);
            log(buf);
        } else if (!r.erro_rede) {
            // 400/404 = pedido invalido (var errada, endpoint morto, range
            // fora do permitido): repetir nao adianta, falha rapido e mostra
            // o motivo + URL para diagnostico.
            if (r.status == 400 || r.status == 404) {
                std::string motivo = r.corpo.substr(0, 300);
                log("  [ERROR] HTTP " + std::to_string(r.status) +
                    " (sem retry): " + motivo + " :: " + url_completa);
                return std::nullopt;
            }
            if (tentativa == max_tentativas) {
                log("  [ERROR] Failure after " + std::to_string(max_tentativas) +
                    " attempts: HTTP " + std::to_string(r.status));
                return std::nullopt;
            }
            double espera = backoff_exponencial(tentativa);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "  [RETRY %d/%d] HTTP %ld. Waiting %.1fs...",
                          tentativa, max_tentativas, r.status, espera);
            log(buf);
            dormir(espera);
        } else {
            if (tentativa == max_tentativas) {
                log("  [ERROR] Failure after " + std::to_string(max_tentativas) +
                    " attempts: " + r.erro_mensagem);
                return std::nullopt;
            }
            double espera = backoff_exponencial(tentativa);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "  [RETRY %d/%d] %s. Waiting %.1fs...",
                          tentativa, max_tentativas, r.erro_mensagem.c_str(), espera);
            log(buf);
            dormir(espera);
        }
    }
    return std::nullopt;
}

// Salvamento general: garante the table (cria sozinha), descobre the colunas
// reais of database and grava with UPSERT by key (merge between APIs).
static void salvar_dataset(const DatasetCfg& ds, const std::vector<Registro>& registros) {
    if (registros.empty()) return;
    garantir_tabela(ds, registros);
    sqlite3* conn = get_connection_para(ds.db);
    std::vector<std::string> campos = colunas_da_tabela(conn, ds.tabela);
    campos.erase(std::remove(campos.begin(), campos.end(), "created_at"), campos.end());
    // mantem order canonica: data,hora,lat,lon first (nomes configurados)
    std::vector<std::string> pref;
    auto pref_add = [&](const std::string& c) {
        if (c.empty() || std::find(pref.begin(), pref.end(), c) != pref.end()) return;
        if (std::find(campos.begin(), campos.end(), c) != campos.end()) pref.push_back(c);
    };
    pref_add(ds.chave_data);
    if (ds.salvar_hora) pref_add(ds.chave_hora);
    pref_add("latitude");
    pref_add("longitude");
    pref_add("timezone");
    for (const auto& c : campos) {
        if (std::find(pref.begin(), pref.end(), c) == pref.end()) pref.push_back(c);
    }
    campos.swap(pref);
    if (campos.empty()) return;

    std::vector<std::string> chave;
    if (!ds.chave_data.empty()) chave.push_back(ds.chave_data);
    if (ds.salvar_hora && !ds.chave_hora.empty() && ds.chave_hora != ds.chave_data)
        chave.push_back(ds.chave_hora);
    chave.push_back("latitude");
    chave.push_back("longitude");

    std::vector<std::string> marc(campos.size() + 1, "?");
    std::string sql = "INSERT INTO \"" + ds.tabela + "\" (" + juntar(campos, ",") +
                      ", created_at) VALUES (" + juntar(marc, ",") + ")";
    // UPSERT: atualiza exclusively the colunas nao-chave (merge between APIs/tabelas iguais)
    std::vector<std::string> upd;
    for (const auto& c : campos) {
        if (std::find(chave.begin(), chave.end(), c) == chave.end())
            upd.push_back("\"" + c + "\"=excluded.\"" + c + "\"");
    }
    if (!upd.empty() && !chave.empty())
        sql += " ON CONFLICT(\"" + juntar(chave, "\",\"") + "\") DO UPDATE SET " + juntar(upd, ",");
    else
        sql += " ON CONFLICT DO NOTHING";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(conn);
        sqlite3_finalize(stmt);
        throw std::runtime_error("SQLite: " + err);
    }
    const std::string agora = agora_iso();
    executar_sql(conn, "BEGIN");
    size_t lote = 0;
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
        if (sqlite3_step(stmt) != SQLITE_DONE)
            log("[ERROR] " + ds.tabela + ": " + sqlite3_errmsg(conn));
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (++lote >= static_cast<size_t>(CFG.max_sql_dates_por_lote)) {
            executar_sql(conn, "COMMIT");
            executar_sql(conn, "BEGIN");
            lote = 0;
        }
    }
    executar_sql(conn, "COMMIT");
    sqlite3_finalize(stmt);
}

// Orquestracao generica: block (dataset, datas, coord) -> file -> engrave.
struct ResultadoGenerico {
    size_t dataset_idx = 0;
    double lat = 0.0, lon = 0.0;
    std::vector<std::string> datas_pedidas;
    std::vector<Registro> registros;
};

using ExistentesGen = std::map<size_t, std::map<Chave, ConjuntoDatas>>;

static ExistentesGen carregar_existentes_generico(const std::vector<std::string>& datas) {
    ExistentesGen out;
    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        const DatasetCfg& ds = CFG.datasets[di];
        sqlite3* conn = get_connection_para(ds.db);
        std::vector<std::string> cols = colunas_da_tabela(conn, ds.tabela);
        bool tem_data =
            !ds.chave_data.empty() &&
            std::find(cols.begin(), cols.end(), ds.chave_data) != cols.end();
        bool tem_lat = std::find(cols.begin(), cols.end(), "latitude") != cols.end();
        bool tem_lon = std::find(cols.begin(), cols.end(), "longitude") != cols.end();
        auto& mapa = out[di];
        for (const auto& c : CFG.coordenadas) mapa[c] = ConjuntoDatas();
        if (!tem_data || !tem_lat || !tem_lon || datas.empty()) continue;
        std::string sql = "SELECT latitude, longitude, \"" + ds.chave_data + "\" FROM \"" +
                          ds.tabela + "\" WHERE \"" + ds.chave_data +
                          "\" BETWEEN ? AND ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            sqlite3_finalize(st);
            continue;
        }
        sqlite3_bind_text(st, 1, datas.front().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, datas.back().c_str(), -1, SQLITE_TRANSIENT);
        std::set<Chave> alvos(CFG.coordenadas.begin(), CFG.coordenadas.end());
        ConjuntoDatas periodo(datas.begin(), datas.end());
        while (sqlite3_step(st) == SQLITE_ROW) {
            double la = sqlite3_column_double(st, 0);
            double lo = sqlite3_column_double(st, 1);
            const unsigned char* tx = sqlite3_column_text(st, 2);
            if (!tx) continue;
            std::string dt(reinterpret_cast<const char*>(tx));
            if (!periodo.count(dt)) continue;
            Chave ch{la, lo};
            if (!alvos.count(ch)) continue;
            mapa[ch].insert(dt);
        }
        sqlite3_finalize(st);
    }
    return out;
}

static std::optional<ResultadoGenerico> baixar_bloco_generico(
    size_t dataset_idx, const std::vector<std::string>& datas, double lat, double lon,
    const ExistentesGen& existentes) {
    std::uniform_real_distribution<double> jitter(0.0, CFG.jitter_inicial_max);
    dormir(jitter(rng()));
    const DatasetCfg& ds = CFG.datasets[dataset_idx];
    Chave chave{lat, lon};
    std::vector<std::string> faltantes;
    {
        std::shared_lock lk(g_mtx_existentes);
        auto it = existentes.find(dataset_idx);
        const ConjuntoDatas* tem =
            (it == existentes.end()) ? nullptr : &(it->second.find(chave) == it->second.end()
                                                       ? datas_vazias()
                                                       : it->second.at(chave));
        for (const auto& d : datas) {
            if (!data_no_intervalo(d, ds)) continue;
            if (!tem || !tem->count(d)) faltantes.push_back(d);
        }
    }
    ResultadoGenerico r;
    r.dataset_idx = dataset_idx;
    r.lat = lat;
    r.lon = lon;
    r.datas_pedidas = faltantes;
    if (!faltantes.empty()) r.registros = baixar_dataset_bloco(ds, faltantes, lat, lon);
    return r;
}

static void processar_generico(const ResultadoGenerico& r, ExistentesGen& existentes,
                               std::map<size_t, long long>& baixados,
                               std::map<size_t, long long>& pulados) {
    const DatasetCfg& ds = CFG.datasets[r.dataset_idx];
    // dias pedidos without retorno = already existentes or abroad of interval of API
    long long dias_retornados = 0;
    {
        std::set<std::string> dias;
        std::string col = ds.chave_data.empty() ? "data" : ds.chave_data;
        for (const auto& reg : r.registros) {
            auto it = reg.find(col);
            if (it != reg.end() && std::holds_alternative<std::string>(it->second))
                dias.insert(std::get<std::string>(it->second));
        }
        dias_retornados = static_cast<long long>(dias.size());
    }
    pulados[r.dataset_idx] += static_cast<long long>(r.datas_pedidas.size()) - dias_retornados;
    if (r.registros.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_write_mtx);
        salvar_dataset(ds, r.registros);
    }
    {
        std::unique_lock<std::shared_mutex> lk(g_mtx_existentes);
        auto& conj = existentes[r.dataset_idx][Chave{r.lat, r.lon}];
        std::string col = ds.chave_data.empty() ? "data" : ds.chave_data;
        for (const auto& reg : r.registros) {
            auto it = reg.find(col);
            if (it != reg.end() && std::holds_alternative<std::string>(it->second))
                conj.insert(std::get<std::string>(it->second));
        }
    }
    baixados[r.dataset_idx] += dias_retornados;
}

// File limitada (download -> processamento)
template <typename T>
class Fila {
public:
    explicit Fila(size_t capacidade) : cap_(capacidade) {}

    void put(T&& valor) {
        std::unique_lock<std::mutex> lk(m_);
        cv_cheia_.wait(lk, [&] {
            return q_.size() < cap_;
        });
        q_.push(std::move(valor));
        cv_vazia_.notify_one();
    }

    // Retorna false when the file foi fechada and esvaziada
    bool get(T& saida) {
        std::unique_lock<std::mutex> lk(m_);
        cv_vazia_.wait(lk, [&] {
            return !q_.empty() || fechada_;
        });
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

static int executar_generico(const std::vector<std::string>& datas, double tempo_load,
                             const ExistentesGen& existentes_inicial) {
    ExistentesGen existentes = existentes_inicial;
    struct TarefaGen {
        size_t ds;
        std::vector<std::string> datas;
        double lat, lon;
    };
    std::vector<TarefaGen> tarefas;
    long long falt = 0;
    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        const DatasetCfg& ds = CFG.datasets[di];
        for (const auto& coord : CFG.coordenadas) {
            Chave ch{coord.first, coord.second};
            std::vector<std::string> f;
            {
                std::shared_lock lk(g_mtx_existentes);
                auto it = existentes.find(di);
                const ConjuntoDatas* tem = nullptr;
                if (it != existentes.end()) {
                    auto jt = it->second.find(ch);
                    if (jt != it->second.end()) tem = &jt->second;
                }
                for (const auto& d : datas) {
                    if (!data_no_intervalo(d, ds)) continue;
                    if (!tem || !tem->count(d)) f.push_back(d);
                }
            }
            if (f.empty()) continue;
            falt += (long long)f.size();
            for (size_t i = 0; i < f.size(); i += (size_t)CFG.chunk_dias) {
                size_t e = std::min(i + (size_t)CFG.chunk_dias, f.size());
                tarefas.push_back({di, std::vector<std::string>(f.begin() + i, f.begin() + e),
                                   coord.first, coord.second});
            }
        }
    }
    std::cout << "[DB] Tasks: " << tarefas.size() << " blocks, " << falt
              << " missing day x coord.\n";
    if (tarefas.empty()) {
        std::cout << "[DB] Nothing to do.\n";
        return 0;
    }
    std::map<size_t, long long> baixados, pulados;
    std::atomic<long long> concluidos{0};
    long long total = (long long)tarefas.size();
    auto inicio = std::chrono::steady_clock::now();
    Fila<ResultadoGenerico> fila((size_t)CFG.download_workers * 2);
    std::atomic<size_t> idx_t{0};
    std::atomic<long long> feitas{0};
    std::cout << "\nStarting " << total << " tasks...\n\n";
    std::vector<std::thread> consumidores;
    for (int i = 0; i < CFG.process_workers; ++i) {
        consumidores.emplace_back([&] {
            try {
                ResultadoGenerico r;
                while (fila.get(r)) {
                    processar_generico(r, existentes, baixados, pulados);
                    long long c = ++concluidos;
                    if (c % 5 == 0 || c == total) {
                        double pct = 100.0 * (double)c / (double)total;
                        double dec = std::chrono::duration<double>(
                                         std::chrono::steady_clock::now() - inicio)
                                         .count();
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "\r[PROGRESSO] %lld/%lld blocos (%.1f%%) - %.0fs", c,
                                      total, pct, dec);
                        std::lock_guard<std::mutex> lk(g_print_mtx);
                        std::cout << buf << std::flush;
                    }
                }
            } catch (const std::exception& e) {
                log(std::string("[ERROR] Consumer: ") + e.what());
            }
        });
    }
    std::vector<std::thread> produtores;
    for (int i = 0; i < CFG.download_workers; ++i) {
        produtores.emplace_back([&] {
            try {
                while (true) {
                    size_t k = idx_t.fetch_add(1);
                    if (k >= tarefas.size()) break;
                    const TarefaGen& t = tarefas[k];
                    auto res = baixar_bloco_generico(t.ds, t.datas, t.lat, t.lon, existentes);
                    if (res) fila.put(std::move(*res));
                    if (feitas.fetch_add(1) + 1 >= total) fila.close();
                }
            } catch (const std::exception& e) {
                log(std::string("[ERROR] Produtor: ") + e.what());
                if (feitas.fetch_add(1) + 1 >= total) fila.close();
            }
        });
    }
    for (auto& t : produtores) t.join();
    for (auto& t : consumidores) t.join();
    double tt = std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
    std::cout << "\n\n============================================================\n"
              << "DOWNLOAD CONCLUIDO!\n"
              << "============================================================\n"
              << "Team total: " << tt << "s\n"
              << "Team burden DB: " << tempo_load << "s\n"
              << "Blocos: " << concluidos.load() << "/" << total << "\n--- ABRIDGEMENT ---\n";
    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        std::cout << CFG.datasets[di].id << " (" << CFG.datasets[di].db << "."
                  << CFG.datasets[di].tabela << "): " << baixados[di]
                  << " dias baixados | " << pulados[di] << " already existentes\n";
    }
    std::cout << "============================================================\n";
    return 0;
}

int main(int argc, char** argv) {
    std::string caminho_ini = argc > 1 ? argv[1] : "download.ini";
    carregar_config(caminho_ini);
    if (CFG.datasets.empty()) {
        std::cerr << "[ERROR] None [dataset:*] definido in " << caminho_ini
                  << ". THE formato old (daily_params/hourly_params/...) not and more "
                     "suportado; declare at less a section [dataset:<id>].\n";
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    iniciar_rate_limit();

    try {
        std::vector<std::string> datas = expandir_periodo(CFG.periodo_inicio, CFG.periodo_fim);
        if (datas.empty()) {
            std::cerr << "[ERROR] Period disabled (start > end) in " << caminho_ini << "\n";
            curl_global_cleanup();
            return 1;
        }

        std::cout << "============================================================\n"
                  << "DOWNLOAD OF DATA CLIMATICOS (generico)\n"
                  << "============================================================\n"
                  << "Configuracao: " << caminho_ini << "\n"
                  << "Spindle schedule: " << CFG.fuso_horario << "\n"
                  << "Database of date: " << CFG.db_path << "\n";
        {
            std::set<std::string> dbs;
            for (const auto& ds : CFG.datasets) dbs.insert(ds.db);
            std::cout << "Bancos target (" << dbs.size() << "): ";
            bool p = false;
            for (const auto& d : dbs) {
                if (p) std::cout << ", ";
                std::cout << d;
                p = true;
            }
            std::cout << "\nDatasets (" << CFG.datasets.size() << "): ";
            p = false;
            for (const auto& ds : CFG.datasets) {
                if (p) std::cout << ", ";
                std::cout << ds.id << "->" << ds.db << "." << ds.tabela;
                p = true;
            }
            std::cout << "\n";
        }

        criar_tabelas_datasets();

        std::cout << "\n[DB] Loading registros existentes...\n";
        auto t0 = std::chrono::steady_clock::now();
        ExistentesGen existentes = carregar_existentes_generico(datas);
        double tload = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (size_t di = 0; di < CFG.datasets.size(); ++di) {
            long long tot = 0;
            auto it = existentes.find(di);
            if (it != existentes.end())
                for (const auto& kv : it->second) tot += (long long)kv.second.size();
            std::cout << "[DB] " << CFG.datasets[di].id << ": " << tot << " dias\n";
        }
        int rc = executar_generico(datas, tload, existentes);
        curl_global_cleanup();
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "\n[ERRO] " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}


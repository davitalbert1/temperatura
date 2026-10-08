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
        std::cerr << "[AVISO] Arquivo de configuracao nao encontrado: " << caminho
                  << " (usando valores padrao)\n";
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
        // Secoes de dataset: aceitam tanto "chave = valor" quanto linhas de
        // lista ("vars = a, b" ou simplesmente "a, b" / "coluna.x = ...").
        if (eh_secao_dataset(secao)) {
            size_t igual = linha.find('=');
            if (igual == std::string::npos) {
                // Linha solta: trata como item de lista generica da secao.
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
                    // "col = api:tipo" dentro de vars tambem e aceito.
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

static bool para_bool(const std::string& s, bool padrao) {
    std::string v = cortar(s);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "1" || v == "true" || v == "sim" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "nao" || v == "não" || v == "no" || v == "off" ||
        v == "desligado")
        return false;
    return padrao;
}

// Nome de coluna/tabela seguro para SQLite: minusculas, [a-z0-9_], sem
// acento, sem espacos. "pm2.5 (µg/m³)" -> "pm2_5_ug_m3".
static std::string normalizar_nome(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-' || c == '.' || c == ' ' || c == '/')
            out += '_';
        // ignora demais simbolos/acentos
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

// "coluna = api[:tipo]" -> {coluna normalizada, variavel da api, tipo}
// Sem ":" o tipo e inferido dos dados (auto). Tipos aceitos:
// real | integer | text (alias: int, float, double, str, string, data, hora).
// Colunas geridas pelo proprio motor: nunca vem de dados da API e nunca
// podem ser sobrescritas (a de data/hora usa os nomes configurados).
static bool coluna_reservada(const std::string& c) {
    return c == "data" || c == "hora" || c == "timezone" || c == "latitude" ||
           c == "longitude" || c == "created_at";
}

static std::string normalizar_tipo(const std::string& t) {
    std::string v = cortar(t);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v.empty() || v == "auto") return "auto";
    if (v == "int" || v == "integer" || v == "bool" || v == "boolean") return "integer";
    if (v == "real" || v == "float" || v == "double" || v == "number" || v == "numeric")
        return "real";
    return "text";
}

// Divide "col = api:tipo" em partes. Retorna false se vazio.
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
    // dir pode ser "variavel:tipo"
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
    std::string coluna;       // nome da coluna no banco (normalizado)
    std::string variavel_api; // nome da variavel na API/JSON
    std::string tipo;         // "auto" | "real" | "integer" | "text"
};

// Dataset generico: uma API (ou recorte dela) -> uma tabela de um banco.
// Varios datasets podem apontar para o MESMO banco (tabelas diferentes ou
// ate a mesma tabela -> merge por chave). Cada dataset tambem pode apontar
// para um banco DIFERENTE (db proprio).
struct DatasetCfg {
    std::string id;              // nome da secao sem "dataset:" (ex: "clima_diario")
    std::string db;              // arquivo sqlite (vazio = db padrao do [geral])
    std::string tabela;          // tabela destino (padrao = id normalizado)
    std::string url;             // URL base da API
    std::string url_passado;     // URL alternativa p/ datas < hoje (ex: archive)
    std::string url_futuro;      // URL alternativa p/ datas >= hoje (ex: forecast)
    std::string bloco = "daily"; // bloco JSON com os dados ("daily"|"hourly"|...)
    std::string params_chave = "daily"; // nome do parametro de query (daily=...&hourly=...)
    std::vector<std::string> vars;      // variaveis pedidas a API
    std::set<std::string> vars_excluir; // variaveis removidas quando usa url_passado
    std::vector<ColunaCfg> colunas;     // overrides opcionais (coluna.*): renomeia/tipa
    std::string data_min;               // so baixa datas >= data_min ("", data ou "hoje")
    std::string data_max;               // so baixa datas <= data_max ("", data ou "hoje")
    std::string granularidade = "dia";  // "dia" (time=YYYY-MM-DD) | "hora" (time completo)
    bool salvar_hora = false;           // cria coluna "hora" com o time completo
    bool salvar_timezone = false;       // cria coluna "timezone"
    std::string chave_data = "data";    // coluna da data ("" = sem coluna de data)
    std::string chave_hora = "hora";    // coluna da hora ("" = sem coluna de hora)
};

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
    // Datasets genericos: cada [dataset:<id>] vira uma tabela em um banco.
    // Varios datasets podem compartilhar o mesmo .db (tabelas iguais ou
    // diferentes -> merge por chave) e um dataset pode ter db proprio.
    std::vector<DatasetCfg> datasets;

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
        d.bloco = cortar(ini.obter_ou(secao, "bloco", "daily"));
        if (d.bloco.empty()) d.bloco = "daily";
        d.params_chave = cortar(ini.obter_ou(secao, "params_chave", d.bloco));
        if (d.params_chave.empty()) d.params_chave = d.bloco;
        // 1) Overrides opcionais coluna.<nome> = <var_api>[:tipo]: renomeiam/
        //    tipam uma coluna descoberta automaticamente (nao sao obrigatorios)
        //    e garantem que a variavel seja pedida a API.
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
            std::map<std::string, std::string> defs; // ordenada -> ordem estavel
            for (const auto& kv : it_sec->second) {
                if (kv.first.rfind("coluna.", 0) == 0) defs[kv.first.substr(7)] = kv.second;
            }
            for (const auto& kv : defs) {
                std::string c, v, tp;
                if (!separar_def_coluna(kv.first + "=" + kv.second, c, v, tp)) continue;
                if (coluna_reservada(c)) continue;
                add_coluna(c, v, tp);
                add_var(v);
            }
        }
        // 2) vars / linhas soltas: apenas pedem variaveis a API. Coluna so e
        //    declarada quando ha tipo explicito ("<var>:<tipo>"); sem isso a
        //    coluna e descoberta automaticamente da resposta JSON (runtime),
        //    com o tipo inferido dos dados no CREATE TABLE / ADD COLUMN.
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
        // 3) vars_excluir e demais chaves de filtro.
        for (const auto& t : dividir(ini.obter_ou(secao, "vars_excluir"), ',')) {
            if (!t.empty()) d.vars_excluir.insert(t);
        }
        d.data_min = cortar(ini.obter_ou(secao, "data_min"));
        d.data_max = cortar(ini.obter_ou(secao, "data_max"));
        std::string gran = cortar(ini.obter_ou(secao, "granularidade", "dia"));
        std::transform(gran.begin(), gran.end(), gran.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        d.granularidade =
            (gran == "hora" || gran == "horario" || gran == "hourly") ? "hora" : "dia";
        d.salvar_hora =
            para_bool(ini.obter_ou(secao, "salvar_hora", d.granularidade == "hora" ? "1" : "0"),
                      d.granularidade == "hora");
        d.salvar_timezone = para_bool(ini.obter_ou(secao, "salvar_timezone", "0"), false);
        std::string cd = cortar(ini.obter_ou(secao, "chave_data", "data"));
        d.chave_data = cd.empty() ? "" : normalizar_nome(cd);
        std::string ch = cortar(ini.obter_ou(secao, "chave_hora", "hora"));
        d.chave_hora = ch.empty() ? "" : normalizar_nome(ch);
        if (d.url.empty()) {
            std::cerr << "[AVISO] Dataset '" << d.id << "' ignorado (url vazia).\n";
            continue;
        }
        if (d.vars.empty())
            std::cerr << "[AVISO] Dataset '" << d.id
                      << "': sem 'vars' - a API decide o que retorna.\n";
        out.push_back(std::move(d));
    }
    return out;
}

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

    if (CFG.download_workers < 1) CFG.download_workers = 1;
    if (CFG.process_workers < 1) CFG.process_workers = 1;
    if (CFG.chunk_dias < 1) CFG.chunk_dias = 1;
    if (CFG.max_sql_dates_por_lote < 1) CFG.max_sql_dates_por_lote = 1;
    if (CFG.requests_per_second <= 0.0) CFG.requests_per_second = 1.0;
    if (CFG.max_retries < 1) CFG.max_retries = 1;
    CFG.datasets = montar_datasets_genericos(ini);
    // db vazio no dataset = db padrao do [geral]
    for (auto& d : CFG.datasets) {
        if (d.db.empty()) d.db = CFG.db_path;
    }
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
// Cooldown absoluto baseado em epoch (evita erro de acumulacao de delta)
static std::chrono::steady_clock::time_point g_cooldown_inicio;
static double g_cooldown_atual = 0.0;
static int g_cooldown_ativo = 0;
static int g_429_consecutivos = 0;

static void iniciar_rate_limit() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_tokens = CFG.requests_per_second;
    g_rps_atual = CFG.requests_per_second;
    g_ultimo_refil = std::chrono::steady_clock::now();
    g_cooldown_inicio = std::chrono::steady_clock::now();
    g_cooldown_atual = 0.0;
    g_cooldown_ativo = 0;
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
    if (g_cooldown_ativo) {
        // Cooldown absoluto baseado em epoch (nao em acumulacao de delta)
        double restante = g_cooldown_atual -
                          std::chrono::duration<double>(agora - g_cooldown_inicio).count();
        if (restante > 0) return restante;
        // Cooldown expirado: limpa flag e continua
        g_cooldown_ativo = 0;
        g_cooldown_atual = 0.0;
    }

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
    // Reduz a taxa pela metade a cada 429, respeitando o piso min_rps
    g_rps_atual = std::max(CFG.min_rps, g_rps_atual * 0.5);
    g_tokens = std::min(g_tokens, g_rps_atual);
    if (g_429_consecutivos >= CFG.circuit_limit_429) {
        // Cooldown absoluto baseado em epoch para evitar deriva de delta
        g_cooldown_inicio = std::chrono::steady_clock::now();
        g_cooldown_atual = CFG.cooldown_429;
        g_cooldown_ativo = 1;
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "  [429] Circuit breaker ativado. Pausa global de %.0fs "
                      "(taxa: %.2f req/s)",
                      g_cooldown_atual, g_rps_atual);
        log(buf);
        g_429_consecutivos = 0;
    }
}

static void restaurar_taxa() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_429_consecutivos = 0;
    g_cooldown_ativo = 0;
    // Retoma gradualmente a taxa a cada sucesso (evita picos apos 429)
    if (g_rps_atual < CFG.requests_per_second)
        g_rps_atual = std::min(CFG.requests_per_second, g_rps_atual * 1.2);
    g_tokens = std::min(g_tokens, g_rps_atual);
}

static void esperar_cooldown_global() {
    double espera;
    {
        std::lock_guard<std::mutex> lk(g_rate_mtx);
        // Cooldown absoluto baseado em epoch (evita erro de acumulacao de delta)
        if (g_cooldown_ativo) {
            espera = g_cooldown_atual - std::chrono::duration<double>(
                std::chrono::steady_clock::now() - g_cooldown_inicio).count();
        } else {
            espera = 0.0;
        }
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

// Banco de dados (SQLite) - uma conexao por thread por arquivo, pois o mesmo
// download pode gravar em varios .db ao mesmo tempo (multiplas APIs).
// Tipos de valor usados pelo motor generico.
using Valor = std::variant<std::monostate, double, std::string>;
using Registro = std::map<std::string, Valor>;

static void executar_sql(sqlite3* conn, const char* sql) {
    char* erro = nullptr;
    if (sqlite3_exec(conn, sql, nullptr, nullptr, &erro) != SQLITE_OK) {
        std::string msg = erro ? erro : "erro desconhecido";
        sqlite3_free(erro);
        throw std::runtime_error("SQLite: " + msg);
    }
}

static void executar_sql(sqlite3* conn, const std::string& sql) {
    executar_sql(conn, sql.c_str());
}

static sqlite3* get_connection_para(const std::string& db_path) {
    // Cache thread-local: caminho -> conexao (um download pode usar N bancos).
    static thread_local std::map<std::string, sqlite3*> conns;
    auto it = conns.find(db_path);
    if (it != conns.end() && it->second) return it->second;
    sqlite3* conn = nullptr;
    if (sqlite3_open(db_path.c_str(), &conn) != SQLITE_OK) {
        std::string msg = conn ? sqlite3_errmsg(conn) : "falha ao abrir";
        if (conn) sqlite3_close(conn);
        throw std::runtime_error("SQLite: nao foi possivel abrir " + db_path + ": " + msg);
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
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low == "hoje" || low == "today" || low == "now") return hoje();
    return s; // data YYYY-MM-DD compara lexicograficamente
}

static bool data_no_intervalo(const std::string& d, const DatasetCfg& ds) {
    std::string mn = resolver_data_limite(ds.data_min, "");
    std::string mx = resolver_data_limite(ds.data_max, "");
    if (!mn.empty() && d < mn) return false;
    if (!mx.empty() && d > mx) return false;
    return true;
}

// Infere o tipo SQLite de uma coluna a partir dos valores observados.
// "auto" -> integer se todos forem inteiros, real se houver decimal,
// text se houver qualquer string/data. Tipo declarado no .ini prevalece.
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

// Cria a tabela do dataset automaticamente (ou evolui com ADD COLUMN).
// Colunas: reservadas (data/hora/lat/lon) + descobertas da resposta da API
// (amostra) + overrides opcionais do .ini + created_at. As colunas ja
// existentes sao verificadas via PRAGMA table_info (so ADD COLUMN o que falta)
// e os tipos vem inferidos dos dados quando nao declarados.
// Chave UNIQUE: (data,lat,lon) p/ diario ou (data,hora,lat,lon) p/ horario.
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
    // Colunas descobertas automaticamente na resposta da API: toda chave do
    // registro que nao seja reservada vira coluna (tipo inferido dos dados).
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
        // chave de deduplicacao
        std::vector<std::string> chave;
        if (!ds.chave_data.empty()) chave.push_back(ds.chave_data);
        if (ds.salvar_hora && !ds.chave_hora.empty() && ds.chave_hora != ds.chave_data)
            chave.push_back(ds.chave_hora);
        chave.push_back("latitude");
        chave.push_back("longitude");
        sql += ",\n  UNIQUE (" + juntar(chave, ", ") + ")\n)";
        executar_sql(conn, sql);
        log("[DB] Tabela criada: " + ds.db + "." + ds.tabela + " (" +
            std::to_string(ordem.size()) + " colunas)");
    } else {
        for (const auto& col : ordem) {
            if (std::find(existentes.begin(), existentes.end(), col) == existentes.end()) {
                std::string decl = "auto";
                auto it = tipo_decl.find(col);
                if (it != tipo_decl.end()) decl = it->second;
                executar_sql(conn, "ALTER TABLE \"" + ds.tabela + "\" ADD COLUMN " + col +
                                       " " + inferir_tipo_coluna(decl, amostra, col));
                log("[DB] Coluna adicionada: " + ds.tabela + "." + col);
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
        executar_sql(conn, "CREATE INDEX IF NOT EXISTS idx_" + ds.tabela + "_data ON \"" +
                               ds.tabela + "\"(\"" + ds.chave_data + "\")");
}

static void criar_tabelas_datasets() {
    // Tabelas genericas ([dataset:*]): cria (ou evolui) cada tabela no seu
    // .db e cria arquivos vazios antecipadamente para que multiplos .db
    // existam mesmo antes do primeiro download.
    for (const auto& ds : CFG.datasets) {
        try {
            std::vector<Registro> vazio;
            garantir_tabela(ds, vazio);
        } catch (const std::exception& e) {
            log(std::string("[AVISO] ") + ds.db + "." + ds.tabela + ": " + e.what());
        }
    }
}

// Registros existentes no banco (carregados uma unica vez)
using Chave = std::pair<double, double>;
using ConjuntoDatas = std::set<std::string>;

// Produtores fazem leitura compartilhada; consumidores fazem escrita exclusiva
static std::shared_mutex g_mtx_existentes;

static const ConjuntoDatas& datas_vazias() {
    static const ConjuntoDatas vazio;
    return vazio;
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

static std::optional<json> baixar_com_retry(const std::string& url,
                                            const std::map<std::string, std::string>& params,
                                            int max_tentativas);

// Parse generico: bloco JSON -> registros. COLUNAS DESCOBERTAS
// AUTOMATICAMENTE: toda chave array do bloco (exceto "time") vira coluna com
// nome normalizado; o tipo e inferido ao criar/evoluir a tabela. Overrides
// opcionais do .ini (coluna.*) apenas renomeiam/tipam.
static std::vector<Registro> parse_dataset(const json& bloco_json, const json& raiz,
                                           double lat, double lon, const DatasetCfg& ds,
                                           const std::set<std::string>* filtro) {
    if (!bloco_json.is_object()) return {};
    std::vector<std::string> times;
    auto it_t = bloco_json.find("time");
    if (it_t != bloco_json.end() && it_t->is_array()) {
        for (const auto& t : *it_t) {
            if (t.is_string()) times.push_back(t.get<std::string>());
        }
    }
    // Overrides opcionais: variavel da API -> coluna renomeada/tipada
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
        if (ds.salvar_hora && !ds.chave_hora.empty()) reg[ds.chave_hora] = t;
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
    if (!ds.url_passado.empty() || !ds.url_futuro.empty()) {
        bool futuro = false;
        try {
            para_data(data);
            futuro = !(data < hoje());
        } catch (...) {
            futuro = true;
        }
        if (!futuro && !ds.url_passado.empty()) return ds.url_passado;
        if (futuro && !ds.url_futuro.empty()) return ds.url_futuro;
    }
    return ds.url;
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
    // Agrupa por URL (passado x futuro) em blocos de chunk_dias.
    size_t i = 0;
    while (i < alvo.size()) {
        std::string url = url_do_dataset(ds, alvo[i]);
        size_t j = i;
        while (j < alvo.size() && url_do_dataset(ds, alvo[j]) == url &&
               (j - i) < static_cast<size_t>(CFG.chunk_dias))
            ++j;
        std::vector<std::string> grupo(alvo.begin() + i, alvo.begin() + j);
        std::vector<std::string> vars = ds.vars;
        if (url == ds.url_passado && !ds.url_passado.empty() && !ds.vars_excluir.empty()) {
            vars.erase(std::remove_if(vars.begin(), vars.end(),
                                      [&](const std::string& v) {
                                          return ds.vars_excluir.count(v);
                                      }),
                       vars.end());
        }
        std::map<std::string, std::string> params = {
            {"latitude", num_str(lat)},
            {"longitude", num_str(lon)},
            {"start_date", grupo.front()},
            {"end_date", grupo.back()},
            {"timezone", CFG.fuso_horario},
        };
        if (!vars.empty()) params[ds.params_chave] = juntar(vars, ",");
        auto dados = baixar_com_retry(url, params, CFG.max_retries);
        if (dados && dados->contains(ds.bloco) && (*dados)[ds.bloco].is_object()) {
            std::set<std::string> filtro(grupo.begin(), grupo.end());
            auto regs = parse_dataset((*dados)[ds.bloco], *dados, lat, lon, ds, &filtro);
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
        r.erro_mensagem = "falha ao iniciar curl";
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

            // Prioridade: Retry-After do servidor > backoff > espera minima
            double espera = CFG.espera_min_429;
            if (r.retry_after > 0) {
                espera = r.retry_after;
            } else {
                espera = backoff_exponencial(tentativa);
            }

            // Se o cooldown global ja esta ativo, espera o cooldown (nao espera dobrado)
            {
                std::lock_guard<std::mutex> lk(g_rate_mtx);
                if (g_cooldown_ativo) {
                    double restante = g_cooldown_atual - std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - g_cooldown_inicio).count();
                    if (restante > espera) espera = restante;
                }
            }
            if (tentativa == max_tentativas) {
                log("  [ERRO] 429 persistente apos " + std::to_string(max_tentativas) + " tentativas");
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

// Salvamento generico: garante a tabela (cria sozinha), descobre as colunas
// reais do banco e grava com UPSERT por chave (merge entre APIs).
static void salvar_dataset(const DatasetCfg& ds, const std::vector<Registro>& registros) {
    if (registros.empty()) return;
    garantir_tabela(ds, registros);
    sqlite3* conn = get_connection_para(ds.db);
    std::vector<std::string> campos = colunas_da_tabela(conn, ds.tabela);
    campos.erase(std::remove(campos.begin(), campos.end(), "created_at"), campos.end());
    // mantem ordem canonica: data,hora,lat,lon primeiro (nomes configurados)
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
    // UPSERT: atualiza so as colunas nao-chave (merge entre APIs/tabelas iguais)
    std::vector<std::string> upd;
    for (const auto& c : campos) {
        if (std::find(chave.begin(), chave.end(), c) == chave.end())
            upd.push_back("\"" + c + "\"=excluded.\"" + c + "\"");
    }
    if (!upd.empty() && !chave.empty())
        sql += " ON CONFLICT(" + juntar(chave, ",") + ") DO UPDATE SET " + juntar(upd, ",");
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
            log("[ERRO] " + ds.tabela + ": " + sqlite3_errmsg(conn));
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

// Orquestracao generica: bloco (dataset, datas, coord) -> fila -> gravar.
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
    // dias pedidos sem retorno = ja existentes ou fora do intervalo da API
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
    pulados[r.dataset_idx] +=
        static_cast<long long>(r.datas_pedidas.size()) - dias_retornados;
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

// Fila limitada (download -> processamento)
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

    // Retorna false quando a fila foi fechada e esvaziada
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
    std::cout << "[DB] Tarefas: " << tarefas.size() << " blocos, " << falt
              << " dias x coord faltantes.\n";
    if (tarefas.empty()) {
        std::cout << "[DB] Nada a fazer.\n";
        return 0;
    }
    std::map<size_t, long long> baixados, pulados;
    std::atomic<long long> concluidos{0};
    long long total = (long long)tarefas.size();
    auto inicio = std::chrono::steady_clock::now();
    Fila<ResultadoGenerico> fila((size_t)CFG.download_workers * 2);
    std::atomic<size_t> idx_t{0};
    std::atomic<long long> feitas{0};
    std::cout << "\nIniciando " << total << " tarefas...\n\n";
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
                log(std::string("[ERRO] Consumidor: ") + e.what());
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
                log(std::string("[ERRO] Produtor: ") + e.what());
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
              << "Tempo total: " << tt << "s\n"
              << "Tempo carregamento DB: " << tempo_load << "s\n"
              << "Blocos: " << concluidos.load() << "/" << total << "\n--- RESUMO ---\n";
    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        std::cout << CFG.datasets[di].id << " (" << CFG.datasets[di].db << "."
                  << CFG.datasets[di].tabela << "): " << baixados[di]
                  << " dias baixados | " << pulados[di] << " ja existentes\n";
    }
    std::cout << "============================================================\n";
    return 0;
}

int main(int argc, char** argv) {
    std::string caminho_ini = argc > 1 ? argv[1] : "download.ini";
    carregar_config(caminho_ini);
    if (CFG.datasets.empty()) {
        std::cerr << "[ERRO] Nenhum [dataset:*] definido em " << caminho_ini
                  << ". O formato antigo (daily_params/hourly_params/...) nao e mais "
                     "suportado; declare ao menos uma secao [dataset:<id>].\n";
        return 1;
    }
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
                  << "DOWNLOAD DE DADOS CLIMATICOS (generico)\n"
                  << "============================================================\n"
                  << "Configuracao: " << caminho_ini << "\n"
                  << "Fuso horario: " << CFG.fuso_horario << "\n"
                  << "Banco de dados: " << CFG.db_path << "\n";
        {
            std::set<std::string> dbs;
            for (const auto& ds : CFG.datasets) dbs.insert(ds.db);
            std::cout << "Bancos destino (" << dbs.size() << "): ";
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

        std::cout << "\n[DB] Carregando registros existentes...\n";
        auto t0 = std::chrono::steady_clock::now();
        ExistentesGen existentes = carregar_existentes_generico(datas);
        double tload =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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


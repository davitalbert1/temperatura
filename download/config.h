#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

// Contexto genérico para interpolação de variáveis: {chave} -> valor
using Contexto = std::map<std::string, std::string>;

std::string aplicar_ctx(std::string s, const Contexto& ctx);
bool chave_ctx_interna(const std::string& k);

struct ColunaCfg {
    std::string coluna; // nome da coluna no SQLite (normalizado)
    std::string variavel_api; // nome da variável na API/JSON
    std::string tipo; // "auto" | "real" | "integer" | "text"
};

struct DatasetCfg {
    std::string id; // id da seção (ex: "clima_diario")
    std::string db; // arquivo .db (vazio = padrão do [geral])
    std::string tabela; // tabela no SQLite
    std::string url; // URL base
    std::string url_passado; // URL para datas < hoje (opcional)
    std::string url_futuro; // URL para datas >= hoje (opcional)
    std::string bloco; // caminho do bloco no JSON (daily, hourly, etc)
    std::string params_chave; // query param que lista as variáveis (daily=...)
    std::vector<std::string> vars; // lista de variáveis pedidas
    std::set<std::string> vars_excluir; // variáveis omitidas no passado/archive
    std::vector<ColunaCfg> colunas; // mapeamento/tipagem de colunas
    std::string data_min; // filtro mínimo (YYYY-MM-DD ou "hoje")
    std::string data_max; // filtro máximo (YYYY-MM-DD ou "hoje")
    std::string chave_data = "data"; // nome da coluna/campo de data
    std::string chave_hora = "hora"; // nome da coluna/campo de hora
    bool salvar_hora = false;
    bool salvar_timezone = false;
    std::string granularidade = "dia"; // dia | hora | nenhuma
    std::string api_keys; // chaves rotativas "k1 | k2"
    bool usar_coordenadas = false; // itera [coordenadas] e injeta lat/lon
    bool iterar_datas = true; // fatia o período em start_date/end_date
    std::string metodo = "GET"; // GET, POST, etc.
    std::string corpo; // HTTP body (templates {chave})
    std::string formato = "auto";// auto | series | records | object
    std::string chave_tempo = "time";
    std::string chave_unica; // colunas UNIQUE personalizadas (csv)
    std::vector<std::string> eixos; // eixos de produto cartesiano (dim:* e/ou coordenadas)
    std::map<std::string, std::string> params; // query params adicionais
    std::map<std::string, std::string> headers; // headers HTTP adicionais
    std::string api_key_em = "query"; // query | header | basic
    std::string api_key_header = "Authorization";
    std::string api_key_prefix = "Bearer";

    // Campos para salvamento em arquivos (ex: Copernicus CDS / ERA5)
    std::string tipo_saida = "db"; // "db" (sqlite) ou "arquivo" (pasta)
    std::string pasta_saida = "dados_era5";
    std::string extensao = "grib";
    std::string dataset_api = "reanalysis-era5-single-levels";
    std::string product_type = "reanalysis";
    std::string data_format = "grib";
    std::string download_format = "unarchived";
    std::vector<std::string> horarios = {"00:00", "12:00"};
    int ano_inicial = 1940;
    int dias_atraso = 7;
    std::string modo_api = "auto";
};

struct DimensaoCfg {
    std::string nome;
    std::vector<std::string> campos;
    std::vector<std::vector<std::string>> linhas;
};

struct Config {
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
    double limite_minuto = 600.0;
    double limite_hora = 5000.0;
    double limite_dia = 10000.0;
    double custo_vars_ref = 10.0;
    double custo_dias_ref = 14.0;
    double custo_dias_fator = 1.5;
    double cooldown_quota = 300.0;
    std::string api_keys;
    std::string api_key_param = "apikey";
    std::string fuso_horario = "America/Sao_Paulo";
    std::string periodo_inicio = "1940-01-01";
    std::string periodo_fim = "2025-12-31";

    std::vector<std::pair<double, double>> coordenadas;
    std::vector<DimensaoCfg> dimensoes;
    std::vector<DatasetCfg> datasets;
};

extern Config CFG;

struct Ini {
    std::map<std::string, std::map<std::string, std::string>> valores;
    std::map<std::string, std::vector<std::string>> listas;
    std::vector<std::string> ordem_secoes;

    bool obter(const std::string& sec, const std::string& chave, std::string& saida) const;
    std::string obter_ou(const std::string& sec, const std::string& chave,
                         const std::string& padrao = "") const;
    std::vector<std::string> lista(const std::string& sec) const;
    bool tem_secao(const std::string& sec) const;
};

void carregar_env(const std::string& caminho = ".env");
std::string obter_env_ou_config(const char* nome_env, const std::string& valor_cfg,
                                const std::string& padrao = "");
Ini ler_ini(const std::string& caminho);
void carregar_config(const std::string& caminho);

std::vector<Contexto> contextos_do_eixo(const std::string& eixo);
std::vector<Contexto> expandir_contextos(const DatasetCfg& ds);
std::string resolver_data_limite(const std::string& v, const std::string& padrao);
bool data_no_intervalo(const std::string& d, const DatasetCfg& ds);

#pragma once
#include <sqlite3.h>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>
#include "config.h"

using Valor = std::variant<std::monostate, double, std::string>;
using Registro = std::map<std::string, Valor>;
using Chave = std::pair<double, double>;
using ConjuntoDatas = std::set<std::string>;
using ExistentesGen = std::map<size_t, std::map<Chave, ConjuntoDatas>>;

void executar_sql(sqlite3* conn, const char* sql);
void executar_sql(sqlite3* conn, const std::string& sql);
sqlite3* get_connection_para(const std::string& db_path);

std::vector<std::string> colunas_da_tabela(sqlite3* conn, const std::string& tabela);
std::vector<std::string> colunas_chave(const DatasetCfg& ds);
void garantir_tabela(const DatasetCfg& ds, const std::vector<Registro>& amostra);
void criar_tabelas_datasets();
void salvar_dataset(const DatasetCfg& ds, const std::vector<Registro>& registros);
ExistentesGen carregar_existentes_generico(const std::vector<std::string>& datas);

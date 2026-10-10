#pragma once
#include "config.h"
#include "storage_sqlite.h"
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

Valor json_para_valor(const json& el);
const json* navegar_json(const json& raiz, const std::string& caminho);
const json* desenrolar_json(const json& j);

std::vector<Registro> parse_series(const json& bloco_json, const json& raiz,
                                   const DatasetCfg& ds, const Contexto& ctx,
                                   const std::set<std::string>* filtro,
                                   bool forcar_hora);

std::vector<Registro> parse_records(const json& arr, const json& raiz,
                                    const DatasetCfg& ds, const Contexto& ctx,
                                    const std::set<std::string>* filtro,
                                    bool forcar_hora);

std::vector<Registro> parse_objeto(const json& obj, const json& raiz,
                                   const DatasetCfg& ds, const Contexto& ctx);

std::vector<Registro> parse_json_generico(const json& raiz, const DatasetCfg& ds,
                                         const Contexto& ctx,
                                         const std::set<std::string>* filtro,
                                         bool forcar_hora, const std::string& bloco_req);

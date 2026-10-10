#include "json_parser.h"
#include "utils.h"
#include <algorithm>

Valor json_para_valor(const json& el) {
    if (el.is_null() || el.is_discarded()) return Valor{};
    if (el.is_number()) return Valor{el.get<double>()};
    if (el.is_boolean()) return Valor{el.get<bool>() ? 1.0 : 0.0};
    if (el.is_string()) return Valor{el.get<std::string>()};
    return Valor{el.dump()};
}

static Valor pegar(const json& dados, const std::string& chave, size_t i) {
    if (!dados.is_object()) return Valor{};
    auto it = dados.find(chave);
    if (it == dados.end() || !it->is_array() || i >= it->size()) return Valor{};
    return json_para_valor((*it)[i]);
}

const json* navegar_json(const json& raiz, const std::string& caminho) {
    if (caminho.empty() || caminho == "." || caminho == "root") return &raiz;
    const json* cur = &raiz;
    for (auto& parte : dividir(caminho, '.')) {
        if (parte.empty() || !cur) return nullptr;
        if (cur->is_array()) {
            try {
                size_t idx = static_cast<size_t>(std::stoul(parte));
                if (idx >= cur->size()) return nullptr;
                cur = &(*cur)[idx];
                continue;
            } catch (...) {
                return nullptr;
            }
        }
        if (!cur->is_object() || !cur->contains(parte)) return nullptr;
        cur = &(*cur)[parte];
    }
    return cur;
}

const json* desenrolar_json(const json& j) {
    if (!j.is_object()) return &j;
    static const char* wraps[] = {"data", "results", "items", "records", "response",
                                  "content", "payload", "result", "rows", "values",
                                  "features"};
    for (const char* w : wraps) {
        if (j.contains(w) && (j[w].is_array() || j[w].is_object())) return &j[w];
    }
    return &j;
}

static void injetar_ctx_registro(Registro& reg, const DatasetCfg& ds, const Contexto& ctx) {
    if (ds.usar_coordenadas) {
        auto ilat = ctx.find("latitude");
        auto ilon = ctx.find("longitude");
        if (ilat != ctx.end()) {
            try {
                reg["latitude"] = std::stod(ilat->second);
            } catch (...) {
                reg["latitude"] = ilat->second;
            }
        }
        if (ilon != ctx.end()) {
            try {
                reg["longitude"] = std::stod(ilon->second);
            } catch (...) {
                reg["longitude"] = ilon->second;
            }
        }
    }
    for (const auto& kv : ctx) {
        if (chave_ctx_interna(kv.first)) continue;
        std::string col = normalizar_nome(kv.first);
        if (col == ds.chave_data || col == ds.chave_hora) continue;
        if (reg.find(col) == reg.end()) reg[col] = kv.second;
    }
}

static void achatar_objeto(const json& obj, const std::string& prefix, Registro& reg,
                           const DatasetCfg& ds, const std::map<std::string, ColunaCfg>& ov) {
    if (!obj.is_object()) return;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().is_object()) {
            std::string p = prefix.empty() ? it.key() : prefix + "_" + it.key();
            achatar_objeto(it.value(), p, reg, ds, ov);
            continue;
        }
        if (it.value().is_array()) continue;
        auto ovi = ov.find(it.key());
        std::string bruto = prefix.empty() ? it.key() : prefix + "_" + it.key();
        std::string col = ovi != ov.end() ? ovi->second.coluna : normalizar_nome(bruto);
        if (coluna_reservada(col) && col != ds.chave_data && col != ds.chave_hora) {
            if (ds.usar_coordenadas && (col == "latitude" || col == "longitude")) {
                // permite sobrescrever lat/lon se retornado pelo JSON
            } else if (col != ds.chave_data && col != ds.chave_hora && col != "timezone") {
                continue;
            }
        }
        if (col == "created_at") continue;
        if (reg.find(col) == reg.end()) reg[col] = json_para_valor(it.value());
    }
}

std::vector<Registro> parse_series(const json& bloco_json, const json& raiz,
                                   const DatasetCfg& ds, const Contexto& ctx,
                                   const std::set<std::string>* filtro,
                                   bool forcar_hora) {
    if (!bloco_json.is_object()) return {};
    std::map<std::string, ColunaCfg> overrides;
    for (const auto& c : ds.colunas) overrides[c.variavel_api] = c;

    std::vector<std::string> times;
    std::string chave_t = ds.chave_tempo.empty() ? "time" : ds.chave_tempo;
    auto it_t = bloco_json.find(chave_t);
    if (it_t == bloco_json.end() || !it_t->is_array()) {
        it_t = bloco_json.find("time");
        if (it_t == bloco_json.end() || !it_t->is_array()) it_t = bloco_json.find("date");
        if (it_t == bloco_json.end() || !it_t->is_array()) it_t = bloco_json.find("datetime");
        if (it_t == bloco_json.end() || !it_t->is_array()) it_t = bloco_json.find("timestamp");
    }
    size_t n = 0;
    if (it_t != bloco_json.end() && it_t->is_array()) {
        n = it_t->size();
        for (const auto& t : *it_t) {
            if (t.is_string()) times.push_back(t.get<std::string>());
            else if (t.is_number()) times.push_back(num_str(t.get<double>()));
            else times.push_back("");
        }
    }
    if (n == 0) {
        for (auto it = bloco_json.begin(); it != bloco_json.end(); ++it) {
            if (it.value().is_array()) n = std::max(n, it.value().size());
        }
        times.assign(n, "");
    }
    std::vector<Registro> regs;
    regs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const std::string& t = i < times.size() ? times[i] : "";
        std::string data = t.substr(0, std::min<size_t>(10, t.size()));
        if (!data.empty() && filtro && !filtro->count(data)) continue;
        if (!data.empty() && !data_no_intervalo(data, ds)) continue;
        Registro reg;
        injetar_ctx_registro(reg, ds, ctx);
        if (!ds.chave_data.empty() && !data.empty()) reg[ds.chave_data] = data;
        if ((ds.salvar_hora || forcar_hora) && !ds.chave_hora.empty() && !t.empty())
            reg[ds.chave_hora] = t;
        if (ds.salvar_timezone) {
            auto itz = raiz.find("timezone");
            if (itz != raiz.end() && itz->is_string())
                reg["timezone"] = itz->get<std::string>();
            else if (bloco_json.contains("timezone") && bloco_json["timezone"].is_string())
                reg["timezone"] = bloco_json["timezone"].get<std::string>();
        }
        for (auto it = bloco_json.begin(); it != bloco_json.end(); ++it) {
            if (!it.value().is_array()) continue;
            if (it.key() == chave_t || it.key() == "time" || it.key() == "date" ||
                it.key() == "datetime" || it.key() == "timestamp")
                continue;
            auto ov = overrides.find(it.key());
            std::string col =
                ov != overrides.end() ? ov->second.coluna : normalizar_nome(it.key());
            if (col == "created_at" || col == ds.chave_data || col == ds.chave_hora) continue;
            if (coluna_reservada(col) && !(ds.usar_coordenadas &&
                                           (col == "latitude" || col == "longitude")))
                continue;
            reg[col] = pegar(bloco_json, it.key(), i);
        }
        regs.push_back(std::move(reg));
    }
    return regs;
}

std::vector<Registro> parse_records(const json& arr, const json& raiz,
                                    const DatasetCfg& ds, const Contexto& ctx,
                                    const std::set<std::string>* filtro,
                                    bool forcar_hora) {
    std::map<std::string, ColunaCfg> overrides;
    for (const auto& c : ds.colunas) overrides[c.variavel_api] = c;
    std::vector<Registro> regs;
    if (!arr.is_array()) return regs;
    regs.reserve(arr.size());
    for (const auto& el : arr) {
        Registro reg;
        injetar_ctx_registro(reg, ds, ctx);
        if (el.is_object()) {
            achatar_objeto(el, "", reg, ds, overrides);
        } else if (el.is_array()) {
            for (size_t i = 0; i < el.size(); ++i) reg["c_" + std::to_string(i)] = json_para_valor(el[i]);
        } else {
            reg["valor"] = json_para_valor(el);
        }
        std::string data;
        if (!ds.chave_data.empty()) {
            auto it = reg.find(ds.chave_data);
            if (it != reg.end() && std::holds_alternative<std::string>(it->second))
                data = std::get<std::string>(it->second).substr(0, 10);
            else {
                for (const char* k : {"date", "time", "datetime", "timestamp", "data"}) {
                    auto jt = el.is_object() ? el.find(k) : el.end();
                    if (el.is_object() && jt != el.end()) {
                        if (jt->is_string()) data = jt->get<std::string>().substr(0, 10);
                        break;
                    }
                }
                if (!data.empty()) reg[ds.chave_data] = data;
            }
        }
        if (!data.empty() && filtro && !filtro->count(data)) continue;
        if (!data.empty() && !data_no_intervalo(data, ds)) continue;
        if ((ds.salvar_hora || forcar_hora) && !ds.chave_hora.empty() && el.is_object()) {
            for (const char* k : {"time", "datetime", "timestamp", "hora"}) {
                if (el.contains(k) && el[k].is_string()) {
                    reg[ds.chave_hora] = el[k].get<std::string>();
                    break;
                }
            }
        }
        if (ds.salvar_timezone && raiz.contains("timezone") && raiz["timezone"].is_string())
            reg["timezone"] = raiz["timezone"].get<std::string>();
        regs.push_back(std::move(reg));
    }
    return regs;
}

std::vector<Registro> parse_objeto(const json& obj, const json& raiz,
                                   const DatasetCfg& ds, const Contexto& ctx) {
    std::map<std::string, ColunaCfg> overrides;
    for (const auto& c : ds.colunas) overrides[c.variavel_api] = c;
    Registro reg;
    injetar_ctx_registro(reg, ds, ctx);
    if (obj.is_object()) achatar_objeto(obj, "", reg, ds, overrides);
    else if (!obj.is_null()) reg["valor"] = json_para_valor(obj);
    if (ds.salvar_timezone && raiz.contains("timezone") && raiz["timezone"].is_string())
        reg["timezone"] = raiz["timezone"].get<std::string>();
    if (reg.size() <= (ds.usar_coordenadas ? 2u : 0u)) return {};
    return {std::move(reg)};
}

static bool parece_series(const json& obj, const std::string& chave_tempo) {
    if (!obj.is_object()) return false;
    size_t arrays = 0, len = 0;
    bool tem_tempo = false;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it.value().is_array() || it.value().empty()) continue;
        if (it.value().front().is_object()) continue;
        arrays++;
        len = std::max(len, it.value().size());
        std::string k = minusculas(it.key());
        if (k == minusculas(chave_tempo) || k == "time" || k == "date" || k == "datetime" ||
            k == "timestamp")
            tem_tempo = true;
    }
    return arrays >= 2 || (arrays >= 1 && tem_tempo) || (arrays >= 1 && len > 1);
}

std::vector<Registro> parse_json_generico(const json& raiz, const DatasetCfg& ds,
                                         const Contexto& ctx,
                                         const std::set<std::string>* filtro,
                                         bool forcar_hora, const std::string& bloco_req) {
    const json* alvo = navegar_json(raiz, bloco_req.empty() ? ds.bloco : bloco_req);
    if (!alvo) alvo = &raiz;
    std::string fmt = ds.formato;
    if (fmt == "auto") {
        const json* unwrapped = alvo;
        if (alvo->is_object() && (ds.bloco.empty() && bloco_req.empty()))
            unwrapped = desenrolar_json(*alvo);
        if (unwrapped->is_array()) fmt = "records";
        else if (parece_series(*unwrapped, ds.chave_tempo)) fmt = "series";
        else if (unwrapped->is_object() && parece_series(*alvo, ds.chave_tempo)) {
            unwrapped = alvo;
            fmt = "series";
        } else
            fmt = unwrapped->is_object() ? "object" : "records";
        alvo = unwrapped;
    } else if (!ds.bloco.empty() || !bloco_req.empty()) {
        // caminho explícito já selecionado
    } else if (alvo->is_object()) {
        const json* u = desenrolar_json(*alvo);
        if (fmt == "records" && u->is_array()) alvo = u;
        else if (fmt == "series" && u->is_object()) alvo = u;
    }

    if (fmt == "series") return parse_series(*alvo, raiz, ds, ctx, filtro, forcar_hora);
    if (fmt == "records") {
        if (alvo->is_array()) return parse_records(*alvo, raiz, ds, ctx, filtro, forcar_hora);
        if (alvo->is_object()) {
            for (auto it = alvo->begin(); it != alvo->end(); ++it) {
                if (it.value().is_array()) return parse_records(it.value(), raiz, ds, ctx, filtro, forcar_hora);
            }
        }
        return {};
    }
    return parse_objeto(*alvo, raiz, ds, ctx);
}

#include "engine_generico.h"
#include "http_client.h"
#include "json_parser.h"
#include "rate_limit.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <shared_mutex>
#include <thread>

static std::shared_mutex g_mtx_existentes;
static std::mutex g_write_mtx;
static std::mutex g_stats_mtx;
static std::mutex g_print_mtx;

static const ConjuntoDatas& datas_vazias() {
    static const ConjuntoDatas vazio;
    return vazio;
}

static std::mt19937& rng() {
    static thread_local std::mt19937 gen{
        static_cast<std::mt19937::result_type>(std::random_device{}() ^
                                               std::hash<std::thread::id>{}(
                                                   std::this_thread::get_id()))};
    return gen;
}

static std::string url_do_dataset(const DatasetCfg& ds, const std::string& data) {
    if (!ds.url_passado.empty() || !ds.url_futuro.empty()) {
        bool futuro = !(data < hoje());
        if (!futuro && !ds.url_passado.empty()) return ds.url_passado;
        if (futuro && !ds.url_futuro.empty()) return ds.url_futuro;
    }
    return ds.url;
}

static std::string escolher_url(const std::string& lista) {
    auto cands = dividir(lista, '|');
    cands.erase(std::remove_if(cands.begin(), cands.end(), [](const std::string& s) {
        return s.empty();
    }), cands.end());
    if (cands.empty()) return lista;
    if (cands.size() == 1) return cands.front();
    static std::atomic<unsigned> g_rr_url{0};
    unsigned base = g_rr_url.fetch_add(1);
    return cands[base % cands.size()];
}

std::vector<Registro> baixar_dataset_bloco(const DatasetCfg& ds,
                                          const std::vector<std::string>& datas,
                                          const Contexto& ctx_base) {
    std::vector<std::string> alvo;
    for (const auto& d : datas) {
        if (data_no_intervalo(d, ds)) alvo.push_back(d);
    }
    if (ds.iterar_datas && alvo.empty()) return {};
    std::sort(alvo.begin(), alvo.end());
    std::vector<Registro> todos;

    size_t i = 0;
    const size_t n_passos = ds.iterar_datas ? alvo.size() : 1;
    while (i < n_passos) {
        std::string lista = url_do_dataset(ds, alvo.empty() ? hoje() : alvo[i]);
        size_t j = i;
        if (ds.iterar_datas) {
            while (j < alvo.size() && url_do_dataset(ds, alvo[j]) == lista &&
                   (j - i) < static_cast<size_t>(CFG.chunk_dias))
                ++j;
        } else {
            j = 1;
        }
        std::vector<std::string> grupo;
        if (ds.iterar_datas) grupo.assign(alvo.begin() + i, alvo.begin() + j);
        std::string bloco_req = ds.bloco;
        std::string chave_req = ds.params_chave;
        std::vector<std::string> vars = ds.vars;

        // Suporte genérico: caso de fallback do Open-Meteo Pollen (se url contiver pollen)
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

        double custo = 1.0;
        if (!vars.empty())
            custo = std::max(custo, static_cast<double>(vars.size()) / CFG.custo_vars_ref);
        try {
            if (!grupo.empty()) {
                Data da = para_data(grupo.front());
                Data db = para_data(grupo.back());
                long d1 = dias_de_civil(da.ano, da.mes, da.dia);
                long d2 = dias_de_civil(db.ano, db.mes, db.dia);
                custo = std::max(custo, (d2 - d1 + 1) * CFG.custo_dias_fator / CFG.custo_dias_ref);
            }
        } catch (...) {
            custo = std::max(custo, static_cast<double>(grupo.size()) * CFG.custo_dias_fator / CFG.custo_dias_ref);
        }

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

        Contexto ctx = ctx_base;
        ctx["start"] = grupo.empty() ? hoje() : grupo.front();
        ctx["end"] = grupo.empty() ? hoje() : grupo.back();
        ctx["start_date"] = ctx["start"];
        ctx["end_date"] = ctx["end"];
        ctx["hoje"] = hoje();
        ctx["timezone"] = CFG.fuso_horario;
        ctx["vars"] = juntar(vars, ",");
        ctx["api_key"] = chave_api;
        ctx["id"] = ds.id;
        ctx["tabela"] = ds.tabela;

        std::map<std::string, std::string> params;
        if (ds.params.empty()) {
            if (ds.usar_coordenadas) {
                auto ilat = ctx.find("latitude");
                auto ilon = ctx.find("longitude");
                if (ilat != ctx.end()) params["latitude"] = ilat->second;
                if (ilon != ctx.end()) params["longitude"] = ilon->second;
            }
            if (ds.iterar_datas && !grupo.empty()) {
                params["start_date"] = grupo.front();
                params["end_date"] = grupo.back();
            }
            if (ds.usar_coordenadas && !CFG.fuso_horario.empty())
                params["timezone"] = CFG.fuso_horario;
        }
        for (const auto& kv : ds.params)
            params[kv.first] = aplicar_ctx(kv.second, ctx);
        if (!vars.empty() && !chave_req.empty() && !params.count(chave_req))
            params[chave_req] = juntar(vars, ",");
        if (!chave_api.empty() && ds.api_key_em == "query")
            params[CFG.api_key_param] = chave_api;

        std::map<std::string, std::string> hdrs;
        for (const auto& kv : ds.headers)
            hdrs[kv.first] = aplicar_ctx(kv.second, ctx);
        if (!chave_api.empty() && ds.api_key_em == "header") {
            std::string pref = ds.api_key_prefix;
            hdrs[ds.api_key_header] = pref.empty() ? chave_api : (pref + " " + chave_api);
        }

        std::string url_req = aplicar_ctx(escolher_url(lista), ctx);
        std::string corpo = aplicar_ctx(ds.corpo, ctx);
        auto dados = baixar_com_retry(url_req, params, CFG.max_retries, custo, chave_api,
                                      ds.metodo, hdrs, corpo);
        if (dados) {
            std::set<std::string> filtro(grupo.begin(), grupo.end());
            const std::set<std::string>* pf = ds.iterar_datas ? &filtro : nullptr;
            auto regs = parse_json_generico(*dados, ds, ctx, pf, eh_polen, bloco_req);
            todos.insert(todos.end(), std::make_move_iterator(regs.begin()),
                         std::make_move_iterator(regs.end()));
        }
        i = j;
    }
    return todos;
}

static std::optional<ResultadoGenerico> baixar_bloco_generico(
    size_t dataset_idx, const std::vector<std::string>& datas, double lat, double lon,
    const Contexto& ctx_tarefa, const ExistentesGen& existentes) {
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
    r.ctx = ctx_tarefa;
    r.datas_pedidas = faltantes;
    if (!faltantes.empty()) {
        r.registros = baixar_dataset_bloco(ds, faltantes, ctx_tarefa);
    }
    return r;
}

static void processar_generico(const ResultadoGenerico& r, ExistentesGen& existentes,
                               std::map<size_t, long long>& baixados,
                               std::map<size_t, long long>& pulados) {
    const DatasetCfg& ds = CFG.datasets[r.dataset_idx];
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
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        pulados[r.dataset_idx] += static_cast<long long>(r.datas_pedidas.size()) - dias_retornados;
    }
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
    {
        std::lock_guard<std::mutex> lk(g_stats_mtx);
        baixados[r.dataset_idx] += dias_retornados;
    }
}

int executar_generico(const std::vector<std::string>& datas, double tempo_load,
                      const ExistentesGen& existentes_inicial) {
    ExistentesGen existentes = existentes_inicial;
    struct TarefaGen {
        size_t ds;
        std::vector<std::string> datas;
        double lat = 0.0, lon = 0.0;
        Contexto ctx;
    };
    std::vector<TarefaGen> tarefas;
    long long falt = 0;

    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        const DatasetCfg& ds = CFG.datasets[di];
        if (ds.tipo_saida == "arquivo") continue;

        // Se o dataset usar coordenadas (ou tiver coordenadas como eixo)
        if (ds.usar_coordenadas || CFG.coordenadas.empty()) {
            std::vector<std::pair<double, double>> lista_coords = CFG.coordenadas;
            if (lista_coords.empty()) {
                lista_coords.push_back({0.0, 0.0});
            }
            for (const auto& coord : lista_coords) {
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

                Contexto ctx;
                if (ds.usar_coordenadas) {
                    ctx["latitude"] = num_str(coord.first);
                    ctx["longitude"] = num_str(coord.second);
                    ctx["lat"] = ctx["latitude"];
                    ctx["lon"] = ctx["longitude"];
                }

                for (size_t i = 0; i < f.size(); i += (size_t)CFG.chunk_dias) {
                    size_t e = std::min(i + (size_t)CFG.chunk_dias, f.size());
                    tarefas.push_back({di, std::vector<std::string>(f.begin() + i, f.begin() + e),
                                       coord.first, coord.second, ctx});
                }
            }
        } else {
            // Expansão por eixos de dimensão arbitrários
            auto contextos = expandir_contextos(ds);
            for (const auto& ctx : contextos) {
                std::vector<std::string> f;
                for (const auto& d : datas) {
                    if (data_no_intervalo(d, ds)) f.push_back(d);
                }
                if (f.empty()) continue;
                falt += (long long)f.size();
                for (size_t i = 0; i < f.size(); i += (size_t)CFG.chunk_dias) {
                    size_t e = std::min(i + (size_t)CFG.chunk_dias, f.size());
                    tarefas.push_back({di, std::vector<std::string>(f.begin() + i, f.begin() + e),
                                       0.0, 0.0, ctx});
                }
            }
        }
    }

    std::cout << "[DB] Tarefas: " << tarefas.size() << " blocos, " << falt
              << " dia x chave pendentes.\n";
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

    std::cout << "\nIniciando " << total << " tarefas (" << CFG.download_workers
              << " download_workers, " << CFG.process_workers << " process_workers)...\n\n";

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
                log(std::string("[ERROR] Consumidor: ") + e.what());
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
                    auto res = baixar_bloco_generico(t.ds, t.datas, t.lat, t.lon, t.ctx, existentes);
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
              << "Tempo total: " << tt << "s\n"
              << "Carga inicial DB: " << tempo_load << "s\n"
              << "Blocos processados: " << concluidos.load() << "/" << total << "\n--- RESUMO ---\n";
    for (size_t di = 0; di < CFG.datasets.size(); ++di) {
        if (CFG.datasets[di].tipo_saida == "arquivo") continue;
        std::cout << CFG.datasets[di].id << " (" << CFG.datasets[di].db << "."
                  << CFG.datasets[di].tabela << "): " << baixados[di]
                  << " registros novos | " << pulados[di] << " ja existentes\n";
    }
    std::cout << "============================================================\n";
    return 0;
}

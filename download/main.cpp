#include "config.h"
#include "engine_generico.h"
#include "file_dataset.h"
#include "rate_limit.h"
#include "storage_sqlite.h"
#include "utils.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    std::string caminho_ini = "download.ini";
    std::vector<std::string> filtros_secao;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string arg_lower = arg;
        std::transform(arg_lower.begin(), arg_lower.end(), arg_lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (arg == "-h" || arg == "--help") {
            std::cout << "Uso: download.exe [caminho_config.ini] [-d dataset_id] [dataset1,dataset2]\n";
            return 0;
        } else if (arg.size() >= 4 && arg_lower.rfind(".ini") == arg.size() - 4) {
            caminho_ini = arg;
        } else if (arg == "-d" || arg == "--dataset" || arg == "-s" || arg == "--secao") {
            if (i + 1 < argc) {
                filtros_secao.push_back(argv[++i]);
            }
        } else {
            if (arg.front() == '-') continue;
            for (const auto& parte : dividir(arg, ',')) {
                if (!parte.empty()) filtros_secao.push_back(parte);
            }
        }
    }

    carregar_env(".env");
    carregar_config(caminho_ini);

    if (!filtros_secao.empty()) {
        std::vector<DatasetCfg> filtrados;
        for (const auto& ds : CFG.datasets) {
            for (const auto& f : filtros_secao) {
                std::string fn = f;
                if (fn.rfind("dataset:", 0) == 0) fn = fn.substr(8);
                std::string id_low = minusculas(ds.id);
                std::string fn_low = minusculas(fn);
                if (ds.id == fn || id_low == fn_low) {
                    filtrados.push_back(ds);
                    break;
                }
            }
        }
        CFG.datasets = std::move(filtrados);
    }

    if (CFG.datasets.empty()) {
        std::cerr << "[ERROR] Nenhum dataset correspondente encontrado em " << caminho_ini << "\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    iniciar_rate_limit();

    try {
        std::vector<DatasetCfg> datasets_db;
        std::vector<DatasetCfg> datasets_arquivo;
        for (const auto& ds : CFG.datasets) {
            if (ds.tipo_saida == "arquivo") {
                datasets_arquivo.push_back(ds);
            } else {
                datasets_db.push_back(ds);
            }
        }

        // 1) Executa datasets em arquivo (ex: ERA5 -> dados_era5/)
        for (const auto& ds : datasets_arquivo) {
            executar_dataset_arquivo(ds);
        }

        // 2) Executa datasets em banco SQLite (ex: Open-Meteo -> clima.db)
        if (!datasets_db.empty()) {
            std::vector<std::string> datas = expandir_periodo(CFG.periodo_inicio, CFG.periodo_fim);
            if (!datas.empty()) {
                std::cout << "============================================================\n"
                          << "DOWNLOAD DE DADOS CLIMATICOS / GENERICOS (BANCO SQLITE)\n"
                          << "============================================================\n"
                          << "Configuracao: " << caminho_ini << "\n"
                          << "Fuso horario: " << CFG.fuso_horario << "\n"
                          << "Banco de dados: " << CFG.db_path << "\n"
                          << "download_workers: " << CFG.download_workers
                          << " | process_workers: " << CFG.process_workers << "\n";

                criar_tabelas_datasets();

                std::cout << "\n[DB] Carregando registros existentes...\n";
                auto t0 = std::chrono::steady_clock::now();
                ExistentesGen existentes = carregar_existentes_generico(datas);
                double tload = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                for (size_t di = 0; di < CFG.datasets.size(); ++di) {
                    if (CFG.datasets[di].tipo_saida == "arquivo") continue;
                    long long tot = 0;
                    auto it = existentes.find(di);
                    if (it != existentes.end())
                        for (const auto& kv : it->second) tot += (long long)kv.second.size();
                    std::cout << "[DB] " << CFG.datasets[di].id << ": " << tot << " dias\n";
                }
                executar_generico(datas, tload, existentes);
            }
        }

        curl_global_cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[ERRO] " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    curl_global_cleanup();
    return 0;
}

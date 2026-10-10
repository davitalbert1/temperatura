#include "rate_limit.h"
#include "config.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

static std::mutex g_rate_mtx;
static std::chrono::steady_clock::time_point g_proximo_slot;
static double g_intervalo = 0.25;
static std::chrono::steady_clock::time_point g_cooldown_fim;
static int g_429_consecutivos = 0;
static std::map<std::string, std::chrono::steady_clock::time_point> g_ep_cooldown;

struct Janela {
    std::deque<std::pair<std::chrono::steady_clock::time_point, double>> ev;
    double soma = 0.0;

    void limpar(std::chrono::steady_clock::time_point agora, double dur) {
        auto corte = agora - std::chrono::duration<double>(dur);
        while (!ev.empty() && !(ev.front().first > corte)) {
            soma -= ev.front().second;
            ev.pop_front();
        }
    }
    double espera(std::chrono::steady_clock::time_point agora, double dur,
                  double limite, double custo) const {
        if (limite <= 0.0 || soma + custo <= limite) return 0.0;
        if (ev.empty()) return 0.0;
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

static std::mt19937& rng() {
    static thread_local std::mt19937 gen{
        static_cast<std::mt19937::result_type>(std::random_device{}() ^
                                               std::hash<std::thread::id>{}(
                                                   std::this_thread::get_id()))};
    return gen;
}

void iniciar_rate_limit() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    auto agora = std::chrono::steady_clock::now();
    g_proximo_slot = agora;
    g_intervalo = 1.0 / CFG.requests_per_second;
    g_cooldown_fim = agora;
    g_429_consecutivos = 0;
    g_orcamento.clear();
    g_ep_cooldown.clear();
}

void dormir(double segundos) {
    if (segundos > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(segundos));
}

void esperar_envio(const std::string& grupo, double custo) {
    static std::atomic<long long> g_ult_log_orc{0};
    for (int volta = 0; volta < 5000; ++volta) {
        double espera = 0.0;
        bool reservado = false;
        bool por_orcamento = false;
        {
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            auto agora = std::chrono::steady_clock::now();
            if (agora < g_cooldown_fim)
                espera = std::chrono::duration<double>(g_cooldown_fim - agora).count();
            if (espera <= 0.0) {
                auto& orc = g_orcamento[grupo];
                orc.limpar(agora);
                espera = orc.espera(agora, custo);
                por_orcamento = espera > 0.0;
            }
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
        if (por_orcamento && espera > 30.0) {
            long long seg = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            long long ult = g_ult_log_orc.load();
            if (seg - ult >= 60 && g_ult_log_orc.compare_exchange_strong(ult, seg)) {
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "  [BUDGET] Cota da API atingida (grupo %s). "
                              "Aguardando %.0fs para liberar...",
                              grupo.c_str(), espera);
                log(buf);
            }
        }
        dormir(espera > 0.0 ? espera : 0.05);
    }
    log("  [WARNING] Budget nao liberou apos muitas esperas; seguindo.");
}

void tratar_429(double espera, const std::string& motivo, const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    auto agora = std::chrono::steady_clock::now();
    double intervalo_max = (CFG.min_rps > 0.0) ? 1.0 / CFG.min_rps : 1.0e9;
    g_intervalo = std::min(intervalo_max, g_intervalo * 2.0);
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
            if (primeiro || espera > 30.0) {
                char buf[220];
                std::snprintf(buf, sizeof(buf),
                              "  [429] Pausa global de %.0fs (%s; intervalo %.2fs)",
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

void restaurar_taxa() {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    g_429_consecutivos = 0;
    double intervalo_min = 1.0 / CFG.requests_per_second;
    if (g_intervalo > intervalo_min)
        g_intervalo = std::max(intervalo_min, g_intervalo * 0.85);
}

double backoff_exponencial(int tentativa) {
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    double bruto = CFG.backoff_inicial * std::pow(2.0, tentativa - 1);
    return std::min(CFG.backoff_maximo, bruto * dist(rng()));
}

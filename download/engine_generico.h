#pragma once
#include "config.h"
#include "storage_sqlite.h"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

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
    size_t cap_;
    std::queue<T> q_;
    std::mutex m_;
    std::condition_variable cv_cheia_;
    std::condition_variable cv_vazia_;
    bool fechada_ = false;
};

struct ResultadoGenerico {
    size_t dataset_idx = 0;
    double lat = 0.0, lon = 0.0;
    Contexto ctx;
    std::vector<std::string> datas_pedidas;
    std::vector<Registro> registros;
};

std::vector<Registro> baixar_dataset_bloco(const DatasetCfg& ds,
                                          const std::vector<std::string>& datas,
                                          const Contexto& ctx_base);

int executar_generico(const std::vector<std::string>& datas, double tempo_load,
                      const ExistentesGen& existentes_inicial);

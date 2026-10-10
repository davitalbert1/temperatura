#include "http_client.h"
#include "config.h"
#include "rate_limit.h"
#include "utils.h"
#include <curl/curl.h>
#include <algorithm>
#include <iostream>

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

std::string escapar_url(const std::string& s) {
    CURL* c = curl_da_thread();
    if (!c) return s;
    char* e = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string r = e ? e : s;
    if (e) curl_free(e);
    return r;
}

std::string montar_url(const std::string& base,
                       const std::map<std::string, std::string>& params) {
    std::string url = base;
    bool primeiro = true;
    for (const auto& [k, v] : params) {
        url += (primeiro ? "?" : "&");
        primeiro = false;
        url += escapar_url(k) + "=" + escapar_url(v);
    }
    return url;
}

RespostaHttp http_requisicao(const std::string& url,
                             const std::string& metodo,
                             const std::map<std::string, std::string>& headers,
                             const std::string& corpo) {
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
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "download-generico/2.0");

    struct curl_slist* hdr = nullptr;
    std::string metodo_u = minusculas(metodo);
    bool tem_corpo = !corpo.empty() && metodo_u != "get" && metodo_u != "head";
    bool tem_json = tem_corpo && !corpo.empty() && (corpo.front() == '{' || corpo.front() == '[');
    bool tem_ct = false;
    for (const auto& kv : headers) {
        if (minusculas(kv.first) == "content-type") tem_ct = true;
        std::string linha = kv.first + ": " + kv.second;
        hdr = curl_slist_append(hdr, linha.c_str());
    }
    if (tem_json && !tem_ct)
        hdr = curl_slist_append(hdr, "Content-Type: application/json");
    if (hdr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);

    if (metodo_u == "post") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, corpo.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)corpo.size());
    } else if (metodo_u != "get" && metodo_u != "head") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, metodo.c_str());
        if (tem_corpo) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, corpo.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)corpo.size());
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    if (hdr) curl_slist_free_all(hdr);
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

std::optional<json> baixar_com_retry(const std::string& url,
                                     const std::map<std::string, std::string>& params,
                                     int max_tentativas, double custo,
                                     const std::string& chave_api,
                                     const std::string& metodo,
                                     const std::map<std::string, std::string>& headers,
                                     const std::string& corpo) {
    std::string url_completa = montar_url(url, params);
    std::string grupo = chave_api.empty() ? std::string("anonymous") : chave_api;
    for (int tentativa = 1; tentativa <= max_tentativas; ++tentativa) {
        esperar_envio(grupo, custo);

        RespostaHttp r = http_requisicao(url_completa, metodo, headers, corpo);

        if (!r.erro_rede && r.status < 400) {
            try {
                restaurar_taxa();
                return json::parse(r.corpo);
            } catch (const std::exception& e) {
                if (tentativa == max_tentativas) {
                    log("  [ERROR] Resposta sem JSON valido apos " +
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
            std::string corpo_resp = r.corpo.substr(0, 1000);
            std::transform(corpo_resp.begin(), corpo_resp.end(), corpo_resp.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            bool quota = corpo_resp.find("daily") != std::string::npos ||
                         corpo_resp.find("per day") != std::string::npos ||
                         corpo_resp.find("hour") != std::string::npos ||
                         corpo_resp.find("minute") != std::string::npos ||
                         corpo_resp.find("month") != std::string::npos;
            double espera;
            if (r.retry_after > 0) espera = r.retry_after;
            else if (quota) espera = CFG.cooldown_quota;
            else espera = std::max(CFG.espera_min_429, backoff_exponencial(tentativa));

            tratar_429(espera, quota ? "cota da API excedida" : "HTTP 429", url);
            if (tentativa == max_tentativas) {
                log("  [ERROR] 429 persistente apos " +
                    std::to_string(max_tentativas) + " tentativas");
                return std::nullopt;
            }
            char buf[220];
            std::snprintf(buf, sizeof(buf),
                          "  [RETRY %d/%d] HTTP 429%s. Pausa global de %.0fs...",
                          tentativa, max_tentativas, quota ? " (cota)" : "",
                          espera);
            log(buf);
        } else if (!r.erro_rede) {
            if (r.status == 400 || r.status == 404) {
                std::string motivo = r.corpo.substr(0, 300);
                log("  [ERROR] HTTP " + std::to_string(r.status) +
                    " (sem retry): " + motivo + " :: " + url_completa);
                return std::nullopt;
            }
            if (tentativa == max_tentativas) {
                log("  [ERROR] Falha apos " + std::to_string(max_tentativas) +
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
                log("  [ERROR] Falha de rede apos " + std::to_string(max_tentativas) +
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

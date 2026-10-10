#pragma once
#include <string>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct RespostaHttp {
    bool erro_rede = false;
    std::string erro_mensagem;
    long status = 0;
    std::string corpo;
    double retry_after = -1.0;
};

std::string escapar_url(const std::string& s);
std::string montar_url(const std::string& base, const std::map<std::string, std::string>& params);
RespostaHttp http_requisicao(const std::string& url,
                             const std::string& metodo = "GET",
                             const std::map<std::string, std::string>& headers = {},
                             const std::string& corpo = "");

std::optional<json> baixar_com_retry(const std::string& url,
                                     const std::map<std::string, std::string>& params,
                                     int max_tentativas, double custo,
                                     const std::string& chave_api,
                                     const std::string& metodo = "GET",
                                     const std::map<std::string, std::string>& headers = {},
                                     const std::string& corpo = "");

#include "utils.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>

static std::mutex g_print_mtx;

void log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_print_mtx);
    std::cout << msg << std::endl;
}

std::string cortar(const std::string& s) {
    const char* espacos = " \t\r\n";
    size_t i = s.find_first_not_of(espacos);
    if (i == std::string::npos) return "";
    size_t f = s.find_last_not_of(espacos);
    return s.substr(i, f - i + 1);
}

std::string minusculas(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return v;
}

std::string sem_comentario(const std::string& s) {
    size_t p = s.find_first_of(";#");
    return p == std::string::npos ? s : s.substr(0, p);
}

std::vector<std::string> dividir(const std::string& s, char sep) {
    std::vector<std::string> partes;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) partes.push_back(cortar(item));
    return partes;
}

std::string juntar(const std::vector<std::string>& itens, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < itens.size(); ++i) {
        if (i) out += sep;
        out += itens[i];
    }
    return out;
}

std::string num_str(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
    std::ostringstream oss;
    oss.precision(std::numeric_limits<double>::max_digits10);
    oss << v;
    return oss.str();
}

std::string normalizar_nome(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-' || c == '.' || c == ' ' || c == '/')
            out += '_';
    }
    std::string limpo;
    bool ultimo_sep = true;
    for (char c : out) {
        bool sep = (c == '_');
        if (sep && ultimo_sep) continue;
        limpo += c;
        ultimo_sep = sep;
    }
    while (!limpo.empty() && limpo.back() == '_') limpo.pop_back();
    if (limpo.empty()) limpo = "col";
    if (std::isdigit(static_cast<unsigned char>(limpo.front()))) limpo = "c_" + limpo;
    return limpo;
}

bool coluna_reservada(const std::string& c) {
    return c == "data" || c == "hora" || c == "timezone" || c == "latitude" ||
           c == "longitude" || c == "created_at";
}

std::string normalizar_tipo(const std::string& t) {
    std::string v = cortar(t);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v.empty() || v == "auto") return "auto";
    if (v == "int" || v == "integer" || v == "bool" || v == "boolean") return "integer";
    if (v == "real" || v == "float" || v == "double" || v == "number" || v == "numeric")
        return "real";
    return "text";
}

bool separar_def_coluna(const std::string& def, std::string& coluna,
                        std::string& variavel_api, std::string& tipo) {
    std::string d = cortar(def);
    if (d.empty()) return false;
    size_t igual = d.find('=');
    std::string esq, dir;
    if (igual == std::string::npos) {
        esq = d;
        dir = d;
    } else {
        esq = cortar(d.substr(0, igual));
        dir = cortar(d.substr(igual + 1));
        if (esq.empty()) esq = dir;
        if (dir.empty()) dir = esq;
    }
    size_t dp = dir.find(':');
    std::string var = cortar(dp == std::string::npos ? dir : dir.substr(0, dp));
    std::string tp = dp == std::string::npos ? "auto" : cortar(dir.substr(dp + 1));
    if (var.empty()) var = esq;
    coluna = normalizar_nome(esq);
    variavel_api = var.empty() ? coluna : var;
    tipo = normalizar_tipo(tp);
    return !coluna.empty();
}

int para_int(const std::string& s, int padrao) {
    try {
        return std::stoi(cortar(s));
    } catch (...) {
        return padrao;
    }
}

double para_double(const std::string& s, double padrao) {
    try {
        return std::stod(cortar(s));
    } catch (...) {
        return padrao;
    }
}

bool para_bool(const std::string& s, bool padrao) {
    std::string v = cortar(s);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "1" || v == "true" || v == "sim" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "nao" || v == "não" || v == "no" || v == "off" ||
        v == "desligado")
        return false;
    return padrao;
}

Data para_data(const std::string& s) {
    Data d;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &d.ano, &d.mes, &d.dia) != 3 || d.mes < 1 ||
        d.mes > 12 || d.dia < 1 || d.dia > 31)
        throw std::runtime_error("data invalida: " + s);
    return d;
}

long dias_de_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

Data civil_de_dias(long z) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = static_cast<long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    Data r;
    r.ano = static_cast<int>(y + (m <= 2));
    r.mes = static_cast<int>(m);
    r.dia = static_cast<int>(d);
    return r;
}

std::string data_texto(const Data& d) {
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", d.ano, d.mes, d.dia);
    return b;
}

std::tm agora_local() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return tmv;
}

std::string hoje() {
    std::tm tmv = agora_local();
    char b[16];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return b;
}

std::string agora_iso() {
    std::tm tmv = agora_local();
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}

std::vector<std::string> expandir_periodo(const std::string& inicio, const std::string& fim) {
    try {
        Data a = para_data(inicio), b = para_data(fim);
        long ia = dias_de_civil(a.ano, a.mes, a.dia);
        long ib = dias_de_civil(b.ano, b.mes, b.dia);
        std::vector<std::string> datas;
        for (long i = ia; i <= ib; ++i) datas.push_back(data_texto(civil_de_dias(i)));
        return datas;
    } catch (const std::exception& e) {
        log(std::string("[WARNING] Erro ao expandir datas: ") + e.what());
        return {inicio, fim};
    }
}

#pragma once
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <ctime>

// Manipulação e sanitização de strings
std::string cortar(const std::string& s);
std::string minusculas(std::string v);
std::string sem_comentario(const std::string& s);
std::vector<std::string> dividir(const std::string& s, char sep);
std::string juntar(const std::vector<std::string>& itens, const std::string& sep);
std::string num_str(double v);
std::string normalizar_nome(const std::string& s);
bool coluna_reservada(const std::string& c);
std::string normalizar_tipo(const std::string& t);
bool separar_def_coluna(const std::string& def, std::string& coluna,
                        std::string& variavel_api, std::string& tipo);

int para_int(const std::string& s, int padrao);
double para_double(const std::string& s, double padrao);
bool para_bool(const std::string& s, bool padrao);

// Utilitários de Data e Hora (Howard Hinnant civil days)
struct Data {
    int ano = 0, mes = 0, dia = 0;
};

Data para_data(const std::string& s);
long dias_de_civil(int y, int m, int d);
Data civil_de_dias(long z);
std::string data_texto(const Data& d);
std::tm agora_local();
std::string hoje();
std::string agora_iso();
std::vector<std::string> expandir_periodo(const std::string& inicio, const std::string& fim);

// Thread-safe logger
void log(const std::string& msg);

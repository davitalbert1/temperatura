#pragma once
#include <string>

void iniciar_rate_limit();
void dormir(double segundos);
void esperar_envio(const std::string& grupo, double custo);
void tratar_429(double espera, const std::string& motivo, const std::string& endpoint);
void restaurar_taxa();
double backoff_exponencial(int tentativa);

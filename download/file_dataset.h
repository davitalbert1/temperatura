#pragma once
#include "config.h"

// Orquestra e processa downloads salvos diretamente em arquivos (ex.: Copernicus CDS/ERA5)
void executar_dataset_arquivo(const DatasetCfg& ds);

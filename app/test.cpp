#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Utilitários
static std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string quoteIdentifier(const std::string& value) {
    // Escapa identificadores SQLite (nomes de tabelas/colunas).
    std::string out = "\"";
    for (char c : value) {
        out += c;
        if (c == '"') out += '"';
    }
    out += '"';
    return out;
}

static std::string escapeSqlString(const std::string& value) {
    std::string out;
    for (char c : value) {
        out += c;
        if (c == '\'') out += '\'';
    }
    return out;
}

static std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        part = trim(part);
        if (!part.empty()) result.push_back(part);
    }
    return result;
}

class IniConfig {
public:
    bool load(const std::string& path) {
        return loadFile(path);
    }

    std::string get(const std::string& section,
                    const std::string& key,
                    const std::string& fallback = "") const {
        const auto sectionIt = data.find(section);
        if (sectionIt == data.end()) return fallback;
        const auto keyIt = sectionIt->second.find(key);
        return keyIt == sectionIt->second.end() ? fallback : keyIt->second;
    }

    std::vector<std::string> sections() const {
        std::vector<std::string> result;
        for (const auto& entry : data) result.push_back(entry.first);
        return result;
    }

private:
    std::map<std::string, std::map<std::string, std::string>> data;

    bool loadFile(const std::string& path) {
        std::ifstream input(path);
        if (!input) return false;

        data.clear();
        std::string line;
        std::string currentSection;

        while (std::getline(input, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            if (line.front() == '[' && line.back() == ']') {
                currentSection = trim(line.substr(1, line.size() - 2));
                data[currentSection];
                continue;
            }

            const auto equals = line.find('=');
            if (equals == std::string::npos || currentSection.empty()) continue;

            const std::string key = trim(line.substr(0, equals));
            const std::string value = trim(line.substr(equals + 1));
            data[currentSection][key] = value;
        }
        return true;
    }

};

class Database {
public:
    sqlite3* handle = nullptr;

    explicit Database(const fs::path& path) {
        const int rc = sqlite3_open_v2(
            path.string().c_str(),
            &handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr
        );

        if (rc != SQLITE_OK) {
            const std::string error = handle ? sqlite3_errmsg(handle) : "erro ao abrir SQLite";
            if (handle) sqlite3_close(handle);
            handle = nullptr;
            throw std::runtime_error("Não foi possível abrir " + path.string() + ": " + error);
        }

        sqlite3_busy_timeout(handle, 10000);
    }

    ~Database() {
        if (handle) sqlite3_close(handle);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
};

using Rows = std::vector<std::vector<std::string>>;

static Rows executeQuery(Database& db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    int rc = sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Erro ao preparar SQL: " + std::string(sqlite3_errmsg(db.handle)));
    }

    Rows rows;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        std::vector<std::string> row;
        const int columns = sqlite3_column_count(statement);
        for (int i = 0; i < columns; ++i) {
            const unsigned char* value = sqlite3_column_text(statement, i);
            row.emplace_back(value ? reinterpret_cast<const char*>(value) : "NULL");
        }
        rows.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE) {
        const std::string error = sqlite3_errmsg(db.handle);
        sqlite3_finalize(statement);
        throw std::runtime_error("Erro ao executar SQL: " + error);
    }

    sqlite3_finalize(statement);
    return rows;
}

static std::vector<std::string> getTables(Database& db) {
    const auto rows = executeQuery(
        db,
        "SELECT name FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
    );

    std::vector<std::string> tables;
    for (const auto& row : rows) {
        if (!row.empty()) tables.push_back(row[0]);
    }
    return tables;
}

static bool tableExists(Database& db, const std::string& table) {
    const auto rows = executeQuery(
        db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='" +
        escapeSqlString(table) + "' LIMIT 1"
    );
    return !rows.empty();
}

static bool columnExists(Database& db, const std::string& table, const std::string& column) {
    if (!tableExists(db, table)) return false;
    const auto rows = executeQuery(db, "PRAGMA table_info(" + quoteIdentifier(table) + ")");
    for (const auto& row : rows) {
        if (row.size() > 1 && row[1] == column) return true;
    }
    return false;
}

// ------------------------------ Relatórios -------------------------------

static void listTables(Database& db) {
    std::cout << "\n=== TABELAS NO BANCO ===\n";
    const auto tables = getTables(db);
    if (tables.empty()) std::cout << "(nenhuma tabela)\n";
    for (const auto& table : tables) std::cout << table << '\n';
}

static void countRecords(Database& db) {
    std::cout << "\n=== TOTAL DE REGISTROS POR TABELA ===\n";
    for (const auto& table : getTables(db)) {
        try {
            const auto rows = executeQuery(
                db, "SELECT COUNT(*) FROM " + quoteIdentifier(table)
            );
            std::cout << table << ": " << (rows.empty() ? "?" : rows[0][0]) << '\n';
        } catch (const std::exception& error) {
            std::cout << table << ": erro -> " << error.what() << '\n';
        }
    }
}

static void showStructure(Database& db, const std::string& table) {
    std::cout << "\n=== ESTRUTURA: " << table << " ===\n";
    if (!tableExists(db, table)) {
        std::cout << "Tabela não encontrada.\n";
        return;
    }

    std::cout << "cid | nome | tipo | notnull | default | pk\n";
    const auto rows = executeQuery(db, "PRAGMA table_info(" + quoteIdentifier(table) + ")");
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) std::cout << " | ";
            std::cout << row[i];
        }
        std::cout << '\n';
    }
}

static void showIndexes(Database& db, const std::string& table) {
    std::cout << "\n=== ÍNDICES: " << table << " ===\n";
    if (!tableExists(db, table)) {
        std::cout << "Tabela não encontrada.\n";
        return;
    }

    const auto rows = executeQuery(db, "PRAGMA index_list(" + quoteIdentifier(table) + ")");
    if (rows.empty()) std::cout << "(sem índices)\n";

    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) std::cout << " | ";
            std::cout << row[i];
        }
        std::cout << '\n';
    }
}

static void showPeriod(Database& db, const std::string& table, const std::string& dateColumn) {
    std::cout << "\n=== PERÍODO: " << table << " ===\n";
    if (!tableExists(db, table) || !columnExists(db, table, dateColumn)) {
        std::cout << "Tabela ou coluna temporal não encontrada (" << dateColumn << ").\n";
        return;
    }

    const auto rows = executeQuery(
        db,
        "SELECT MIN(" + quoteIdentifier(dateColumn) + "), "
        "MAX(" + quoteIdentifier(dateColumn) + "), COUNT(*) FROM " +
        quoteIdentifier(table)
    );

    if (rows.empty() || rows[0][0] == "NULL") {
        std::cout << "Sem dados\n";
        return;
    }

    std::cout << "Início: " << rows[0][0] << '\n'
              << "Fim: " << rows[0][1] << '\n'
              << "Total: " << rows[0][2] << '\n';
}

static void showLatest(Database& db, const std::string& table,
                       const std::string& dateColumn,
                       const std::vector<std::string>& preferredColumns,
                       int limit) {
    std::cout << "\n=== ÚLTIMOS REGISTROS: " << table << " ===\n";
    if (!tableExists(db, table) || !columnExists(db, table, dateColumn)) {
        std::cout << "Tabela ou coluna temporal não encontrada (" << dateColumn << ").\n";
        return;
    }

    std::vector<std::string> columns;
    for (const auto& col : preferredColumns) {
        if (columnExists(db, table, col)) columns.push_back(col);
    }

    // Se o esquema da API for diferente do exemplo, mostra todas as colunas.
    if (columns.empty()) {
        const auto info = executeQuery(db, "PRAGMA table_info(" + quoteIdentifier(table) + ")");
        for (const auto& row : info) {
            if (row.size() > 1) columns.push_back(row[1]);
        }
    }

    if (columns.empty()) {
        std::cout << "Nenhuma coluna disponível.\n";
        return;
    }

    std::string sql = "SELECT ";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i) sql += ", ";
        sql += quoteIdentifier(columns[i]);
    }
    sql += " FROM " + quoteIdentifier(table) +
           " ORDER BY " + quoteIdentifier(dateColumn) +
           " DESC LIMIT " + std::to_string(std::max(1, limit));

    for (const auto& row : executeQuery(db, sql)) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) std::cout << " | ";
            std::cout << columns[i] << ": " << row[i];
        }
        std::cout << '\n';
    }
}

// Converte "YYYY-MM-DD", "YYYY-MM-DDTHH:MM:SS" ou "YYYY-MM-DD HH:MM:SS".
static std::time_t parseTimestamp(std::string value) {
    // Ignora sufixo de fração de segundo ou timezone para a comparação básica.
    if (value.size() > 19) value = value.substr(0, 19);
    std::tm tm{};
    std::istringstream input(value);

    if (value.size() == 10) {
        input >> std::get_time(&tm, "%Y-%m-%d");
    } else {
        input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (input.fail()) {
            input.clear();
            input.str(value);
            tm = {};
            input >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        }
    }

    if (input.fail()) return static_cast<std::time_t>(-1);
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

static void checkGaps(Database& db, const std::string& table,
                      const std::string& dateColumn, int expectedSeconds) {
    std::cout << "\n=== LACUNAS TEMPORAIS: " << table << " ===\n";
    if (!tableExists(db, table) || !columnExists(db, table, dateColumn)) {
        std::cout << "Tabela ou coluna temporal não encontrada (" << dateColumn << ").\n";
        return;
    }

    const auto rows = executeQuery(
        db, "SELECT " + quoteIdentifier(dateColumn) + " FROM " + quoteIdentifier(table) +
            " WHERE " + quoteIdentifier(dateColumn) + " IS NOT NULL ORDER BY " +
            quoteIdentifier(dateColumn)
    );

    bool foundGap = false;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto previous = parseTimestamp(rows[i - 1][0]);
        const auto current = parseTimestamp(rows[i][0]);

        if (previous == static_cast<std::time_t>(-1) ||
            current == static_cast<std::time_t>(-1)) {
            std::cout << "Data não reconhecida: " << rows[i - 1][0]
                      << " / " << rows[i][0] << '\n';
            continue;
        }

        const auto difference = static_cast<long long>(current - previous);
        if (difference != expectedSeconds) {
            foundGap = true;
            std::cout << "GAP/INTERVALO: " << rows[i - 1][0] << " -> " << rows[i][0]
                      << " (diferença: " << difference << " segundos)\n";
        }
    }

    if (!foundGap) std::cout << "Nenhum intervalo irregular encontrado (ou menos de dois registros).\n";
}

int main(int argc, char* argv[]) {
    try {
        fs::path configPath;

        if (argc >= 2) {
            configPath = fs::path(argv[1]);
        } else {
            const fs::path cwd = fs::current_path();
            configPath = cwd.parent_path() / "download.ini";
            if (!fs::exists(configPath)) configPath = cwd / "download.ini";
        }

        IniConfig config;
        if (!config.load(configPath.string())) {
            throw std::runtime_error(
                "Não foi possível ler download.ini: " + fs::absolute(configPath).string() +
                "\nInforme o caminho como argumento do programa."
            );
        }

        const fs::path configDirectory = fs::absolute(configPath).parent_path();
        const std::string defaultDb = config.get("geral", "db_path", "clima.db");

        auto resolveDatabasePath = [&](const std::string& name) {
            fs::path path(name.empty() ? defaultDb : name);
            if (path.is_relative()) path = configDirectory / path;
            return path.lexically_normal();
        };

        int latestLimit = 5;
        try {
            latestLimit = std::stoi(config.get("geral", "ultimos_registros", "5"));
        } catch (...) {
            latestLimit = 5;
        }
        latestLimit = std::max(1, latestLimit);

        struct Dataset {
            std::string id;
            std::string table;
            fs::path database;
            std::string granularity;
            std::string dateColumn;
        };

        std::vector<Dataset> datasets;
        std::set<std::string> databasePaths;

        for (const auto& section : config.sections()) {
            if (section.rfind("dataset:", 0) != 0) continue;

            const std::string id = section.substr(std::string("dataset:").size());
            const std::string table = config.get(section, "tabela", id);
            const fs::path dbPath = resolveDatabasePath(config.get(section, "db", defaultDb));
            const std::string granularity = config.get(section, "granularidade", "dia");
            const std::string dateColumn = config.get(
                section, "coluna_data", granularity == "hora" ? "time" : "date"
            );

            datasets.push_back({id, table, dbPath, granularity, dateColumn});
            databasePaths.insert(dbPath.string());
        }

        if (datasets.empty()) {
            const fs::path dbPath = resolveDatabasePath(defaultDb);
            databasePaths.insert(dbPath.string());
        }

        std::cout << "Configuração carregada: " << fs::absolute(configPath).string() << '\n';
        std::cout << "Bancos a analisar: " << databasePaths.size() << '\n';

        for (const auto& databasePath : databasePaths) {
            std::cout << "BANCO: " << databasePath;

            Database db(databasePath);
            listTables(db);

            bool datasetForThisDb = false;
            for (const auto& dataset : datasets) {
                if (dataset.database.string() != databasePath) continue;
                datasetForThisDb = true;

                showStructure(db, dataset.table);
                showIndexes(db, dataset.table);
                showPeriod(db, dataset.table, dataset.dateColumn);

                const bool hourly = dataset.granularity == "hora";
                checkGaps(db, dataset.table, dataset.dateColumn, hourly ? 3600 : 86400);

                if (hourly) {
                    showLatest(db, dataset.table, dataset.dateColumn,
                        {"time", "hora", "latitude", "longitude", "temperature_2m",
                         "apparent_temperature", "relativehumidity_2m", "precipitation"},
                        latestLimit);
                } else {
                    showLatest(db, dataset.table, dataset.dateColumn,
                        {"date", "data", "latitude", "longitude", "weathercode",
                         "temperature_2m_max", "temperature_2m_min", "sunrise", "sunset",
                         "precipitation_sum"},
                        latestLimit);
                }
            }

            if (!datasetForThisDb) {
                std::cout << "\nNenhuma seção [dataset:<id>] configurada; mostrando contagens.\n";
            }
            countRecords(db);
        }

        std::cout << "\nAnálise concluída.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "\nERRO: " << error.what() << '\n';
        return 1;
    }
}

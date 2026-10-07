import sqlite3
from datetime import datetime, timedelta

DB_NAME = "clima.db"

def contar_registros():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute("""
        SELECT name 
        FROM sqlite_master 
        WHERE type='table'
    """)

    tabelas = cursor.fetchall()

    print("\n=== TOTAL DE REGISTROS POR TABELA ===")

    for (tabela,) in tabelas:
        try:
            cursor.execute(f"SELECT COUNT(*) FROM {tabela}")
            total = cursor.fetchone()[0]
            print(f"{tabela}: {total}")
        except Exception as e:
            print(f"{tabela}: erro -> {e}")

    conn.close()

def periodo_dados(tabela, coluna_data):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute(f"""
        SELECT 
            MIN({coluna_data}),
            MAX({coluna_data}),
            COUNT(*)
        FROM {tabela}
    """)

    inicio, fim, total = cursor.fetchone()
    conn.close()

    print(f"\n=== PERÍODO: {tabela} ===")

    if inicio is None:
        print("Sem dados")
        return

    print(f"Início : {inicio}")
    print(f"Fim    : {fim}")
    print(f"Total  : {total}")

# ==============================
# LISTAR TABELAS
# ==============================
def listar_tabelas():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute("""
        SELECT name FROM sqlite_master
        WHERE type='table'
    """)

    tabelas = cursor.fetchall()
    conn.close()

    print("\n=== TABELAS NO BANCO ===")
    for t in tabelas:
        print(t[0])


# ==============================
# MOSTRAR ESTRUTURA
# ==============================
def estrutura_tabela(nome_tabela):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute(f"PRAGMA table_info({nome_tabela})")
    colunas = cursor.fetchall()

    conn.close()

    print(f"\n=== ESTRUTURA: {nome_tabela} ===")
    print("cid | nome | tipo | notnull | default | pk")
    for c in colunas:
        print(c)


# ==============================
# MOSTRAR ÍNDICES
# ==============================
def mostrar_indices(nome_tabela):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute(f"PRAGMA index_list({nome_tabela})")
    indices = cursor.fetchall()

    conn.close()

    print(f"\n=== ÍNDICES: {nome_tabela} ===")
    for i in indices:
        print(i)


# ==============================
# ÚLTIMOS REGISTROS HOURLY
# ==============================
def ultimos_hourly(limite=5):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute(f"""
        SELECT 
            local,
            data_hora,
            temperatura,
            umidade,
            sensacao,
            precipitacao
        FROM clima_hourly
        ORDER BY data_hora DESC
        LIMIT {limite}
    """)

    rows = cursor.fetchall()
    conn.close()

    print("\n=== ÚLTIMOS HOURLY ===")
    for r in rows:
        print(r)


# ==============================
# ÚLTIMOS REGISTROS DAILY
# ==============================
def ultimos_daily(limite=5):
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute(f"""
        SELECT 
            local,
            data,
            nascer_sol,
            por_sol
        FROM clima_daily
        ORDER BY data DESC
        LIMIT {limite}
    """)

    rows = cursor.fetchall()
    conn.close()

    print("\n=== ÚLTIMOS DAILY ===")
    for r in rows:
        print(r)

def verificar_gaps():
    conn = sqlite3.connect(DB_NAME)
    cursor = conn.cursor()

    cursor.execute("""
        SELECT data_hora 
        FROM clima_hourly
        ORDER BY data_hora
    """)

    datas = [datetime.fromisoformat(row[0]) for row in cursor.fetchall()]
    conn.close()

    print("\n=== GAPS REAIS ===")

    for i in range(1, len(datas)):
        esperado = datas[i-1] + timedelta(hours=1)

        if datas[i] != esperado:
            print(f"GAP REAL: {datas[i-1]} → {datas[i]}")

# ==============================
# MAIN
# ==============================
def main():
    listar_tabelas()

    tabelas = ["clima_hourly", "clima_daily"]

    for t in tabelas:
        estrutura_tabela(t)
        mostrar_indices(t)

    # NOVO: período dos dados
    periodo_dados("clima_hourly", "data_hora")
    periodo_dados("clima_daily", "data")
    verificar_gaps()
    contar_registros()

    # últimos registros
    ultimos_hourly()
    ultimos_daily()


if __name__ == "__main__":
    main()
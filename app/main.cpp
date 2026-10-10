#include <QApplication>
#include <QWidget>
#include <QTableView>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QSqlError>
#include <QSqlRecord>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QDateEdit>
#include <QLabel>
#include <QComboBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QCheckBox>

// Viewer generico: abre qualquer .db, lista tabelas e colunas sozinho,
// sem presumir arquitetura.
class GenericoApp : public QWidget {
private:
    QTableView *table;
    QSqlQueryModel *model;
    QComboBox *comboDb;
    QComboBox *comboTabela;
    QComboBox *comboData;
    QComboBox *comboValor;
    QLineEdit *inputDb;
    QLineEdit *inputLocal;
    QLineEdit *inputValor;
    QDateEdit *dataInicio;
    QDateEdit *dataFim;
    QCheckBox *chkTodasColunas;
    QStringList dbs;
    bool temLatLon = false;

    QString dbAtual() const {
        return inputDb->text().trimmed();
    }
    QString tabelaAtual() const {
        return comboTabela->currentText();
    }
    static QString aspas(const QString &id) {
        QString esc = id;
        esc.replace("\"", "\"\"");
        return "\"" + esc + "\"";
    }

public:
    GenericoApp();
    void escolherDb();
    void recarregarDbs();
    void trocarDb();
    void trocarTabela();
    QString montarSelect();
    QString montarWhere();
    void carregarTudo();
    void filtrar();
};

GenericoApp::GenericoApp() {
    setWindowTitle("Viewer Generico - qualquer banco/tabela");
    dbs << "clima.db";
    table = new QTableView();
    model = new QSqlQueryModel(this);
    table->setModel(model);
    inputDb = new QLineEdit("clima.db");
    comboDb = new QComboBox();
    QPushButton *btnAbrir = new QPushButton("Abrir...");
    QPushButton *btnRecarregar = new QPushButton("Recarregar DBs");
    comboTabela = new QComboBox();
    comboData = new QComboBox();
    comboValor = new QComboBox();
    inputLocal = new QLineEdit();
    inputLocal->setPlaceholderText("lat,lon (opcional)");
    inputValor = new QLineEdit();
    inputValor->setPlaceholderText("valor minimo (opcional)");
    dataInicio = new QDateEdit();
    dataFim = new QDateEdit();
    dataInicio->setCalendarPopup(true);
    dataFim->setCalendarPopup(true);
    dataInicio->clear();
    dataFim->clear();
    chkTodasColunas = new QCheckBox("Todas as colunas");
    chkTodasColunas->setChecked(true);
    QPushButton *btnFiltrar = new QPushButton("Filtrar");
    QPushButton *btnReset = new QPushButton("Mostrar tudo");
    QHBoxLayout *l1 = new QHBoxLayout();
    l1->addWidget(new QLabel("Banco:"));
    l1->addWidget(inputDb, 2);
    l1->addWidget(btnAbrir);
    l1->addWidget(btnRecarregar);
    l1->addWidget(new QLabel("Tabela:"));
    l1->addWidget(comboTabela, 2);
    QHBoxLayout *l2 = new QHBoxLayout();
    l2->addWidget(new QLabel("Col.data:"));
    l2->addWidget(comboData);
    l2->addWidget(new QLabel("De:"));
    l2->addWidget(dataInicio);
    l2->addWidget(new QLabel("Ate:"));
    l2->addWidget(dataFim);
    l2->addWidget(new QLabel("Local(lat,lon):"));
    l2->addWidget(inputLocal);
    l2->addWidget(new QLabel("Col.num:"));
    l2->addWidget(comboValor);
    l2->addWidget(inputValor);
    l2->addWidget(chkTodasColunas);
    l2->addWidget(btnFiltrar);
    l2->addWidget(btnReset);
    QVBoxLayout *layout = new QVBoxLayout();
    layout->addLayout(l1);
    layout->addLayout(l2);
    layout->addWidget(table);
    setLayout(layout);
    connect(btnAbrir, &QPushButton::clicked, this, &GenericoApp::escolherDb);
    connect(btnRecarregar, &QPushButton::clicked, this, &GenericoApp::recarregarDbs);
    connect(comboDb, &QComboBox::currentTextChanged, this, &GenericoApp::trocarDb);
    connect(comboTabela, &QComboBox::currentTextChanged, this, &GenericoApp::trocarTabela);
    connect(btnFiltrar, &QPushButton::clicked, this, &GenericoApp::filtrar);
    connect(btnReset, &QPushButton::clicked, this, &GenericoApp::carregarTudo);
    recarregarDbs();
}

void GenericoApp::escolherDb() {
    QString f = QFileDialog::getOpenFileName(this, "Abrir banco", "", "*.db *.sqlite");
    if (f.isEmpty()) return;
    if (!dbs.contains(f)) dbs << f;
    inputDb->setText(f);
    recarregarDbs();
}

void GenericoApp::recarregarDbs() {
    comboDb->clear();
    for (const QString &d : dbs) comboDb->addItem(d);
    int i = comboDb->findText(dbAtual());
    if (i >= 0) comboDb->setCurrentIndex(i);
    trocarDb();
}

void GenericoApp::trocarDb() {
    QString db = comboDb->currentText();
    if (!db.isEmpty()) inputDb->setText(db);
    if (QSqlDatabase::contains("gen")) QSqlDatabase::removeDatabase("gen");
    QSqlDatabase c = QSqlDatabase::addDatabase("QSQLITE", "gen");
    c.setDatabaseName(dbAtual());
    if (!c.open()) {
        QMessageBox::critical(this, "Erro", "Nao abriu: " + c.lastError().text());
        return;
    }
    QSqlQuery q(c);
    q.exec("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    comboTabela->clear();
    while (q.next()) comboTabela->addItem(q.value(0).toString());
    if (comboTabela->count() > 0) trocarTabela();
}

void GenericoApp::trocarTabela() {
    QSqlDatabase c = QSqlDatabase::database("gen");
    QString t = tabelaAtual();
    if (t.isEmpty()) return;
    QSqlRecord rec = c.record(t);
    comboData->clear();
    comboValor->clear();
    bool temLat = false, temLon = false;
    for (int i = 0; i < rec.count(); ++i) {
        QString n = rec.fieldName(i);
        QString low = n.toLower();
        if (low == "data" || low.contains("data") || low == "hora" || low.contains("time") ||
            low == "created_at")
            comboData->addItem(n);
        comboValor->addItem(n);
        if (low == "latitude") temLat = true;
        if (low == "longitude") temLon = true;
    }
    temLatLon = temLat && temLon;
    carregarTudo();
}

QString GenericoApp::montarSelect() {
    QSqlDatabase c = QSqlDatabase::database("gen");
    if (chkTodasColunas->isChecked()) return "*";
    QSqlRecord rec = c.record(tabelaAtual());
    QStringList cols;
    for (int i = 0; i < rec.count() && i < 8; ++i) cols << aspas(rec.fieldName(i));
    return cols.isEmpty() ? QString("*") : cols.join(", ");
}

QString GenericoApp::montarWhere() {
    QStringList w;
    QString dc = comboData->currentText();
    if (!dc.isEmpty() && dataInicio->date().isValid() && dataFim->date().isValid()) {
        QString d1 = dataInicio->date().toString("yyyy-MM-dd");
        QString d2 = dataFim->date().toString("yyyy-MM-dd");
        w << aspas(dc) + " BETWEEN '" + d1 + "' AND '" + d2 + " 23:59:59'";
    }
    QString loc = inputLocal->text().trimmed();
    if (!loc.isEmpty() && temLatLon) {
        QStringList p = loc.split(",");
        if (p.size() == 2) {
            bool ok1 = false, ok2 = false;
            double la = p[0].trimmed().toDouble(&ok1);
            double lo = p[1].trimmed().toDouble(&ok2);
            if (ok1 && ok2) w << QString("latitude = %1 AND longitude = %2").arg(la).arg(lo);
        }
    }
    QString cv = comboValor->currentText();
    QString vv = inputValor->text().trimmed();
    if (!cv.isEmpty() && !vv.isEmpty()) {
        bool ok = false;
        vv.toDouble(&ok);
        if (ok) w << aspas(cv) + " >= " + vv;
    }
    return w.isEmpty() ? QString() : QString("WHERE ") + w.join(" AND ");
}

void GenericoApp::carregarTudo() {
    QString t = tabelaAtual();
    if (t.isEmpty()) return;
    QString dc = comboData->currentText();
    QString sql = "SELECT " + montarSelect() + " FROM " + aspas(t) + " ";
    if (!dc.isEmpty()) sql += "ORDER BY " + aspas(dc) + " DESC ";
    sql += "LIMIT 500;";
    model->setQuery(sql, QSqlDatabase::database("gen"));
    if (model->lastError().isValid())
        QMessageBox::critical(this, "Erro SQL", model->lastError().text());
}

void GenericoApp::filtrar() {
    QString t = tabelaAtual();
    if (t.isEmpty()) return;
    QString dc = comboData->currentText();
    QString sql = "SELECT " + montarSelect() + " FROM " + aspas(t) + " " + montarWhere() + " ";
    if (!dc.isEmpty()) sql += "ORDER BY " + aspas(dc) + " DESC ";
    sql += "LIMIT 500;";
    model->setQuery(sql, QSqlDatabase::database("gen"));
    if (model->lastError().isValid())
        QMessageBox::critical(this, "Erro SQL", model->lastError().text());
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    GenericoApp w;
    w.resize(1200, 600);
    w.show();
    return app.exec();
}

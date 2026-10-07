#include <QApplication>
#include <QWidget>
#include <QTableView>
#include <QSqlDatabase>
#include <QSqlQueryModel>
#include <QSqlError>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QDateEdit>
#include <QTimeEdit>
#include <QLabel>
#include <QMessageBox>

class ClimaApp : public QWidget {
private:
    QTableView *table;
    QSqlQueryModel *model;

    QLineEdit *inputLocal;
    QLineEdit *inputChuva;

    QDateEdit *dataInicio;
    QDateEdit *dataFim;

    QTimeEdit *horaInicio;
    QTimeEdit *horaFim;

public:
    ClimaApp() {
        setWindowTitle("Clima Viewer - Filtros Opcionais");

        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
        db.setDatabaseName("clima.db");

        if (!db.open()) {
            QMessageBox::critical(this, "Erro", "Não abriu banco");
            exit(1);
        }

        inputLocal = new QLineEdit();
        inputChuva = new QLineEdit();

        dataInicio = new QDateEdit();
        dataFim = new QDateEdit();
        horaInicio = new QTimeEdit();
        horaFim = new QTimeEdit();

        inputLocal->setPlaceholderText("Local (opcional)");
        inputChuva->setPlaceholderText("Chuva mínima (opcional)");

        dataInicio->setCalendarPopup(true);
        dataFim->setCalendarPopup(true);

        dataInicio->clear();
        dataFim->clear();

        horaInicio->clear();
        horaFim->clear();

        QPushButton *btnFiltrar = new QPushButton("Filtrar");
        QPushButton *btnReset = new QPushButton("Mostrar tudo");

        table = new QTableView();
        model = new QSqlQueryModel();
        table->setModel(model);

        QHBoxLayout *filters = new QHBoxLayout();

        filters->addWidget(new QLabel("Local:"));
        filters->addWidget(inputLocal);

        filters->addWidget(new QLabel("Data Início:"));
        filters->addWidget(dataInicio);

        filters->addWidget(new QLabel("Data Fim:"));
        filters->addWidget(dataFim);

        filters->addWidget(new QLabel("Hora Ini:"));
        filters->addWidget(horaInicio);

        filters->addWidget(new QLabel("Hora Fim:"));
        filters->addWidget(horaFim);

        filters->addWidget(new QLabel("Chuva >= "));
        filters->addWidget(inputChuva);

        filters->addWidget(btnFiltrar);
        filters->addWidget(btnReset);

        QVBoxLayout *layout = new QVBoxLayout();
        layout->addLayout(filters);
        layout->addWidget(table);

        setLayout(layout);

        connect(btnFiltrar, &QPushButton::clicked, this, &ClimaApp::filtrar);
        connect(btnReset, &QPushButton::clicked, this, &ClimaApp::carregarTudo);
        carregarTudo();
    }

    void carregarTudo() {
        QString sql =
            "SELECT data_hora, local, temperatura, precipitacao "
            "FROM clima_hourly "
            "ORDER BY data_hora DESC "
            "LIMIT 500;";

        model->setQuery(sql, QSqlDatabase::database());

        if (model->lastError().isValid()) QMessageBox::critical(this, "Erro SQL", model->lastError().text());
    }

    void filtrar() {
        QString sql =
            "SELECT data_hora, local, temperatura, precipitacao "
            "FROM clima_hourly WHERE 1=1";

        QString local = inputLocal->text().trimmed();
        QString chuva = inputChuva->text().trimmed();

        QString d1 = dataInicio->date().isValid() ? dataInicio->date().toString("yyyy-MM-dd") + " 00:00:00" : "";
        QString d2 = dataFim->date().isValid() ? dataFim->date().toString("yyyy-MM-dd") + " 23:59:59" : "";

        if (!local.isEmpty()) sql += " AND local = '" + local + "'";
        if (!d1.isEmpty() && !d2.isEmpty()) sql += " AND data_hora BETWEEN '" + d1 + "' AND '" + d2 + "'";
        if (!chuva.isEmpty()) sql += " AND precipitacao >= " + chuva;

        sql += " ORDER BY data_hora DESC LIMIT 500;";

        model->setQuery(sql, QSqlDatabase::database());

        if (model->lastError().isValid()) QMessageBox::critical(this, "Erro SQL", model->lastError().text());
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    ClimaApp w;
    w.resize(1200, 600);
    w.show();

    return app.exec();
}
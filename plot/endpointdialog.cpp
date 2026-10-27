#include "endpointdialog.h"

#include "plotpaths.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

namespace {

QString seriesSummary(const PlotGroup &group)
{
    QStringList names;
    for (const PlotSeries &series : group.series) {
        if (!series.title.isEmpty())
            names.append(series.title);
    }
    return names.isEmpty() ? QStringLiteral("(no series)") : names.join(QStringLiteral(", "));
}

} // namespace

EndpointDialog::EndpointDialog(const QString &endpointsPath, QWidget *parent)
    : QDialog(parent)
    , m_path(endpointsPath)
{
    setWindowTitle(tr("Choose curve group"));
    resize(720, 340);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels(
            {tr("Group"), tr("Series"), tr("State"), tr("Pipe")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    auto *refreshButton = new QPushButton(tr("Refresh"), this);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Open)->setText(tr("Connect"));

    auto *topRow = new QHBoxLayout;
    topRow->addWidget(m_status, 1);
    topRow->addWidget(refreshButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topRow);
    layout->addWidget(m_table, 1);
    layout->addWidget(buttons);

    connect(refreshButton, &QPushButton::clicked, this, &EndpointDialog::reload);
    connect(buttons, &QDialogButtonBox::accepted, this, &EndpointDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this] { acceptSelection(); });

    reload();
}

void EndpointDialog::reload()
{
    const PlotEndpointFile file = readPlotEndpoints(m_path);

    m_table->setRowCount(0);
    if (!file.valid) {
        m_status->setText(tr("Cannot read %1 (%2).\nStart %3 and enable Curve in the "
                             "CSV panel first.")
                                  .arg(m_path, file.error, toolsExecutableName()));
        return;
    }

    if (file.groups.isEmpty()) {
        m_status->setText(tr("%1 is running but exposes no curve group.\n"
                             "Tick Curve on a CSV row and enable the curve icon of a numeric "
                             "column.")
                                  .arg(toolsExecutableName()));
        return;
    }

    m_status->setText(tr("Session %1 (%2 group(s)) - updated %3")
                              .arg(file.sessionId)
                              .arg(file.groups.size())
                              .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));

    for (const PlotGroup &group : std::as_const(file.groups)) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *titleItem =
                new QTableWidgetItem(group.title.isEmpty() ? group.key : group.title);
        titleItem->setData(Qt::UserRole, group.key);
        auto *seriesItem = new QTableWidgetItem(seriesSummary(group));
        auto *stateItem = new QTableWidgetItem(group.attached ? tr("in use") : tr("free"));
        auto *pipeItem = new QTableWidgetItem(group.pipe);
        m_table->setItem(row, 0, titleItem);
        m_table->setItem(row, 1, seriesItem);
        m_table->setItem(row, 2, stateItem);
        m_table->setItem(row, 3, pipeItem);
    }

    m_table->resizeRowsToContents();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 2) && m_table->item(row, 2)->text() == tr("free")) {
            m_table->selectRow(row);
            break;
        }
    }
    if (m_table->selectedItems().isEmpty() && m_table->rowCount() > 0)
        m_table->selectRow(0);
}

void EndpointDialog::acceptSelection()
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_table->item(row, 0)) {
        m_status->setText(tr("Select a group first."));
        return;
    }
    m_selectedKey = m_table->item(row, 0)->data(Qt::UserRole).toString();
    accept();
}

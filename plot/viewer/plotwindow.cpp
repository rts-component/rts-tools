#include "plot/viewer/plotwindow.h"

#include "plot/viewer/endpointdialog.h"
#include "plot/viewer/plotcanvas.h"
#include "plot/viewer/plotclient.h"
#include "plot/viewer/ploticons.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QSet>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString chipStyle(const QColor &color)
{
    return QStringLiteral("QToolButton { background:%1; border:1px solid #9aa2ad;"
                          " border-radius:3px; }"
                          "QToolButton:hover { border:1px solid #4a90d9; }")
            .arg(color.name());
}

QString windowStyleSheet()
{
    return QStringLiteral(
        "QMainWindow, QWidget#plotRoot { background: #f0f0f0; }"
        "QWidget#seriesPanel { background: #e4e4e4; }"
        "QWidget#plotHeader { background: #f7f7f7; border-bottom: 1px solid #d5d5d5; }"
        "QLabel { color: #202020; }"
        "QCheckBox { color: #202020; spacing: 5px; }"
        "QComboBox { background: #ffffff; color: #202020; border: 1px solid #b8b8b8;"
        "  padding: 1px 6px; }"
        "QComboBox QAbstractItemView { background: #ffffff; color: #202020; }"
        "QToolButton { background: transparent; border: none; color: #202020; padding: 2px; }"
        "QToolButton:hover { background: #dfe4ea; border-radius: 4px; }"
        "QToolButton#plotAction { background: #ffffff; border: 1px solid #ccd2da;"
        "  border-radius: 4px; padding: 2px 8px; color: #2b3038; }"
        "QToolButton#plotAction:hover { background: #f1f5fa; border-color: #a9b2be; }"
        "QToolButton#plotAction:pressed { background: #e2e8f0; border-color: #97a1ad; }"
        "QToolButton#plotPause { background: #e0662a; border: 1px solid #c8551e;"
        "  border-radius: 4px; padding: 2px 10px; color: #ffffff; font-weight: 600; }"
        "QToolButton#plotPause:hover { background: #ea7639; border-color: #d0601f; }"
        "QToolButton#plotPause:pressed { background: #cb5a22; }"
        "QToolButton#plotPause:checked { background: #ffffff; color: #c05420;"
        "  border: 1px solid #e0662a; }"
        "QStatusBar { background: #f7f7f7; color: #4a4a4a; }"
        "QStatusBar QLabel { color: #4a4a4a; }"
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: #e4e4e4; width: 10px; }"
        "QScrollBar::handle:vertical { background: #b8b8b8; border-radius: 5px; }");
}

// 面板里带图标和文字的按钮：objectName 决定 QSS 里用哪套样式
QToolButton *makeActionButton(QWidget *parent, const QString &objectName, const QIcon &icon,
                              const QString &text, const QString &toolTip, int height)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIconSize(QSize(14, 14));
    button->setIcon(icon);
    button->setText(text);
    button->setToolTip(toolTip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(height);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

// 无边距的横向布局，面板里的每一行都是这个形状
QHBoxLayout *makeRow(int spacing)
{
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(spacing);
    return row;
}

} // namespace

// ---------------------------------------------------------------- 窗口

PlotWindow::PlotWindow(PlotClient *client, QWidget *parent)
    : QMainWindow(parent)
    , m_client(client)
{
    setWindowTitle(tr("rts-plot"));
    resize(1180, 640);
    setStyleSheet(windowStyleSheet());

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("plotRoot"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QWidget *header = buildHeader(root);
    QWidget *panel = buildSidePanel(root);

    // 绘图区
    m_canvas = new PlotCanvas(m_client, root);
    rebuildHLineRows(); // 需要画布已创建

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(panel);
    body->addWidget(m_canvas, 1);

    rootLayout->addWidget(header);
    rootLayout->addLayout(body, 1);
    setCentralWidget(root);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);

    connectSignals();

    refreshTitle();
    refreshStatus();
    rebuildSeriesRows();
}

// 顶部标题行
QWidget *PlotWindow::buildHeader(QWidget *root)
{
    auto *header = new QWidget(root);
    header->setObjectName(QStringLiteral("plotHeader"));
    header->setFixedHeight(34);
    m_titleLabel = new QLabel(header);
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setTextFormat(Qt::PlainText);
    m_connectionLabel = new QLabel(header);
    m_connectionLabel->setFont(titleFont);

    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 0, 8, 0);
    headerLayout->setSpacing(8);
    headerLayout->addWidget(m_titleLabel, 1);
    headerLayout->addWidget(m_connectionLabel); // 连接状态靠最右
    return header;
}

// 左侧面板：暂停/适配/清空、自动缩放、缓存上限、测量、系列列表、水平参考线
QWidget *PlotWindow::buildSidePanel(QWidget *root)
{
    auto *panel = new QWidget(root);
    panel->setObjectName(QStringLiteral("seriesPanel"));
    panel->setFixedWidth(232);

    m_pauseButton = makeActionButton(panel, QStringLiteral("plotPause"),
                                     PlotIcons::pause(Qt::white), tr("Pause"),
                                     tr("Pause / resume the view"), 32);
    m_pauseButton->setCheckable(true);

    auto *fitButton = makeActionButton(panel, QStringLiteral("plotAction"),
                                       PlotIcons::fit(QColor(0x3a, 0x40, 0x48)), tr("Fit"),
                                       tr("Fit all data to the view and follow new samples"), 26);
    auto *clearButton = makeActionButton(panel, QStringLiteral("plotAction"),
                                         PlotIcons::trash(QColor(0x3a, 0x40, 0x48)), tr("Clear"),
                                         tr("Clear buffered samples of every series"), 26);

    auto *toolsRow = new QVBoxLayout;
    toolsRow->setContentsMargins(0, 0, 0, 2);
    toolsRow->setSpacing(5);
    toolsRow->addWidget(m_pauseButton);

    QHBoxLayout *toolsSubRow = makeRow(5);
    toolsSubRow->addWidget(fitButton);
    toolsSubRow->addWidget(clearButton);
    toolsRow->addLayout(toolsSubRow);

    m_autoXCheck = new QCheckBox(tr("X"), panel);
    m_autoXCheck->setChecked(true);
    m_autoYCheck = new QCheckBox(tr("Y"), panel);
    m_autoYCheck->setChecked(true);
    QHBoxLayout *autoRow = makeRow(6);
    autoRow->addWidget(new QLabel(tr("Auto:"), panel));
    autoRow->addWidget(m_autoXCheck);
    autoRow->addWidget(m_autoYCheck);
    autoRow->addStretch();

    m_cacheCombo = new QComboBox(panel);
    // 0 = 不设软上限，实际受 PlotClient 的硬上限约束，标签如实写出
    m_cacheCombo->addItem(tr("Max (%1)").arg(QLocale().toString(PlotClient::kMaxSamplesPerSeries)),
                          0);
    m_cacheCombo->addItem(QStringLiteral("1 000"), 1000);
    m_cacheCombo->addItem(QStringLiteral("10 000"), 10000);
    m_cacheCombo->addItem(QStringLiteral("100 000"), 100000);
    m_measureCheck = new QCheckBox(tr("X Measure"), panel);
    m_measureCheck->setEnabled(false);
    m_measureCheck->setToolTip(tr("Pause first, then drag the two vertical lines to measure "
                                  "the index / time difference"));
    m_measureYCheck = new QCheckBox(tr("Y Measure"), panel);
    m_measureYCheck->setEnabled(false);
    m_measureYCheck->setToolTip(tr("Pause first, then drag the two horizontal lines to measure "
                                   "the value difference"));
    QHBoxLayout *measureRow = makeRow(6);
    measureRow->addWidget(m_measureCheck);
    measureRow->addWidget(m_measureYCheck);
    measureRow->addStretch();
    QHBoxLayout *cacheRow = makeRow(6);
    cacheRow->addWidget(new QLabel(tr("Cache:"), panel));
    cacheRow->addWidget(m_cacheCombo, 1);

    m_seriesHost = new QWidget;
    m_seriesLayout = new QVBoxLayout(m_seriesHost);
    m_seriesLayout->setContentsMargins(0, 0, 0, 0);
    m_seriesLayout->setSpacing(1);
    m_seriesLayout->setAlignment(Qt::AlignTop);

    auto *scroll = new QScrollArea(panel);
    scroll->setWidget(m_seriesHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(80);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(8, 8, 8, 6);
    panelLayout->setSpacing(8);
    panelLayout->addLayout(toolsRow);
    panelLayout->addLayout(autoRow);
    panelLayout->addLayout(cacheRow);
    panelLayout->addLayout(measureRow);
    panelLayout->addWidget(scroll, 1);

    // 水平参考线（最多 5 条）：左下角
    auto *hlineTitle = new QLabel(tr("Horizontal lines"), panel);
    m_hlineAddButton = new QToolButton(panel);
    m_hlineAddButton->setObjectName(QStringLiteral("plotAction"));
    m_hlineAddButton->setText(QStringLiteral("+"));
    m_hlineAddButton->setFixedSize(22, 22);
    m_hlineAddButton->setCursor(Qt::PointingHandCursor);
    QHBoxLayout *hlineHeader = makeRow(-1); // -1 = 沿用样式默认间距
    hlineHeader->addWidget(hlineTitle, 1);
    hlineHeader->addWidget(m_hlineAddButton);
    m_hlineLayout = new QVBoxLayout;
    m_hlineLayout->setContentsMargins(0, 0, 0, 0);
    m_hlineLayout->setSpacing(2);
    panelLayout->addLayout(hlineHeader);
    panelLayout->addLayout(m_hlineLayout);

    connect(m_hlineAddButton, &QToolButton::clicked, this, &PlotWindow::addHLine);
    connect(fitButton, &QToolButton::clicked, this, &PlotWindow::fitView);
    connect(clearButton, &QToolButton::clicked, this, [this] { m_client->clearAllSamples(); });
    return panel;
}

// 控件与画布/数据源之间的信号连接；需要 m_canvas 已创建
void PlotWindow::connectSignals()
{
    connect(m_autoXCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setAutoFitX(on);
    });
    connect(m_autoYCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setAutoY(on);
    });
    connect(m_cacheCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_client->setSampleLimit(m_cacheCombo->currentData().toInt());
    });
    connect(m_pauseButton, &QToolButton::toggled, this, &PlotWindow::setPaused);
    connect(m_measureCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setMeasure(on);
    });
    connect(m_measureYCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_canvas->setMeasureY(on);
    });

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(50);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        refreshValues();
        // 缩放/平移会改掉视口模式，把复选框同步回来
        if (m_canvasDirty) {
            m_canvasDirty = false;
            m_canvas->update();
        }
        // 缩放/拖动后也要跟着新数据刷新
        {
            const QSignalBlocker blockerX(m_autoXCheck);
            m_autoXCheck->setChecked(m_canvas->autoFitX());
            const QSignalBlocker blockerY(m_autoYCheck);
            m_autoYCheck->setChecked(m_canvas->autoY());
        }
    });
    m_refreshTimer->start();

    connect(m_client, &PlotClient::stateChanged, this, [this] {
        refreshStatus();
        applySuggestedPosition();
    });
    connect(m_client, &PlotClient::metaChanged, this, &PlotWindow::rebuildSeriesRows);
    connect(m_client, &PlotClient::samplesAppended, this, [this] { m_canvasDirty = true; });
}

void PlotWindow::chooseGroup()
{
    EndpointDialog dialog(m_client->endpointsFilePath(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_positionApplied = false;
    m_client->setAutoReconnect(true);
    m_client->connectToGroup(dialog.selectedGroupKey());
}

void PlotWindow::refreshTitle()
{
    const QString title = m_client->title().isEmpty() ? m_client->groupKey() : m_client->title();
    const QString name = title.isEmpty() ? tr("(no group)") : title;
    const bool connected = m_client->state() == PlotClient::State::Connected;

    // 名字在左；连接状态在最右，用颜色区分
    m_titleLabel->setText(name);
    m_connectionLabel->setText(connected ? tr("Connected") : tr("Disconnected"));
    m_connectionLabel->setStyleSheet(QStringLiteral("color: %1;")
                                             .arg(connected ? QStringLiteral("#1e7b34")
                                                            : QStringLiteral("#b3261e")));
    m_connectionLabel->setToolTip(statusDetail());
    m_titleLabel->setToolTip(statusDetail());
}

QString PlotWindow::statusDetail() const
{
    QStringList parts;
    const QString state = m_client->placeholder().isEmpty() ? m_client->stateText()
                                                            : m_client->placeholder();
    if (!state.isEmpty())
        parts.append(state);
    if (!m_client->pipe().isEmpty())
        parts.append(m_client->pipe());
    return parts.join(QLatin1Char('\n'));
}

void PlotWindow::addHLine()
{
    static const QColor kColors[PlotCanvas::kMaxHLines] = {
        QColor(0xff, 0xd5, 0x4f), QColor(0x4d, 0xd0, 0xe1), QColor(0xf0, 0x62, 0x92),
        QColor(0xae, 0xd5, 0x81), QColor(0xff, 0xff, 0xff),
    };
    PlotCanvas::HLine line;
    // 取一个没被占用的序号和颜色
    for (int n = 1; n <= PlotCanvas::kMaxHLines; ++n) {
        const QString name = tr("Line %1").arg(n);
        const bool used = std::any_of(m_canvas->hlines().cbegin(), m_canvas->hlines().cend(),
                                      [&name](const PlotCanvas::HLine &other) {
                                          return other.name == name;
                                      });
        if (!used) {
            line.name = name;
            line.color = kColors[n - 1];
            break;
        }
    }
    line.value = m_canvas->viewCenterY();
    if (m_canvas->addHLine(line))
        rebuildHLineRows();
}

void PlotWindow::rebuildHLineRows()
{
    while (QLayoutItem *item = m_hlineLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }

    const QVector<PlotCanvas::HLine> lines = m_canvas->hlines();
    for (int i = 0; i < lines.size(); ++i) {
        const PlotCanvas::HLine &line = lines.at(i);
        auto *row = new QWidget;

        auto *visible = new QCheckBox(row);
        visible->setChecked(line.visible);
        visible->setToolTip(tr("Show / hide this line"));

        auto *name = new QLineEdit(line.name, row);
        name->setMinimumWidth(44);
        name->setPlaceholderText(tr("Name"));
        name->setToolTip(tr("Line name"));

        auto *value = new QDoubleSpinBox(row);
        value->setRange(-1.0e15, 1.0e15);
        value->setDecimals(4);
        value->setButtonSymbols(QAbstractSpinBox::NoButtons);
        value->setValue(line.value);
        value->setFixedWidth(84);
        value->setToolTip(tr("Line value (Y)"));

        auto *chip = new QToolButton(row);
        chip->setFixedSize(16, 16);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tr("Click to change this line's colour"));
        chip->setStyleSheet(chipStyle(line.color));

        auto *remove = new QToolButton(row);
        remove->setText(QStringLiteral("\u00d7"));
        remove->setToolTip(tr("Delete this line"));
        remove->setAutoRaise(true);
        remove->setFixedSize(18, 18);
        remove->setCursor(Qt::PointingHandCursor);

        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 1, 2, 1);
        layout->setSpacing(4);
        layout->addWidget(visible);
        layout->addWidget(name, 1);
        layout->addWidget(value);
        layout->addWidget(chip);
        layout->addWidget(remove);
        m_hlineLayout->addWidget(row);

        auto edit = [this, i](auto &&change) {
            if (i >= m_canvas->hlines().size())
                return;
            PlotCanvas::HLine updated = m_canvas->hlines().at(i);
            change(updated);
            m_canvas->setHLine(i, updated);
        };
        connect(visible, &QCheckBox::toggled, this, [edit](bool on) {
            edit([on](PlotCanvas::HLine &l) { l.visible = on; });
        });
        connect(name, &QLineEdit::textChanged, this, [edit](const QString &text) {
            edit([&text](PlotCanvas::HLine &l) { l.name = text; });
        });
        connect(value, &QDoubleSpinBox::valueChanged, this, [edit](double v) {
            edit([v](PlotCanvas::HLine &l) { l.value = v; });
        });
        connect(chip, &QToolButton::clicked, this, [this, i, chip] {
            if (i >= m_canvas->hlines().size())
                return;
            PlotCanvas::HLine updated = m_canvas->hlines().at(i);
            const QColor picked = QColorDialog::getColor(updated.color, this, tr("Line colour"));
            if (!picked.isValid())
                return;
            updated.color = picked;
            m_canvas->setHLine(i, updated);
            chip->setStyleSheet(chipStyle(picked));
        });
        connect(remove, &QToolButton::clicked, this, [this, i] {
            m_canvas->removeHLine(i);
            // 在按钮自己的槽函数里不能立刻销毁它，等事件循环回来再重建
            QTimer::singleShot(0, this, &PlotWindow::rebuildHLineRows);
        });
    }
    m_hlineAddButton->setEnabled(lines.size() < PlotCanvas::kMaxHLines);
    m_hlineAddButton->setToolTip(lines.size() < PlotCanvas::kMaxHLines
                                         ? tr("Add a horizontal line")
                                         : tr("At most %1 lines").arg(PlotCanvas::kMaxHLines));
}

void PlotWindow::rebuildSeriesRows()
{
    // 清空整个列表（包括之前插入的占位提示）
    while (QLayoutItem *item = m_seriesLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    m_rows.clear();

    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    for (const PlotClient::SeriesData &series : m_client->series()) {
        SeriesRow row;
        row.widget = new QWidget(m_seriesHost);

        row.visible = new QCheckBox(row.widget);
        row.visible->setChecked(true);
        row.visible->setToolTip(tr("Show / hide this curve"));

        row.name = new QLabel(series.title.isEmpty() ? tr("Series %1").arg(series.id) : series.title,
                              row.widget);
        row.name->setMinimumWidth(56);
        row.name->setStyleSheet(QStringLiteral("color: #202020; background: transparent;"));

        row.value = new QLabel(QStringLiteral("--"), row.widget);
        row.value->setFont(fixedFont);
        row.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.value->setMinimumWidth(58);
        row.value->setStyleSheet(QStringLiteral("color: #0b6b3a; background: transparent;"));

        row.chip = new QToolButton(row.widget);
        row.chip->setFixedSize(16, 16);
        row.chip->setCursor(Qt::PointingHandCursor);
        row.chip->setToolTip(tr("Click to change this curve's colour"));
        row.chip->setStyleSheet(chipStyle(seriesDisplayColor(series)));

        auto *layout = new QHBoxLayout(row.widget);
        layout->setContentsMargins(2, 3, 2, 3);
        layout->setSpacing(6);
        layout->addWidget(row.visible);
        layout->addWidget(row.name, 1);
        layout->addWidget(row.value);
        layout->addWidget(row.chip);

        m_seriesLayout->addWidget(row.widget);
        m_rows.insert(series.id, row);

        const quint32 id = series.id;
        connect(row.visible, &QCheckBox::toggled, this, [this, id](bool on) {
            m_canvas->setSeriesHidden(id, !on);
        });
        connect(row.chip, &QToolButton::clicked, this, [this, id] {
            changeSeriesColor(id);
        });
    }

    if (m_rows.isEmpty()) {
        auto *placeholder = new QLabel(tr("No series in this group"), m_seriesHost);
        placeholder->setObjectName(QStringLiteral("plotHint"));
        placeholder->setAlignment(Qt::AlignCenter);
        m_seriesLayout->addWidget(placeholder);
    }

    refreshTitle();
    refreshValues();
}

void PlotWindow::refreshValues()
{
    for (const PlotClient::SeriesData &series : m_client->series()) {
        const auto it = m_rows.constFind(series.id);
        if (it == m_rows.constEnd() || !it->value)
            continue;
        it->value->setText(series.samples.isEmpty() ? QStringLiteral("--")
                                                  : QString::number(double(series.samples.lastValue()),
                                                                    'g', 6));
    }

    const QString status =
            tr("Index: %1 - %2    Spacing: %3 px    Sampling: 1/%4    Samples: %5")
                    .arg(qRound64(m_canvas->viewMin()))
                    .arg(qRound64(m_canvas->viewMax()))
                    .arg(m_canvas->pixelSpacing(), 0, 'f', 2)
                    .arg(m_canvas->stride())
                    .arg(m_client->totalSamples());
    const qint64 hidden = m_canvas->hiddenRight();
    m_statusLabel->setText(hidden > 0 ? status + tr("    (+%1 new)").arg(hidden) : status);
}

void PlotWindow::refreshStatus()
{
    refreshTitle();
    // 没有数据时画布中央显示连接状态文字，状态一变就要重绘
    m_canvasDirty = true;
}

void PlotWindow::changeSeriesColor(quint32 seriesId)
{
    PlotClient::SeriesData *series = m_client->findSeries(seriesId);
    if (!series)
        return;
    const QColor picked = QColorDialog::getColor(seriesDisplayColor(*series), this, tr("Curve colour"));
    if (!picked.isValid())
        return;
    m_client->setSeriesColor(seriesId, picked);
    const auto row = m_rows.constFind(seriesId);
    if (row != m_rows.constEnd() && row->chip)
        row->chip->setStyleSheet(chipStyle(picked));
    m_canvas->update();
}

void PlotWindow::fitView()
{
    m_canvas->fit();
    // 暂停时铺满后画面是冻结的（Auto X 不会勾上），复选框以画布的实际状态为准
    const QSignalBlocker blockerX(m_autoXCheck);
    m_autoXCheck->setChecked(m_canvas->autoFitX());
    const QSignalBlocker blockerY(m_autoYCheck);
    m_autoYCheck->setChecked(m_canvas->autoY());
    m_canvasDirty = true;
}

void PlotWindow::setPaused(bool paused)
{
    m_pauseButton->setText(paused ? tr("Resume") : tr("Pause"));
    m_pauseButton->setIcon(paused ? PlotIcons::play(QColor(0xc0, 0x54, 0x20))
                                  : PlotIcons::pause(Qt::white));
    m_canvas->setPaused(paused);
    // 测量只在暂停时可用；恢复时画布会自动退出测量，复选框跟着取消
    m_measureCheck->setEnabled(paused);
    m_measureYCheck->setEnabled(paused);
    if (!paused) {
        m_measureCheck->setChecked(false);
        m_measureYCheck->setChecked(false);
    }
    m_canvasDirty = true;
}

void PlotWindow::applySuggestedPosition()
{
    if (m_positionApplied || !m_client->hasSuggestedPosition())
        return;
    if (m_client->state() != PlotClient::State::Connected)
        return;
    m_positionApplied = true;
    move(m_client->suggestedPosition());
}

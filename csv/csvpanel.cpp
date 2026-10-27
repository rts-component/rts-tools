#include "csv/csvpanel.h"

#include "csv/csvgroupdialog.h"
#include "csv/csvicons.h"
#include "csv/csvrowlist.h"
#include "plot/server/plotlauncher.h"
#include "plot/server/plotserver.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kMaxBufferBytes = 64 * 1024; // 一直没有换行时最多留这么多字节
constexpr int kUpdateIntervalMs = 80;      // 合并高频数据，避免每行都刷界面

// 曲线配色（同一系列在重启后保持同色：用 key 的稳定哈希取模）
const QColor kSeriesColors[] = {
    QColor(0x1e, 0x88, 0xe5), QColor(0xe5, 0x39, 0x35), QColor(0x43, 0xa0, 0x47),
    QColor(0xfb, 0x8c, 0x00), QColor(0x8e, 0x24, 0xaa), QColor(0x00, 0x89, 0x7b),
    QColor(0x6d, 0x4c, 0x41), QColor(0x54, 0x6e, 0x7a),
};

quint32 stableHash(const QString &text)
{
    quint32 hash = 2166136261u; // FNV-1a，避免 qHash 的进程内随机种子导致颜色跳动
    for (const QChar ch : text) {
        hash ^= quint32(ch.unicode());
        hash *= 16777619u;
    }
    return hash;
}

QColor colorForKey(const QString &key)
{
    const int count = int(sizeof(kSeriesColors) / sizeof(kSeriesColors[0]));
    return kSeriesColors[stableHash(key) % quint32(count)];
}

QString curveKeyFor(const CsvGroup &group, int fieldIndex)
{
    const QString id = group.id.isEmpty() ? QStringLiteral("nogroup") : group.id.left(8);
    return id + QLatin1Char('f') + QString::number(fieldIndex);
}

} // namespace

CsvPanel::CsvPanel(QWidget *parent)
    : QWidget(parent)
    , m_list(new CsvRowList(this))
    , m_placeholder(new QLabel(this))
    , m_parseCheckBox(new QCheckBox(this))
    , m_statusLabel(new QLabel(this))
    , m_copyButton(new QPushButton(tr("Copy"), this))
    , m_addButton(new QPushButton(QStringLiteral("+"), this))
    , m_removeButton(new QPushButton(QStringLiteral("\u2212"), this))
    , m_upButton(new QPushButton(QStringLiteral("\u25b2"), this))
    , m_downButton(new QPushButton(QStringLiteral("\u25bc"), this))
{
    m_plotServer = new PlotServer(this);
    m_plotLauncher = new PlotLauncher(m_plotServer, this);
    connect(m_plotLauncher, &PlotLauncher::launchFailed, this, [this](const QString &message) {
        // 构造期间不弹窗，等事件循环起来再提示
        QTimer::singleShot(0, this, [this, message] {
            QMessageBox::warning(this, tr("Curve"), message);
        });
    });
    // 图表窗口被关掉（或崩溃后不再重启）：这一行的 Curve 勾选也要取消
    connect(m_plotLauncher, &PlotLauncher::viewerClosed, this, [this](const QString &groupKey) {
        for (int index = 0; index < m_config.groups.size(); ++index) {
            CsvGroup &group = m_config.groups[index];
            if (group.id != groupKey)
                continue;
            if (!group.curve)
                return;
            group.curve = false;
            saveConfig();
            m_list->setCurveChecked(index, false);
            syncCurveSeries();
            return;
        }
    });
    // 用户在曲线窗口里改了颜色：写回配置并持久化，两个窗口保持一致
    connect(m_plotServer, &PlotServer::seriesColorChanged, this,
            [this](const QString &seriesKey, const QColor &color) {
                for (int groupIndex = 0; groupIndex < m_config.groups.size(); ++groupIndex) {
                    CsvGroup &group = m_config.groups[groupIndex];
                    for (int fieldIndex = 0; fieldIndex < group.fields.size(); ++fieldIndex) {
                        if (curveKeyFor(group, fieldIndex) != seriesKey)
                            continue;
                        group.fields[fieldIndex].color = color.name();
                        saveConfig();
                        return;
                    }
                }
            });
    setObjectName(QStringLiteral("csvPanel"));
    setStyleSheet(QStringLiteral(
            "QLabel#csvPlaceholder { color: #8a8a8a; }"
            "QPushButton#csvToolButton { min-height: 0px; padding: 1px 6px; }"));

    // 列表头（对应参考图的 No. / 数据展示面板 / 曲线）
    m_headerIndex = new QLabel(this);
    m_headerPanel = new QLabel(this);
    m_headerCurve = new QLabel(this);
    m_headerPanel->setAlignment(Qt::AlignCenter);
    m_headerIndex->setMinimumWidth(34);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(8, 2, 8, 2);
    header->setSpacing(6);
    header->addWidget(m_headerIndex);
    header->addWidget(m_headerPanel, 1);
    header->addWidget(m_headerCurve);

    m_placeholder->setObjectName(QStringLiteral("csvPlaceholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);

    m_statusLabel->setObjectName(QStringLiteral("csvStatus"));
    for (QPushButton *button : {m_copyButton, m_addButton, m_removeButton, m_upButton, m_downButton}) {
        button->setObjectName(QStringLiteral("csvToolButton"));
        button->setCursor(Qt::PointingHandCursor);
    }
    m_copyButton->setText(QString());
    m_copyButton->setIcon(CsvIcons::copy());
    m_copyButton->setIconSize(QSize(14, 14));
    m_copyButton->setToolTip(tr("Duplicate the selected row"));
    m_addButton->setToolTip(tr("Add a new row"));
    m_removeButton->setToolTip(tr("Delete the selected row"));
    m_upButton->setToolTip(tr("Move the selected row up"));
    m_downButton->setToolTip(tr("Move the selected row down"));
    for (QPushButton *button : {m_copyButton, m_addButton, m_removeButton, m_upButton, m_downButton})
        button->setFixedWidth(28);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(2, 0, 2, 0);
    footer->setSpacing(4);
    footer->addWidget(m_parseCheckBox);
    footer->addStretch();
    footer->addWidget(m_statusLabel);
    footer->addSpacing(8);
    footer->addWidget(m_copyButton);
    footer->addWidget(m_addButton);
    footer->addWidget(m_removeButton);
    footer->addWidget(m_upButton);
    footer->addWidget(m_downButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(header);
    layout->addWidget(m_placeholder);
    layout->addWidget(m_list, 1);
    layout->addLayout(footer);

    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(kUpdateIntervalMs);
    connect(m_updateTimer, &QTimer::timeout, this, &CsvPanel::applyPendingValues);

    connect(m_parseCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        m_buffer.clear();
        m_config.enabled = checked;
        updateStatus();
        saveConfig();
    });
    connect(m_addButton, &QPushButton::clicked, this, &CsvPanel::addGroup);
    connect(m_copyButton, &QPushButton::clicked, this, &CsvPanel::copySelectedGroup);
    connect(m_removeButton, &QPushButton::clicked, this, &CsvPanel::removeSelectedGroup);
    connect(m_upButton, &QPushButton::clicked, this, [this] { moveSelectedGroup(-1); });
    connect(m_downButton, &QPushButton::clicked, this, [this] { moveSelectedGroup(1); });

    connect(m_list, &CsvRowList::selectRequested, this, &CsvPanel::selectGroup);
    connect(m_list, &CsvRowList::editRequested, this, &CsvPanel::editGroup);
    connect(m_list, &CsvRowList::menuRequested, this, &CsvPanel::showGroupMenu);
    connect(m_list, &CsvRowList::enabledToggled, this, [this](int groupIndex, bool on) {
        if (groupIndex < 0 || groupIndex >= m_config.groups.size())
            return;
        m_config.groups[groupIndex].enabled = on;
        saveConfig();
        syncCurveSeries(); // 关掉某一行时，它的曲线也不该再画
    });
    connect(m_list, &CsvRowList::curveToggled, this, [this](int groupIndex, bool on) {
        if (groupIndex < 0 || groupIndex >= m_config.groups.size())
            return;

        CsvGroup &group = m_config.groups[groupIndex];
        const bool anyPlotted = std::any_of(
                group.fields.cbegin(), group.fields.cend(),
                [](const CsvField &field) { return field.numeric && field.curve; });
        if (on && !anyPlotted) {
            // 这一行还没有选中要画的数据：不允许打勾，勾选框退回未选中并说明原因
            group.curve = false;
            saveConfig();
            m_list->setGroup(groupIndex, group);
            syncCurveSeries();
            showHint(tr("No column selected for this row yet - click the curve icon of a "
                        "numeric column to pick the data."));
            return;
        }

        group.curve = on;
        saveConfig();
        syncCurveSeries();
    });
    connect(m_list, &CsvRowList::fieldCurveToggled, this,
            [this](int groupIndex, int fieldIndex, bool on) {
                if (groupIndex < 0 || groupIndex >= m_config.groups.size() || fieldIndex < 0
                    || fieldIndex >= m_config.groups.at(groupIndex).fields.size())
                    return;

                CsvGroup &group = m_config.groups[groupIndex];
                group.fields[fieldIndex].curve = on;
                saveConfig();
                syncCurveSeries();

                if (on && !group.curve) {
                    showHint(tr("Tick Curve of this row first - the row decides whether its "
                                "columns are drawn."));
                }
            });

    m_config = loadCsvParseConfig();
    bool assignedIds = false;
    for (CsvGroup &group : m_config.groups) {
        if (group.id.isEmpty()) {
            group.id = newGroupId(); // 旧配置没有 id：补一个并落盘，保证曲线系列名稳定
            assignedIds = true;
        }
    }
    if (m_config.groups.isEmpty()) {
        CsvGroup group;
        group.id = newGroupId();
        m_config.groups.append(group);
        assignedIds = true;
    }

    // 行名：空名或旧的默认名 "Unnamed" 统一补成 Unnamed1、Unnamed2 ……
    bool namedGroups = false;
    for (CsvGroup &group : m_config.groups) {
        if (group.name.trimmed().isEmpty()
            || group.name.compare(QStringLiteral("Unnamed"), Qt::CaseInsensitive) == 0) {
            group.name.clear();
            namedGroups = true;
        }
    }
    if (namedGroups) {
        for (CsvGroup &group : m_config.groups) {
            if (group.name.isEmpty())
                group.name = nextAutoGroupName();
        }
    }

    if (assignedIds || namedGroups)
        saveConfig();
    m_parseCheckBox->setChecked(m_config.enabled);

    rebuildGroups();
    selectGroup(m_config.groups.isEmpty() ? -1 : 0);
    retranslateUi();
}

void CsvPanel::feed(const QByteArray &bytes)
{
    if (bytes.isEmpty() || !m_parseCheckBox->isChecked())
        return;

    m_buffer.append(bytes);
    if (m_buffer.size() > kMaxBufferBytes)
        m_buffer.remove(0, m_buffer.size() - kMaxBufferBytes);

    int start = 0;
    while (true) {
        const int newline = m_buffer.indexOf('\n', start);
        if (newline < 0)
            break;
        QString line = QString::fromLocal8Bit(m_buffer.mid(start, newline - start));
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        start = newline + 1;
        processLine(line);
    }

    if (start > 0)
        m_buffer.remove(0, start);
    updateStatus();
}

void CsvPanel::resetCounters()
{
    m_parsedLines = 0;
    m_skippedLines = 0;
    updateStatus();
}

void CsvPanel::clearData()
{
    m_buffer.clear();
    for (int index = 0; index < m_pendingDirty.size(); ++index)
        m_pendingDirty[index] = false;
    m_parsedLines = 0;
    m_skippedLines = 0;
    for (QStringList &values : m_values)
        values.clear();
    for (QStringList &values : m_pendingValues)
        values.clear();
    m_list->clearValues();
    updateStatus();
}

void CsvPanel::processLine(const QString &line)
{
    if (line.trimmed().isEmpty())
        return;

    bool matched = false;
    for (int index = 0; index < m_config.groups.size(); ++index) {
        CsvGroup &group = m_config.groups[index];
        if (!group.enabled || group.fields.isEmpty())
            continue; // 没配置列名的行不参与解析

        const QStringList fields = splitCsvLine(line, decodeSeparator(group.separator));
        if (!looksLikeCsvRecord(fields))
            continue;

        // 列数匹配才更新这一行
        if (fields.size() != group.fields.size())
            continue;

        // 声明为数字的列必须是合法数字，否则整条视为乱码丢弃
        bool valid = true;
        for (int field = 0; field < group.fields.size(); ++field) {
            if (group.fields.at(field).numeric && !isNumericValue(fields.at(field))) {
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;

        matched = true;
        m_values[index] = fields;
        m_pendingValues[index] = fields;
        m_pendingDirty[index] = true;

        // 曲线：同一条记录共用一个时间戳，各字段按自增 index 进入对应系列的缓冲
        if (index < m_curveKeys.size()) {
            const QStringList &keys = m_curveKeys.at(index);
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            for (int field = 0; field < keys.size() && field < fields.size(); ++field) {
                if (keys.at(field).isEmpty())
                    continue;
                m_plotServer->append(keys.at(field), fields.at(field).toDouble(), now);
            }
        }

        ++m_parsedLines;
    }

    if (!matched)
        ++m_skippedLines;

    if (matched && !m_updateTimer->isActive())
        m_updateTimer->start();
    updateStatus();
}

void CsvPanel::applyPendingValues()
{
    for (int index = 0; index < m_list->count() && index < m_pendingDirty.size(); ++index) {
        if (!m_pendingDirty.at(index))
            continue;
        m_pendingDirty[index] = false;
        m_list->setValues(index, m_pendingValues.at(index));
    }
}

void CsvPanel::rebuildGroups()
{
    // 增删/移动行时，其余行已经显示的值不能被清掉：按行 id 把旧的值带到新位置
    struct Saved { QStringList values, pending; bool dirty = false; };
    QHash<QString, Saved> saved;
    for (int index = 0; index < m_rowIds.size(); ++index)
        saved.insert(m_rowIds.at(index), {m_values.at(index), m_pendingValues.at(index),
                                          m_pendingDirty.at(index)});

    m_rowIds.clear();
    m_values.clear();
    m_pendingValues.clear();
    m_pendingDirty.clear();
    for (const CsvGroup &group : std::as_const(m_config.groups)) {
        const Saved item = saved.value(group.id);
        m_rowIds.append(group.id);
        m_values.append(item.values);
        m_pendingValues.append(item.pending);
        m_pendingDirty.append(item.dirty);
    }

    m_list->setGroups(m_config.groups);
    m_placeholder->setVisible(m_list->count() == 0);
    syncCurveSeries();
}

void CsvPanel::refreshGroup(int index)
{
    if (index < 0 || index >= m_list->count())
        return;
    m_list->setGroup(index, m_config.groups.at(index));
    syncCurveSeries();
}

void CsvPanel::selectGroup(int index)
{
    if (index < 0 || index >= m_list->count())
        return;
    m_selectedIndex = index;
    m_list->setSelected(index);
}

void CsvPanel::addGroup()
{
    CsvGroup group;
    group.id = newGroupId();
    group.name = nextAutoGroupName();
    m_config.groups.append(group);
    rebuildGroups();
    selectGroup(m_config.groups.size() - 1);
    saveConfig();
}

void CsvPanel::copySelectedGroup()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= m_config.groups.size())
        return;
    CsvGroup copy = m_config.groups.at(m_selectedIndex);
    copy.id = newGroupId(); // 副本是新的系列，不能沿用原 id
    copy.name = uniqueGroupName(tr("%1 copy").arg(copy.name));
    m_config.groups.insert(m_selectedIndex + 1, copy);
    rebuildGroups();
    selectGroup(m_selectedIndex + 1);
    saveConfig();
}

void CsvPanel::removeSelectedGroup()
{
    if (m_selectedIndex < 0 || m_selectedIndex >= m_config.groups.size())
        return;
    m_config.groups.removeAt(m_selectedIndex);
    rebuildGroups();

    if (m_config.groups.isEmpty())
        selectGroup(-1);
    else
        selectGroup(qMin(m_selectedIndex, int(m_config.groups.size()) - 1));

    saveConfig();
}

void CsvPanel::moveSelectedGroup(int offset)
{
    const int from = m_selectedIndex;
    const int to = from + offset;
    if (from < 0 || to < 0 || to >= m_config.groups.size())
        return;

    m_config.groups.move(from, to);
    rebuildGroups();
    selectGroup(to);
    saveConfig();
}

void CsvPanel::editGroup(int index)
{
    if (index < 0 || index >= m_config.groups.size())
        return;

    if (m_config.groups.at(index).curve) {
        showHint(tr("Close the curve window of this row before editing it."));
        return;
    }

    CsvGroupDialog dialog(m_config.groups.at(index), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_config.groups[index] = dialog.group();
    refreshGroup(index);
    selectGroup(index);
    saveConfig();
}

void CsvPanel::showGroupMenu(int index, const QPoint &globalPos)
{
    selectGroup(index);

    QMenu menu(this);
    menu.addAction(tr("Edit..."), this, [this, index] { editGroup(index); });
    menu.addAction(tr("Duplicate"), this, &CsvPanel::copySelectedGroup);
    menu.addSeparator();
    menu.addAction(tr("Move up"), this, [this] { moveSelectedGroup(-1); });
    menu.addAction(tr("Move down"), this, [this] { moveSelectedGroup(1); });
    menu.addSeparator();
    menu.addAction(tr("Delete"), this, &CsvPanel::removeSelectedGroup);
    menu.exec(globalPos);
}

void CsvPanel::retranslateUi()
{
    m_headerIndex->setText(tr("No."));
    m_headerPanel->setText(tr("Data panel"));
    m_headerCurve->setText(tr("Curve"));

    m_placeholder->setText(tr("No data rows yet. Use + to add one."));
    m_parseCheckBox->setText(tr("Start parsing"));
    m_copyButton->setToolTip(tr("Duplicate the selected row"));
    m_addButton->setToolTip(tr("Add a new row"));
    m_removeButton->setToolTip(tr("Delete the selected row"));
    m_upButton->setToolTip(tr("Move the selected row up"));
    m_downButton->setToolTip(tr("Move the selected row down"));

    updateStatus();
    // 行卡片上的提示文字是绘制时取 tr()，重绘一次就会换成新语言
    m_list->viewport()->update();
}

void CsvPanel::updateStatus()
{
    if (!m_parseCheckBox->isChecked()) {
        m_statusLabel->setText(tr("Parsing is off"));
        return;
    }
    m_statusLabel->setText(
            tr("%1 line(s) parsed, %2 skipped").arg(m_parsedLines).arg(m_skippedLines));
}

void CsvPanel::saveConfig()
{
    saveCsvParseConfig(m_config);
}

void CsvPanel::rebuildCurveKeys()
{
    m_curveKeys.clear();
    for (const CsvGroup &group : std::as_const(m_config.groups)) {
        QStringList keys;
        keys.reserve(group.fields.size());
        for (int field = 0; field < group.fields.size(); ++field) {
            const CsvField &definition = group.fields.at(field);
            const bool plotted = group.enabled && group.curve && definition.numeric
                    && definition.curve;
            keys.append(plotted ? curveKeyFor(group, field) : QString());
        }
        m_curveKeys.append(keys);
    }
}

void CsvPanel::syncCurveSeries()
{
    rebuildCurveKeys();

    QVector<PlotServer::GroupDefinition> groups;
    for (int groupIndex = 0; groupIndex < m_config.groups.size(); ++groupIndex) {
        const CsvGroup &group = m_config.groups.at(groupIndex);
        const QStringList &keys = m_curveKeys.at(groupIndex);

        PlotServer::GroupDefinition definition;
        definition.key = group.id.isEmpty() ? QStringLiteral("nogroup") : group.id;
        definition.title = group.name.isEmpty() ? tr("Row %1").arg(groupIndex + 1) : group.name;

        for (int field = 0; field < keys.size(); ++field) {
            if (keys.at(field).isEmpty())
                continue;

            const CsvField &csvField = group.fields.at(field);
            PlotServer::SeriesDefinition series;
            series.key = keys.at(field);
            series.title = csvField.name.isEmpty() ? tr("Column %1").arg(field + 1)
                                                   : csvField.name;
            series.color = csvField.color.isEmpty() ? colorForKey(series.key)
                                                    : QColor(csvField.color);
            if (!series.color.isValid())
                series.color = colorForKey(series.key);
            definition.series.append(series);
        }

        if (definition.series.isEmpty())
            continue; // 该行没有要画的列：不留空窗口
        groups.append(definition);
    }

    m_plotServer->syncSeries(groups);
    m_plotLauncher->syncSeries(groups);
}

int CsvPanel::curveSeriesCount() const
{
    int count = 0;
    for (const QStringList &keys : m_curveKeys) {
        for (const QString &key : keys) {
            if (!key.isEmpty())
                ++count;
        }
    }
    return count;
}

void CsvPanel::showHint(const QString &text)
{
    if (!m_hint) {
        m_hint = new QLabel(this);
        m_hint->setObjectName(QStringLiteral("csvHint"));
        // 只做提示：不接收鼠标事件，不会挡住下面的行/按钮
        m_hint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_hint->setStyleSheet(QStringLiteral(
                "QLabel#csvHint { background: #fff4e5; color: #8a5300;"
                " border: 1px solid #f0c98a; border-radius: 4px; padding: 6px 10px; }"));

        m_hintTimer = new QTimer(this);
        m_hintTimer->setSingleShot(true);
        m_hintTimer->setInterval(5000);
        connect(m_hintTimer, &QTimer::timeout, this, [this] {
            if (m_hint)
                m_hint->hide();
        });
    }

    m_hint->setText(text);
    m_hint->adjustSize();
    placeHint();
    m_hint->raise();
    m_hint->show();
    m_hintTimer->start();
}

void CsvPanel::placeHint()
{
    if (!m_hint)
        return;
    const QSize hint = m_hint->sizeHint();
    m_hint->resize(qMin(hint.width(), qMax(240, width() - 24)), hint.height());
    // 贴在底部按钮行上方，避免挡住列表表头和行的 Check 框
    m_hint->move(qMax(8, (width() - m_hint->width()) / 2),
                 qMax(8, height() - m_hint->height() - 46));
}

void CsvPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_hint && m_hint->isVisible())
        placeHint();
}

bool CsvPanel::isGroupNameUsed(const QString &name) const
{
    for (const CsvGroup &group : m_config.groups) {
        if (group.name.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString CsvPanel::nextAutoGroupName() const
{
    for (int index = 1; index < 10000; ++index) {
        const QString candidate = tr("Unnamed%1").arg(index);
        if (!isGroupNameUsed(candidate))
            return candidate;
    }
    return tr("Unnamed");
}

QString CsvPanel::uniqueGroupName(const QString &base) const
{
    const QString trimmed = base.trimmed();
    if (!trimmed.isEmpty() && !isGroupNameUsed(trimmed))
        return trimmed;
    const QString stem = trimmed.isEmpty() ? tr("Unnamed") : trimmed;
    for (int index = 2; index < 10000; ++index) {
        const QString candidate = QStringLiteral("%1 %2").arg(stem).arg(index);
        if (!isGroupNameUsed(candidate))
            return candidate;
    }
    return stem;
}






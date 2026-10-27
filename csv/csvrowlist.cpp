#include "csv/csvrowlist.h"

#include "csv/csvicons.h"

#include <QFontMetrics>
#include <QHash>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolTip>

namespace {

constexpr int kOuterMargin = 2;
constexpr int kRowSpacing = 6;
constexpr int kPadLeft = 6;
constexpr int kPadTop = 4;
constexpr int kPadBottom = 6;
constexpr int kHeaderHeight = 22;
constexpr int kHeaderSpacing = 6;
constexpr int kCardsGap = 5;
constexpr int kCardWidth = 118;
constexpr int kCardHeight = 62;
constexpr int kCardSpacing = 4;
constexpr int kCheckSize = 16;
constexpr int kIconSize = 14;

const QColor kRowNormalBg(0xe9, 0xee, 0xf5);
const QColor kRowNormalBorder(0xd3, 0xdb, 0xe5);
const QColor kRowAlternateBg(0xff, 0xff, 0xff);
const QColor kRowAlternateBorder(0xe2, 0xe8, 0xf0);
const QColor kRowSelectedBg(0xdc, 0xec, 0xfb);
const QColor kAccent(0x00, 0x9b, 0xdb);
const QColor kCardBg(0x5b, 0x8f, 0xbf);
const QColor kValueColor(0x6f, 0xf0, 0xc4);
const QColor kHintColor(0x8b, 0x95, 0xa1);
const QString kNoValue = QStringLiteral("—");

const QIcon &chartIcon(bool on)
{
    static const QIcon off = CsvIcons::chart(false);
    static const QIcon onIcon = CsvIcons::chart(true);
    return on ? onIcon : off;
}

const QIcon &pencilIcon()
{
    static const QIcon icon = CsvIcons::pencil();
    return icon;
}

const QIcon &menuIcon()
{
    static const QIcon icon = CsvIcons::menu();
    return icon;
}

void drawIcon(QPainter *painter, const QIcon &icon, const QRect &rect)
{
    const QRect target(rect.center().x() - kIconSize / 2, rect.center().y() - kIconSize / 2,
                       kIconSize, kIconSize);
    icon.paint(painter, target);
}

} // namespace

CsvRowList::CsvRowList(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false); // paintEvent fills everything itself
    verticalScrollBar()->setSingleStep(24);
}

void CsvRowList::setGroups(const QVector<CsvGroup> &groups)
{
    // Rows keep the values they were showing: adding, deleting or moving a row must not
    // blank the others. Rows are matched by id; a changed column count invalidates them.
    QHash<QString, Row> old;
    for (const Row &row : std::as_const(m_rows)) {
        if (!row.group.id.isEmpty())
            old.insert(row.group.id, row);
    }

    m_rows.clear();
    m_rows.reserve(groups.size());
    for (const CsvGroup &group : groups) {
        Row row;
        row.group = group;
        const auto it = old.constFind(group.id);
        if (it != old.constEnd() && it->group.fields.size() == group.fields.size())
            row.values = it->values;
        m_rows.append(row);
    }

    if (m_selected >= m_rows.size())
        m_selected = -1;
    m_hover = Hit();
    m_pressed = Hit();
    relayout();
    viewport()->update();
}

void CsvRowList::setGroup(int index, const CsvGroup &group)
{
    if (index < 0 || index >= m_rows.size())
        return;

    Row &row = m_rows[index];
    if (row.group.fields.size() != group.fields.size())
        row.values.clear();
    row.group = group;
    relayout();
    viewport()->update();
}

void CsvRowList::setSelected(int index)
{
    if (index == m_selected)
        return;
    m_selected = index;
    viewport()->update();
}

void CsvRowList::setCurveChecked(int index, bool checked)
{
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows[index].group.curve = checked;
    invalidateRow(index);
}

void CsvRowList::setValues(int index, const QStringList &values)
{
    if (index < 0 || index >= m_rows.size() || m_rows.at(index).values == values)
        return;
    m_rows[index].values = values;
    invalidateRow(index);
}

void CsvRowList::clearValues()
{
    for (Row &row : m_rows)
        row.values.clear();
    viewport()->update();
}

int CsvRowList::rowWidth() const
{
    return qMax(0, viewport()->width() - 2 * kOuterMargin);
}

int CsvRowList::cardsPerRow(int rowWidth) const
{
    return qMax(1, (rowWidth - 12) / (kCardWidth + kCardSpacing));
}

int CsvRowList::rowHeight(const Row &row, int rowWidth) const
{
    int cards = 0;
    if (row.group.fields.isEmpty()) {
        cards = fontMetrics().height();
    } else {
        const int perRow = cardsPerRow(rowWidth);
        const int lines = (row.group.fields.size() + perRow - 1) / perRow;
        cards = lines * kCardHeight + (lines - 1) * kCardSpacing;
    }
    return kPadTop + kHeaderHeight + kCardsGap + cards + kPadBottom;
}

void CsvRowList::relayout()
{
    const int width = rowWidth();
    int top = kOuterMargin;
    for (Row &row : m_rows) {
        row.top = top;
        row.height = rowHeight(row, width);
        top += row.height + kRowSpacing;
    }
    m_contentHeight = m_rows.isEmpty() ? 0 : top - kRowSpacing + kOuterMargin;

    QScrollBar *bar = verticalScrollBar();
    bar->setPageStep(viewport()->height());
    bar->setRange(0, qMax(0, m_contentHeight - viewport()->height()));
}

CsvRowList::RowGeometry CsvRowList::geometryFor(int rowWidth) const
{
    RowGeometry g;
    const int centerY = kPadTop + kHeaderHeight / 2;
    int x = kPadLeft;
    g.enabled = QRect(x, centerY - kCheckSize / 2, kCheckSize, kCheckSize);
    x = g.enabled.right() + 1 + kHeaderSpacing;
    g.index = QRect(x, centerY - 9, 18, 18);
    x = g.index.right() + 1 + kHeaderSpacing;
    g.edit = QRect(x, centerY - 10, 22, 20);
    x = g.edit.right() + 1 + kHeaderSpacing;

    g.menu = QRect(rowWidth - kPadLeft - 20, centerY - 10, 20, 20);
    g.curve = QRect(g.menu.left() - kHeaderSpacing - kCheckSize, centerY - kCheckSize / 2,
                    kCheckSize, kCheckSize);
    g.name = QRect(x, kPadTop, qMax(0, g.curve.left() - kHeaderSpacing - x), kHeaderHeight);

    g.cardsTop = kPadTop + kHeaderHeight + kCardsGap;
    g.cardsPerRow = cardsPerRow(rowWidth);
    return g;
}

QRect CsvRowList::rowRect(int index) const
{
    const Row &row = m_rows.at(index);
    return QRect(kOuterMargin, row.top, rowWidth(), row.height);
}

QRect CsvRowList::cardRect(int index, int card) const
{
    const RowGeometry g = geometryFor(rowWidth());
    Q_UNUSED(index)
    return QRect(kPadLeft + (card % g.cardsPerRow) * (kCardWidth + kCardSpacing),
                 g.cardsTop + (card / g.cardsPerRow) * (kCardHeight + kCardSpacing),
                 kCardWidth, kCardHeight);
}

QRect CsvRowList::fxRect(const QRect &card)
{
    return QRect(card.right() + 1 - 6 - 16 - 3 - 20, card.top() + 4, 20, 16);
}

QRect CsvRowList::cardCurveRect(const QRect &card)
{
    return QRect(card.right() + 1 - 6 - 16, card.top() + 4, 16, 16);
}

CsvRowList::Hit CsvRowList::hitTest(const QPoint &viewportPos) const
{
    Hit hit;
    const int y = viewportPos.y() + verticalScrollBar()->value();
    for (int i = 0; i < m_rows.size(); ++i) {
        const QRect rect = rowRect(i);
        if (!rect.contains(QPoint(viewportPos.x(), y)))
            continue;

        hit.row = i;
        const Row &row = m_rows.at(i);
        const QPoint local = QPoint(viewportPos.x(), y) - rect.topLeft();
        const RowGeometry g = geometryFor(rect.width());

        if (g.enabled.contains(local)) {
            hit.part = Part::Enabled;
        } else if (g.edit.contains(local) && !isLocked(row)) {
            hit.part = Part::Edit;
        } else if (g.curve.contains(local)) {
            hit.part = Part::CurveCheck;
        } else if (g.menu.contains(local)) {
            hit.part = Part::Menu;
        } else {
            for (int card = 0; card < row.group.fields.size(); ++card) {
                const QRect cr = cardRect(i, card);
                if (!cr.contains(local) || !row.group.fields.at(card).numeric)
                    continue;
                if (fxRect(cr).contains(local)) {
                    hit.part = Part::Fx;
                    hit.card = card;
                } else if (cardCurveRect(cr).contains(local) && !isLocked(row)) {
                    hit.part = Part::CardCurve;
                    hit.card = card;
                }
                break;
            }
        }
        return hit;
    }
    return hit;
}

QRect CsvRowList::partRect(const Hit &hit) const
{
    if (hit.row < 0 || hit.row >= m_rows.size())
        return QRect();

    const QRect base = rowRect(hit.row);
    const RowGeometry g = geometryFor(base.width());
    QRect local;
    switch (hit.part) {
    case Part::Enabled: local = g.enabled; break;
    case Part::Edit: local = g.edit; break;
    case Part::CurveCheck: local = g.curve; break;
    case Part::Menu: local = g.menu; break;
    case Part::Fx: local = fxRect(cardRect(hit.row, hit.card)); break;
    case Part::CardCurve: local = cardCurveRect(cardRect(hit.row, hit.card)); break;
    case Part::None: return QRect();
    }
    return local.translated(base.topLeft());
}

QString CsvRowList::toolTipFor(const Hit &hit) const
{
    switch (hit.part) {
    case Part::Enabled:
        return tr("Use this row");
    case Part::Edit:
        return tr("Edit name, separator and columns");
    case Part::CurveCheck:
        return tr("Show curve for this row (needs at least one column with the curve icon)");
    case Part::Menu:
        return tr("More actions");
    case Part::Fx:
        return tr("Formula conversion (not implemented yet)");
    case Part::CardCurve: {
        const Row &row = m_rows.at(hit.row);
        if (isLocked(row))
            return tr("Close the curve window to change the curves");
        return row.group.fields.at(hit.card).curve ? tr("Hide this field's curve")
                                                    : tr("Show this field on the curve");
    }
    case Part::None:
        break;
    }
    return QString();
}

void CsvRowList::invalidateRow(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    QRect rect = rowRect(index);
    rect.translate(0, -verticalScrollBar()->value());
    viewport()->update(rect.adjusted(-1, -1, 1, 1));
}

void CsvRowList::paintRow(QPainter *painter, int index, int width) const
{
    const Row &row = m_rows.at(index);
    const CsvGroup &group = row.group;
    const RowGeometry g = geometryFor(width);

    // Row background and border
    const bool selected = index == m_selected;
    QColor bg = index % 2 == 1 ? kRowAlternateBg : kRowNormalBg;
    QColor border = index % 2 == 1 ? kRowAlternateBorder : kRowNormalBorder;
    if (selected) {
        bg = kRowSelectedBg;
        border = kAccent;
    }
    painter->setPen(QPen(border, 1));
    painter->setBrush(bg);
    painter->drawRoundedRect(QRectF(0.5, 0.5, width - 1, row.height - 1), 4, 4);

    const bool locked = isLocked(row);

    // Enabled / curve checkboxes drawn by the style
    auto drawCheck = [&](const QRect &rect, bool checked) {
        QStyleOptionButton option;
        option.initFrom(this);
        option.rect = rect;
        option.state = QStyle::State_Enabled | (checked ? QStyle::State_On : QStyle::State_Off);
        style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, painter, this);
    };
    drawCheck(g.enabled, group.enabled);
    drawCheck(g.curve, group.curve);

    // Index badge
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0xe6, 0xea, 0xef));
    painter->drawRoundedRect(g.index, 3, 3);
    painter->setPen(QColor(0x4a, 0x55, 0x63));
    painter->drawText(g.index.adjusted(0, 0, -3, 0), Qt::AlignRight | Qt::AlignVCenter,
                      QString::number(index));

    // Edit button
    const bool editHover = m_hover.row == index && m_hover.part == Part::Edit;
    const bool editDown = m_pressed.row == index && m_pressed.part == Part::Edit && editHover;
    painter->setOpacity(locked ? 0.4 : 1.0);
    painter->setPen(QPen(editHover ? kAccent : QColor(0xcc, 0xd4, 0xdd), 1));
    painter->setBrush(editDown ? QColor(0xe3, 0xe9, 0xf0)
                               : editHover ? QColor(Qt::white) : QColor(0xf6, 0xf8, 0xfa));
    painter->drawRoundedRect(QRectF(g.edit).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    drawIcon(painter, pencilIcon(), g.edit);
    painter->setOpacity(1.0);

    // Menu button
    const bool menuHover = m_hover.row == index && m_hover.part == Part::Menu;
    const bool menuDown = m_pressed.row == index && m_pressed.part == Part::Menu && menuHover;
    if (menuHover) {
        painter->setPen(QPen(QColor(0xcf, 0xd6, 0xde), 1));
        painter->setBrush(menuDown ? QColor(0xd7, 0xe0, 0xea) : QColor(0xe3, 0xe9, 0xf0));
        painter->drawRoundedRect(QRectF(g.menu).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
    drawIcon(painter, menuIcon(), g.menu);

    // Name
    QFont nameFont = font();
    nameFont.setWeight(QFont::DemiBold);
    painter->setFont(nameFont);
    painter->setPen(palette().color(QPalette::Text));
    const QString name = group.name.isEmpty() ? tr("Unnamed") : group.name;
    painter->drawText(g.name, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, g.name.width()));
    painter->setFont(font());

    // Cards, or the hint for an unconfigured row
    if (group.fields.isEmpty()) {
        painter->setPen(kHintColor);
        painter->drawText(QRect(kPadLeft, g.cardsTop, width - 2 * kPadLeft, fontMetrics().height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          tr("No columns - click the pencil to configure"));
        return;
    }

    painter->setOpacity(group.enabled ? 1.0 : 0.45);
    QFont valueFont = font();
    valueFont.setPointSizeF(15);
    valueFont.setWeight(QFont::DemiBold);
    for (int card = 0; card < group.fields.size(); ++card) {
        const CsvField &field = group.fields.at(card);
        const QRect cr = cardRect(index, card);

        painter->setPen(Qt::NoPen);
        painter->setBrush(kCardBg);
        painter->drawRoundedRect(cr, 4, 4);

        const QRect curveRect = cardCurveRect(cr);
        const QRect fx = fxRect(cr);
        const int nameRight = field.numeric ? fx.left() - 3 : cr.right() + 1 - 6;
        const QRect nameRect(cr.left() + 6, cr.top() + 4, qMax(0, nameRight - cr.left() - 6), 16);
        painter->setFont(nameFont);
        painter->setPen(Qt::white);
        const QString fieldName = field.name.isEmpty() ? tr("Field %1").arg(card + 1) : field.name;
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(fieldName, Qt::ElideRight,
                                                            nameRect.width()));

        if (field.numeric) {
            // fx button
            painter->setPen(QPen(QColor(0xcf, 0xd6, 0xde), 1));
            painter->setBrush(QColor(0xee, 0xf3, 0xf7));
            painter->drawRoundedRect(QRectF(fx).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            QFont fxFont = font();
            fxFont.setItalic(true);
            painter->setFont(fxFont);
            painter->setPen(QColor(0x3d, 0x6f, 0x9e));
            painter->drawText(fx, Qt::AlignCenter, QStringLiteral("fx"));

            // Curve toggle
            const bool hover = m_hover.row == index && m_hover.part == Part::CardCurve
                    && m_hover.card == card;
            if (field.curve) {
                painter->setPen(QPen(Qt::white, 1));
                painter->setBrush(hover ? QColor(0xe8, 0xf4, 0xff) : QColor(Qt::white));
                painter->drawRoundedRect(QRectF(curveRect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            } else if (hover && !locked) {
                painter->setPen(QPen(QColor(255, 255, 255, 204), 1));
                painter->setBrush(QColor(255, 255, 255, 64));
                painter->drawRoundedRect(QRectF(curveRect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            }
            if (locked)
                painter->setOpacity((group.enabled ? 1.0 : 0.45) * 0.4);
            drawIcon(painter, chartIcon(field.curve), curveRect);
            painter->setOpacity(group.enabled ? 1.0 : 0.45);
        }

        const QString text = card < row.values.size() ? row.values.at(card) : kNoValue;
        painter->setFont(valueFont);
        painter->setPen(kValueColor);
        const QRect valueRect(cr.left() + 6, cr.top() + 4 + 16, cr.width() - 12,
                              cr.height() - 4 - 16 - 4);
        painter->drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(valueFont).elidedText(text, Qt::ElideRight,
                                                             valueRect.width()));
    }
    painter->setOpacity(1.0);
}

void CsvRowList::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(event->rect(), Qt::white);

    const int scroll = verticalScrollBar()->value();
    const int width = rowWidth();
    const QRect visible(0, scroll, viewport()->width(), viewport()->height());
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (row.top + row.height < visible.top() || row.top > visible.bottom())
            continue;
        painter.save();
        painter.translate(kOuterMargin, row.top - scroll);
        paintRow(&painter, i, width);
        painter.restore();
    }
}

void CsvRowList::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
    viewport()->update();
}

void CsvRowList::scrollContentsBy(int, int)
{
    viewport()->update();
}

void CsvRowList::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }

    m_pressed = hitTest(event->position().toPoint());
    if (m_pressed.row >= 0) {
        emit selectRequested(m_pressed.row);
        invalidateRow(m_pressed.row);
    }
}

void CsvRowList::mouseMoveEvent(QMouseEvent *event)
{
    const Hit hit = hitTest(event->position().toPoint());
    if (hit == m_hover)
        return;

    const int oldRow = m_hover.row;
    m_hover = hit;
    invalidateRow(oldRow);
    invalidateRow(hit.row);
    viewport()->setCursor(hit.part == Part::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

void CsvRowList::leaveEvent(QEvent *event)
{
    QAbstractScrollArea::leaveEvent(event);
    const int oldRow = m_hover.row;
    m_hover = Hit();
    invalidateRow(oldRow);
}

void CsvRowList::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }

    const Hit pressed = m_pressed;
    m_pressed = Hit();
    invalidateRow(pressed.row);
    if (pressed.part == Part::None || pressed != hitTest(event->position().toPoint()))
        return;

    const int index = pressed.row;
    Row &row = m_rows[index];
    switch (pressed.part) {
    case Part::Enabled:
        row.group.enabled = !row.group.enabled;
        invalidateRow(index);
        emit enabledToggled(index, row.group.enabled);
        break;
    case Part::CurveCheck:
        row.group.curve = !row.group.curve;
        invalidateRow(index);
        emit curveToggled(index, row.group.curve);
        break;
    case Part::Edit:
        emit editRequested(index);
        break;
    case Part::Menu: {
        const QRect rect = partRect(pressed).translated(0, -verticalScrollBar()->value());
        emit menuRequested(index, viewport()->mapToGlobal(rect.bottomLeft()));
        break;
    }
    case Part::Fx: {
        const QRect rect = partRect(pressed).translated(0, -verticalScrollBar()->value());
        QToolTip::showText(viewport()->mapToGlobal(rect.bottomLeft()),
                           tr("Formula conversion is not implemented yet"), viewport());
        break;
    }
    case Part::CardCurve: {
        CsvField &field = row.group.fields[pressed.card];
        field.curve = !field.curve;
        invalidateRow(index);
        emit fieldCurveToggled(index, pressed.card, field.curve);
        break;
    }
    case Part::None:
        break;
    }
}

bool CsvRowList::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const Hit hit = hitTest(help->pos());
        const QString text = toolTipFor(hit);
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QRect rect = partRect(hit);
            rect.translate(0, -verticalScrollBar()->value());
            QToolTip::showText(help->globalPos(), text, viewport(), rect);
        }
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

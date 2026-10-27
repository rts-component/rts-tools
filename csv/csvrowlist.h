#ifndef CSVROWLIST_H
#define CSVROWLIST_H

#include "csv/csvparser.h"

#include <QAbstractScrollArea>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QVector>

// CSV 面板里的“行列表”：每行 = 启用勾选 / 序号 / 编辑 / 名称 / 曲线勾选 / 菜单 + 字段卡片。
//
// 整个列表只有 viewport 这一块绘制面：所有行都在 paintEvent 里画出来，鼠标命中由本类自己判断。
// 这样增删行、滚动都不会创建/销毁/移动子控件，任何结构变化都是整块 viewport 重绘，
// 不会留下被删除行的残影（此前用 QScrollArea + 每行一个 QWidget 时，分数缩放下会残留细横线）。
class CsvRowList : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit CsvRowList(QWidget *parent = nullptr);

    int count() const { return m_rows.size(); }

    // 整体换一套行（保持滚动位置在合法范围内），已显示的值清空
    void setGroups(const QVector<CsvGroup> &groups);
    // 只更新一行的定义（卡片重建、值保留不了：与旧实现一致，值清空）
    void setGroup(int index, const CsvGroup &group);

    void setSelected(int index);
    int selected() const { return m_selected; }

    // 只改“曲线”勾选状态，不触发信号
    void setCurveChecked(int index, bool checked);

    void setValues(int index, const QStringList &values);
    void clearValues();

signals:
    void selectRequested(int index);
    void editRequested(int index);
    void enabledToggled(int index, bool on);
    void curveToggled(int index, bool on);
    void fieldCurveToggled(int index, int fieldIndex, bool on);
    void menuRequested(int index, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool viewportEvent(QEvent *event) override;

private:
    friend class TstCsvRowList;

    enum class Part { None, Enabled, Edit, CurveCheck, Menu, Fx, CardCurve };

    struct Row
    {
        CsvGroup group;
        QStringList values;
        int top = 0;    // 内容坐标
        int height = 0;
    };

    struct Hit
    {
        int row = -1;
        Part part = Part::None;
        int card = -1;

        bool operator==(const Hit &other) const
        {
            return row == other.row && part == other.part && card == other.card;
        }
        bool operator!=(const Hit &other) const { return !(*this == other); }
    };

    // 一行里各元素的位置（行内坐标，原点是行左上角）
    struct RowGeometry
    {
        QRect enabled, index, edit, name, curve, menu;
        int cardsTop = 0;
        int cardsPerRow = 1;
    };

    void relayout();
    int rowHeight(const Row &row, int rowWidth) const;
    int rowWidth() const;
    int cardsPerRow(int rowWidth) const;
    RowGeometry geometryFor(int rowWidth) const;
    QRect rowRect(int index) const; // 内容坐标
    QRect cardRect(int index, int card) const; // 行内坐标
    static QRect fxRect(const QRect &card);
    static QRect cardCurveRect(const QRect &card);

    bool isLocked(const Row &row) const { return row.group.curve; }
    Hit hitTest(const QPoint &viewportPos) const;
    QRect partRect(const Hit &hit) const; // 内容坐标
    QString toolTipFor(const Hit &hit) const;
    void invalidateRow(int index);

    void paintRow(QPainter *painter, int index, int width) const;

    QVector<Row> m_rows;
    int m_selected = -1;
    int m_contentHeight = 0;
    Hit m_hover;
    Hit m_pressed;
};

#endif // CSVROWLIST_H

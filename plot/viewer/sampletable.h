#ifndef SAMPLETABLE_H
#define SAMPLETABLE_H

#include <QVector>
#include <QtGlobal>

#include <map>

class SampleTable;

// 一组曲线共用的采样表：环形缓冲 + 按列存放。
// 一行 = 一次采样（index + 时间戳 + 每条曲线一个值）。index / 时间戳只存一份，
// 每条曲线只有自己的 float 列，所以 N 条曲线每行占 12 + 4N 字节（原来是 24N）。
// 各列的环形下标完全一致，丢弃最旧的行是 O(1)。index 单调递增，可以二分查找。
class SampleTable
{
public:
    struct Point
    {
        quint32 index = 0;
        qint64 timestampMs = 0;
        float value = 0.0f;
    };

    int size() const { return m_count; }
    bool isEmpty() const { return m_count == 0; }

    quint32 indexAt(int i) const { return m_index.at(slot(i)); }
    qint64 timestampAt(int i) const { return m_timestamp.at(slot(i)); }
    float valueAt(const QVector<float> &column, int i) const { return column.at(slot(i)); }

    // 新增一列（一条曲线）。已有的行补 0；返回的指针在这一列被删除前一直有效
    const QVector<float> *addColumn(quint32 id)
    {
        QVector<float> &column = m_columns[id];
        column = QVector<float>(m_index.size());
        return &column;
    }
    void removeColumn(quint32 id) { m_columns.erase(id); }

    // 追加一行。新行各列先沿用上一行的值，随后用 setValue 覆盖；返回新行的位置
    int appendRow(quint32 index, qint64 timestampMs)
    {
        if (m_count == m_index.size())
            grow();
        const int at = slot(m_count);
        for (auto &entry : m_columns) {
            QVector<float> &column = entry.second;
            column[at] = m_count > 0 ? column.at(slot(m_count - 1)) : 0.0f;
        }
        m_index[at] = index;
        m_timestamp[at] = timestampMs;
        return m_count++;
    }

    void setValue(quint32 id, int pos, float value)
    {
        const auto it = m_columns.find(id);
        if (it != m_columns.end())
            it->second[slot(pos)] = value;
    }

    // 精确查找 index 所在的行，没有返回 -1
    int find(quint32 index) const
    {
        if (m_count == 0)
            return -1;
        if (indexAt(m_count - 1) == index) // 同一行的其余曲线几乎总是命中最后一行
            return m_count - 1;
        const int at = lowerBound(index);
        return at < m_count && indexAt(at) == index ? at : -1;
    }

    void dropFront(int n)
    {
        n = qBound(0, n, m_count);
        if (n == 0)
            return;
        m_head = (m_head + n) & (m_index.size() - 1);
        m_count -= n;
    }

    void trimTo(int limit)
    {
        if (limit >= 0 && m_count > limit)
            dropFront(m_count - limit);
    }

    // 清空所有行并释放内存，列（曲线）本身保留
    void clear()
    {
        m_index = {};
        m_timestamp = {};
        for (auto &entry : m_columns)
            entry.second = {};
        m_head = 0;
        m_count = 0;
    }

    // 第一个 index >= x 的位置（没有则返回 size()）
    int lowerBound(double x) const
    {
        int low = 0;
        int high = m_count;
        while (low < high) {
            const int mid = low + (high - low) / 2;
            if (indexAt(mid) < x)
                low = mid + 1;
            else
                high = mid;
        }
        return low;
    }

    // 第一个 index > x 的位置（没有则返回 size()）
    int upperBound(double x) const
    {
        int low = 0;
        int high = m_count;
        while (low < high) {
            const int mid = low + (high - low) / 2;
            if (indexAt(mid) <= x)
                low = mid + 1;
            else
                high = mid;
        }
        return low;
    }

    // 离 index 最近的行（距离相同取靠前的）；表为空返回 -1
    int nearest(quint32 index) const
    {
        if (m_count == 0)
            return -1;
        int at = lowerBound(index);
        if (at == m_count)
            return m_count - 1;
        if (at > 0 && qAbs(qint64(indexAt(at - 1)) - qint64(index))
                              <= qAbs(qint64(indexAt(at)) - qint64(index)))
            --at;
        return at;
    }

private:
    int slot(int i) const { return (m_head + i) & (m_index.size() - 1); }

    void grow()
    {
        const int capacity = m_index.isEmpty() ? 1024 : m_index.size() * 2; // 始终是 2 的幂
        QVector<quint32> index(capacity);
        QVector<qint64> timestamp(capacity);
        for (int i = 0; i < m_count; ++i) {
            index[i] = indexAt(i);
            timestamp[i] = timestampAt(i);
        }
        for (auto &entry : m_columns) {
            QVector<float> &column = entry.second;
            QVector<float> grown(capacity);
            for (int i = 0; i < m_count && !column.isEmpty(); ++i)
                grown[i] = column.at(slot(i));
            column = std::move(grown);
        }
        m_index = std::move(index);
        m_timestamp = std::move(timestamp);
        m_head = 0;
    }

    QVector<quint32> m_index;
    QVector<qint64> m_timestamp;
    std::map<quint32, QVector<float>> m_columns; // 节点地址稳定，SampleView 可以直接持有指针
    int m_head = 0;
    int m_count = 0;
};

// 某一条曲线在 SampleTable 里的只读视图（一列），接口和按行遍历的容器类似
class SampleView
{
public:
    using Point = SampleTable::Point;

    SampleView() = default;
    SampleView(const SampleTable *table, const QVector<float> *column)
        : m_table(table), m_column(column) {}

    int size() const { return m_table ? m_table->size() : 0; }
    bool isEmpty() const { return size() == 0; }

    quint32 indexAt(int i) const { return m_table->indexAt(i); }
    qint64 timestampAt(int i) const { return m_table->timestampAt(i); }
    float valueAt(int i) const { return m_table->valueAt(*m_column, i); }
    Point at(int i) const { return { indexAt(i), timestampAt(i), valueAt(i) }; }

    quint32 firstIndex() const { return indexAt(0); }
    quint32 lastIndex() const { return indexAt(size() - 1); }
    float lastValue() const { return valueAt(size() - 1); }
    qint64 lastTimestamp() const { return timestampAt(size() - 1); }

    int lowerBound(double x) const { return m_table ? m_table->lowerBound(x) : 0; }
    int upperBound(double x) const { return m_table ? m_table->upperBound(x) : 0; }
    int nearest(quint32 index) const { return m_table ? m_table->nearest(index) : -1; }

private:
    const SampleTable *m_table = nullptr;
    const QVector<float> *m_column = nullptr;
};

#endif // SAMPLETABLE_H

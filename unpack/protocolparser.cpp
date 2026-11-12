#include "unpack/protocolparser.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QtGlobal>

#include <cstring>

namespace {

constexpr int kMaxPayloadBytes = 4096;  // 长度字段声称的数据区上限，超出视为误同步
constexpr int kMaxBufferBytes = 64 * 1024;

bool isTrailerType(ProtoFieldType type)
{
    return type == ProtoFieldType::Checksum || type == ProtoFieldType::Tail;
}

quint64 readInteger(const QByteArray &bytes, bool bigEndian)
{
    quint64 value = 0;
    const int count = int(bytes.size());
    for (int i = 0; i < count; ++i) {
        const quint8 byte = quint8(bytes.at(bigEndian ? i : count - 1 - i));
        value = (value << 8) | byte;
    }
    return value;
}

quint8 crc8(const QByteArray &data)
{
    quint8 crc = 0;
    for (const char ch : data) {
        crc ^= quint8(ch);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? quint8((crc << 1) ^ 0x07) : quint8(crc << 1);
    }
    return crc;
}

quint16 crc16Modbus(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (const char ch : data) {
        crc ^= quint8(ch);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? quint16((crc >> 1) ^ 0xA001) : quint16(crc >> 1);
    }
    return crc;
}

quint16 crc16Ccitt(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (const char ch : data) {
        crc ^= quint16(quint8(ch)) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}

} // namespace

int protoFieldSize(const ProtoField &field)
{
    switch (field.type) {
    case ProtoFieldType::Header:
    case ProtoFieldType::Tail:
        return int(field.value.size());
    case ProtoFieldType::Seq:
    case ProtoFieldType::Id:
    case ProtoFieldType::Length:
    case ProtoFieldType::Byte1:
        return 1;
    case ProtoFieldType::Byte2:
        return 2;
    case ProtoFieldType::Byte3:
        return 3;
    case ProtoFieldType::Byte4:
        return 4;
    case ProtoFieldType::Byte8:
        return 8;
    case ProtoFieldType::Data:
        return 0;
    case ProtoFieldType::Checksum:
        return checksumSize(field.checksum);
    }
    return 0;
}

bool protoTypeIsData(ProtoFieldType type)
{
    return type == ProtoFieldType::Byte1 || type == ProtoFieldType::Byte2
            || type == ProtoFieldType::Byte3 || type == ProtoFieldType::Byte4
            || type == ProtoFieldType::Byte8;
}

bool protoFieldShowable(const ProtoField &field)
{
    return protoTypeIsData(field.type) || field.type == ProtoFieldType::Seq
            || field.type == ProtoFieldType::Id || field.type == ProtoFieldType::Length
            || field.type == ProtoFieldType::Data;
}

bool protoFieldNumeric(const ProtoField &field)
{
    return field.type != ProtoFieldType::Data && field.conversion != ProtoConversion::Hex
            && field.conversion != ProtoConversion::Binary;
}

int checksumSize(ProtoChecksum algorithm)
{
    switch (algorithm) {
    case ProtoChecksum::Sum8:
    case ProtoChecksum::Xor8:
    case ProtoChecksum::Crc8:
        return 1;
    case ProtoChecksum::Sum16:
    case ProtoChecksum::Crc16Modbus:
    case ProtoChecksum::Crc16Ccitt:
        return 2;
    }
    return 1;
}

static quint32 fieldChecksum(const ProtoField &field, const QByteArray &data);

quint32 computeChecksum(ProtoChecksum algorithm, const QByteArray &data)
{
    switch (algorithm) {
    case ProtoChecksum::Sum8: {
        quint32 sum = 0;
        for (const char ch : data)
            sum += quint8(ch);
        return sum & 0xFF;
    }
    case ProtoChecksum::Xor8: {
        quint8 value = 0;
        for (const char ch : data)
            value ^= quint8(ch);
        return value;
    }
    case ProtoChecksum::Crc8:
        return crc8(data);
    case ProtoChecksum::Sum16: {
        quint32 sum = 0;
        for (const char ch : data)
            sum += quint8(ch);
        return sum & 0xFFFF;
    }
    case ProtoChecksum::Crc16Modbus:
        return crc16Modbus(data);
    case ProtoChecksum::Crc16Ccitt:
        return crc16Ccitt(data);
    }
    return 0;
}

// 按字段设置计算校验值：可选取反（按校验字节数取反，如 Sum8 取反 = 0xFF - sum）
static quint32 fieldChecksum(const ProtoField &field, const QByteArray &data)
{
    quint32 value = computeChecksum(field.checksum, data);
    if (field.invertChecksum) {
        const int size = checksumSize(field.checksum);
        const quint32 mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
        value = ~value & mask;
    }
    return value;
}

bool protoResolveRange(const QVector<ProtoField> &layout, int index, int *from, int *to)
{
    const int count = int(layout.size());
    if (index < 0 || index >= count)
        return false;
    const ProtoField &field = layout.at(index);
    const bool isLength = field.type == ProtoFieldType::Length;
    if (!isLength && field.type != ProtoFieldType::Checksum)
        return false;

    int first = field.rangeFrom;
    int last = field.rangeTo;
    const bool autoFirst = first < 0;
    const bool autoLast = last < 0;
    if (isLength) {
        if (autoFirst)
            first = index + 1;
        if (autoLast) { // 到第一个校验 / 帧尾之前
            last = count - 1;
            for (int i = index + 1; i < count; ++i) {
                if (isTrailerType(layout.at(i).type)) {
                    last = i - 1;
                    break;
                }
            }
        }
    } else {
        if (autoFirst)
            first = 0;
        if (autoLast)
            last = index - 1;
    }
    *from = first;
    *to = last;

    if (first < 0 || last >= count)
        return false;
    if (first > last)
        return isLength && autoFirst && autoLast; // 只有"自动"的长度范围允许为空
    // 校验不能把自己和后面的字段算进去
    return isLength || last < index;
}

bool validateProtocol(const QVector<ProtoField> &layout, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (layout.isEmpty())
        return fail(QStringLiteral("empty"));
    if (layout.first().type != ProtoFieldType::Header || layout.first().value.isEmpty())
        return fail(QStringLiteral("header"));

    int lengthCount = 0;
    int dataCount = 0;
    int dataIndex = -1;
    bool inTrailer = false;
    for (int i = 0; i < layout.size(); ++i) {
        const ProtoField &field = layout.at(i);
        if (i > 0 && field.type == ProtoFieldType::Header)
            return fail(QStringLiteral("header-position"));
        if (field.type == ProtoFieldType::Length)
            ++lengthCount;
        if (field.type == ProtoFieldType::Data) {
            ++dataCount;
            dataIndex = i;
            if (lengthCount == 0)
                return fail(QStringLiteral("data-needs-length"));
        }
        if (field.type == ProtoFieldType::Tail && field.value.isEmpty())
            return fail(QStringLiteral("tail"));
        if (inTrailer && !isTrailerType(field.type))
            return fail(QStringLiteral("after-trailer"));
        if (isTrailerType(field.type))
            inTrailer = true;
    }
    if (lengthCount > 1)
        return fail(QStringLiteral("length"));
    if (dataCount > 1)
        return fail(QStringLiteral("data-count"));

    for (int i = 0; i < layout.size(); ++i) {
        const ProtoField &field = layout.at(i);
        if (field.type != ProtoFieldType::Length && field.type != ProtoFieldType::Checksum)
            continue;
        int from = 0;
        int to = 0;
        if (!protoResolveRange(layout, i, &from, &to))
            return fail(QStringLiteral("range"));
        // 变长的 Data 段必须落在长度范围内，否则无法由长度算出它有多长
        if (field.type == ProtoFieldType::Length && dataIndex >= 0 && (dataIndex < from || dataIndex > to))
            return fail(QStringLiteral("data-range"));
    }
    return true;
}

QVector<int> protocolDisplayIndexes(const QVector<ProtoField> &layout)
{
    QVector<int> indexes;
    for (int i = 0; i < layout.size(); ++i) {
        if (layout.at(i).show && protoFieldShowable(layout.at(i)))
            indexes.append(i);
    }
    return indexes;
}

QStringList protocolDisplayNames(const QVector<ProtoField> &layout)
{
    QStringList names;
    for (const int index : protocolDisplayIndexes(layout)) {
        const ProtoField &field = layout.at(index);
        names.append(field.name.isEmpty() ? QStringLiteral("#%1").arg(index) : field.name);
    }
    return names;
}

QString protoBitRangeText(const ProtoBitGroup &group)
{
    return group.high == group.low ? QString::number(group.low)
                                   : QStringLiteral("%1:%2").arg(group.high).arg(group.low);
}

bool protoParseBitRange(const QString &text, int *low, int *high)
{
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    bool okHigh = false;
    bool okLow = false;
    int h = parts.value(0).trimmed().toInt(&okHigh);
    int l = parts.size() == 1 ? h : parts.value(1).trimmed().toInt(&okLow);
    if (parts.size() == 1)
        okLow = okHigh;
    if (parts.size() > 2 || !okHigh || !okLow)
        return false;
    if (h < l)
        std::swap(h, l);
    if (l < 0 || h > 63)
        return false;
    *low = l;
    *high = h;
    return true;
}

QString protoBitValuesText(const ProtoBitGroup &group)
{
    QStringList items;
    for (auto it = group.valueNames.cbegin(); it != group.valueNames.cend(); ++it)
        items.append(QStringLiteral("%1=%2").arg(it.key()).arg(it.value()));
    return items.join(QLatin1Char(';'));
}

QMap<int, QString> protoParseBitValues(const QString &text)
{
    QMap<int, QString> result;
    const QStringList items = text.split(QRegularExpression(QStringLiteral("[;；]")));
    for (const QString &item : items) {
        const int eq = item.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        bool ok = false;
        const int value = item.left(eq).trimmed().toInt(&ok);
        const QString name = item.mid(eq + 1).trimmed();
        if (ok && value >= 0 && !name.isEmpty())
            result.insert(value, name);
    }
    return result;
}

QString protoBitGroupsDisplay(const QVector<ProtoBitGroup> &groups, quint64 raw)
{
    QStringList parts;
    for (const ProtoBitGroup &group : groups) {
        const int width = group.high - group.low + 1;
        const quint64 mask = width >= 64 ? ~quint64(0) : ((quint64(1) << width) - 1);
        const int value = int((raw >> group.low) & mask);
        const QString valueText = group.valueNames.value(value, QString::number(value));
        parts.append(group.name.isEmpty() ? valueText : group.name + QLatin1String(": ") + valueText);
    }
    // 每组一行（以换行结尾）：面板据此把它画成"名称 | 取值"的小表格
    return parts.isEmpty() ? QString() : parts.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString formatHexBytes(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

bool parseHexBytes(const QString &text, QByteArray *out)
{
    QString digits;
    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[\\s,;]+")),
                                          Qt::SkipEmptyParts);
    for (QString token : tokens) {
        if (token.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            token.remove(0, 2);
        digits += token;
    }
    if (digits.size() % 2 != 0)
        return false;
    for (const QChar ch : std::as_const(digits)) {
        if (!QStringLiteral("0123456789abcdefABCDEF").contains(ch))
            return false;
    }
    if (out)
        *out = QByteArray::fromHex(digits.toLatin1());
    return true;
}

QString protoFieldPreview(const ProtoField &field)
{
    switch (field.type) {
    case ProtoFieldType::Header:
    case ProtoFieldType::Tail:
        return formatHexBytes(field.value);
    case ProtoFieldType::Id:
        return field.value.isEmpty() ? QStringLiteral("00") : formatHexBytes(field.value);
    case ProtoFieldType::Data:
        return QStringLiteral("...");
    default:
        break;
    }
    return formatHexBytes(QByteArray(protoFieldSize(field), '\0'));
}

namespace {

QString protoTr(const char *text)
{
    return QCoreApplication::translate("Protocol", text);
}

void appendInteger(QByteArray *frame, quint64 value, int size, bool bigEndian)
{
    for (int i = 0; i < size; ++i) {
        const int shift = bigEndian ? (size - 1 - i) * 8 : i * 8;
        frame->append(char((value >> shift) & 0xFF));
    }
}

// 把文本解析成能放进 size 字节的原始位模式
bool encodeNumber(const ProtoField &field, const QString &text, bool hexNumbers, quint64 *raw,
                  QString *error)
{
    const int size = protoFieldSize(field);
    const QString token = text.trimmed();
    auto fail = [&](const QString &why) {
        if (error)
            *error = protoTr(QT_TRANSLATE_NOOP("Protocol", "Value \"%1\" for field \"%2\": %3")).arg(token, field.name, why);
        return false;
    };

    if (field.conversion == ProtoConversion::Float) {
        bool ok = false;
        const double value = token.toDouble(&ok);
        if (!ok)
            return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "not a number")));
        if (size == 4) {
            const float single = float(value);
            quint32 bits = 0;
            std::memcpy(&bits, &single, sizeof(bits));
            *raw = bits;
        } else {
            std::memcpy(raw, &value, sizeof(*raw));
        }
        return true;
    }

    QString digits = token;
    int base = 10;
    if (field.conversion == ProtoConversion::Binary) {
        base = 2;
        digits.remove(QLatin1Char(' '));
    } else if (field.conversion == ProtoConversion::Hex || hexNumbers) {
        base = 16;
    }
    bool negative = false;
    if (digits.startsWith(QLatin1Char('-'))) {
        negative = true;
        digits.remove(0, 1);
    }
    if (digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        base = 16;
        digits.remove(0, 2);
    }
    bool ok = false;
    const quint64 magnitude = digits.toULongLong(&ok, base);
    if (!ok || digits.isEmpty())
        return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "not a valid number")));

    const int bits = size * 8;
    if (negative) {
        if (field.conversion == ProtoConversion::UInt)
            return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "negative value for an unsigned field")));
        const quint64 limit = quint64(1) << (bits - 1); // 有符号能表示的最大负数的绝对值
        if (magnitude > limit)
            return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "does not fit in %1 byte(s)")).arg(size));
        *raw = (~magnitude + 1) & (bits >= 64 ? ~quint64(0) : ((quint64(1) << bits) - 1));
        return true;
    }
    if (bits < 64 && magnitude >= (quint64(1) << bits))
        return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "does not fit in %1 byte(s)")).arg(size));
    if (field.conversion == ProtoConversion::Int && bits < 64 && magnitude >= (quint64(1) << (bits - 1)))
        return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "does not fit in %1 byte(s)")).arg(size));
    *raw = magnitude;
    return true;
}

} // namespace

int protoDataFieldCount(const QVector<ProtoField> &layout)
{
    int count = 0;
    for (const ProtoField &field : layout)
        count += protoTypeIsData(field.type) ? 1 : 0;
    return count;
}

bool encodeFrame(const QVector<ProtoField> &layout, const QString &text, bool hexNumbers,
                 quint8 seq, QByteArray *out, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    QString problem;
    if (!validateProtocol(layout, &problem))
        return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "The frame format is not valid. Open the format editor and fix it.")));

    // 把文本切成数值；记下每个数值在原文里的位置，Data 段要取它们之间的原文
    struct Token { QString text; int start; int end; };
    QVector<Token> tokens;
    const bool hasData = protoHasDataBlock(layout);
    const QRegularExpression separator(hasData ? QStringLiteral("\\S+") : QStringLiteral("[^\\s,;]+"));
    for (QRegularExpressionMatchIterator it = separator.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        tokens.append({match.captured(), int(match.capturedStart()), int(match.capturedEnd())});
    }

    // 固定数据字段分布在 Data 段两侧：前面的取开头的数值，后面的取结尾的数值
    const int fixedCount = protoDataFieldCount(layout);
    int beforeCount = 0;
    for (const ProtoField &field : layout) {
        if (field.type == ProtoFieldType::Data)
            break;
        beforeCount += protoTypeIsData(field.type) ? 1 : 0;
    }
    if (!hasData)
        beforeCount = fixedCount;

    if (hasData ? tokens.size() < fixedCount : tokens.size() != fixedCount) {
        return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "This frame format needs %1 value(s), separated by spaces; got %2."))
                            .arg(fixedCount)
                            .arg(tokens.size()));
    }

    // Data 段的原文：夹在前后固定数值之间
    QString dataText;
    if (hasData) {
        const int from = beforeCount > 0 ? tokens.at(beforeCount - 1).end : 0;
        const int afterCount = fixedCount - beforeCount;
        const int to = afterCount > 0 ? tokens.at(tokens.size() - afterCount).start : int(text.size());
        dataText = text.mid(from, to - from).trimmed();
    }

    // 先逐字段生成字节：长度、校验先占位，等全帧的各字段大小确定后再回填
    const int count = int(layout.size());
    QVector<QByteArray> parts(count);
    int fixedSeen = 0;
    for (int i = 0; i < count; ++i) {
        const ProtoField &field = layout.at(i);
        switch (field.type) {
        case ProtoFieldType::Header:
        case ProtoFieldType::Tail:
            parts[i] = field.value;
            break;
        case ProtoFieldType::Seq:
            parts[i] = QByteArray(1, char(seq));
            break;
        case ProtoFieldType::Id:
            parts[i] = QByteArray(1, field.value.isEmpty() ? char(0) : field.value.at(0));
            break;
        case ProtoFieldType::Length:
        case ProtoFieldType::Checksum:
            parts[i] = QByteArray(protoFieldSize(field), '\0');
            break;
        case ProtoFieldType::Data: {
            if (field.conversion == ProtoConversion::Text) {
                parts[i] = dataText.toLocal8Bit();
            } else if (!parseHexBytes(dataText, &parts[i])) {
                return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "The data part must be hex bytes, e.g. 01 0A FF.")));
            }
            break;
        }
        default: {
            const int k = fixedSeen++;
            const Token &token = tokens.at(k < beforeCount ? k : int(tokens.size()) - fixedCount + k);
            quint64 raw = 0;
            if (!encodeNumber(field, token.text, hexNumbers, &raw, error))
                return false;
            appendInteger(&parts[i], raw, protoFieldSize(field), field.bigEndian);
            break;
        }
        }
    }

    auto rangeBytes = [&](int from, int to) {
        int bytes = 0;
        for (int i = from; i <= to; ++i)
            bytes += int(parts.at(i).size());
        return bytes;
    };

    // 长度：范围内各字段的总字节数（校验的字节数是固定的，占位时已经算进去）
    for (int i = 0; i < count; ++i) {
        if (layout.at(i).type != ProtoFieldType::Length)
            continue;
        int from = 0;
        int to = 0;
        protoResolveRange(layout, i, &from, &to);
        const int bytes = rangeBytes(from, to);
        if (bytes > 255)
            return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "The data length %1 does not fit in the length field.")).arg(bytes));
        parts[i] = QByteArray(1, char(bytes));
    }

    // 校验：按顺序算，后面的校验可以把前面的校验也算进范围
    for (int i = 0; i < count; ++i) {
        const ProtoField &field = layout.at(i);
        if (field.type != ProtoFieldType::Checksum)
            continue;
        int from = 0;
        int to = 0;
        protoResolveRange(layout, i, &from, &to);
        QByteArray covered;
        for (int j = from; j <= to; ++j)
            covered.append(parts.at(j));
        parts[i].clear();
        appendInteger(&parts[i], fieldChecksum(field, covered), protoFieldSize(field),
                      field.bigEndian);
    }

    if (out) {
        out->clear();
        for (const QByteArray &part : std::as_const(parts))
            out->append(part);
    }
    return true;
}

bool protoHasDataBlock(const QVector<ProtoField> &layout)
{
    for (const ProtoField &field : layout) {
        if (field.type == ProtoFieldType::Data)
            return true;
    }
    return false;
}

void ProtocolParser::setLayout(const QVector<ProtoField> &layout)
{
    m_layout = layout;
    m_valid = validateProtocol(layout);
    reset();
}

void ProtocolParser::reset()
{
    m_buffer.clear();
    m_good = 0;
    m_bad = 0;
}

QString ProtocolParser::decode(const ProtoField &field, const QByteArray &bytes) const
{
    if (field.type == ProtoFieldType::Data) {
        // 变长数据段：按文本或十六进制字节显示（字节顺序与长度无关，按线上顺序）
        return field.conversion == ProtoConversion::Text ? QString::fromLocal8Bit(bytes)
                                                          : formatHexBytes(bytes);
    }

    const int size = int(bytes.size());
    const quint64 raw = readInteger(bytes, field.bigEndian);

    switch (field.conversion) {
    case ProtoConversion::UInt:
        return QString::number(raw);
    case ProtoConversion::Int: {
        if (size >= 8)
            return QString::number(qint64(raw));
        const quint64 signBit = quint64(1) << (size * 8 - 1);
        const qint64 value = (raw & signBit) ? qint64(raw) - (qint64(1) << (size * 8)) : qint64(raw);
        return QString::number(value);
    }
    case ProtoConversion::Float:
        if (size == 4) {
            const quint32 bits = quint32(raw);
            float value = 0;
            std::memcpy(&value, &bits, sizeof(value));
            return QString::number(double(value), 'g', 9);
        }
        if (size == 8) {
            double value = 0;
            std::memcpy(&value, &raw, sizeof(value));
            return QString::number(value, 'g', 17);
        }
        return QString::number(raw);
    case ProtoConversion::Hex:
        return QStringLiteral("0x") + QString::number(raw, 16).toUpper().rightJustified(size * 2, QLatin1Char('0'));
    case ProtoConversion::Text:
        return QString::fromLocal8Bit(bytes);
    case ProtoConversion::Binary: {
        if (!field.bitGroups.isEmpty())
            return protoBitGroupsDisplay(field.bitGroups, raw);
        QStringList groups;
        for (int i = size - 1; i >= 0; --i) // 从最高字节开始，每字节 8 位
            groups.append(QString::number((raw >> (i * 8)) & 0xFF, 2).rightJustified(8, QLatin1Char('0')));
        return groups.join(QLatin1Char(' '));
    }
    }
    return QString();
}

ProtocolParser::Result ProtocolParser::tryParse(QStringList *values, int *consumed)
{
    // 调用前保证 m_buffer 以帧头开头
    const int count = int(m_layout.size());
    QVector<int> sizes(count, 0);
    QVector<int> offsets(count, 0);
    int lengthIndex = -1;
    int dataIndex = -1;
    for (int i = 0; i < count; ++i) {
        sizes[i] = protoFieldSize(m_layout.at(i));
        if (m_layout.at(i).type == ProtoFieldType::Length)
            lengthIndex = i;
        else if (m_layout.at(i).type == ProtoFieldType::Data)
            dataIndex = i;
    }

    int lengthFrom = 0;
    int lengthTo = -1;
    qint64 lengthValue = 0;
    if (lengthIndex >= 0) {
        protoResolveRange(m_layout, lengthIndex, &lengthFrom, &lengthTo);

        // Length 之前都是定长字段，读到它就能知道后面的情况
        int lengthOffset = 0;
        for (int i = 0; i < lengthIndex; ++i)
            lengthOffset += sizes.at(i);
        const int prefixSize = lengthOffset + sizes.at(lengthIndex);
        if (m_buffer.size() < prefixSize)
            return Result::NeedMore;

        const ProtoField &lengthField = m_layout.at(lengthIndex);
        lengthValue = qint64(readInteger(m_buffer.mid(lengthOffset, sizes.at(lengthIndex)),
                                         lengthField.bigEndian));

        if (dataIndex >= 0) {
            // 变长 Data 段 = 长度值 - 范围内其余固定字段
            int fixedInRange = 0;
            for (int i = lengthFrom; i <= lengthTo; ++i) {
                if (i != dataIndex)
                    fixedInRange += sizes.at(i);
            }
            const qint64 dataLength = lengthValue - fixedInRange;
            if (dataLength < 0 || dataLength > kMaxPayloadBytes)
                return Result::Reject;
            sizes[dataIndex] = int(dataLength);
        } else {
            // 没有变长段时帧长是固定的，长度值必须与范围内的字节数一致
            int bytes = 0;
            for (int i = lengthFrom; i <= lengthTo; ++i)
                bytes += sizes.at(i);
            if (lengthValue != bytes)
                return Result::Reject;
        }
    }

    int total = 0;
    for (int i = 0; i < count; ++i) {
        offsets[i] = total;
        total += sizes.at(i);
    }
    if (m_buffer.size() < total)
        return Result::NeedMore;

    const QByteArray frame = m_buffer.left(total);

    // 校验 Id / Tail / Checksum
    for (int i = 0; i < count; ++i) {
        const ProtoField &field = m_layout.at(i);
        const int size = sizes.at(i);
        const int offset = offsets.at(i);

        if (field.type == ProtoFieldType::Id && !field.value.isEmpty()) {
            if (frame.mid(offset, size) != field.value)
                return Result::Filtered;
        } else if (field.type == ProtoFieldType::Tail) {
            if (frame.mid(offset, size) != field.value)
                return Result::Reject;
        } else if (field.type == ProtoFieldType::Checksum) {
            int from = 0;
            int to = 0;
            protoResolveRange(m_layout, i, &from, &to);
            const int start = offsets.at(from);
            const int end = offsets.at(to) + sizes.at(to);
            const quint32 expected = fieldChecksum(field, frame.mid(start, end - start));
            const quint32 actual = quint32(readInteger(frame.mid(offset, size), field.bigEndian));
            if (expected != actual)
                return Result::Reject;
        }
    }

    values->clear();
    for (const int index : protocolDisplayIndexes(m_layout))
        values->append(decode(m_layout.at(index), frame.mid(offsets.at(index), sizes.at(index))));
    *consumed = total;
    return Result::Frame;
}

QVector<QStringList> ProtocolParser::feed(const QByteArray &bytes)
{
    QVector<QStringList> frames;
    if (!m_valid || bytes.isEmpty())
        return frames;

    m_buffer.append(bytes);
    const QByteArray &header = m_layout.first().value;

    while (true) {
        const int start = int(m_buffer.indexOf(header));
        if (start < 0) {
            // 末尾可能是帧头的前半截，保留 header.size()-1 个字节
            const int keep = int(header.size()) - 1;
            if (m_buffer.size() > keep)
                m_buffer.remove(0, m_buffer.size() - keep);
            break;
        }
        if (start > 0)
            m_buffer.remove(0, start);

        QStringList values;
        int consumed = 0;
        const Result result = tryParse(&values, &consumed);
        if (result == Result::NeedMore)
            break;
        if (result == Result::Frame) {
            ++m_good;
            frames.append(values);
            m_buffer.remove(0, consumed);
            continue;
        }
        if (result == Result::Reject)
            ++m_bad;
        // 被过滤或不合法：越过这个帧头，继续往后找
        m_buffer.remove(0, 1);
    }

    if (m_buffer.size() > kMaxBufferBytes)
        m_buffer.remove(0, m_buffer.size() - kMaxBufferBytes);
    return frames;
}

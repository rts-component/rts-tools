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
    bool inTrailer = false;
    for (int i = 0; i < layout.size(); ++i) {
        const ProtoField &field = layout.at(i);
        if (i > 0 && field.type == ProtoFieldType::Header)
            return fail(QStringLiteral("header-position"));
        if (field.type == ProtoFieldType::Length)
            ++lengthCount;
        if (field.type == ProtoFieldType::Data) {
            ++dataCount;
            if (lengthCount == 0)
                return fail(QStringLiteral("data-needs-length"));
        }
        if (field.type == ProtoFieldType::Tail && field.value.isEmpty())
            return fail(QStringLiteral("tail"));
        if (inTrailer && !isTrailerType(field.type))
            return fail(QStringLiteral("after-trailer"));
        if (isTrailerType(field.type))
            inTrailer = true;
        // 长度字段之前不能有不定长内容，所以 Length 之前只能是定长字段（天然满足）
    }
    if (lengthCount > 1)
        return fail(QStringLiteral("length"));
    if (dataCount > 1)
        return fail(QStringLiteral("data-count"));
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

    QByteArray frame;
    int lengthPos = -1;
    int payloadStart = -1;
    int fixedSeen = 0;
    int trailerStart = int(layout.size());

    for (int i = 0; i < layout.size(); ++i) {
        const ProtoField &field = layout.at(i);
        if (field.type == ProtoFieldType::Checksum || field.type == ProtoFieldType::Tail) {
            trailerStart = i;
            break;
        }
        switch (field.type) {
        case ProtoFieldType::Header:
            frame.append(field.value);
            break;
        case ProtoFieldType::Seq:
            frame.append(char(seq));
            break;
        case ProtoFieldType::Id:
            frame.append(field.value.isEmpty() ? char(0) : field.value.at(0));
            break;
        case ProtoFieldType::Length:
            lengthPos = int(frame.size());
            frame.append(char(0)); // 稍后回填
            payloadStart = int(frame.size());
            break;
        case ProtoFieldType::Data: {
            if (field.conversion == ProtoConversion::Text) {
                frame.append(dataText.toLocal8Bit());
            } else {
                QByteArray bytes;
                if (!parseHexBytes(dataText, &bytes))
                    return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "The data part must be hex bytes, e.g. 01 0A FF.")));
                frame.append(bytes);
            }
            break;
        }
        default: {
            const int k = fixedSeen++;
            const Token &token = tokens.at(k < beforeCount ? k : int(tokens.size()) - fixedCount + k);
            quint64 raw = 0;
            if (!encodeNumber(field, token.text, hexNumbers, &raw, error))
                return false;
            appendInteger(&frame, raw, protoFieldSize(field), field.bigEndian);
            break;
        }
        }
    }

    if (lengthPos >= 0) {
        const int length = int(frame.size()) - payloadStart; // 数据区字节数
        // 长度字段的值 = 数据区字节数 - 偏移（解析时：数据区 = 值 + 偏移）
        int offset = 0;
        for (const ProtoField &field : layout) {
            if (field.type == ProtoFieldType::Length)
                offset = field.lengthOffset;
        }
        const int encoded = length - offset;
        if (encoded < 0 || encoded > 255)
            return fail(protoTr(QT_TRANSLATE_NOOP("Protocol", "The data length %1 does not fit in the length field.")).arg(length));
        frame[lengthPos] = char(encoded);
    }

    for (int i = trailerStart; i < layout.size(); ++i) {
        const ProtoField &field = layout.at(i);
        if (field.type == ProtoFieldType::Tail) {
            frame.append(field.value);
        } else if (field.type == ProtoFieldType::Checksum) {
            const int start = field.checksumFromHeader ? 0 : protoFieldSize(layout.first());
            const quint32 sum = computeChecksum(field.checksum, frame.mid(start));
            appendInteger(&frame, sum, protoFieldSize(field), field.bigEndian);
        }
    }

    if (out)
        *out = frame;
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
    int prefixSize = 0;     // 到 Length（含）为止的字节数；没有 Length 时 = 全部定长字段
    int lengthIndex = -1;
    int trailerSize = 0;    // Length 之后的 Checksum / Tail 总字节数

    for (int i = 0; i < m_layout.size(); ++i) {
        const ProtoField &field = m_layout.at(i);
        if (field.type == ProtoFieldType::Length) {
            lengthIndex = i;
            break;
        }
    }

    // 各字段在帧内的偏移，Length 之后的位置要等读到长度才能确定
    QVector<int> offsets(m_layout.size(), 0);
    QVector<int> sizes(m_layout.size(), 0);
    for (int i = 0; i < m_layout.size(); ++i)
        sizes[i] = protoFieldSize(m_layout.at(i));
    int payloadLength = 0;
    int total = 0;

    if (lengthIndex < 0) {
        int pos = 0;
        for (int i = 0; i < m_layout.size(); ++i) {
            offsets[i] = pos;
            pos += protoFieldSize(m_layout.at(i));
        }
        total = pos;
    } else {
        int pos = 0;
        for (int i = 0; i <= lengthIndex; ++i) {
            offsets[i] = pos;
            pos += protoFieldSize(m_layout.at(i));
        }
        prefixSize = pos;
        for (int i = lengthIndex + 1; i < m_layout.size(); ++i) {
            if (isTrailerType(m_layout.at(i).type))
                trailerSize += protoFieldSize(m_layout.at(i));
        }
        if (m_buffer.size() < prefixSize)
            return Result::NeedMore;

        const ProtoField &lengthField = m_layout.at(lengthIndex);
        const quint64 raw = readInteger(m_buffer.mid(offsets[lengthIndex], protoFieldSize(lengthField)),
                                        lengthField.bigEndian);
        const qint64 length = qint64(raw) + lengthField.lengthOffset;
        if (length < 0 || length > kMaxPayloadBytes)
            return Result::Reject;
        payloadLength = int(length);

        // 变长 Data 段 = 数据区 - 其余固定字段
        int dataIndex = -1;
        int fixedPayload = 0;
        for (int i = lengthIndex + 1; i < m_layout.size(); ++i) {
            if (isTrailerType(m_layout.at(i).type))
                continue;
            if (m_layout.at(i).type == ProtoFieldType::Data)
                dataIndex = i;
            else
                fixedPayload += sizes.at(i);
        }
        if (dataIndex >= 0) {
            if (payloadLength < fixedPayload)
                return Result::Reject;
            sizes[dataIndex] = payloadLength - fixedPayload;
        }

        int payloadPos = prefixSize;
        int trailerPos = prefixSize + payloadLength;
        for (int i = lengthIndex + 1; i < m_layout.size(); ++i) {
            const ProtoField &field = m_layout.at(i);
            if (isTrailerType(field.type)) {
                offsets[i] = trailerPos;
                trailerPos += sizes.at(i);
            } else {
                offsets[i] = payloadPos;
                payloadPos += sizes.at(i);
            }
        }
        total = prefixSize + payloadLength + trailerSize;
    }

    if (m_buffer.size() < total)
        return Result::NeedMore;

    const QByteArray frame = m_buffer.left(total);
    const int payloadEnd = lengthIndex < 0 ? total : prefixSize + payloadLength;

    // 校验 Id / Tail / Checksum
    for (int i = 0; i < m_layout.size(); ++i) {
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
            const int start = field.checksumFromHeader ? 0 : protoFieldSize(m_layout.first());
            if (offset < start)
                return Result::Reject;
            const quint32 expected = computeChecksum(field.checksum, frame.mid(start, offset - start));
            const quint32 actual = quint32(readInteger(frame.mid(offset, size), field.bigEndian));
            if (expected != actual)
                return Result::Reject;
        }
    }

    values->clear();
    for (const int index : protocolDisplayIndexes(m_layout)) {
        const ProtoField &field = m_layout.at(index);
        const int size = sizes.at(index);
        const int offset = offsets.at(index);
        // 数据区比声明的短时，放不下的字段留空
        const bool inPayload = lengthIndex >= 0 && index > lengthIndex;
        if (offset + size > total || (inPayload && offset + size > payloadEnd)) {
            values->append(QString());
            continue;
        }
        values->append(decode(field, frame.mid(offset, size)));
    }
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

#include "unpack/protocolparser.h"

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
            || field.type == ProtoFieldType::Id || field.type == ProtoFieldType::Length;
}

bool protoFieldNumeric(const ProtoField &field)
{
    return field.conversion != ProtoConversion::Hex && field.conversion != ProtoConversion::Binary;
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
    bool inTrailer = false;
    for (int i = 0; i < layout.size(); ++i) {
        const ProtoField &field = layout.at(i);
        if (i > 0 && field.type == ProtoFieldType::Header)
            return fail(QStringLiteral("header-position"));
        if (field.type == ProtoFieldType::Length)
            ++lengthCount;
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
    default:
        break;
    }
    return formatHexBytes(QByteArray(protoFieldSize(field), '\0'));
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

        int payloadPos = prefixSize;
        int trailerPos = prefixSize + payloadLength;
        for (int i = lengthIndex + 1; i < m_layout.size(); ++i) {
            const ProtoField &field = m_layout.at(i);
            if (isTrailerType(field.type)) {
                offsets[i] = trailerPos;
                trailerPos += protoFieldSize(field);
            } else {
                offsets[i] = payloadPos;
                payloadPos += protoFieldSize(field);
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
        const int size = protoFieldSize(field);
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
        const int size = protoFieldSize(field);
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

#ifndef PROTOCOLPARSER_H
#define PROTOCOLPARSER_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// 二进制协议帧解析：帧头 + 序号/ID/长度 + 数据 + 校验 + 帧尾。
// 字段按顺序排列，由用户在界面里组装；这里只负责"定义 → 从字节流里切帧并解码"。

enum class ProtoFieldType {
    Header,   // 固定字节序列，用于同步帧起点
    Seq,      // 帧序号（1 字节计数）
    Id,       // 帧 ID（1 字节，可设期望值用于过滤）
    Length,   // 帧长度：其后到校验/帧尾之前的数据区字节数
    Byte1,
    Byte2,
    Byte3,
    Byte4,
    Byte8,
    Checksum, // 校验
    Tail,     // 固定字节序列
};

enum class ProtoConversion {
    UInt,
    Int,
    Float, // 仅 4 / 8 字节：IEEE754 单/双精度
    Hex,
};

enum class ProtoChecksum {
    Sum8,
    Xor8,
    Crc8,       // poly 0x07
    Sum16,
    Crc16Modbus,
    Crc16Ccitt, // CCITT-FALSE，init 0xFFFF
};

struct ProtoField
{
    ProtoFieldType type = ProtoFieldType::Byte1;
    QString name;
    QByteArray value;                               // Header / Tail / Id 的期望字节（Id 为空 = 不过滤）
    ProtoConversion conversion = ProtoConversion::UInt;
    bool bigEndian = false;                         // "高字节在前"；对多字节数据、校验有效
    bool show = true;                               // 是否显示到面板
    int lengthOffset = 0;                           // Length：数据区字节数 = 帧里的值 + 偏移
    ProtoChecksum checksum = ProtoChecksum::Sum8;
    bool checksumFromHeader = true;                 // 校验范围：true 从帧头开始，false 从帧头之后开始
};

// 字段在帧中占的字节数；Length 不能写死时（Header/Tail 取 value 长度）按类型给出
int protoFieldSize(const ProtoField &field);
bool protoTypeIsData(ProtoFieldType type);   // 1/2/3/4/8 Byte
bool protoFieldShowable(const ProtoField &field);
// 转换方式是否产生数字（决定 fx / 曲线是否可用）
bool protoFieldNumeric(const ProtoField &field);
int checksumSize(ProtoChecksum algorithm);

quint32 computeChecksum(ProtoChecksum algorithm, const QByteArray &data);

// 定义本身是否可用；不可用时给出原因
bool validateProtocol(const QVector<ProtoField> &layout, QString *error = nullptr);

// 面板上要显示的字段（按顺序）
QStringList protocolDisplayNames(const QVector<ProtoField> &layout);
QVector<int> protocolDisplayIndexes(const QVector<ProtoField> &layout);

// 帧的十六进制预览（占位用，例如 "AA 55 B0 00 00 00"）
QString protoFieldPreview(const ProtoField &field);

// 把字节序列文本（"AA 55" / "0xAA,0x55" / "aa55"）转成字节；失败返回 false
bool parseHexBytes(const QString &text, QByteArray *out);
QString formatHexBytes(const QByteArray &bytes);

class ProtocolParser
{
public:
    void setLayout(const QVector<ProtoField> &layout);
    void reset();

    // 喂入原始字节；返回本次解出的帧（每帧一个值列表，与 protocolDisplayNames 逐项对应）
    QVector<QStringList> feed(const QByteArray &bytes);

    quint64 goodFrames() const { return m_good; }
    quint64 badFrames() const { return m_bad; } // 校验/帧尾/长度不符的帧

private:
    enum class Result { NeedMore, Frame, Reject, Filtered };
    Result tryParse(QStringList *values, int *consumed);
    QString decode(const ProtoField &field, const QByteArray &bytes) const;

    QVector<ProtoField> m_layout;
    bool m_valid = false;
    QByteArray m_buffer;
    quint64 m_good = 0;
    quint64 m_bad = 0;
};

#endif // PROTOCOLPARSER_H

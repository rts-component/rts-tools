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
    Data,     // 变长数据段：占数据区里除固定字段之外的全部字节（必须有 Length）
    Checksum, // 校验
    Tail,     // 固定字节序列
};

enum class ProtoConversion {
    UInt,
    Int,
    Float, // 仅 4 / 8 字节：IEEE754 单/双精度
    Hex,
    Binary, // 每个字节 8 位，高位在前，字节之间空格分隔
    Text,   // 仅变长 Data 段：按文本显示
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
// （变长的 Data 段返回 0，实际长度由帧里的 Length 决定）
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

// ---- 组包（发送侧）：解码的逆操作 ----
// 数据字段（1/2/3/4/8Byte）的个数，组包时要按顺序给出这么多个值
int protoDataFieldCount(const QVector<ProtoField> &layout);
// 是否有变长 Data 段
bool protoHasDataBlock(const QVector<ProtoField> &layout);

// 按协议定义把 values 组成一帧：帧头/帧尾/ID 取定义里的值，序号用 seq，
// 长度与校验自动计算。数值按字段的"数据转换"和字节顺序编码：
// 无符号/有符号为十进制（hexNumbers 为 true 或带 0x 前缀时按十六进制），
// 十六进制字段按十六进制，二进制字段按二进制，浮点按 IEEE754。
// 失败时返回 false 并在 error 里给出可读的原因。
// 数值直接给整段文本：按空白（没有 Data 段时也接受 , ;）切出。
// 有 Data 段时，它前面的固定字段取开头的数值、后面的取结尾的数值，
// 中间剩下的原文（保留其中的空格 / 标点）就是 Data 段的内容：十六进制字节或文本。
bool encodeFrame(const QVector<ProtoField> &layout, const QString &text, bool hexNumbers,
                 quint8 seq, QByteArray *out, QString *error = nullptr);

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

#include "flash/gdbremoteprobe.h"

#include <QObject>
#include <QElapsedTimer>
#include <QThread>
#include <QXmlStreamReader>

namespace {

constexpr int kMaxResend = 3;
constexpr int kPollSliceMs = 100; // 长等待按这个粒度切开，好响应取消
constexpr int kMaxXmlIncludeDepth = 4;
constexpr int kLegacyXpsr = 25; // 没有 target.xml 时 ARM Cortex-M 的传统编号
constexpr int kInterruptReplyMs = 2000;

quint8 checksum(const QByteArray &payload)
{
    quint8 sum = 0;
    for (char c : payload)
        sum = quint8(sum + quint8(c));
    return sum;
}

QByteArray hexValue(quint32 value, int width = 0)
{
    return QByteArray::number(value, 16).rightJustified(width, '0');
}

// 寄存器值在 RSP 里是目标字节序（小端）的十六进制
QByteArray littleEndianHex(quint32 value)
{
    QByteArray bytes(4, '\0');
    for (int i = 0; i < 4; ++i)
        bytes[i] = char((value >> (8 * i)) & 0xFF);
    return bytes.toHex();
}

bool isStopReply(const QByteArray &payload)
{
    return !payload.isEmpty() && (payload[0] == 'T' || payload[0] == 'S');
}

} // namespace

GdbRemoteProbe::~GdbRemoteProbe()
{
    close();
}

QByteArray GdbRemoteProbe::escapePayload(const QByteArray &payload)
{
    QByteArray out;
    out.reserve(payload.size());
    for (char c : payload) {
        if (c == '$' || c == '#' || c == '}' || c == '*') {
            out.append('}');
            out.append(char(c ^ 0x20));
        } else {
            out.append(c);
        }
    }
    return out;
}

QByteArray GdbRemoteProbe::encodePacket(const QByteArray &payload)
{
    const QByteArray escaped = escapePayload(payload);
    QByteArray packet;
    packet.append('$');
    packet.append(escaped);
    packet.append('#');
    packet.append(QByteArray::number(checksum(escaped), 16).rightJustified(2, '0'));
    return packet;
}

QByteArray GdbRemoteProbe::decodePayload(const QByteArray &raw)
{
    QByteArray out;
    for (int i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '}' && i + 1 < raw.size()) {
            out.append(char(raw[++i] ^ 0x20));
        } else if (c == '*' && i + 1 < raw.size() && !out.isEmpty()) {
            const int count = quint8(raw[++i]) - 29; // 游程编码：重复前一个字符
            out.append(QByteArray(count > 0 ? count : 0, out.back()));
        } else {
            out.append(c);
        }
    }
    return out;
}

bool GdbRemoteProbe::open(quint16 port, int timeoutMs, const std::function<bool()> &alive)
{
    close();
    m_buffer.clear();
    m_running = false;
    m_registerMap.clear();
    m_registerMap.insert(Probe::XPSR, kLegacyXpsr);

    QElapsedTimer timer;
    timer.start();
    while (true) {
        m_socket.connectToHost(QStringLiteral("127.0.0.1"), port);
        if (m_socket.waitForConnected(500))
            break;
        m_socket.abort();
        if (cancelled())
            return setError(QObject::tr("Cancelled"));
        if (alive && !alive())
            return setError(QObject::tr("The GDB server exited before accepting a connection"));
        if (timer.elapsed() >= timeoutMs)
            return setError(QObject::tr("Cannot connect to the GDB server on port %1").arg(port));
        QThread::msleep(100);
    }
    m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);

    QByteArray reply;
    if (transact("qSupported:swbreak+;hwbreak+", &reply)) {
        for (const QByteArray &item : reply.split(';')) {
            if (item.startsWith("PacketSize=")) {
                bool ok = false;
                const int size = item.mid(11).toInt(&ok, 16);
                if (ok && size >= 64)
                    m_packetSize = size;
            }
        }
    }

    // 查询一次停止状态，确认服务器已经连上目标
    if (!transact("?", &reply))
        return false;
    resolveRegisters();
    return true;
}

void GdbRemoteProbe::close()
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        if (m_running)
            interrupt();
        QByteArray reply;
        transact("D", &reply, 1000);
    }
    m_socket.abort();
    m_buffer.clear();
    m_running = false;
}

bool GdbRemoteProbe::fillBuffer(int timeoutMs)
{
    if (!m_socket.bytesAvailable() && !m_socket.waitForReadyRead(timeoutMs))
        return false;
    m_buffer.append(m_socket.readAll());
    return true;
}

bool GdbRemoteProbe::sendPacket(const QByteArray &payload)
{
    const QByteArray packet = encodePacket(payload);
    for (int attempt = 0; attempt < kMaxResend; ++attempt) {
        m_socket.write(packet);
        if (!m_socket.waitForBytesWritten(2000))
            return setError(QObject::tr("Failed to send to the GDB server"));

        // 等对方的 '+'（收到）或 '-'（要求重发）
        QElapsedTimer timer;
        timer.start();
        while (true) {
            const int ack = m_buffer.indexOf('+');
            const int nak = m_buffer.indexOf('-');
            if (ack >= 0 && (nak < 0 || ack < nak)) {
                m_buffer.remove(0, ack + 1);
                return true;
            }
            if (nak >= 0) {
                m_buffer.remove(0, nak + 1);
                break;
            }
            // 没有 ack 但已经有包头：服务器可能工作在不回 ack 的模式，直接当作成功
            if (m_buffer.contains('$'))
                return true;
            const int remaining = 2000 - int(timer.elapsed());
            if (remaining <= 0 || !fillBuffer(remaining))
                return setError(QObject::tr("The GDB server did not respond"));
        }
    }
    return setError(QObject::tr("The GDB server rejected the packet"));
}

bool GdbRemoteProbe::receivePacket(QByteArray *payload, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (true) {
        const int start = m_buffer.indexOf('$');
        if (start >= 0) {
            const int hash = m_buffer.indexOf('#', start);
            if (hash >= 0 && m_buffer.size() >= hash + 3) {
                const QByteArray raw = m_buffer.mid(start + 1, hash - start - 1);
                const QByteArray sumText = m_buffer.mid(hash + 1, 2);
                m_buffer.remove(0, hash + 3);
                bool ok = false;
                const int expected = sumText.toInt(&ok, 16);
                if (!ok || expected != checksum(raw)) {
                    m_socket.write("-");
                    continue;
                }
                m_socket.write("+");
                m_socket.flush();
                *payload = decodePayload(raw);
                return true;
            }
        }
        const int remaining = timeoutMs - int(timer.elapsed());
        if (remaining <= 0 || !fillBuffer(remaining))
            return setError(QObject::tr("The GDB server did not respond"));
    }
}

bool GdbRemoteProbe::transact(const QByteArray &payload, QByteArray *reply, int timeoutMs)
{
    if (!isOpen())
        return setError(QObject::tr("Not connected to the GDB server"));
    if (!sendPacket(payload))
        return false;
    while (true) {
        if (!receivePacket(reply, timeoutMs))
            return false;
        // 服务器经 O 包转发的控制台输出（"OK" 只有两个字节，不算）
        if (reply->size() > 2 && reply->startsWith('O'))
            continue;
        return true;
    }
}

bool GdbRemoteProbe::expectOk(const QByteArray &payload)
{
    QByteArray reply;
    if (!transact(payload, &reply))
        return false;
    if (reply != "OK")
        return setError(QObject::tr("The GDB server replied \"%1\"").arg(QString::fromLatin1(reply)));
    return true;
}

bool GdbRemoteProbe::monitor(const QString &command)
{
    // 回复是 OK，或者（J-Link 等）直接是命令输出文本的十六进制；E 开头是错误
    QByteArray reply;
    if (!transact("qRcmd," + command.toLatin1().toHex(), &reply))
        return false;
    if (reply == "OK")
        return true;
    const bool isErrorCode = reply.size() == 3 && reply[0] == 'E';
    if (isErrorCode || reply.isEmpty())
        return setError(QObject::tr("The GDB server replied \"%1\"").arg(QString::fromLatin1(reply)));
    return true;
}

bool GdbRemoteProbe::interrupt()
{
    m_socket.write("\x03", 1);
    m_socket.waitForBytesWritten(1000);
    QByteArray reply;
    while (receivePacket(&reply, kInterruptReplyMs)) {
        if (isStopReply(reply)) {
            m_running = false;
            return true;
        }
    }
    m_running = false;
    return false;
}

bool GdbRemoteProbe::halt()
{
    if (m_running)
        return interrupt();
    return monitor(QStringLiteral("halt"));
}

bool GdbRemoteProbe::reset()
{
    if (m_running && !interrupt())
        return false;
    if (!monitor(m_resetCommand))
        return false;
    return monitor(QStringLiteral("halt"));
}

bool GdbRemoteProbe::resume()
{
    if (!isOpen())
        return setError(QObject::tr("Not connected to the GDB server"));
    if (!sendPacket("c"))
        return false;
    m_running = true;
    return true;
}

bool GdbRemoteProbe::readXml(const QString &annex, QString *xml)
{
    xml->clear();
    QByteArray text;
    while (true) {
        QByteArray reply;
        const QByteArray request = "qXfer:features:read:" + annex.toLatin1() + ":"
                + QByteArray::number(text.size(), 16) + "," + QByteArray::number(0x400, 16);
        if (!transact(request, &reply) || reply.isEmpty()
            || (reply[0] != 'm' && reply[0] != 'l'))
            return false;
        text.append(reply.mid(1));
        if (reply[0] == 'l')
            break;
        if (text.size() > 256 * 1024)
            return false;
    }
    *xml = QString::fromUtf8(text);
    return true;
}

QHash<int, int> GdbRemoteProbe::parseRegisterMap(const QString &targetXml,
                                                 const std::function<QString(const QString &)> &include)
{
    QHash<int, int> map;
    int next = 0;

    // 按文档顺序给 <reg> 编号（regnum 属性能改写编号）；<xi:include> 就地展开
    std::function<void(const QString &, int)> walk = [&](const QString &xml, int depth) {
        QXmlStreamReader reader(xml);
        while (!reader.atEnd()) {
            if (reader.readNext() != QXmlStreamReader::StartElement)
                continue;
            if (reader.name() == QLatin1String("include") && include && depth < kMaxXmlIncludeDepth) {
                const QString href = reader.attributes().value(QStringLiteral("href")).toString();
                if (!href.isEmpty())
                    walk(include(href), depth + 1);
            } else if (reader.name() == QLatin1String("reg")) {
                const auto attributes = reader.attributes();
                bool ok = false;
                const int regnum = attributes.value(QStringLiteral("regnum")).toInt(&ok);
                if (ok)
                    next = regnum;
                const QString name = attributes.value(QStringLiteral("name")).toString().toLower();
                int probeRegister = -1;
                if (name.size() >= 2 && name[0] == QLatin1Char('r') && name.mid(1).toInt(&ok) >= 0 && ok
                    && name.mid(1).toInt() <= 12)
                    probeRegister = name.mid(1).toInt();
                else if (name == QLatin1String("sp") || name == QLatin1String("r13"))
                    probeRegister = Probe::SP;
                else if (name == QLatin1String("lr") || name == QLatin1String("r14"))
                    probeRegister = Probe::LR;
                else if (name == QLatin1String("pc") || name == QLatin1String("r15"))
                    probeRegister = Probe::PC;
                else if (name == QLatin1String("xpsr") || name == QLatin1String("cpsr")
                         || name == QLatin1String("psr"))
                    probeRegister = Probe::XPSR;
                if (probeRegister >= 0 && !map.contains(probeRegister))
                    map.insert(probeRegister, next);
                ++next;
            }
        }
    };
    walk(targetXml, 0);
    return map;
}

void GdbRemoteProbe::resolveRegisters()
{
    // 寄存器编号以服务器给的 target.xml 为准（J-Link 的 xpsr 是 25，openocd 不是）；
    // 取不到就沿用默认编号
    QString xml;
    if (!readXml(QStringLiteral("target.xml"), &xml))
        return;
    const QHash<int, int> parsed = parseRegisterMap(xml, [this](const QString &href) {
        QString included;
        return readXml(href, &included) ? included : QString();
    });
    for (auto it = parsed.cbegin(); it != parsed.cend(); ++it)
        m_registerMap.insert(it.key(), it.value());
}

bool GdbRemoteProbe::waitHalted(int timeoutMs)
{
    if (!m_running)
        return true;

    QByteArray reply;
    QElapsedTimer timer;
    timer.start();
    while (true) {
        if (cancelled()) {
            interrupt();
            return setError(QObject::tr("Cancelled"));
        }
        const int remaining = timeoutMs - int(timer.elapsed());
        if (remaining <= 0) {
            interrupt(); // 超时：把内核强制停住，免得它在后台继续跑
            return setError(QObject::tr("Timed out waiting for the target to stop"));
        }
        if (!receivePacket(&reply, qMin(remaining, kPollSliceMs)))
            continue; // 这一小段没有数据，回到循环检查取消和总超时

        if (isStopReply(reply)) {
            m_running = false;
            return true;
        }
        if (!reply.isEmpty() && (reply[0] == 'W' || reply[0] == 'X')) {
            m_running = false;
            return setError(QObject::tr("The target exited unexpectedly"));
        }
    }
}

bool GdbRemoteProbe::readMemory(quint32 address, int length, QByteArray *out)
{
    out->clear();
    const int chunk = qMax(16, (m_packetSize - 16) / 2); // 回复是十六进制，一字节占两个字符
    for (int done = 0; done < length;) {
        const int count = qMin(chunk, length - done);
        QByteArray reply;
        if (!transact("m" + hexValue(address + quint32(done)) + "," + hexValue(quint32(count)), &reply))
            return false;
        const QByteArray bytes = QByteArray::fromHex(reply);
        if (reply.size() == 3 && reply[0] == 'E')
            return setError(QObject::tr("Cannot read target memory at 0x%1")
                                    .arg(address + quint32(done), 8, 16, QLatin1Char('0')));
        if (bytes.size() != count)
            return setError(QObject::tr("Short read from target memory at 0x%1")
                                    .arg(address + quint32(done), 8, 16, QLatin1Char('0')));
        out->append(bytes);
        done += count;
    }
    return true;
}

bool GdbRemoteProbe::writeMemory(quint32 address, const QByteArray &data)
{
    const int chunk = qMax(16, (m_packetSize - 48) / 2);
    for (int done = 0; done < data.size();) {
        const int count = qMin(chunk, int(data.size()) - done);
        const QByteArray request = "M" + hexValue(address + quint32(done)) + ","
                + hexValue(quint32(count)) + ":" + data.mid(done, count).toHex();
        if (!expectOk(request))
            return setError(QObject::tr("Cannot write target memory at 0x%1: %2")
                                    .arg(address + quint32(done), 8, 16, QLatin1Char('0'))
                                    .arg(lastError()));
        done += count;
    }
    return true;
}

bool GdbRemoteProbe::readRegister(int index, quint32 *value)
{
    QByteArray reply;
    if (!transact("p" + hexValue(quint32(registerNumber(index))), &reply))
        return false;
    const QByteArray bytes = QByteArray::fromHex(reply);
    if (bytes.size() != 4)
        return setError(QObject::tr("Cannot read register %1").arg(index));
    *value = 0;
    for (int i = 0; i < 4; ++i)
        *value |= quint32(quint8(bytes[i])) << (8 * i);
    return true;
}

bool GdbRemoteProbe::writeRegister(int index, quint32 value)
{
    if (!expectOk("P" + hexValue(quint32(registerNumber(index))) + "=" + littleEndianHex(value)))
        return setError(QObject::tr("Cannot write register %1: %2").arg(index).arg(lastError()));
    return true;
}

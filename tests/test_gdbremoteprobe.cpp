#include "flash/gdbremoteprobe.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <thread>

namespace {

// 假的 GDB Server：在自己的线程里收包、按内容回复，记录收到的请求
class FakeGdbServer : public QThread
{
public:
    quint16 port() const { return m_port; }
    QStringList requests() const
    {
        QMutexLocker lock(&m_mutex);
        return m_requests;
    }
    void stopServer() { m_stop = true; }

    // 对 'c' 的处理：true 立即回 T05，false 一直不回（等 Ctrl-C）
    std::atomic_bool stopImmediately{true};
    std::atomic_bool corruptNextChecksum{false};
    // 提供 target.xml 时，xpsr 的编号是 16（像 openocd），而不是传统的 25
    std::atomic_bool serveTargetXml{false};

    void start()
    {
        QThread::start();
        m_ready.acquire();
    }

protected:
    void run() override
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost, 0);
        m_port = server.serverPort();
        m_ready.release();

        if (!server.waitForNewConnection(5000))
            return;
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray buffer;
        while (!m_stop && socket->state() == QAbstractSocket::ConnectedState) {
            if (!socket->waitForReadyRead(50))
                continue;
            buffer.append(socket->readAll());
            while (true) {
                if (!buffer.isEmpty() && buffer[0] == '\x03') {
                    buffer.remove(0, 1);
                    record("<ctrl-c>");
                    send(socket, "T02");
                    continue;
                }
                const int start = buffer.indexOf('$');
                const int hash = buffer.indexOf('#', start);
                if (start < 0 || hash < 0 || buffer.size() < hash + 3)
                    break;
                const QByteArray raw = buffer.mid(start + 1, hash - start - 1);
                buffer.remove(0, hash + 3);
                socket->write("+");
                handle(socket, GdbRemoteProbe::decodePayload(raw));
            }
        }
        socket->disconnectFromHost();
    }

private:
    static QByteArray le(quint32 value)
    {
        QByteArray b(4, '\0');
        for (int i = 0; i < 4; ++i)
            b[i] = char((value >> (8 * i)) & 0xFF);
        return b.toHex();
    }

    void record(const QString &text)
    {
        QMutexLocker lock(&m_mutex);
        m_requests.append(text);
    }

    void send(QTcpSocket *socket, const QByteArray &payload)
    {
        QByteArray packet = GdbRemoteProbe::encodePacket(payload);
        if (corruptNextChecksum.exchange(false))
            packet[packet.size() - 1] = packet[packet.size() - 1] == '0' ? '1' : '0';
        socket->write(packet);
        socket->flush();
    }

    void handle(QTcpSocket *socket, const QByteArray &payload)
    {
        record(QString::fromLatin1(payload));
        if (payload.startsWith("qSupported")) {
            send(socket, "PacketSize=100;swbreak+");
        } else if (payload.startsWith("qXfer:features:read:target.xml:")) {
            if (!serveTargetXml) {
                send(socket, "");
                return;
            }
            const QByteArray xml =
                    "<target><feature><reg name=\"r0\"/><reg name=\"r1\"/><reg name=\"r2\"/><reg name=\"r3\"/>"
                    "<reg name=\"r4\"/><reg name=\"r5\"/><reg name=\"r6\"/><reg name=\"r7\"/><reg name=\"r8\"/>"
                    "<reg name=\"r9\"/><reg name=\"r10\"/><reg name=\"r11\"/><reg name=\"r12\"/><reg name=\"sp\"/>"
                    "<reg name=\"lr\"/><reg name=\"pc\"/><reg name=\"xpsr\"/></feature></target>";
            send(socket, "l" + xml);
        } else if (payload == "?") {
            send(socket, "T05");
        } else if (payload.startsWith('m')) {
            const QList<QByteArray> parts = payload.mid(1).split(',');
            const quint32 address = parts[0].toUInt(nullptr, 16);
            const int length = parts[1].toInt(nullptr, 16);
            if (address == 0xDEAD0000u) {
                send(socket, "E14");
                return;
            }
            QByteArray data(length, '\0');
            for (int i = 0; i < length; ++i)
                data[i] = char((address + quint32(i)) & 0xFF);
            send(socket, data.toHex());
        } else if (payload.startsWith('M')) {
            send(socket, "OK");
        } else if (payload.startsWith('p')) {
            send(socket, le(0x11223300u + payload.mid(1).toUInt(nullptr, 16)));
        } else if (payload.startsWith('P')) {
            send(socket, "OK");
        } else if (payload == "c") {
            if (stopImmediately)
                send(socket, "T05");
        } else if (payload.startsWith("qRcmd,")) {
            send(socket, "O" + QByteArray("console text").toHex()); // 控制台输出，客户端应跳过
            // J-Link 的 reset 回复是输出文本的十六进制而不是 OK
            send(socket, payload.endsWith("7265736574") ? QByteArray("526573657474696e67") : QByteArray("OK"));
        } else if (payload == "D") {
            send(socket, "OK");
        } else {
            send(socket, "");
        }
    }

    mutable QMutex m_mutex;
    QStringList m_requests;
    QSemaphore m_ready;
    std::atomic_bool m_stop{false};
    quint16 m_port = 0;
};

} // namespace

class TestGdbRemoteProbe : public QObject
{
    Q_OBJECT

private slots:
    void encodesPackets()
    {
        QCOMPARE(GdbRemoteProbe::encodePacket("OK"), QByteArray("$OK#9a"));
        QCOMPARE(GdbRemoteProbe::encodePacket("g"), QByteArray("$g#67"));
        // 转义：$ # } * 变成 } 加上异或 0x20
        QCOMPARE(GdbRemoteProbe::escapePayload("a#b$c}d*"), QByteArray("a}\x03" "b}\x04" "c}]d}\x0a"));
        const QByteArray tricky("x$#}*y");
        QCOMPARE(GdbRemoteProbe::decodePayload(GdbRemoteProbe::escapePayload(tricky)), tricky);
        // 游程编码：0* 表示再重复 '0' 若干次（'*' 后的字符减 29）
        QCOMPARE(GdbRemoteProbe::decodePayload("0* "), QByteArray("0000"));
    }

    void readsAndWritesMemoryInChunks()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY2(probe.open(server.port(), 3000), qPrintable(probe.lastError()));

        QByteArray data;
        QVERIFY2(probe.readMemory(0x1000, 300, &data), qPrintable(probe.lastError()));
        QCOMPARE(data.size(), 300);
        for (int i = 0; i < 300; ++i)
            QCOMPARE(quint8(data[i]), quint8((0x1000 + i) & 0xFF));

        QVERIFY2(probe.writeMemory(0x20000000, QByteArray(300, 'x')), qPrintable(probe.lastError()));
        probe.close();
        server.stopServer();
        server.wait();

        const QStringList requests = server.requests();
        int reads = 0;
        int writes = 0;
        for (const QString &request : requests) {
            reads += request.startsWith(QLatin1Char('m')) ? 1 : 0;
            if (request.startsWith(QLatin1Char('M'))) {
                ++writes;
                QVERIFY(request.size() <= 0x100); // 不超过协商的 PacketSize
            }
        }
        QVERIFY(reads >= 3);
        QVERIFY(writes >= 3);
        QVERIFY(requests.contains(QStringLiteral("D"))); // 关闭时 detach
    }

    void memoryErrorIsReported()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        QByteArray data;
        QVERIFY(!probe.readMemory(0xDEAD0000u, 4, &data));
        QVERIFY(probe.lastError().contains(QStringLiteral("dead0000"), Qt::CaseInsensitive));
        probe.close();
        server.stopServer();
        server.wait();
    }

    void registersAreLittleEndian()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        quint32 value = 0;
        QVERIFY(probe.readRegister(Probe::PC, &value));
        QCOMPARE(value, 0x1122330Fu);
        QVERIFY(probe.writeRegister(Probe::XPSR, 0x01000000));
        probe.close();
        server.stopServer();
        server.wait();
        QVERIFY(server.requests().contains(QStringLiteral("pf")));
        QVERIFY(server.requests().contains(QStringLiteral("P19=00000001"))); // xPSR = reg 25，小端
    }

    void resumeAndWaitHalted()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        QVERIFY(probe.resume());
        QVERIFY2(probe.waitHalted(2000), qPrintable(probe.lastError()));
        probe.close();
        server.stopServer();
        server.wait();
    }

    void waitHaltedTimeoutInterruptsTarget()
    {
        FakeGdbServer server;
        server.stopImmediately = false;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        QVERIFY(probe.resume());
        QVERIFY(!probe.waitHalted(300));
        QVERIFY(probe.lastError().contains(QStringLiteral("Timed out")));
        probe.close();
        server.stopServer();
        server.wait();
        QVERIFY(server.requests().contains(QStringLiteral("<ctrl-c>")));
    }

    void monitorSkipsConsoleOutput()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        QVERIFY2(probe.reset(), qPrintable(probe.lastError()));
        probe.close();
        server.stopServer();
        server.wait();
        QVERIFY(server.requests().contains(QStringLiteral("qRcmd,7265736574"))); // "reset"
    }

    void badChecksumIsRejected()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        // 假服务器没有重发功能，所以只检查：坏校验和的回复让这次调用失败，而不是返回错误数据
        server.corruptNextChecksum = true;
        quint32 value = 0;
        QVERIFY(!probe.readRegister(Probe::R0, &value));
        probe.close();
        server.stopServer();
        server.wait();
    }

    void registerNumbersComeFromTargetXml()
    {
        FakeGdbServer server;
        server.serveTargetXml = true;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        QVERIFY(probe.writeRegister(Probe::XPSR, 0x01000000));
        quint32 value = 0;
        QVERIFY(probe.readRegister(Probe::XPSR, &value));
        probe.close();
        server.stopServer();
        server.wait();
        QVERIFY(server.requests().contains(QStringLiteral("P10=00000001"))); // xpsr 是第 17 个 reg，编号 16
        QVERIFY(server.requests().contains(QStringLiteral("p10")));
    }

    void parsesRegisterMap()
    {
        const QHash<int, int> sequential = GdbRemoteProbe::parseRegisterMap(
                QStringLiteral("<target><feature><reg name=\"r0\"/><reg name=\"r1\"/><reg name=\"sp\"/>"
                               "<reg name=\"pc\"/><reg name=\"xpsr\" regnum=\"25\"/><reg name=\"msp\"/></feature></target>"));
        QCOMPARE(sequential.value(Probe::R1), 1);
        QCOMPARE(sequential.value(Probe::SP), 2);
        QCOMPARE(sequential.value(Probe::PC), 3);
        QCOMPARE(sequential.value(Probe::XPSR), 25); // regnum 改写编号
        QVERIFY(!sequential.contains(Probe::LR));

        // 主文件用 xi:include 引入寄存器定义，编号接着往下数
        const QHash<int, int> included = GdbRemoteProbe::parseRegisterMap(
                QStringLiteral("<target xmlns:xi=\"http://www.w3.org/2001/XInclude\"><xi:include href=\"core.xml\"/></target>"),
                [](const QString &href) {
                    return href == QLatin1String("core.xml")
                            ? QStringLiteral("<feature><reg name=\"r0\"/><reg name=\"xpsr\"/></feature>")
                            : QString();
                });
        QCOMPARE(included.value(Probe::XPSR), 1);
    }

    void batchRegisterAccess()
    {
        FakeGdbServer server;
        server.start();
        GdbRemoteProbe probe;
        QVERIFY(probe.open(server.port(), 3000));
        QVERIFY2(probe.writeRegisters({{Probe::R0, 1}, {Probe::R1, 2}, {Probe::PC, 0x20000010}}),
                 qPrintable(probe.lastError()));
        QVector<quint32> values;
        QVERIFY2(probe.readRegisters({Probe::PC, Probe::R0}, &values), qPrintable(probe.lastError()));
        QCOMPARE(values, QVector<quint32>({0x1122330Fu, 0x11223300u}));
        probe.close();
        server.stopServer();
        server.wait();
        const QStringList requests = server.requests();
        QVERIFY(requests.contains(QStringLiteral("P0=01000000")));
        QVERIFY(requests.contains(QStringLiteral("Pf=10000020")));
    }

    void cancelStopsWaitingForHalt()
    {
        FakeGdbServer server;
        server.stopImmediately = false;
        server.start();
        GdbRemoteProbe probe;
        std::atomic_bool cancel{false};
        probe.setCancelCheck([&cancel] { return cancel.load(); });
        QVERIFY(probe.open(server.port(), 3000));
        QVERIFY(probe.resume());
        // waitHalted 是同步阻塞的，没有事件循环，所以用另一个线程来点"取消"
        std::thread canceller([&cancel] {
            QThread::msleep(200);
            cancel = true;
        });
        QElapsedTimer timer;
        timer.start();
        const bool halted = probe.waitHalted(10000);
        canceller.join();
        QVERIFY(!halted);
        QVERIFY(timer.elapsed() < 3000); // 远小于 10 秒超时
        QCOMPARE(probe.lastError(), QStringLiteral("Cancelled"));
        probe.close();
        server.stopServer();
        server.wait();
        QVERIFY(server.requests().contains(QStringLiteral("<ctrl-c>")));
    }

    void cancelStopsConnecting()
    {
        GdbRemoteProbe probe;
        probe.setCancelCheck([] { return true; });
        QVERIFY(!probe.open(1, 5000)); // 没有服务器在听，应立刻因取消而返回
        QCOMPARE(probe.lastError(), QStringLiteral("Cancelled"));
    }

    void openFailsWhenServerIsGone()
    {
        GdbRemoteProbe probe;
        QVERIFY(!probe.open(1, 300, [] { return false; }));
        QVERIFY(probe.lastError().contains(QStringLiteral("exited")));
    }
};

QTEST_MAIN(TestGdbRemoteProbe)
#include "test_gdbremoteprobe.moc"

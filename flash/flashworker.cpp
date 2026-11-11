#include "flash/flashworker.h"

#include "flash/firmwareimage.h"
#include "flash/flashalgo.h"
#include "flash/flmfile.h"
#include "flash/gdbremoteprobe.h"
#include "flash/probeserver.h"

#include "common/jlinkpaths.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryFile>

namespace {

constexpr int kConnectTimeoutMs = 20000;
constexpr int kCommanderTimeoutMs = 10 * 60 * 1000;

} // namespace

FlashWorker::FlashWorker(QObject *parent)
    : QObject(parent)
{
}

// 调 JLinkExe 跑一段命令脚本；输出里要出现 okMarker 才算成功
bool FlashWorker::runJLinkCommander(const FlashRequest &request, const QString &script,
                                    const QString &okMarker, QString *error)
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("JLink.exe");
#else
    const QString name = QStringLiteral("JLinkExe");
#endif
    const QString program = JLinkPaths::findJLinkTool(name);
    if (program.isEmpty()) {
        *error = tr("J-Link software not found (%1). Install the SEGGER J-Link package.").arg(name);
        return false;
    }
    QTemporaryFile commandFile;
    if (!commandFile.open()) {
        *error = tr("Cannot create a temporary file: %1").arg(commandFile.errorString());
        return false;
    }
    commandFile.write((script + QStringLiteral("exit\n")).toLocal8Bit());
    commandFile.close();

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    JLinkPaths::hideConsoleWindow(&process);
    process.start(program, {QStringLiteral("-device"), request.device,
                            QStringLiteral("-if"), QStringLiteral("SWD"),
                            QStringLiteral("-speed"), QString::number(request.speedKHz),
                            QStringLiteral("-autoconnect"), QStringLiteral("1"),
                            QStringLiteral("-NoGui"), QStringLiteral("1"),
                            QStringLiteral("-ExitOnError"), QStringLiteral("1"),
                            QStringLiteral("-CommandFile"), commandFile.fileName()});
    if (!process.waitForStarted(5000)) {
        *error = tr("Cannot start %1: %2").arg(program, process.errorString());
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(100)) {
        if (m_cancel) {
            process.kill();
            process.waitForFinished(2000);
            *error = tr("Cancelled");
            return false;
        }
        if (timer.elapsed() > kCommanderTimeoutMs) {
            process.kill();
            process.waitForFinished(2000);
            *error = tr("J-Link timed out");
            return false;
        }
    }
    const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
    emit log(output);
    // 标志要独占一行（连接时的 "Connecting to J-Link ...O.K." 不算）
    bool marked = false;
    QString errorLine;
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        const QString text = line.trimmed();
        if (text == okMarker || text.startsWith(okMarker + QLatin1Char(' ')))
            marked = true;
        if (text.contains(QLatin1String("Error")) || text.startsWith(QLatin1String("Could not")))
            errorLine = text;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !marked) {
        *error = errorLine.isEmpty() ? tr("J-Link operation failed")
                                     : tr("J-Link operation failed: %1").arg(errorLine);
        return false;
    }
    return true;
}

void FlashWorker::run(const FlashRequest &request)
{
    using Operation = FlashRequest::Operation;

    auto done = [this](bool success, const QString &message) {
        emit finished(success, message);
    };

    // 子阶段的进度映射到总进度的一段 [from, to]
    auto phase = [this](int from, int to) {
        FlashCallbacks callbacks;
        callbacks.log = [this](const QString &text) { emit log(text); };
        callbacks.progress = [this, from, to](int percent) {
            emit progress(from + percent * (to - from) / 100);
        };
        callbacks.cancelled = [this] { return m_cancel.load(); };
        return callbacks;
    };

    const QString cancelledMessage = tr("Cancelled");

    FlmFile flm;
    QString error;
    const bool needFlm = request.operation != Operation::Verify && !request.useJLinkCommander;
    const bool needFirmware = request.operation != Operation::Erase;

    if (needFlm) {
        if (!FlmFile::load(request.flmPath, &flm, &error))
            return done(false, error);
        emit log(tr("Flash algorithm: %1, %2 KB at 0x%3, page %4 bytes, %5 sector group(s)")
                         .arg(flm.deviceName)
                         .arg(flm.deviceSize / 1024)
                         .arg(flm.deviceAddress, 8, 16, QLatin1Char('0'))
                         .arg(flm.pageSize)
                         .arg(flm.sectors.size()));
    }

    QVector<FirmwareSegment> firmware;
    if (needFirmware) {
        if (!FirmwareImage::load(request.firmwarePath, request.binAddress, &firmware, &error))
            return done(false, error);
        emit log(tr("Firmware: %1 bytes in %2 segment(s)")
                         .arg(FirmwareImage::totalSize(firmware)).arg(firmware.size()));
    }

    if (m_cancel)
        return done(false, cancelledMessage);

    // 不用 FLM：先由 J-Link Commander 完成擦除/下载，之后下面的流程只做校验和复位
    if (request.useJLinkCommander) {
        QString commanderError;
        if (request.operation == Operation::Erase) {
            emit log(tr("Erasing with J-Link"));
            if (!runJLinkCommander(request, QStringLiteral("erase\n"), QStringLiteral("Erasing done"), &commanderError))
                return done(false, m_cancel ? cancelledMessage : commanderError);
            emit progress(100);
            return done(true, tr("Erase completed"));
        }
        emit log(tr("Programming with J-Link"));
        QString script;
        if (firmware.size() == 1 && request.firmwarePath.endsWith(QLatin1String(".bin"), Qt::CaseInsensitive))
            script = QStringLiteral("loadbin \"%1\", 0x%2\n")
                             .arg(QDir::toNativeSeparators(request.firmwarePath)).arg(request.binAddress, 0, 16);
        else
            script = QStringLiteral("loadfile \"%1\"\n").arg(QDir::toNativeSeparators(request.firmwarePath));
        if (!runJLinkCommander(request, script, QStringLiteral("O.K."), &commanderError))
            return done(false, m_cancel ? cancelledMessage : commanderError);
        emit progress(50);
    }

    ProbeServer server;
    ProbeServer::Config config;
    const bool dapLink = request.probe == FlashRequest::ProbeType::DapLink;
    config.kind = dapLink ? ProbeServer::Kind::OpenOcdCmsisDap : ProbeServer::Kind::JLink;
    config.device = request.device;
    config.speedKHz = request.speedKHz;
    GdbRemoteProbe probe;
    probe.setCancelCheck([this] { return m_cancel.load(); });
    if (dapLink)
        probe.setResetCommand(QStringLiteral("reset halt"));

    // 服务器端口是先探测空闲再交给它绑定的，中间可能被别的进程抢走；这种情况下服务器会立刻退出，换端口重试一次
    QString connectError;
    bool connected = false;
    for (int attempt = 0; attempt < 2 && !connected; ++attempt) {
        if (dapLink)
            emit log(tr("Starting OpenOCD for DAPLink"));
        else
            emit log(tr("Starting J-Link GDB server for %1").arg(request.device));
        if (!server.start(config, &error))
            return done(false, error);
        if (probe.open(server.port(), kConnectTimeoutMs, [&server] { return server.isAlive(); })) {
            connected = true;
            break;
        }
        connectError = probe.lastError();
        const QString serverOutput = server.output().trimmed();
        if (!serverOutput.isEmpty())
            connectError += QLatin1Char('\n') + serverOutput;
        const bool targetProblem = serverOutput.contains(QLatin1String(dapLink ? "Error:" : "Could not connect"));
        if (m_cancel || targetProblem || server.isAlive())
            break;
        server.stop();
    }
    if (!connected)
        return done(false, m_cancel ? cancelledMessage : connectError);
    emit log(tr("Connected to the target"));

    if (m_cancel)
        return done(false, cancelledMessage);
    if (!probe.reset()) {
        emit log(tr("Reset failed (%1), trying to halt only").arg(probe.lastError()));
        if (!probe.halt())
            return done(false, probe.lastError());
    }

    bool success = false;
    QString message;

    switch (request.operation) {
    case Operation::Erase: {
        FlashAlgo algo(probe, flm, request.ramAddress, request.ramSize, phase(0, 100));
        success = algo.load() && algo.eraseChip();
        message = success ? tr("Erase completed") : algo.error();
        break;
    }
    case Operation::Program: {
        if (!request.useJLinkCommander) {
            FlashAlgo algo(probe, flm, request.ramAddress, request.ramSize, phase(0, 50));
            if (!algo.load() || !algo.program(firmware)) {
                message = algo.error();
                break;
            }
        }
        emit log(tr("Verifying"));
        if (!FlashAlgo::verify(probe, firmware, phase(50, 100), &message))
            break;
        success = true;
        message = tr("Program completed and verified");
        break;
    }
    case Operation::Verify:
        emit log(tr("Verifying"));
        success = FlashAlgo::verify(probe, firmware, phase(0, 100), &message);
        if (success)
            message = tr("Verify passed");
        break;
    }

    if (success)
        emit progress(100);

    if (success && request.runAfter && request.operation != Operation::Erase) {
        // 复位后内核停在复位向量，随后 detach，J-Link 服务器会放开内核让程序跑起来
        // OpenOCD 在 detach 时不会放开内核，所以复位后要明确让它跑起来
        if (!probe.reset() || (dapLink && !probe.runFreely(QStringLiteral("resume"))))
            message += QLatin1Char('\n') + tr("Warning: the target could not be reset: %1").arg(probe.lastError());
    } else if (!success) {
        // 失败/取消：内核可能还停在算法的 RAM 残片里，复位一次让目标回到干净的状态
        if (!probe.reset())
            emit log(tr("Reset after failure did not succeed: %1").arg(probe.lastError()));
    }
    probe.close();
    server.stop();
    done(success, message);
}

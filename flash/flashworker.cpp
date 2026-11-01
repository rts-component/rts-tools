#include "flash/flashworker.h"

#include "flash/firmwareimage.h"
#include "flash/flashalgo.h"
#include "flash/flmfile.h"
#include "flash/gdbremoteprobe.h"
#include "flash/probeserver.h"

namespace {

constexpr int kConnectTimeoutMs = 20000;

} // namespace

FlashWorker::FlashWorker(QObject *parent)
    : QObject(parent)
{
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
    const bool needFlm = request.operation != Operation::Verify;
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

    ProbeServer server;
    ProbeServer::Config config;
    config.device = request.device;
    config.speedKHz = request.speedKHz;
    GdbRemoteProbe probe;
    probe.setCancelCheck([this] { return m_cancel.load(); });

    // 服务器端口是先探测空闲再交给它绑定的，中间可能被别的进程抢走；这种情况下服务器会立刻退出，换端口重试一次
    QString connectError;
    bool connected = false;
    for (int attempt = 0; attempt < 2 && !connected; ++attempt) {
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
        const bool targetProblem = serverOutput.contains(QLatin1String("Could not connect"));
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
        FlashAlgo algo(probe, flm, request.ramAddress, request.ramSize, phase(0, 50));
        if (!algo.load() || !algo.program(firmware)) {
            message = algo.error();
            break;
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
        if (!probe.reset())
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

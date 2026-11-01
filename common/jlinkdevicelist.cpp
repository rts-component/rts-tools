#include "common/jlinkdevicelist.h"

#include "common/jlinkpaths.h"
#include "common/jsonstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringListModel>

namespace {

QString deviceListPath()
{
    return JsonStore::settingsPath(QStringLiteral("rtt-devices.csv"));
}

} // namespace

bool JLinkDeviceList::parseLine(const QString &line, QString *device, RamArea *ram)
{
    static const QRegularExpression head(
            QStringLiteral("^\"[^\"]*\",\\s*\"([^\"]*)\",\\s*\"[^\"]*\""));
    const auto match = head.match(line);
    if (!match.hasMatch() || match.captured(1) == QLatin1String("Device"))
        return false;
    *device = match.captured(1);
    *ram = RamArea();

    // 取第 3 列之后的顶层大括号组：第 1 组是 Flash，第 2 组是 RAM
    QStringList groups;
    int depth = 0;
    int start = 0;
    for (int i = match.capturedEnd(); i < line.size(); ++i) {
        if (line[i] == QLatin1Char('{')) {
            if (depth++ == 0)
                start = i;
        } else if (line[i] == QLatin1Char('}') && depth > 0) {
            if (--depth == 0)
                groups << line.mid(start, i - start + 1);
        }
    }
    if (groups.size() < 2)
        return true;

    static const QRegularExpression pair(
            QStringLiteral("\\{\\s*(0x[0-9A-Fa-f]+)\\s*,\\s*(0x[0-9A-Fa-f]+)\\s*\\}"));
    auto it = pair.globalMatch(groups[1]);
    while (it.hasNext()) {
        const auto area = it.next();
        bool okAddress = false;
        bool okSize = false;
        const quint32 address = area.captured(1).toUInt(&okAddress, 16);
        const quint32 size = area.captured(2).toUInt(&okSize, 16);
        if (okAddress && okSize && size != 0) {
            ram->address = address;
            ram->size = size;
            break;
        }
    }
    return true;
}

JLinkDeviceList::RamArea JLinkDeviceList::ramArea(const QString &device) const
{
    return m_ram.value(device.toLower()); // 型号输入框不区分大小写，查找也一样
}

void JLinkDeviceList::reload()
{
    QStringList devices;
    m_ram.clear();
    QFile file(deviceListPath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            QString device;
            RamArea ram;
            if (!parseLine(QString::fromUtf8(file.readLine()), &device, &ram))
                continue;
            devices << device;
            if (ram.isValid() && !m_ram.contains(device.toLower()))
                m_ram.insert(device.toLower(), ram);
        }
    }
    devices.removeDuplicates();
    if (!devices.isEmpty())
        m_model->setStringList(devices);
}

JLinkDeviceList::JLinkDeviceList(QObject *parent)
    : QObject(parent)
    , m_model(new QStringListModel(this))
{
}

void JLinkDeviceList::load()
{
    reload();
    if (m_model->rowCount() == 0)
        refresh();
}

void JLinkDeviceList::refresh()
{
    if (m_process)
        return;
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("JLink.exe");
#else
    const QString name = QStringLiteral("JLinkExe");
#endif
    const QString program = JLinkPaths::findJLinkTool(name);
    if (program.isEmpty())
        return;

    QDir().mkpath(QFileInfo(deviceListPath()).absolutePath());
    m_process = new QProcess(this);
    JLinkPaths::hideConsoleWindow(m_process);
    emit refreshingChanged(true);
    connect(m_process, &QProcess::finished, this, [this] {
        if (!m_process)
            return; // 起不来/崩溃时可能收到两次，只处理第一次
        reload();
        m_process->deleteLater();
        m_process = nullptr;
        emit refreshingChanged(false);
    });
    // 起不来时 Qt 不会发 finished；崩溃时会发，不用我们补
    connect(m_process, &QProcess::errorOccurred, m_process, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_process)
            emit m_process->finished(-1, QProcess::CrashExit);
    });
    // 在设置目录里运行、只传文件名：Commander 按空格切分参数，完整路径里有空格或中文会出错
    const QFileInfo listFile(deviceListPath());
    m_process->setWorkingDirectory(listFile.absolutePath());
    m_process->start(program, {QStringLiteral("-NoGui"), QStringLiteral("1")});
    m_process->write("ExpDevList " + listFile.fileName().toLocal8Bit() + "\nexit\n");
}

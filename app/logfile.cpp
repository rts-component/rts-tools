#include "app/logfile.h"

#include "common/plotpaths.h"

#include <QDateTime>
#include <QDir>

namespace {

constexpr qint64 kCopyChunkBytes = 128 * 1024;

} // namespace

LogFile::~LogFile()
{
    close();
}

bool LogFile::open(const QString &portName, QString *error)
{
    close();

    const QString dirPath = appDataDir() + QStringLiteral("/logs");
    if (!QDir().mkpath(dirPath)) {
        if (error)
            *error = tr("Cannot create log folder: %1").arg(dirPath);
        return false;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    m_path = QStringLiteral("%1/%2_%3.log").arg(dirPath, portName, stamp);

    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        if (error) {
            *error = tr("Cannot write log file %1: %2").arg(m_path, m_file.errorString());
        }
        m_path.clear();
        return false;
    }
    return true;
}

void LogFile::close()
{
    if (!m_file.isOpen())
        return;
    m_file.flush();
    m_file.close();
}

void LogFile::write(const QString &text)
{
    if (!m_file.isOpen() || text.isEmpty())
        return;
    m_file.write(text.toUtf8());
    m_file.flush();
}

void LogFile::truncate()
{
    if (!m_file.isOpen())
        return;
    m_file.resize(0);
    m_file.seek(0);
}

void LogFile::flush()
{
    if (m_file.isOpen())
        m_file.flush();
}

bool LogFile::copyTo(const QString &target, QString *error)
{
    flush();

    QFile source(m_path);
    QFile destination(target);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = tr("Cannot save %1: %2").arg(target, destination.errorString());
        return false;
    }
    while (!source.atEnd())
        destination.write(source.read(kCopyChunkBytes));
    return true;
}

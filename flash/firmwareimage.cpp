#include "flash/firmwareimage.h"

#include "flash/elf32.h"

#include <QObject>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace FirmwareImage {

bool load(const QString &path, quint32 binAddress, QVector<FirmwareSegment> *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();

    QVector<FirmwareSegment> segments;
    if (suffix == QLatin1String("hex")) {
        if (!parseIntelHex(bytes, &segments, error))
            return false;
    } else if (suffix == QLatin1String("elf") || suffix == QLatin1String("axf")) {
        if (!parseElf(bytes, &segments, error))
            return false;
    } else {
        segments.append({binAddress, bytes});
    }

    segments = normalized(segments);
    if (segments.isEmpty() || totalSize(segments) == 0) {
        if (error)
            *error = QObject::tr("Firmware file is empty");
        return false;
    }
    *out = segments;
    return true;
}

bool parseIntelHex(const QByteArray &text, QVector<FirmwareSegment> *out, QString *error)
{
    auto fail = [error](int lineNumber, const QString &message) {
        if (error)
            *error = QObject::tr("Intel HEX line %1: %2").arg(lineNumber).arg(message);
        return false;
    };

    QVector<FirmwareSegment> segments;
    quint32 upper = 0; // 扩展段/线性地址
    bool ended = false;
    int lineNumber = 0;

    for (const QByteArray &rawLine : text.split('\n')) {
        ++lineNumber;
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        if (line[0] != ':')
            return fail(lineNumber, QObject::tr("missing ':'"));

        const QByteArray bin = QByteArray::fromHex(line.mid(1));
        if (line.size() % 2 == 0 || bin.size() < 5 || bin.size() != (line.size() - 1) / 2)
            return fail(lineNumber, QObject::tr("malformed record"));

        const int length = quint8(bin[0]);
        const quint32 offset = (quint32(quint8(bin[1])) << 8) | quint8(bin[2]);
        const int type = quint8(bin[3]);
        if (bin.size() != length + 5)
            return fail(lineNumber, QObject::tr("length mismatch"));

        quint8 sum = 0;
        for (char c : bin)
            sum = quint8(sum + quint8(c));
        if (sum != 0)
            return fail(lineNumber, QObject::tr("checksum error"));

        const QByteArray payload = bin.mid(4, length);
        switch (type) {
        case 0x00: {
            const quint32 address = upper + offset;
            if (!segments.isEmpty()
                && quint64(segments.last().address) + segments.last().data.size() == address)
                segments.last().data.append(payload);
            else
                segments.append({address, payload});
            break;
        }
        case 0x01:
            ended = true;
            break;
        case 0x02:
        case 0x04:
            if (length != 2)
                return fail(lineNumber, QObject::tr("bad address record"));
            upper = ((quint32(quint8(payload[0])) << 8) | quint8(payload[1]))
                    << (type == 0x02 ? 4 : 16);
            break;
        default: // 03/05 是入口地址，烧录用不到
            break;
        }
        if (ended)
            break;
    }

    if (!ended)
        return fail(lineNumber, QObject::tr("missing end-of-file record"));
    *out = segments;
    return true;
}

bool parseElf(const QByteArray &bytes, QVector<FirmwareSegment> *out, QString *error)
{
    Elf32 elf;
    if (!Elf32::parse(bytes, &elf, error))
        return false;

    QVector<FirmwareSegment> segments;
    for (const ElfSegment &seg : elf.segments) {
        if (seg.type != ElfSegment::kTypeLoad || seg.fileSize == 0)
            continue;
        segments.append({seg.physicalAddress, elf.data.mid(qsizetype(seg.offset), qsizetype(seg.fileSize))});
    }
    if (segments.isEmpty()) {
        if (error)
            *error = QObject::tr("ELF file has no loadable segments");
        return false;
    }
    *out = segments;
    return true;
}

QVector<FirmwareSegment> normalized(QVector<FirmwareSegment> segments)
{
    segments.erase(std::remove_if(segments.begin(), segments.end(),
                                  [](const FirmwareSegment &s) { return s.data.isEmpty(); }),
                   segments.end());
    std::stable_sort(segments.begin(), segments.end(),
                     [](const FirmwareSegment &a, const FirmwareSegment &b) { return a.address < b.address; });

    QVector<FirmwareSegment> merged;
    for (const FirmwareSegment &segment : std::as_const(segments)) {
        if (!merged.isEmpty()
            && quint64(merged.last().address) + merged.last().data.size() == segment.address)
            merged.last().data.append(segment.data);
        else
            merged.append(segment);
    }
    return merged;
}

quint64 totalSize(const QVector<FirmwareSegment> &segments)
{
    quint64 total = 0;
    for (const FirmwareSegment &segment : segments)
        total += quint64(segment.data.size());
    return total;
}

} // namespace FirmwareImage

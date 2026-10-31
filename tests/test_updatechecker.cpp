#include "app/updatechecker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {
QByteArray sampleRelease()
{
    return QJsonDocument(QJsonObject{
        {"tag_name", "v0.2.0"},
        {"body", "notes"},
        {"html_url", "https://example.com/r"},
        {"assets", QJsonArray{
            QJsonObject{{"name", "rts-tools_0.2.0_x64.zip"}, {"size", 10},
                        {"browser_download_url", "https://example.com/a.zip"}},
            QJsonObject{{"name", "rts-tools_0.2.0_amd64.deb"}, {"size", 20},
                        {"browser_download_url", "https://example.com/a.deb"}}}}
    }).toJson();
}
} // namespace

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private slots:
    void isNewer_data()
    {
        QTest::addColumn<QString>("remote");
        QTest::addColumn<QString>("local");
        QTest::addColumn<bool>("expected");
        QTest::newRow("patch") << "0.1.2" << "0.1.1" << true;
        QTest::newRow("prefix") << "v0.2.0" << "0.1.9" << true;
        QTest::newRow("numeric not lexical") << "0.10.0" << "0.9.0" << true;
        QTest::newRow("same") << "v0.1.1" << "0.1.1" << false;
        QTest::newRow("older") << "0.1.0" << "0.1.1" << false;
        QTest::newRow("garbage") << "latest" << "0.1.1" << false;
    }
    void isNewer()
    {
        QFETCH(QString, remote);
        QFETCH(QString, local);
        QFETCH(bool, expected);
        QCOMPARE(UpdateChecker::isNewer(remote, local), expected);
    }

    void parseRelease_picksPlatformAsset()
    {
        const QByteArray json = sampleRelease();
        ReleaseInfo info;
        QVERIFY(UpdateChecker::parseRelease(json, ".deb", &info));
        QCOMPARE(info.version, QString("0.2.0"));
        QCOMPARE(info.assetName, QString("rts-tools_0.2.0_amd64.deb"));
        QCOMPARE(info.assetSize, qint64(20));
        QCOMPARE(info.assetUrl, QUrl("https://example.com/a.deb"));

        QVERIFY(UpdateChecker::parseRelease(json, ".pkg", &info));
        QVERIFY(info.assetUrl.isEmpty());
    }

    void parseRelease_rejectsBadJson()
    {
        ReleaseInfo info;
        QVERIFY(!UpdateChecker::parseRelease("not json", ".deb", &info));
        QVERIFY(!UpdateChecker::parseRelease(R"({"message":"Not Found"})", ".deb", &info));
    }
};

QTEST_APPLESS_MAIN(TestUpdateChecker)
#include "test_updatechecker.moc"

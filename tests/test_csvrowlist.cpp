#include "csv/csvrowlist.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

namespace {

CsvGroup makeGroup(const QString &id, int fields)
{
    CsvGroup group;
    group.id = id;
    group.name = id;
    for (int i = 0; i < fields; ++i) {
        CsvField field;
        field.name = QStringLiteral("f%1").arg(i);
        field.numeric = true;
        group.fields.append(field);
    }
    return group;
}

} // namespace

class TstCsvRowList : public QObject
{
    Q_OBJECT

private slots:
    void valuesSurviveAddRemoveMove();
    void clickOnPartsEmitsSignals();
};

void TstCsvRowList::valuesSurviveAddRemoveMove()
{
    CsvRowList list;
    QVector<CsvGroup> groups{makeGroup("a", 2), makeGroup("b", 2)};
    list.setGroups(groups);
    list.setValues(0, {"1", "2"});

    groups.append(makeGroup("c", 3));
    list.setGroups(groups);
    QCOMPARE(list.count(), 3);
    QCOMPARE(list.m_rows.at(0).values, QStringList({"1", "2"}));

    list.clearValues();
    QVERIFY(list.m_rows.at(0).values.isEmpty());

    list.setValues(0, {"1", "2"});
    groups.removeAt(2);
    list.setGroups(groups);
    QCOMPARE(list.count(), 2);
    QCOMPARE(list.m_rows.at(0).values, QStringList({"1", "2"}));
}

void TstCsvRowList::clickOnPartsEmitsSignals()
{
    CsvRowList list;
    list.resize(600, 400);
    list.show();
    list.setGroups({makeGroup("a", 2)});
    QSignalSpy enabled(&list, &CsvRowList::enabledToggled);
    QSignalSpy select(&list, &CsvRowList::selectRequested);

    // 第一行的启用勾选框：行左上 (2,2) + 勾选框位置
    QTest::mouseClick(list.viewport(), Qt::LeftButton, {}, QPoint(2 + 6 + 8, 2 + 4 + 11));
    QCOMPARE(select.count(), 1);
    QCOMPARE(enabled.count(), 1);
    QCOMPARE(enabled.first().at(1).toBool(), false);
}

QTEST_MAIN(TstCsvRowList)
#include "test_csvrowlist.moc"

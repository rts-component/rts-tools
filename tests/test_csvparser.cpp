#include "csv/csvparser.h"

#include <QtTest>

class TestCsvParser : public QObject
{
    Q_OBJECT

private slots:
    void splitPlain();
    void splitTrimsFields();
    void splitQuoted();
    void splitEscapedQuote();
    void splitMultiCharSeparator();
    void splitEmptySeparatorKeepsLine();
    void decodeSeparatorEscapes();
    void looksLikeRecord();
    void numericValues();
    void groupIdsAreUnique();
};

void TestCsvParser::splitPlain()
{
    QCOMPARE(splitCsvLine(QStringLiteral("1,2,3"), QStringLiteral(",")),
             QStringList({"1", "2", "3"}));
    QCOMPARE(splitCsvLine(QStringLiteral("a,,c"), QStringLiteral(",")),
             QStringList({"a", "", "c"}));
}

void TestCsvParser::splitTrimsFields()
{
    QCOMPARE(splitCsvLine(QStringLiteral(" 1 , 2 ,3 "), QStringLiteral(",")),
             QStringList({"1", "2", "3"}));
}

// 引号内的分隔符不切分
void TestCsvParser::splitQuoted()
{
    QCOMPARE(splitCsvLine(QStringLiteral("\"a,b\",c"), QStringLiteral(",")),
             QStringList({"a,b", "c"}));
}

void TestCsvParser::splitEscapedQuote()
{
    QCOMPARE(splitCsvLine(QStringLiteral("\"say \"\"hi\"\"\",x"), QStringLiteral(",")),
             QStringList({"say \"hi\"", "x"}));
}

void TestCsvParser::splitMultiCharSeparator()
{
    QCOMPARE(splitCsvLine(QStringLiteral("1;;2;;3"), QStringLiteral(";;")),
             QStringList({"1", "2", "3"}));
}

void TestCsvParser::splitEmptySeparatorKeepsLine()
{
    QCOMPARE(splitCsvLine(QStringLiteral("a,b"), QString()), QStringList({"a,b"}));
}

void TestCsvParser::decodeSeparatorEscapes()
{
    QCOMPARE(decodeSeparator(QStringLiteral("\\t")), QStringLiteral("\t"));
    QCOMPARE(decodeSeparator(QStringLiteral("\\n")), QStringLiteral("\n"));
    QCOMPARE(decodeSeparator(QStringLiteral(";")), QStringLiteral(";"));
}

void TestCsvParser::looksLikeRecord()
{
    QVERIFY(looksLikeCsvRecord({"1", "2"}));
    QVERIFY(looksLikeCsvRecord({"", "2"}));
    QVERIFY(!looksLikeCsvRecord({"1"}));     // 只有一列
    QVERIFY(!looksLikeCsvRecord({"", ""}));  // 全空
    QVERIFY(!looksLikeCsvRecord({}));
}

void TestCsvParser::numericValues()
{
    QVERIFY(isNumericValue(QStringLiteral("12")));
    QVERIFY(isNumericValue(QStringLiteral("-3.5e2")));
    QVERIFY(!isNumericValue(QString()));
    QVERIFY(!isNumericValue(QStringLiteral("nan")));
    QVERIFY(!isNumericValue(QStringLiteral("inf")));
    QVERIFY(!isNumericValue(QStringLiteral("12abc")));
}

void TestCsvParser::groupIdsAreUnique()
{
    QVERIFY(newGroupId() != newGroupId());
    QVERIFY(!newGroupId().contains(QLatin1Char('{')));
}

QTEST_APPLESS_MAIN(TestCsvParser)
#include "test_csvparser.moc"

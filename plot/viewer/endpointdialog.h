#ifndef ENDPOINTDIALOG_H
#define ENDPOINTDIALOG_H

#include "common/plotendpoints.h"

#include <QDialog>

class QLabel;
class QTableWidget;

// 手动选择要连接的组（读取 rts-tools 写出的端点清单）
class EndpointDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EndpointDialog(const QString &endpointsPath, QWidget *parent = nullptr);

    QString selectedGroupKey() const { return m_selectedKey; }

    void reload();

private:
    void acceptSelection();

    QString m_path;
    QString m_selectedKey;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};

#endif // ENDPOINTDIALOG_H

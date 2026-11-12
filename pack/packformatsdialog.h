#ifndef PACKFORMATSDIALOG_H
#define PACKFORMATSDIALOG_H

#include "pack/packconfig.h"

#include <QDialog>

class QListWidget;

// 管理组包格式：新增 / 重命名 / 编辑帧定义 / 删除
class PackFormatsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PackFormatsDialog(const QVector<PackFormat> &formats, QWidget *parent = nullptr);

    QVector<PackFormat> formats() const { return m_formats; }

private:
    void refreshList(int select);
    void addFormat();
    void editFormat();
    void removeFormat();
    void renameCurrent();
    QString uniqueName(const QString &base) const;

    QVector<PackFormat> m_formats;
    QListWidget *m_list = nullptr;
    bool m_updating = false;
};

#endif // PACKFORMATSDIALOG_H

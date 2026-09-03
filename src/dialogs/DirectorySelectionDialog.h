//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef DIRECTORYSELECTIONDIALOG_H
#define DIRECTORYSELECTIONDIALOG_H

#include <QDialog>
#include <QStringList>

class QFileSystemModel;
class QLineEdit;
class QPushButton;
class QTreeView;

class DirectorySelectionDialog : public QDialog {
  Q_OBJECT

public:
  explicit DirectorySelectionDialog(const QString &title,
                                    QWidget *parent = nullptr);

  QStringList selectedDirectories() const;

  static QStringList getExistingDirectories(QWidget *parent,
                                            const QString &title);

private:
  void navigateToLocation();
  void updateSelectButton();

  QFileSystemModel *mModel;
  QLineEdit *mLocation;
  QTreeView *mTree;
  QPushButton *mSelect;
};

#endif

//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef STARTDIALOG_H
#define STARTDIALOG_H

#include <QDialog>
#include <QHash>
#include <QModelIndex>
#include <optional>

class Footer;
class MainWindow;
class QDialogButtonBox;
class QListView;
class QPushButton;
class QTreeView;

class StartDialog : public QDialog {
  Q_OBJECT

public:
  StartDialog(QWidget *parent = nullptr);

  void accept() override;

  static StartDialog *openSharedInstance();

protected:
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

private:
  void updateButtons();

  void edit(const QModelIndex &index = QModelIndex());
  void remove();

  MainWindow *openWindow(
      const QString &repo,
      std::optional<bool> updateSubmodules = std::nullopt);

  QHash<QString, bool> mSubmoduleUpdateOverrides;

  QListView *mRepoList;
  Footer *mRepoFooter;
  QAction *mClone;
  QAction *mOpen;
  QAction *mInit;

  QTreeView *mHostTree;
  Footer *mHostFooter;

  QDialogButtonBox *mButtonBox;
};

#endif

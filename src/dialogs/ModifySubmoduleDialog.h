//
//          Copyright (c) 2026 NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef MODIFYSUBMODULEDIALOG_H
#define MODIFYSUBMODULEDIALOG_H

#include "git/Submodule.h"
#include <QDialog>

class QLineEdit;
class QPushButton;

class ModifySubmoduleDialog : public QDialog {
  Q_OBJECT

public:
  ModifySubmoduleDialog(const git::Submodule &submodule,
                        QWidget *parent = nullptr);

  QString name() const;
  QString path() const;
  QString url() const;
  QString branch() const;

private:
  void update();

  QLineEdit *mName;
  QLineEdit *mPath;
  QLineEdit *mUrl;
  QLineEdit *mBranch;
  QPushButton *mSave;
};

#endif

//
//          Copyright (c) 2026 NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef ADDSUBMODULEDIALOG_H
#define ADDSUBMODULEDIALOG_H

#include <QDialog>

class QLineEdit;
class QPushButton;

class AddSubmoduleDialog : public QDialog {
  Q_OBJECT

public:
  AddSubmoduleDialog(QWidget *parent = nullptr);

  QString url() const;
  QString path() const;
  QString branch() const;

private:
  void update();

  QLineEdit *mUrl;
  QLineEdit *mPath;
  QLineEdit *mBranch;
  QPushButton *mAdd;
};

#endif

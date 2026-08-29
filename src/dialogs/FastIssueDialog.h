//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//


#ifndef FASTISSUEDIALOG_H
#define FASTISSUEDIALOG_H

#include <QDialog>
#include <QPointer>

class GitHub;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QTextEdit;

class FastIssueDialog : public QDialog {
  Q_OBJECT

public:
  FastIssueDialog(GitHub *github, QWidget *parent = nullptr);

private:
  QString body() const;
  void createIssue();

  QPointer<GitHub> mGitHub;
  QLineEdit *mTitle;
  QTextEdit *mDetails;
  QCheckBox *mDiagnostics;
  QPushButton *mCreateButton;
};

#endif

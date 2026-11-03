//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef WORKTREEDIALOG_H
#define WORKTREEDIALOG_H

#include "git/Branch.h"
#include "git/Repository.h"
#include <QDialog>
#include <QSet>

class QLabel;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class ReferenceList;

class WorktreeDialog : public QDialog {
  Q_OBJECT

public:
  WorktreeDialog(const git::Repository &repo, QWidget *parent = nullptr);

  git::Branch branch() const;
  QString localBranchName() const;
  QString worktreeName() const;
  QString path() const;
  bool initializeSubmodules() const;

private:
  void selectBranch(const git::Reference &ref);
  void selectBranchSource(bool remote);
  void update();
  QString uniqueLocalBranchName(const git::Branch &branch) const;
  QString safeWorktreeName(const QString &branchName) const;

  git::Repository mRepo;
  git::Branch mBranch;
  QString mWorktreeRoot;
  QSet<QString> mWorktreeNames;

  QRadioButton *mLocalBranchSource;
  QRadioButton *mRemoteBranchSource;
  QStackedWidget *mBranchPicker;
  QStackedWidget *mLocalBranchField;
  ReferenceList *mLocalBranches;
  ReferenceList *mRemoteBranches;
  QLabel *mLocalBranchLabel;
  QLineEdit *mLocalBranchName;
  QLabel *mPath;
  QCheckBox *mInitializeSubmodules;
  QLabel *mReason;
  QPushButton *mCreate;
  QString mWorktreeName;
};

#endif

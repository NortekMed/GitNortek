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
class QLineEdit;
class QPushButton;
class ReferenceList;

class WorktreeDialog : public QDialog {
  Q_OBJECT

public:
  WorktreeDialog(const git::Repository &repo, QWidget *parent = nullptr);

  git::Branch branch() const;
  QString localBranchName() const;
  QString worktreeName() const;
  QString path() const;

private:
  void selectBranch(const git::Reference &ref);
  void update();
  QString uniqueLocalBranchName(const git::Branch &branch) const;
  QString safeWorktreeName(const QString &branchName) const;

  git::Repository mRepo;
  git::Branch mBranch;
  QString mWorktreeRoot;
  QSet<QString> mWorktreeNames;

  ReferenceList *mBranches;
  QLabel *mLocalBranchLabel;
  QLineEdit *mLocalBranchName;
  QLineEdit *mPath;
  QLabel *mReason;
  QPushButton *mCreate;
  QString mWorktreeName;
};

#endif

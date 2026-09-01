//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "WorktreeDialog.h"
#include "git/Worktree.h"
#include "ui/ReferenceList.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

WorktreeDialog::WorktreeDialog(const git::Repository &repo, QWidget *parent)
    : QDialog(parent), mRepo(repo) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Create Worktree"));

  const QList<git::Worktree> worktrees = repo.worktrees();
  for (const git::Worktree &worktree : worktrees) {
    mWorktreeNames.insert(worktree.name());
    if (worktree.isMain() && worktree.name() == QStringLiteral("Home") &&
        !worktree.path().isEmpty()) {
      mWorktreeRoot = worktree.path() + QStringLiteral(".worktrees");
    }
  }

  const auto kinds =
      ReferenceView::LocalBranches | ReferenceView::RemoteBranches;
  mBranches = new ReferenceList(repo, kinds, this);
  mBranches->setObjectName(QStringLiteral("WorktreeBranch"));
  mBranches->setAccessibleName(tr("Branch"));

  mLocalBranchLabel = new QLabel(tr("Local Branch:"), this);
  mLocalBranchName = new QLineEdit(this);
  mLocalBranchName->setObjectName(QStringLiteral("WorktreeLocalBranchName"));
  mLocalBranchName->setAccessibleName(tr("Local branch name"));
  mLocalBranchLabel->setBuddy(mLocalBranchName);

  mPath = new QLineEdit(this);
  mPath->setObjectName(QStringLiteral("WorktreeTargetPath"));
  mPath->setAccessibleName(tr("Target path"));
  mPath->setReadOnly(true);

  mInitializeSubmodules = new QCheckBox(tr("Initialize submodules"), this);
  mInitializeSubmodules->setObjectName(
      QStringLiteral("WorktreeInitializeSubmodules"));
  mInitializeSubmodules->setChecked(true);

  mReason = new QLabel(this);
  mReason->setObjectName(QStringLiteral("WorktreeValidationMessage"));
  mReason->setAccessibleName(tr("Worktree validation message"));
  mReason->setWordWrap(true);

  QLabel *branchLabel = new QLabel(tr("Branch:"), this);
  branchLabel->setBuddy(mBranches);
  QLabel *pathLabel = new QLabel(tr("Target Path:"), this);
  pathLabel->setBuddy(mPath);

  QFormLayout *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(branchLabel, mBranches);
  form->addRow(mLocalBranchLabel, mLocalBranchName);
  form->addRow(pathLabel, mPath);
  form->addRow(mInitializeSubmodules);

  QDialogButtonBox *buttons = new QDialogButtonBox(this);
  buttons->addButton(QDialogButtonBox::Cancel);
  mCreate =
      buttons->addButton(tr("Create Worktree"), QDialogButtonBox::AcceptRole);
  mCreate->setObjectName(QStringLiteral("WorktreeCreate"));
  mCreate->setAccessibleName(tr("Create Worktree"));
  mCreate->setEnabled(false);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(mReason);
  layout->addWidget(buttons);

  connect(mBranches, &ReferenceList::referenceSelected, this,
          &WorktreeDialog::selectBranch);
  connect(mLocalBranchName, &QLineEdit::textChanged, this,
          &WorktreeDialog::update);

  selectBranch(mBranches->currentReference());
}

git::Branch WorktreeDialog::branch() const { return mBranch; }

QString WorktreeDialog::localBranchName() const {
  return mBranch.isRemoteBranch() ? mLocalBranchName->text() : QString();
}

QString WorktreeDialog::worktreeName() const { return mWorktreeName; }

QString WorktreeDialog::path() const { return mPath->text(); }

bool WorktreeDialog::initializeSubmodules() const {
  return mInitializeSubmodules->isChecked();
}

void WorktreeDialog::selectBranch(const git::Reference &ref) {
  mBranch = git::Branch(ref);
  const bool remote = mBranch.isRemoteBranch();
  mLocalBranchLabel->setVisible(remote);
  mLocalBranchName->setVisible(remote);
  if (remote)
    mLocalBranchName->setText(uniqueLocalBranchName(mBranch));
  update();
}

void WorktreeDialog::update() {
  QString reason;
  QString resultingBranchName;
  QString worktreeBranchName;

  if (!mBranch.isValid() ||
      (!mBranch.isLocalBranch() && !mBranch.isRemoteBranch())) {
    reason = tr("Select a local or remote branch.");
  } else if (mBranch.isLocalBranch()) {
    resultingBranchName = mBranch.name();
    worktreeBranchName = resultingBranchName;
    if (mBranch.isCheckedOut())
      reason = tr("This branch is already checked out in another worktree.");
  } else {
    resultingBranchName = mLocalBranchName->text();
    worktreeBranchName = mBranch.name();
    if (!git::Branch::isNameValid(resultingBranchName)) {
      reason = tr("Enter a valid local branch name.");
    } else if (mRepo.lookupBranch(resultingBranchName, GIT_BRANCH_LOCAL)
                   .isValid()) {
      reason = tr("A local branch with this name already exists.");
    }
  }

  mWorktreeName = safeWorktreeName(worktreeBranchName);
  const QString targetPath = mWorktreeRoot.isEmpty() || mWorktreeName.isEmpty()
                                 ? QString()
                                 : QDir(mWorktreeRoot).filePath(mWorktreeName);
  mPath->setText(targetPath);

  if (reason.isEmpty() && mWorktreeRoot.isEmpty()) {
    reason = tr("The Home worktree path is unavailable.");
  } else if (reason.isEmpty() && QFileInfo::exists(targetPath)) {
    reason = tr("The target path already exists.");
  } else if (reason.isEmpty()) {
    const QFileInfo rootInfo(mWorktreeRoot);
    if (rootInfo.exists() && !rootInfo.isDir())
      reason = tr("The worktree root is not a directory.");
  }

  mReason->setText(reason);
  mReason->setVisible(!reason.isEmpty());
  mCreate->setEnabled(reason.isEmpty() && !targetPath.isEmpty());
}

QString WorktreeDialog::uniqueLocalBranchName(const git::Branch &branch) const {
  const QString leaf = branch.name().section('/', -1);
  if (!mRepo.lookupBranch(leaf, GIT_BRANCH_LOCAL).isValid())
    return leaf;

  const QString remote = branch.name().section('/', 0, 0);
  const QString base = remote + QLatin1Char('-') + leaf;
  QString candidate = base;
  int suffix = 2;
  while (mRepo.lookupBranch(candidate, GIT_BRANCH_LOCAL).isValid())
    candidate = base + QLatin1Char('-') + QString::number(suffix++);
  return candidate;
}

QString WorktreeDialog::safeWorktreeName(const QString &branchName) const {
  QString base;
  base.reserve(branchName.size());
  const QString invalid = QStringLiteral("<>:\"\\|?*");
  for (const QChar character : branchName) {
    if (character == QLatin1Char('/')) {
      base += QLatin1Char('-');
    } else if (character.category() == QChar::Other_Control ||
               invalid.contains(character)) {
      base += QLatin1Char('-');
    } else {
      base += character;
    }
  }

  while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' ')))
    base.chop(1);
  if (base.isEmpty())
    return QString();

  const QString device = base.section(QLatin1Char('.'), 0, 0).toUpper();
  const QSet<QString> reserved = {QStringLiteral("CON"),
                                  QStringLiteral("PRN"),
                                  QStringLiteral("AUX"),
                                  QStringLiteral("NUL"),
                                  QStringLiteral("CLOCK$")};
  const bool numberedDevice =
      device.size() == 4 &&
      (device.startsWith(QStringLiteral("COM")) ||
       device.startsWith(QStringLiteral("LPT"))) &&
      device.at(3) >= QLatin1Char('1') && device.at(3) <= QLatin1Char('9');
  if (reserved.contains(device) || numberedDevice)
    base += QLatin1Char('-');

  QString candidate = base;
  int suffix = 2;
  while (mWorktreeNames.contains(candidate) ||
         (!mWorktreeRoot.isEmpty() &&
          QFileInfo::exists(QDir(mWorktreeRoot).filePath(candidate)))) {
    candidate = base + QLatin1Char('-') + QString::number(suffix++);
  }
  return candidate;
}

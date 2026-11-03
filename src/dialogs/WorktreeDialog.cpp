//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "WorktreeDialog.h"
#include "git/Worktree.h"
#include "ui/ReferenceList.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

WorktreeDialog::WorktreeDialog(const git::Repository &repo, QWidget *parent)
    : QDialog(parent), mRepo(repo) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Create Worktree"));

  QScreen *screen = parent && parent->screen()
                        ? parent->screen()
                        : QGuiApplication::primaryScreen();
  const int maximumDialogWidth =
      screen ? qMax(320, screen->availableGeometry().width() - 32) : 1200;
  setMaximumWidth(maximumDialogWidth);

  const QList<git::Worktree> worktrees = repo.worktrees();
  for (const git::Worktree &worktree : worktrees) {
    mWorktreeNames.insert(worktree.name());
    if (worktree.isMain() && worktree.name() == QStringLiteral("Home") &&
        !worktree.path().isEmpty()) {
      mWorktreeRoot = worktree.path() + QStringLiteral(".worktrees");
    }
  }

  QWidget *branchSource = new QWidget(this);
  QHBoxLayout *branchSourceLayout = new QHBoxLayout(branchSource);
  branchSourceLayout->setContentsMargins(0, 0, 0, 0);

  mLocalBranchSource = new QRadioButton(tr("Local branch"), branchSource);
  mLocalBranchSource->setObjectName(
      QStringLiteral("WorktreeLocalBranchSource"));
  mLocalBranchSource->setAccessibleName(tr("Local branch"));
  mRemoteBranchSource = new QRadioButton(tr("Remote branch"), branchSource);
  mRemoteBranchSource->setObjectName(
      QStringLiteral("WorktreeRemoteBranchSource"));
  mRemoteBranchSource->setAccessibleName(tr("Remote branch"));
  QButtonGroup *branchSourceGroup = new QButtonGroup(branchSource);
  branchSourceGroup->setExclusive(true);
  branchSourceGroup->addButton(mLocalBranchSource);
  branchSourceGroup->addButton(mRemoteBranchSource);
  mLocalBranchSource->setChecked(true);
  branchSourceLayout->addWidget(mLocalBranchSource);
  branchSourceLayout->addWidget(mRemoteBranchSource);
  branchSourceLayout->addStretch();

  mLocalBranches = new ReferenceList(repo, ReferenceView::LocalBranches, this);
  mLocalBranches->setObjectName(QStringLiteral("WorktreeLocalBranch"));
  mLocalBranches->setAccessibleName(tr("Local branch"));
  mRemoteBranches =
      new ReferenceList(repo, ReferenceView::RemoteBranches, this);
  mRemoteBranches->setObjectName(QStringLiteral("WorktreeRemoteBranch"));
  mRemoteBranches->setAccessibleName(tr("Remote branch"));
  mBranchPicker = new QStackedWidget(this);
  mBranchPicker->setObjectName(QStringLiteral("WorktreeBranchPicker"));
  mBranchPicker->setAccessibleName(tr("Branch"));
  mBranchPicker->addWidget(mLocalBranches);
  mBranchPicker->addWidget(mRemoteBranches);
  mBranchPicker->setCurrentWidget(mLocalBranches);

  bool hasRemoteBranches = false;
  for (const git::Branch &branch : repo.branches(GIT_BRANCH_REMOTE)) {
    if (!branch.isRemoteHead()) {
      hasRemoteBranches = true;
      break;
    }
  }
  mRemoteBranchSource->setEnabled(hasRemoteBranches);
  if (!hasRemoteBranches)
    mRemoteBranchSource->setToolTip(
        tr("Fetch a remote branch before creating a worktree from it."));

  mLocalBranchLabel = new QLabel(tr("Local Branch:"), this);
  mLocalBranchLabel->setAccessibleName(tr("Local branch name"));
  mLocalBranchLabel->setMinimumWidth(mLocalBranchLabel->sizeHint().width());
  mLocalBranchName = new QLineEdit(this);
  mLocalBranchName->setObjectName(QStringLiteral("WorktreeLocalBranchName"));
  mLocalBranchName->setAccessibleName(tr("Local branch name"));
  mLocalBranchLabel->setBuddy(mLocalBranchName);
  mLocalBranchField = new QStackedWidget(this);
  mLocalBranchField->setObjectName(QStringLiteral("WorktreeLocalBranchField"));
  QWidget *emptyLocalBranchField = new QWidget(mLocalBranchField);
  QHBoxLayout *emptyLocalBranchLayout = new QHBoxLayout(emptyLocalBranchField);
  emptyLocalBranchLayout->setContentsMargins(0, 0, 0, 0);
  emptyLocalBranchLayout->addItem(
      new QSpacerItem(mLocalBranchName->sizeHint().width(),
                      mLocalBranchName->sizeHint().height(), QSizePolicy::Fixed,
                      QSizePolicy::Fixed));
  mLocalBranchField->addWidget(emptyLocalBranchField);
  mLocalBranchField->addWidget(mLocalBranchName);
  mLocalBranchField->setCurrentWidget(emptyLocalBranchField);

  mPath = new QLabel(this);
  mPath->setObjectName(QStringLiteral("WorktreeTargetPath"));
  mPath->setAccessibleName(tr("Target path"));
  mPath->setTextFormat(Qt::PlainText);
  mPath->setWordWrap(true);
  mPath->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::TextSelectableByKeyboard);
  mPath->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
  mPath->setMargin(4);
  mPath->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  mInitializeSubmodules = new QCheckBox(tr("Initialize submodules"), this);
  mInitializeSubmodules->setObjectName(
      QStringLiteral("WorktreeInitializeSubmodules"));
  mInitializeSubmodules->setChecked(true);

  mReason = new QLabel(this);
  mReason->setObjectName(QStringLiteral("WorktreeValidationMessage"));
  mReason->setAccessibleName(tr("Worktree validation message"));
  mReason->setWordWrap(true);
  mReason->setMinimumHeight(2 * mReason->fontMetrics().height());

  QLabel *branchSourceLabel = new QLabel(tr("Start from:"), this);
  branchSourceLabel->setBuddy(mLocalBranchSource);
  QLabel *branchLabel = new QLabel(tr("Branch:"), this);
  branchLabel->setBuddy(mBranchPicker);
  QLabel *pathLabel = new QLabel(tr("Target Path:"), this);
  pathLabel->setBuddy(mPath);

  QFormLayout *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::DontWrapRows);
  form->addRow(branchSourceLabel, branchSource);
  form->addRow(branchLabel, mBranchPicker);
  form->addRow(mLocalBranchLabel, mLocalBranchField);
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

  const int labelColumnWidth = qMax(qMax(branchSourceLabel->sizeHint().width(),
                                         branchLabel->sizeHint().width()),
                                    qMax(mLocalBranchLabel->sizeHint().width(),
                                         pathLabel->sizeHint().width()));
  int formSpacing = form->horizontalSpacing();
  if (formSpacing < 0)
    formSpacing =
        style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, this);
  const QMargins formMargins = form->contentsMargins();
  const QMargins layoutMargins = layout->contentsMargins();
  const int fixedHorizontalSpace = labelColumnWidth + formSpacing +
                                   formMargins.left() + formMargins.right() +
                                   layoutMargins.left() + layoutMargins.right();
  const int maximumFieldWidth =
      qMax(1, maximumDialogWidth - fixedHorizontalSpace);
  mPath->setMaximumWidth(maximumFieldWidth);

  QSize branchPickerSize = mLocalBranches->sizeHint();
  branchPickerSize =
      branchPickerSize.expandedTo(mLocalBranches->minimumSizeHint())
          .expandedTo(mRemoteBranches->sizeHint())
          .expandedTo(mRemoteBranches->minimumSizeHint());
  branchPickerSize.setWidth(qMin(branchPickerSize.width(), maximumFieldWidth));
  mLocalBranches->setFixedSize(branchPickerSize);
  mRemoteBranches->setFixedSize(branchPickerSize);
  mBranchPicker->setFixedSize(branchPickerSize);

  connect(mLocalBranches, &ReferenceList::referenceSelected, this,
          [this](const git::Reference &ref) {
            if (mLocalBranchSource->isChecked())
              selectBranch(ref);
          });
  connect(mRemoteBranches, &ReferenceList::referenceSelected, this,
          [this](const git::Reference &ref) {
            if (mRemoteBranchSource->isChecked())
              selectBranch(ref);
          });
  connect(mLocalBranchSource, &QRadioButton::toggled, this,
          [this](bool checked) {
            if (checked)
              selectBranchSource(false);
          });
  connect(mRemoteBranchSource, &QRadioButton::toggled, this,
          [this](bool checked) {
            if (checked)
              selectBranchSource(true);
          });
  connect(mLocalBranchName, &QLineEdit::textChanged, this,
          &WorktreeDialog::update);

  selectBranch(mLocalBranches->currentReference());

  QString widestTargetPath;
  int widestTargetPathWidth = 0;
  const QFontMetrics pathMetrics = mPath->fontMetrics();
  auto considerTargetPaths =
      [this, &widestTargetPath, &widestTargetPathWidth,
       &pathMetrics](const QList<git::Branch> &branches) {
        for (const git::Branch &branch : branches) {
          if (branch.isRemoteHead())
            continue;

          const QString name = safeWorktreeName(branch.name());
          if (name.isEmpty())
            continue;

          const QString path = QDir(mWorktreeRoot).filePath(name);
          const int width = pathMetrics.horizontalAdvance(path);
          if (width > widestTargetPathWidth) {
            widestTargetPath = path;
            widestTargetPathWidth = width;
          }
        }
      };
  if (!mWorktreeRoot.isEmpty()) {
    considerTargetPaths(repo.branches(GIT_BRANCH_LOCAL));
    considerTargetPaths(repo.branches(GIT_BRANCH_REMOTE));
  }

  const QString selectedTargetPath = mPath->text();
  const int selectedTargetPathMinimumWidth = mPath->minimumWidth();
  if (!widestTargetPath.isEmpty()) {
    mPath->setText(widestTargetPath);
    const int pathFrameWidth = 2 * mPath->frameWidth() + 2 * mPath->margin();
    const int targetPathFieldWidth =
        qMin(widestTargetPathWidth + pathFrameWidth, maximumFieldWidth);
    mPath->setMinimumWidth(
        qMax(selectedTargetPathMinimumWidth, targetPathFieldWidth));
  }
  layout->activate();
  adjustSize();
  const int desiredWidth = qMin(
      qMax(sizeHint().width(), minimumSizeHint().width()), maximumDialogWidth);
  resize(desiredWidth, height());
  layout->activate();
  const int targetPathHeight = mPath->hasHeightForWidth()
                                   ? mPath->heightForWidth(mPath->width())
                                   : mPath->sizeHint().height();
  mPath->setMinimumHeight(qMax(mPath->minimumHeight(), targetPathHeight));
  mPath->setMinimumWidth(selectedTargetPathMinimumWidth);
  mPath->setText(selectedTargetPath);
  layout->activate();
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

void WorktreeDialog::selectBranchSource(bool remote) {
  ReferenceList *branches = remote ? mRemoteBranches : mLocalBranches;
  mBranchPicker->setCurrentWidget(branches);
  selectBranch(branches->currentReference());
}

void WorktreeDialog::selectBranch(const git::Reference &ref) {
  mBranch = git::Branch(ref);
  const bool remote = mBranch.isRemoteBranch();
  mLocalBranchLabel->setText(remote ? tr("Local Branch:") : QString());
  mLocalBranchField->setCurrentIndex(remote ? 1 : 0);
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

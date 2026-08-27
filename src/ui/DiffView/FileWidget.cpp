
#include "FileWidget.h"
#include "DisclosureButton.h"
#include "EditButton.h"
#include "DiscardButton.h"
#include "LineStats.h"
#include "FileLabel.h"
#include "ConflictResolverWidget.h"
#include "FileConflictResolverWidget.h"
#include "HunkWidget.h"
#include "Images.h"
#include "conf/Constants.h"
#include "conf/Settings.h"
#include "git/Commit.h"
#include "git/Patch.h"
#include "git2/checkout.h"
#include "git2/diff.h"
#include "ui/RepoView.h"
#include "ui/Badge.h"
#include "ui/FileContextMenu.h"
#include "git/Repository.h"

#include "git/Buffer.h"
#include "git/Blob.h"
#include "git/Id.h"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>

namespace {
bool disclosure = false;
constexpr int kMaxDisplayedChangedLines = 5000;
} // namespace

_FileWidget::Header::Header(const git::Diff &diff, const git::Patch &patch,
                            bool binary, bool lfs, bool submodule,
                            QWidget *parent)
    : QFrame(parent), mDiff(diff), mPatch(patch), mSubmodule(submodule) {
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

  QString name = patch.name();
  mCheck = new QCheckBox(this);
  mCheck->setVisible(diff.isStatusDiff());
  mCheck->setTristate(true);

  mStatusBadge = new Badge({}, this);

  git::Patch::LineStats lineStats;
  lineStats.additions = 0;
  lineStats.deletions = 0;
  mStats = new LineStats(lineStats, this);
  mStats->setVisible(false);

  mFileLabel = new FileLabel(name, submodule, this);

  QHBoxLayout *buttons = new QHBoxLayout;
  buttons->setContentsMargins(0, 0, 0, 0);
  buttons->setSpacing(4);

  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->addWidget(mCheck);
  layout->addSpacing(4);
  layout->addWidget(mStatusBadge);
  layout->addWidget(mStats);
  layout->addWidget(mFileLabel, 1);
  layout->addStretch();
  layout->addLayout(buttons);

  // Add LFS buttons.
  if (lfs) {
    Badge *lfsBadge = new Badge(
        {Badge::Label(Badge::Label::Type::LFS, FileWidget::tr("LFS"), true)},
        this);
    buttons->addWidget(lfsBadge);

    QToolButton *lfsLockButton = new QToolButton(this);
    bool locked = patch.repo().lfsIsLocked(patch.name());
    lfsLockButton->setText(locked ? FileWidget::tr("Unlock")
                                  : FileWidget::tr("Lock"));
    buttons->addWidget(lfsLockButton);

    connect(lfsLockButton, &QToolButton::clicked, [this, patch] {
      bool locked = patch.repo().lfsIsLocked(patch.name());
      RepoView::parentView(this)->lfsSetLocked({patch.name()}, !locked);
    });

    git::RepositoryNotifier *notifier = patch.repo().notifier();
    connect(notifier, &git::RepositoryNotifier::lfsLocksChanged, this,
            [patch, lfsLockButton] {
              bool locked = patch.repo().lfsIsLocked(patch.name());
              lfsLockButton->setText(locked ? FileWidget::tr("Unlock")
                                            : FileWidget::tr("Lock"));
            });

    mLfsButton = new QToolButton(this);
    mLfsButton->setText(FileWidget::tr("Show Object"));
    mLfsButton->setCheckable(true);

    buttons->addWidget(mLfsButton);
    buttons->addSpacing(8);
  }

  // Add edit button.
  mEdit = new EditButton(patch, -1, binary, lfs, this);
  mEdit->setToolTip(FileWidget::tr("Edit File"));
  buttons->addWidget(mEdit);

  // Add discard button.
  mDiscardButton = new DiscardButton(this);
  mDiscardButton->setVisible(false);
  mDiscardButton->setToolTip(FileWidget::tr("Discard File"));
  buttons->addWidget(mDiscardButton);
  connect(mDiscardButton, &QToolButton::clicked, this,
          &_FileWidget::Header::discard);

  mDisclosureButton = new DisclosureButton(this);
  mDisclosureButton->setToolTip(mDisclosureButton->isChecked()
                                    ? FileWidget::tr("Collapse File")
                                    : FileWidget::tr("Expand File"));
  connect(mDisclosureButton, &DisclosureButton::toggled, [this] {
    mDisclosureButton->setToolTip(mDisclosureButton->isChecked()
                                      ? FileWidget::tr("Collapse File")
                                      : FileWidget::tr("Expand File"));
  });
  mDisclosureButton->setVisible(disclosure);
  buttons->addWidget(mDisclosureButton);

  mMarkResolved = new QToolButton(this);
  mMarkResolved->setObjectName("ConflictMarkResolved");
  mMarkResolved->setText(HunkWidget::tr("Mark Resolved"));
  mMarkResolved->setEnabled(false);
  mMarkResolved->setStyleSheet(
      "QToolButton { background: #b8860b; color: white; border-radius: 3px; "
      "padding: 4px 10px; font-weight: 700; }"
      "QToolButton:disabled { background: palette(mid); color: "
      "palette(disabled-text); }");

  mClear = new QToolButton(this);
  mClear->setObjectName("ConflictFileClear");
  mClear->setText(HunkWidget::tr("Clear"));
  connect(mClear, &QToolButton::clicked, [this] {
    mClear->setVisible(false);
    mCurrent->setEnabled(true);
    mIncoming->setEnabled(true);
    mResolution = git::Patch::ConflictResolution::Unresolved;
    mMarkResolved->setEnabled(false);
  });

  mCurrent = new QToolButton(this);
  mCurrent->setObjectName("ConflictFileCurrent");
  mCurrent->setStyleSheet(
      Application::theme()->diffButtonStyle(Theme::Diff::Ours));
  connect(mCurrent, &QToolButton::clicked, [this] {
    mClear->setVisible(true);
    mCurrent->setEnabled(false);
    mIncoming->setEnabled(true);
    mResolution = git::Patch::ConflictResolution::Ours;
    mMarkResolved->setEnabled(!mPatch.hasMalformedConflicts());
  });

  mIncoming = new QToolButton(this);
  mIncoming->setObjectName("ConflictFileIncoming");
  mIncoming->setStyleSheet(
      Application::theme()->diffButtonStyle(Theme::Diff::Theirs));
  connect(mIncoming, &QToolButton::clicked, [this] {
    mClear->setVisible(true);
    mCurrent->setEnabled(true);
    mIncoming->setEnabled(false);
    mResolution = git::Patch::ConflictResolution::Theirs;
    mMarkResolved->setEnabled(!mPatch.hasMalformedConflicts());
  });

  buttons->addWidget(mClear);
  buttons->addWidget(mCurrent);
  buttons->addWidget(mIncoming);
  buttons->addWidget(mMarkResolved);

  updatePatch(patch);

  if (!diff.isStatusDiff())
    return;

  // Respond to check changes.
  connect(mCheck, &QCheckBox::clicked, [this](bool staged) {
    if (staged)
      emit stageStateChanged(Qt::Checked);
    else
      emit stageStateChanged(Qt::Unchecked);
  });

  // Set initial check state.
  updateCheckState();
}

void _FileWidget::Header::updatePatch(const git::Patch &patch) {
  auto status = patch.status();
  QList<Badge::Label> labels = {Badge::Label(
      Badge::Label::Type::Status, QChar(git::Diff::statusChar(status)))};

  git::Patch::LineStats lineStats = patch.lineStats();
  mStats->setStats(lineStats);
  mStats->setVisible(lineStats.additions > 0 || lineStats.deletions > 0);

  mFileLabel->setName(patch.name());
  if (status == GIT_DELTA_RENAMED)
    mFileLabel->setOldName(patch.name(git::Diff::OldFile));

  mEdit->updatePatch(patch, -1);

  auto isConflicted = status == GIT_DELTA_CONFLICTED;
  bool showFileSolverButtons = patch.count() == 0;

  if (isConflicted) {
    auto conflict = mDiff.index().conflict(patch.name());
    auto ours = QString();
    auto theirs = QString();

    mCurrent->setText(HunkWidget::tr("Current"));
    mIncoming->setText(HunkWidget::tr("Incoming"));

    if (conflict.ancestor.isNull()) {
      if (!conflict.ours.isNull()) {
        ours = "A";

        if (conflict.theirs.isNull()) {
          mIncoming->setText(tr("Incoming: Delete"));
        }
      }

      if (!conflict.theirs.isNull()) {
        theirs = "A";

        if (conflict.ours.isNull()) {
          mCurrent->setText(tr("Current: Delete"));
        }
      }

    } else {
      if (conflict.ours.isNull() && conflict.theirs.isNull()) {
        showFileSolverButtons = false;
      }

      if (conflict.ours.isNull()) {
        ours = "D";
        mCurrent->setText(tr("Current: Delete"));
      } else if (conflict.ours != conflict.ancestor) {
        ours = "M";
      }

      if (conflict.theirs.isNull()) {
        theirs = "D";
        mIncoming->setText(tr("Incoming: Delete"));
      } else if (conflict.theirs != conflict.ancestor) {
        theirs = "M";
      }
    }

    if (!ours.isEmpty() && ours == theirs) {
      labels.append(
          Badge::Label(Badge::Label::Type::Conflict, tr("both: %1").arg(ours)));
    } else {
      if (!ours.isEmpty()) {
        labels.append(Badge::Label(Badge::Label::Type::Conflict,
                                   tr("ours: %1").arg(ours)));
      }
      if (!theirs.isEmpty()) {
        labels.append(Badge::Label(Badge::Label::Type::Conflict,
                                   tr("theirs: %1").arg(theirs)));
      }
    }
  }

  mStatusBadge->setLabels(labels);

  mCurrent->setVisible(isConflicted && showFileSolverButtons);
  mIncoming->setVisible(isConflicted && showFileSolverButtons);
  mMarkResolved->setVisible(isConflicted);
  mClear->setVisible(showFileSolverButtons &&
                     mResolution != git::Patch::Unresolved);
  mCurrent->setEnabled(mResolution != git::Patch::Ours);
  mIncoming->setEnabled(mResolution != git::Patch::Theirs);

  mDiscardButton->setVisible(mDiff.isStatusDiff() && !mSubmodule &&
                             !isConflicted);
}
QCheckBox *_FileWidget::Header::check() const { return mCheck; }

DisclosureButton *_FileWidget::Header::disclosureButton() const {
  return mDisclosureButton;
}

QToolButton *_FileWidget::Header::lfsButton() const { return mLfsButton; }

void _FileWidget::Header::setStageState(git::Index::StagedState state) {
  if (state == git::Index::Staged)
    mCheck->setCheckState(Qt::Checked);
  else if (state == git::Index::Unstaged)
    mCheck->setCheckState(Qt::Unchecked);
  else
    mCheck->setCheckState(Qt::PartiallyChecked);
}

void _FileWidget::Header::mouseDoubleClickEvent(QMouseEvent *event) {
  if (mDisclosureButton->isEnabled())
    mDisclosureButton->toggle();
}

void _FileWidget::Header::contextMenuEvent(QContextMenuEvent *event) {
  RepoView *view = RepoView::parentView(this);
  FileContextMenu menu(view, {mPatch.name()}, mDiff.index());
  menu.exec(event->globalPos());
}

void _FileWidget::Header::updateCheckState() {
  bool disabled = false;
  Qt::CheckState state = Qt::Unchecked;
  switch (mDiff.index().isStaged(mPatch.name())) {
    case git::Index::Disabled:
      disabled = true;
      break;

    case git::Index::Unstaged:
      break;

    case git::Index::PartiallyStaged:
      state = Qt::PartiallyChecked;
      break;

    case git::Index::Staged:
      state = Qt::Checked;
      break;

    case git::Index::Conflicted:
      disabled = true;
      break;
  }

  mCheck->setCheckState(state);
  mCheck->setEnabled(!disabled);
}

// ###############################################################################
// ###############      FileWidget ###########################################
// ###############################################################################

FileWidget::FileWidget(DiffView *view, const git::Diff &diff,
                       const git::Patch &patch, const git::Patch &staged,
                       const QModelIndex modelIndex, const QString &name,
                       const QString &path, bool submodule, QWidget *parent)
    : QWidget(parent), mView(view), mDiff(diff), mPatch(patch), mStaged(staged),
      mModelIndex(modelIndex) {
  auto stageState = static_cast<git::Index::StagedState>(
      mModelIndex.data(Qt::CheckStateRole).toInt());
  setObjectName("FileWidget");
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  bool binary = patch.isBinary();
  if (patch.isUntracked()) {
    QFile dev(path);
    if (dev.open(QFile::ReadOnly)) {
      // Limit the read to kMaxReadBinary number of bytes to determine if the
      // file is binary or not
      QByteArray content = dev.read(kMaxReadBinary);
      git::Buffer buffer(content.constData(), content.length());
      binary = buffer.isBinary();
    }
  }

  bool lfs = patch.isLfsPointer();
  mHeader =
      new _FileWidget::Header(diff, patch, binary, lfs, submodule, parent);
  mHeader->setStageState(stageState);
  connect(mHeader, &_FileWidget::Header::stageStateChanged, this,
          &FileWidget::headerCheckStateChanged);
  connect(mHeader, &_FileWidget::Header::discard, this, &FileWidget::discard);
  connect(mHeader->markResolvedButton(), &QToolButton::clicked, this,
          &FileWidget::markResolved);
  layout->addWidget(mHeader);

  DisclosureButton *disclosureButton = mHeader->disclosureButton();
  if (disclosure)
    connect(disclosureButton, &DisclosureButton::toggled, [this](bool visible) {
      if (mHeader->lfsButton() && !visible) {
        mHunks.first()->setVisible(false);
        if (!mImages.isEmpty())
          mImages.first()->setVisible(false);
        return;
      }

      if (mHeader->lfsButton() && visible) {
        bool checked = mHeader->lfsButton()->isChecked();
        mHunks.first()->setVisible(!checked);
        if (!mImages.isEmpty())
          mImages.first()->setVisible(checked);
        return;
      }

      foreach (HunkWidget *hunk, mHunks)
        hunk->setVisible(visible);
      if (mResolver)
        mResolver->setVisible(visible);
      if (mFileResolver)
        mFileResolver->setVisible(visible);
    });

  if (diff.isStatusDiff()) {
    // Collapse on check.
    if (disclosure)
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
      connect(mHeader->check(), &QCheckBox::checkStateChanged,
              [this](Qt::CheckState state) {
#else
      connect(mHeader->check(), &QCheckBox::stateChanged, [this](int state) {
#endif
                mHeader->disclosureButton()->setChecked(state != Qt::Checked);
              });
  }

  if (patch.isConflicted() && patch.count() == 0) {
    mHeader->hideFileSolverButtons();
    mFileResolver = new FileConflictResolverWidget(
        patch, mDiff.index().conflict(patch.name()), this);
    layout->addWidget(mFileResolver, 1);
    connect(mFileResolver, &FileConflictResolverWidget::resolutionChanged, this,
            &FileWidget::updateMarkResolvedState);
    updateMarkResolvedState();
    return;
  }

  // Try to load an image from the file.
  if (binary) {
    layout->addWidget(addImage(disclosureButton, mPatch));
    return;
  }

  const git::Patch::LineStats stats = patch.lineStats();
  const int changedLines = stats.additions + stats.deletions;
  if (changedLines > kMaxDisplayedChangedLines) {
    mDiffSuppressed = true;
    QLabel *message =
        new QLabel(tr("Diff not shown because it contains %1 changed lines.")
                       .arg(changedLines),
                   this);
    message->setContentsMargins(8, 8, 8, 8);
    message->setWordWrap(true);
    layout->addWidget(message);
    return;
  }

  mHunkLayout = new QVBoxLayout();
  layout->addLayout(mHunkLayout);
  layout->addSpacerItem(new QSpacerItem(
      0, 0, QSizePolicy::Expanding,
      QSizePolicy::Expanding)); // so the hunkwidget is always starting from top
                                // and is not distributed over the hole
                                // filewidget

  updatePatch(patch, staged, name, path, submodule);

  // LFS
  if (QToolButton *lfsButton = mHeader->lfsButton()) {
    connect(lfsButton, &QToolButton::clicked,
            [this, layout, disclosureButton, lfsButton](bool checked) {
              lfsButton->setText(checked ? tr("Show Pointer")
                                         : tr("Show Object"));
              mHunks.first()->setVisible(!checked);

              // Image already loaded.
              if (!mImages.isEmpty()) {
                mImages.first()->setVisible(checked);
                return;
              }

              // Load image.
              layout->addWidget(addImage(disclosureButton, mPatch, true));
            });
  }

  // Start hidden when the file is checked.
  bool expand = (mHeader->check()->checkState() == Qt::Unchecked);

  if (Settings::instance()
              ->value(Setting::Id::AutoCollapseAddedFiles)
              .toBool() == true &&
      patch.status() == GIT_DELTA_ADDED)
    expand = false;

  if (Settings::instance()
              ->value(Setting::Id::AutoCollapseDeletedFiles)
              .toBool() == true &&
      patch.status() == GIT_DELTA_DELETED)
    expand = false;

  disclosureButton->setChecked(expand);

  updateMarkResolvedState();
}

void FileWidget::updateHunks(git::Patch stagedPatch) {
  mStaged = stagedPatch;
  for (auto hunk : mHunks)
    hunk->load(stagedPatch, true);
}

bool FileWidget::isEmpty() {
  return !mDiffSuppressed && mHunks.isEmpty() && mImages.isEmpty() &&
         !mResolver && !mFileResolver;
}

void FileWidget::setStageState(git::Index::StagedState state) {
  mHeader->setStageState(state);

  for (auto hunk : mHunks)
    hunk->setStageState(state);
}

QModelIndex FileWidget::modelIndex() { return mModelIndex; }

QToolButton *_FileWidget::Header::markResolvedButton() const {
  return mMarkResolved;
}

void _FileWidget::Header::setMarkResolvedEnabled(bool enabled) {
  mMarkResolved->setEnabled(enabled);
}

void _FileWidget::Header::hideFileSolverButtons() {
  mClear->hide();
  mCurrent->hide();
  mIncoming->hide();
}

git::Patch::ConflictResolution _FileWidget::Header::resolution() const {
  return mResolution;
}

void FileWidget::updatePatch(const git::Patch &patch, const git::Patch &staged,
                             const QString &name, const QString &path,
                             bool submodule) {
  mPatch = patch;
  mStaged = staged;
  mHeader->updatePatch(patch);

  bool lfs = patch.isLfsPointer();

  // Remove all hunks.
  QLayoutItem *child;
  while ((child = mHunkLayout->takeAt(0)) != 0) {
    if (QWidget *widget = child->widget())
      widget->deleteLater();
    delete child;
  }
  mHunks.clear();
  mResolver = nullptr;
  mFileResolver = nullptr;
  // Add untracked file content.
  if (patch.isUntracked()) {
    if (!QFileInfo(path).isDir())
      mHunkLayout->addWidget(addHunk(mDiff, patch, staged, -1, lfs, submodule));
    return;
  }

  if (patch.isConflicted() && patch.count() > 0 &&
      !patch.hasMalformedConflicts()) {
    mResolver = new ConflictResolverWidget(patch, this);
    mHunkLayout->addWidget(mResolver);
    connect(
        mResolver, &ConflictResolverWidget::completenessChanged, this,
        [this](bool complete) { mHeader->setMarkResolvedEnabled(complete); });
    updateMarkResolvedState();
    return;
  }

  // Generate a diff between the head tree and index.
  QSet<int> stagedHunks;
  if (staged.isValid()) {
    for (int i = 0; i < staged.count(); ++i)
      stagedHunks.insert(staged.lineNumber(i, 0, git::Diff::OldFile));
  }

  if (mDiff.isStatusDiff()) {
    // Partially fetching not supported yet, because then a rework
    // on the linestaging must be done
    // Add diff hunks.
    int hunkCount = patch.count();
    for (int hidx = 0; hidx < hunkCount; ++hidx) {
      HunkWidget *hunk = addHunk(mDiff, patch, staged, hidx, lfs, submodule);
      patch.lineNumber(hidx, 0, git::Diff::OldFile);
      mHunkLayout->addWidget(hunk);
    }
  } else {
    if (canFetchMore())
      fetchMore();
  }

  updateMarkResolvedState();
}

_FileWidget::Header *FileWidget::header() const { return mHeader; }

QString FileWidget::name() const { return mPatch.name(); }

QList<HunkWidget *> FileWidget::hunks() const { return mHunks; }

QWidget *FileWidget::addImage(DisclosureButton *button, const git::Patch patch,
                              bool lfs) {
  Images *images = new Images(patch, lfs, this);

  // Hide on file collapse.
  if (!lfs && disclosure)
    connect(button, &DisclosureButton::toggled, images, &QLabel::setVisible);

  // Remember image.
  mImages.append(images);

  return images;
}

HunkWidget *FileWidget::addHunk(const git::Diff &diff, const git::Patch &patch,
                                const git::Patch &staged, int index, bool lfs,
                                bool submodule) {
  HunkWidget *hunk =
      new HunkWidget(mView, diff, patch, staged, index, lfs, submodule, this);

  connect(hunk, &HunkWidget::stageStateChanged,
          [this, hunk](git::Index::StagedState state) {
            this->stageHunks(hunk, state, false);
          });
  connect(hunk, &HunkWidget::discardSignal, this, &FileWidget::discardHunk);
  connect(hunk, &HunkWidget::resolutionChanged, this,
          &FileWidget::updateMarkResolvedState);
  TextEditor *editor = hunk->editor(false);

  // Respond to editor diagnostic signal.
  connect(editor, &TextEditor::diagnosticAdded,
          [this](int line, const TextEditor::Diagnostic &diag) {
            emit diagnosticAdded(diag.kind);
          });

  // Remember hunk.
  mHunks.append(hunk);

  return hunk;
}

void FileWidget::stageHunks(const HunkWidget *hunk,
                            git::Index::StagedState stageState,
                            bool completeFile, bool completeFileStaged) {
  if (mSupressStaging)
    return;

  git::Index index = mDiff.index();
  if (!index.isValid()) // why the index can be invalid?
    return;

  int staged = 0;
  int unstaged = 0;
  for (int i = 0; i < mHunks.size(); ++i) {
    git::Index::StagedState state;
    if (mHunks[i] == hunk)
      state =
          stageState; // because the current hunk did not change the state yet.
    else
      state = mHunks[i]->stageState();
    if (state == git::Index::Staged)
      staged++;
    else if (state == git::Index::Unstaged)
      unstaged++;
  }

  mSuppressUpdate = true;

  if ((staged == mHunks.size() && mHunks.size() > 0) ||
      (completeFile && completeFileStaged)) {
    // if the file does not contain hunks, it should be always staged!
    emit stageStateChanged(mModelIndex, git::Index::Staged);
    mSuppressUpdate = false;
    return;
  } else if (completeFile && !completeFileStaged) {
    emit stageStateChanged(mModelIndex, git::Index::Unstaged);
    mSuppressUpdate = false;
    return;
  }

  if (unstaged == mHunks.size()) {
    emit stageStateChanged(mModelIndex, git::Index::Unstaged);
    mSuppressUpdate = false;
    return;
  }

  // when changing a line in a file,
  // two lines are visible, the old one and the new one.
  // When staging only one line, for example the added line,
  // then the unstaged (removed line) and the added line shall be
  // available in the file.
  QString name = mPatch.name();
  git::Repository repo = mPatch.repo();
  git::Blob blob = repo.lookupBlob(repo.workdirId(name));

  QByteArray fileContent = blob.content();

  QByteArray buffer;
  QList<QList<QByteArray>> image;
  git::Patch::populatePreimage(image, fileContent);
  for (int i = 0; i < mHunks.size(); ++i) {
    QByteArray hunk_content;
    hunk_content = mHunks[i]->apply();
    mPatch.apply(image, i, hunk_content);
  }
  buffer = mPatch.generateResult(image);

  // Add the buffer to the index.
  index.add(mPatch.name(), buffer);
  mSuppressUpdate = false;

  // TODO: index.add should notify the model directly!
  emit stageStateChanged(mModelIndex, git::Index::PartiallyStaged);
}

void FileWidget::discardHunk() {
  HunkWidget *hunk = static_cast<HunkWidget *>(QObject::sender());
  git::Repository repo = mPatch.repo();
  if (mPatch.isUntracked()) {
    repo.workdir().remove(mPatch.name());
    return;
  }

  QString name = mPatch.name();
  git::Blob blob = repo.lookupBlob(repo.workdirId(name));
  QByteArray fileContent = blob.content();

  QByteArray buffer;
  for (int i = 0; i < mHunks.size(); ++i) {
    QByteArray hunk_content;
    if (mHunks[i] == hunk) {
      hunk_content = mHunks[i]->hunk();
      buffer = mPatch.apply(i, hunk_content, fileContent);
    }
  }

  QSaveFile file(repo.workdir().filePath(name));
  if (!file.open(QFile::WriteOnly))
    return;

  file.write(buffer);
  if (!file.commit())
    return;

  // FIXME: Work dir changed?
  RepoView::parentView(this)->refresh();
}

bool FileWidget::canFetchMore() const {
  return !mDiffSuppressed && mHunks.count() < mPatch.count();
}

/*!
 * \brief DiffView::fetchMore
 * Fetch count more patches
 * use a while loop with canFetchMore() to get all
 */
int FileWidget::fetchMore(int count) {
  if (mDiffSuppressed)
    return 0;

  int counter = 0;
  RepoView *view = RepoView::parentView(this);
  git::Repository repo = view->repo();

  // Add widgets.
  int patchCount = mPatch.count();
  int hunksCount = mHunks.count();

  // Fetch all hunks
  if (count < 0)
    count = patchCount;

  bool lfs = mPatch.isLfsPointer();
  QString name = mPatch.name();
  bool submodule = repo.lookupSubmodule(name).isValid();

  for (int i = hunksCount; i < patchCount && i < (hunksCount + count); ++i) {
    HunkWidget *hunk = addHunk(mDiff, mPatch, mStaged, i, lfs, submodule);
    mHunkLayout->addWidget(hunk);
    counter++;
  }
  return counter;
}

void FileWidget::fetchAll(int index) {
  // Load all patches up to and including index.
  int hunksCount = mHunks.count();
  while ((index < 0 || hunksCount <= index) && canFetchMore())
    fetchMore();
}

void FileWidget::updateMarkResolvedState() {
  if (!mPatch.isConflicted() || mPatch.hasMalformedConflicts()) {
    mHeader->setMarkResolvedEnabled(false);
    return;
  }

  if (mPatch.count() == 0) {
    const git::Index::Conflict conflict =
        mPatch.repo().index().conflict(mPatch.name());
    bool bothDeleted = conflict.ours.isNull() && conflict.theirs.isNull();
    bool sideSelected =
        mFileResolver && mFileResolver->resolution() != git::Patch::Unresolved;
    mHeader->setMarkResolvedEnabled(bothDeleted || sideSelected);
    return;
  }

  if (mResolver) {
    mHeader->setMarkResolvedEnabled(mResolver->isComplete());
    return;
  }

  bool complete = mHunks.size() == mPatch.count();
  for (HunkWidget *hunk : mHunks)
    complete = complete && hunk->resolution() != git::Patch::Unresolved;
  mHeader->setMarkResolvedEnabled(complete);
}

void FileWidget::markResolved() {
  if (!mPatch.isConflicted() || mPatch.hasMalformedConflicts())
    return;

  git::Repository repo = mPatch.repo();
  QString path = repo.workdir().filePath(mPatch.name());
  RepoView *view = RepoView::parentView(this);
  if (!mPatch.conflictFileMatches()) {
    view->refresh();
    return;
  }
  const git::Index::Conflict conflict = mDiff.index().conflict(mPatch.name());

  if (mPatch.count() > 0) {
    if (!mResolver || !mResolver->isComplete())
      return;
    const int untouched = mResolver->untouchedBlockCount();
    if (untouched > 0 &&
        QMessageBox::warning(
            this, tr("Resolve blocks without source selections?"),
            tr("%n conflict block(s) have no Current or Incoming selection. "
               "The Result will be used exactly as shown.",
               nullptr, untouched),
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Ok)
      return;
    const QByteArray result = mResolver->result();
    QByteArray original;
    for (const QByteArray &line : mPatch.conflictFileLines())
      original.append(line);

    if (!repo.index().conflictMatches(mPatch.name(), conflict)) {
      view->refresh();
      return;
    }

    QSaveFile file(path);
    if (!file.open(QFile::WriteOnly))
      return;
    if (file.write(result) != result.size() || !file.commit())
      return;

    if (!repo.index().resolveConflict(mPatch.name(), conflict)) {
      QFile current(path);
      if (current.open(QFile::ReadOnly) && current.readAll() == result) {
        current.close();
        QSaveFile restore(path);
        if (restore.open(QFile::WriteOnly) &&
            restore.write(original) == original.size())
          restore.commit();
      }
      view->refresh();
      return;
    }

    for (int i = 0; i < mPatch.count(); ++i)
      mPatch.setConflictResolution(i, git::Patch::Unresolved);
  } else {
    const git::Patch::ConflictResolution resolution =
        mFileResolver ? mFileResolver->resolution() : git::Patch::Unresolved;
    git::Id id;
    git_filemode_t mode = GIT_FILEMODE_UNREADABLE;

    if (resolution == git::Patch::Ours) {
      id = conflict.ours;
      mode = conflict.oursMode;
    } else if (resolution == git::Patch::Theirs) {
      id = conflict.theirs;
      mode = conflict.theirsMode;
    } else if (!conflict.ours.isNull() || !conflict.theirs.isNull()) {
      return;
    }

    if (id.isNull()) {
      if (mode != GIT_FILEMODE_UNREADABLE)
        return;
    } else {
      if (!id.isValid() || mode == GIT_FILEMODE_UNREADABLE)
        return;
    }

    if (!repo.index().resolveConflict(mPatch.name(), conflict, id, mode))
      return;

    view->refresh();
    return;
  }

  view->refresh();
}

void FileWidget::discard() {
  QString name = mPatch.name();
  bool untracked = mPatch.isUntracked();
  QString path = mPatch.repo().workdir().filePath(name);
  QString arg = QFileInfo(path).isDir() ? FileWidget::tr("Directory")
                                        : FileWidget::tr("File");
  QString title = untracked ? FileWidget::tr("Remove %1?").arg(arg)
                            : FileWidget::tr("Discard Changes?");
  QString text =
      untracked ? FileWidget::tr("Are you sure you want to remove '%1'?")
                : FileWidget::tr(
                      "Are you sure you want to discard all changes in '%1'?");
  QMessageBox *dialog = new QMessageBox(
      QMessageBox::Warning, title, text.arg(name), QMessageBox::Cancel, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setInformativeText(FileWidget::tr("This action cannot be undone."));

  QString button = untracked ? FileWidget::tr("Remove %1").arg(arg)
                             : FileWidget::tr("Discard Changes");
  QPushButton *discard = dialog->addButton(button, QMessageBox::AcceptRole);
  connect(discard, &QPushButton::clicked,
          [this] { emit discarded(mModelIndex); });

  dialog->exec();
}

void FileWidget::headerCheckStateChanged(int state) {
  assert(state != Qt::PartiallyChecked); // makes no sense, that the user can
                                         // select partially selected

  if (state == Qt::Checked)
    emit stageStateChanged(mModelIndex, git::Index::Staged);
  else
    emit stageStateChanged(mModelIndex, git::Index::Unstaged);
}

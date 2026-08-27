//
//          Copyright (c) 2020
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Martin Marmsoler
//

#include "ContextMenuButton.h"
#include "DoubleTreeWidget.h"
#include "BlameEditor.h"
#include "DiffTreeModel.h"
#include "FileContextMenu.h"
#include "StatePushButton.h"
#include "TreeProxy.h"
#include "TreeView.h"
#include "Debug.h"
#include "conf/Settings.h"
#include "DiffView/DiffView.h"
#include "git/Index.h"
#include "git/Config.h"
#include "git/Patch.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QButtonGroup>
#include <qnamespace.h>
#include <qtreeview.h>
#include <functional>

namespace {

const QString kExpandAll = QString(QObject::tr("Expand all"));
const QString kCollapseAll = QString(QObject::tr("Collapse all"));
const QString kStagedFiles = QString(QObject::tr("Staged Files"));
const QString kUnstagedFiles = QString(QObject::tr("Unstaged Files"));
const QString kCommitedFiles = QString(QObject::tr("Committed Files"));
const QString kAllFiles = QString(QObject::tr("Workdir Files"));

class SegmentedButton : public QWidget {
public:
  SegmentedButton(QWidget *parent = nullptr) : QWidget(parent) {
    mLayout = new QHBoxLayout(this);
    mLayout->setContentsMargins(0, 0, 0, 0);
    mLayout->setSpacing(0);
  }

  void addButton(QAbstractButton *button, const QString &text = QString(),
                 bool checkable = false) {
    button->setToolTip(text);
    button->setCheckable(checkable);

    mLayout->addWidget(button);
    mButtons.addButton(button, mButtons.buttons().size());
  }

  const QButtonGroup *buttonGroup() const { return &mButtons; }

private:
  QHBoxLayout *mLayout;
  QButtonGroup mButtons;
};

} // namespace

QAction *DoubleTreeWidget::setupAppearanceAction(const char *name,
                                                 Setting::Id id,
                                                 bool defaultValue) {
  QAction *action = new QAction(tr(name));
  action->setCheckable(true);
  action->setChecked(Settings::instance()->value(id, defaultValue).toBool());
  connect(action, &QAction::triggered, this, [this, id](bool checked) {
    Settings::instance()->setValue(id, checked);
    mSelectedFile.filename =
        ""; // When switching view, it is not possible to restore
    RepoView::parentView(this)->refresh();
  });
  return action;
}

DoubleTreeWidget::DoubleTreeWidget(const git::Repository &repo, QWidget *parent)
    : ContentWidget(parent) {
  // first column
  // top (Buttons to switch between Blame editor and DiffView)
  SegmentedButton *segmentedButton = new SegmentedButton(this);
  mBlameButton = new QPushButton(tr("Blame"), this);
  mBlameButton->setObjectName("BlameViewButton");
  segmentedButton->addButton(mBlameButton, tr("Show Blame Editor"), true);
  mDiffButton = new QPushButton(tr("Diff"), this);
  mDiffButton->setObjectName("DiffViewButton");
  segmentedButton->addButton(mDiffButton, tr("Show Diff View"), true);

  // Context button.
  ContextMenuButton *contextButton = new ContextMenuButton(this);
  QMenu *contextMenu = new QMenu(this);
  contextButton->setMenu(contextMenu);

  QToolButton *closeButton = new QToolButton(this);
  closeButton->setObjectName("CloseFileInspection");
  closeButton->setAccessibleName(tr("Close Blame and Diff"));
  closeButton->setToolTip(tr("Close"));
  closeButton->setAutoRaise(true);
  closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));

  QAction *singleTree = setupAppearanceAction(
      "Single View", Setting::Id::ShowChangedFilesInSingleView);
  QAction *listView =
      setupAppearanceAction("List View", Setting::Id::ShowChangedFilesAsList);
  QAction *multiColumn = setupAppearanceAction(
      "Multi Column", Setting::Id::ShowChangedFilesMultiColumn, true);
  RepoView::parentView(this)->refresh(); // apply read settings

  QAction *hideUntrackedFiles = setupAppearanceAction(
      "Hide Untracked Files", Setting::Id::HideUntracked, false);

  contextMenu->addAction(singleTree);
  contextMenu->addAction(listView);
  contextMenu->addAction(multiColumn);
  contextMenu->addAction(hideUntrackedFiles);
  QHBoxLayout *buttonLayout = new QHBoxLayout();
  buttonLayout->addStretch();
  buttonLayout->addWidget(segmentedButton);
  buttonLayout->addStretch();
  buttonLayout->addWidget(contextButton);
  buttonLayout->addWidget(closeButton);

  // bottom (Stacked widget with Blame editor and DiffView)
  QVBoxLayout *fileViewLayout = new QVBoxLayout();
  mFileView = new QStackedWidget(this);
  mEditor = new BlameEditor(repo, this);
  mDiffView = new DiffView(repo, this);
  mFileView->addWidget(mEditor);
  mFileView->addWidget(mDiffView);

  fileViewLayout->addLayout(buttonLayout);
  fileViewLayout->addWidget(mFileView);
  mFileView->setCurrentIndex(DoubleTreeWidget::Diff);
  mFileView->show();
  QWidget *fileView = new QWidget(this);
  fileView->setObjectName("FileInspectionView");
  fileView->setLayout(fileViewLayout);

  auto *repoView = RepoView::parentView(this);
  Q_ASSERT(repoView);
  repoView->setFileInspectionWidget(fileView);
  connect(closeButton, &QToolButton::clicked, this,
          &DoubleTreeWidget::closeFileInspection);

  // second column
  // staged files
  QVBoxLayout *vBoxLayout = new QVBoxLayout();
  stagedFiles = new TreeView(this, "Staged");
  stagedFiles->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
  stagedFiles->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(stagedFiles, &QWidget::customContextMenuRequested,
          [this, repoView](const QPoint &pos) {
            showFileContextMenu(pos, repoView, stagedFiles, true);
          });

  mDiffTreeModel = new DiffTreeModel(repo, this);
  mDiffView->setModel(mDiffTreeModel);
  Q_ASSERT(repoView);
  connect(mDiffTreeModel, &DiffTreeModel::updateSubmodules,
          [repoView](const QList<git::Submodule> &submodules, bool recursive,
                     bool init, bool force_checkout) {
            repoView->updateSubmodules(submodules, recursive, init,
                                       force_checkout);
          });

  stagedFiles->setModel(new TreeProxy(true, mDiffTreeModel, this));
  connect(stagedFiles, &QAbstractItemView::doubleClicked,
          [this, repoView](const QModelIndex &index) {
            openExternalDiffTool(index, repoView, true);
          });

  QHBoxLayout *hBoxLayout = new QHBoxLayout();
  QLabel *label = new QLabel(kStagedFiles);
  hBoxLayout->addWidget(label);
  hBoxLayout->addStretch();
  collapseButtonStagedFiles =
      new StatePushButton(kCollapseAll, kExpandAll, this);
  hBoxLayout->addWidget(collapseButtonStagedFiles);

  vBoxLayout->addLayout(hBoxLayout);
  vBoxLayout->addWidget(stagedFiles);
  mStagedWidget = new QWidget();
  mStagedWidget->setLayout(vBoxLayout);

  // unstaged files
  vBoxLayout = new QVBoxLayout();
  unstagedFiles = new TreeView(this, "Unstaged");
  unstagedFiles->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
  unstagedFiles->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(unstagedFiles, &QWidget::customContextMenuRequested,
          [this, repoView](const QPoint &pos) {
            showFileContextMenu(pos, repoView, unstagedFiles, false);
          });

  unstagedFiles->setModel(new TreeProxy(false, mDiffTreeModel, this));
  connect(unstagedFiles, &QAbstractItemView::doubleClicked,
          [this, repoView](const QModelIndex &index) {
            openExternalDiffTool(index, repoView, false);
          });

  hBoxLayout = new QHBoxLayout();
  mUnstagedCommitedFiles = new QLabel(kUnstagedFiles);
  hBoxLayout->addWidget(mUnstagedCommitedFiles);
  mConflictSummary = new QLabel(this);
  mConflictSummary->setObjectName("ConflictSummary");
  mConflictSummary->setVisible(false);
  hBoxLayout->addWidget(mConflictSummary);
  hBoxLayout->addStretch();
  mUnresolvedOnly = new QCheckBox(tr("Unresolved only"), this);
  mUnresolvedOnly->setObjectName("UnresolvedOnly");
  mUnresolvedOnly->setVisible(false);
  hBoxLayout->addWidget(mUnresolvedOnly);
  mPreviousConflict = new QToolButton(this);
  mPreviousConflict->setObjectName("PreviousConflict");
  mPreviousConflict->setToolTip(tr("Previous unresolved file"));
  mPreviousConflict->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
  mPreviousConflict->setVisible(false);
  hBoxLayout->addWidget(mPreviousConflict);
  mNextConflict = new QToolButton(this);
  mNextConflict->setObjectName("NextConflict");
  mNextConflict->setToolTip(tr("Next unresolved file"));
  mNextConflict->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
  mNextConflict->setVisible(false);
  hBoxLayout->addWidget(mNextConflict);
  mShowAllFiles = new QCheckBox(tr("Show all files"), this);
  mShowAllFiles->setVisible(false);
  hBoxLayout->addWidget(mShowAllFiles);
  mStageAllChanges = new QPushButton(tr("Stage All Changes"), this);
  mStageAllChanges->setObjectName("StageAllChangesButton");
  mStageAllChanges->setStyleSheet(QStringLiteral(
      "QPushButton#StageAllChangesButton {"
      "  background-color: #36c96b; color: #102817;"
      "  border: 1px solid #2ead5b; border-radius: 3px; padding: 4px 10px;"
      "}"
      "QPushButton#StageAllChangesButton:hover {"
      "  background-color: #4bd77d;"
      "}"
      "QPushButton#StageAllChangesButton:pressed {"
      "  background-color: #2eaa59; color: #ffffff;"
      "}"
      "QPushButton#StageAllChangesButton:disabled {"
      "  background-color: #71877a; color: #e5ebe7; border-color: #71877a;"
      "}"));
  hBoxLayout->addWidget(mStageAllChanges);
  collapseButtonUnstagedFiles =
      new StatePushButton(kCollapseAll, kExpandAll, this);
  mStageAllChanges->setFixedHeight(
      collapseButtonUnstagedFiles->sizeHint().height());
  hBoxLayout->addWidget(collapseButtonUnstagedFiles);

  vBoxLayout->addLayout(hBoxLayout);
  vBoxLayout->addWidget(unstagedFiles);
  QWidget *unstagedWidget = new QWidget();
  unstagedWidget->setLayout(vBoxLayout);

  // splitter between the staged and unstaged section
  QSplitter *treeViewSplitter = new QSplitter(Qt::Vertical, this);
  treeViewSplitter->setHandleWidth(10);
  treeViewSplitter->addWidget(mStagedWidget);
  treeViewSplitter->addWidget(unstagedWidget);
  treeViewSplitter->setStretchFactor(0, 0);
  treeViewSplitter->setStretchFactor(1, 1);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(treeViewSplitter);

  setLayout(layout);

  const QButtonGroup *viewGroup = segmentedButton->buttonGroup();
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
  connect(
      viewGroup, QOverload<int>::of(&QButtonGroup::idClicked), this,
      [this](int id) {
        mFileView->setCurrentIndex(id);
        // Change selection mode.
        if (id == Blame) {
          stagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
        } else {
          stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
      });
#else
  connect(
      viewGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
      [this, viewGroup](QAbstractButton *button) {
        mFileView->setCurrentIndex(viewGroup->id(button));
        // Change selection mode.
        if (viewGroup->id(button) == Blame) {
          stagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
        } else {
          stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
      });
#endif

  connect(mDiffTreeModel, &DiffTreeModel::checkStateChanged, this,
          &DoubleTreeWidget::treeModelStateChanged);

  connect(stagedFiles, &TreeView::filesSelected, this,
          &DoubleTreeWidget::filesSelected);
  connect(stagedFiles, &TreeView::fileSelectionRequested, this,
          &DoubleTreeWidget::openFileInspection);
  connect(stagedFiles, &TreeView::collapseCountChanged, this,
          &DoubleTreeWidget::collapseCountChanged);

  connect(unstagedFiles, &TreeView::filesSelected, this,
          &DoubleTreeWidget::filesSelected);
  connect(unstagedFiles, &TreeView::fileSelectionRequested, this,
          &DoubleTreeWidget::openFileInspection);
  connect(unstagedFiles, &TreeView::collapseCountChanged, this,
          &DoubleTreeWidget::collapseCountChanged);

  connect(collapseButtonStagedFiles, &StatePushButton::clicked, this,
          &DoubleTreeWidget::toggleCollapseStagedFiles);
  connect(collapseButtonUnstagedFiles, &StatePushButton::clicked, this,
          &DoubleTreeWidget::toggleCollapseUnstagedFiles);
  connect(mStageAllChanges, &QPushButton::clicked, repoView, &RepoView::stage);
  connect(mShowAllFiles, &QCheckBox::toggled, this, [this] { setDiff(mDiff); });
  connect(mUnresolvedOnly, &QCheckBox::toggled, this, [this](bool checked) {
    static_cast<TreeProxy *>(stagedFiles->model())->setUnresolvedOnly(checked);
    static_cast<TreeProxy *>(unstagedFiles->model())
        ->setUnresolvedOnly(checked);
    if (checked)
      unstagedFiles->expandAll();
  });
  connect(mPreviousConflict, &QToolButton::clicked, this,
          [this] { selectAdjacentConflict(-1); });
  connect(mNextConflict, &QToolButton::clicked, this,
          [this] { selectAdjacentConflict(1); });

  connect(repo.notifier(), &git::RepositoryNotifier::indexChanged, this,
          [this](const QStringList &paths) {
            mDiffTreeModel->refresh(paths);
            QMetaObject::invokeMethod(
                this, [this] { updateStageAllChangesButton(); },
                Qt::QueuedConnection);
          });

  RepoView *view = RepoView::parentView(this);
  connect(mEditor, &BlameEditor::linkActivated, view, &RepoView::visitLink);
}

QModelIndex DoubleTreeWidget::selectedIndex() const {
  TreeProxy *proxy = static_cast<TreeProxy *>(stagedFiles->model());
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    return proxy->mapToSource(indexes.first());
  }

  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  if (!indexes.isEmpty()) {
    return proxy->mapToSource(indexes.first());
  }
  return QModelIndex();
}

static void addNodeToMenu(const git::Index &index, QStringList &files,
                          const Node *node, bool staged, bool statusDiff) {
  Debug("DoubleTreeWidgetr addNodeToMenu()" << node->name());

  if (node->hasChildren()) {
    for (auto child : node->children()) {
      addNodeToMenu(index, files, child, staged, statusDiff);
    }

  } else {
    auto path = node->path(true);

    auto stageState = index.isStaged(path);

    if ((staged && stageState != git::Index::Unstaged) ||
        (!staged && stageState != git::Index::Staged) || !statusDiff) {
      files.append(path);
    }
  }
}

void DoubleTreeWidget::showFileContextMenu(const QPoint &pos, RepoView *view,
                                           QTreeView *tree, bool staged) {
  QStringList files;
  QModelIndexList indexes = tree->selectionModel()->selectedIndexes();
  const auto diff = view->diff();
  if (!diff.isValid())
    return;

  const bool statusDiff = diff.isStatusDiff();
  foreach (const QModelIndex &index, indexes) {
    auto node = index.data(Qt::UserRole).value<Node *>();

    addNodeToMenu(view->repo().index(), files, node, staged, statusDiff);
  }

  if (files.isEmpty())
    return;

  auto menu = new FileContextMenu(view, files, git::Index(), tree);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  menu->popup(tree->mapToGlobal(pos));
}

void DoubleTreeWidget::openExternalDiffTool(const QModelIndex &index,
                                            RepoView *view, bool staged) {
  const auto diff = view->diff();
  if (!diff.isValid())
    return;

  const bool statusDiff = diff.isStatusDiff();
  QStringList files;
  auto node = index.data(Qt::UserRole).value<Node *>();
  addNodeToMenu(view->repo().index(), files, node, staged, statusDiff);
  if (files.isEmpty())
    return;

  FileContextMenu fileMenu(view, files, git::Index(), nullptr);
  auto doubleClickAction = fileMenu.doubleClickAction();
  if (doubleClickAction)
    doubleClickAction->trigger();
}

QList<QModelIndex> DoubleTreeWidget::selectedIndices() const {
  QList<QModelIndex> list;

  TreeProxy *proxy = static_cast<TreeProxy *>(stagedFiles->model());
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  for (auto index : indexes)
    list.append(proxy->mapToSource(index));

  proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  for (auto index : indexes)
    list.append(proxy->mapToSource(index));

  return list;
}

QString DoubleTreeWidget::selectedFile() const {
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    return indexes.first().data(Qt::DisplayRole).toString();
  }

  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    return indexes.first().data(Qt::DisplayRole).toString();
  }
  return "";
}

/*!
 * \brief DoubleTreeWidget::setDiff
 * \param diff
 * \param file
 * \param pathspec
 */
void DoubleTreeWidget::setDiff(const git::Diff &diff, const QString &file,
                               const QString &pathspec) {
  Q_UNUSED(file)
  Q_UNUSED(pathspec)

  mSetDiffCounter++;
  bool ignoreSelectionChange = mIgnoreSelectionChange;
  mIgnoreSelectionChange = true;

  DebugRefresh("time: " << QDateTime::currentDateTime()
                        << "Counter: " << mSetDiffCounter);

  mDiff = diff;

  // Remember selection.
  storeSelection();

  // Reset model.
  // because of this, the content in the view is shown.
  TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  DiffTreeModel *model = static_cast<DiffTreeModel *>(proxy->sourceModel());

  // Single tree & list view.
  bool singleTree =
      Settings::instance()
          ->value(Setting::Id::ShowChangedFilesInSingleView, false)
          .toBool();
  bool listView = Settings::instance()
                      ->value(Setting::Id::ShowChangedFilesAsList, false)
                      .toBool();
  const bool multiColumn =
      Settings::instance()
          ->value(Setting::Id::ShowChangedFilesMultiColumn, true)
          .toBool();

  // Widget modifications.
  model->enableListView(listView);
  model->setMultiColumn(multiColumn);
  const bool commitDiff = diff.isValid() && !diff.isStatusDiff();
  const bool showAllFiles = commitDiff && mShowAllFiles->isChecked();
  if (showAllFiles)
    model->setTree(RepoView::parentView(this)->tree(), diff);
  else
    model->setDiff(diff);
  stagedFiles->setRootIsDecorated(!listView);
  unstagedFiles->setRootIsDecorated(!listView);
  // mUnstagedCommitedFiles->setVisible(!singleTree);
  collapseButtonStagedFiles->setVisible(!listView);
  collapseButtonUnstagedFiles->setVisible(!listView);
  updateStageAllChangesButton();
  updateConflictUi();

  unstagedFiles->updateView(); // Must be before expandAll/collapseAll is done,
                               // otherwise the collapse counter is wrong
  stagedFiles->updateView();

  // If statusDiff, there exist no staged/unstaged, but only
  // the commited files must be shown
  if (!diff.isValid() || diff.isStatusDiff()) {
    mUnstagedCommitedFiles->setText(singleTree ? kAllFiles : kUnstagedFiles);
    mUnstagedCommitedFiles->setEnabled(true);
    mShowAllFiles->setVisible(false);
    if (diff.isValid() && diff.count() < fileCountExpansionThreshold)
      stagedFiles->expandAll();
    else
      stagedFiles->collapseAll();

    proxy->enableFilter(!singleTree);
    mStagedWidget->setVisible(!singleTree);
  } else {
    mUnstagedCommitedFiles->setText(kCommitedFiles);
    mUnstagedCommitedFiles->setEnabled(!showAllFiles);
    mShowAllFiles->setVisible(true);
    mStagedWidget->setVisible(false);
  }

  // do not expand if to many files exist, it takes really long
  // So do it only when there are less than 100
  if (diff.isValid() && diff.count() < fileCountExpansionThreshold)
    unstagedFiles->expandAll();
  else
    unstagedFiles->collapseAll();

  // Clear editor.
  mEditor->clear();

  mDiffView->setDiff(diff);

  // Restore selection.
  if (diff.isValid() && !mFileInspectionClosed)
    loadSelection();

  mIgnoreSelectionChange = ignoreSelectionChange;

  DebugRefresh("finished, time: " << QDateTime::currentDateTime()
                                  << "Counter: " << mSetDiffCounter);
}

void DoubleTreeWidget::find() { mEditor->find(); }

void DoubleTreeWidget::findNext() { mEditor->findNext(); }

void DoubleTreeWidget::findPrevious() { mEditor->findPrevious(); }

void DoubleTreeWidget::cancelBackgroundTasks() { mEditor->cancelBlame(); }

void DoubleTreeWidget::updateStageAllChangesButton() {
  const bool statusDiff = mDiff.isValid() && mDiff.isStatusDiff();
  mStageAllChanges->setVisible(statusDiff);
  mStageAllChanges->setEnabled(statusDiff &&
                               RepoView::parentView(this)->isStageEnabled());
}

void DoubleTreeWidget::updateConflictUi() {
  int unresolvedFiles = 0;
  int unresolvedBlocks = 0;
  if (mDiff.isValid() && mDiff.isStatusDiff()) {
    for (int i = 0; i < mDiff.count(); ++i) {
      git::Patch patch = mDiff.patch(i);
      if (!patch.isConflicted())
        continue;
      ++unresolvedFiles;
      unresolvedBlocks += patch.count();
    }
  }

  if (unresolvedFiles == 0) {
    mConflictSessionTotal = 0;
    mUnresolvedOnly->setChecked(false);
  } else {
    mConflictSessionTotal = qMax(mConflictSessionTotal, unresolvedFiles);
  }

  QString summary;
  int resolvedFiles = mConflictSessionTotal - unresolvedFiles;
  if (resolvedFiles > 0) {
    summary = tr("%1 of %2 resolved | %3 blocks remaining")
                  .arg(resolvedFiles)
                  .arg(mConflictSessionTotal)
                  .arg(unresolvedBlocks);
  } else {
    summary = tr("%1 unresolved files | %2 blocks")
                  .arg(unresolvedFiles)
                  .arg(unresolvedBlocks);
  }
  mConflictSummary->setText(summary);

  bool visible = unresolvedFiles > 0;
  mConflictSummary->setVisible(visible);
  mUnresolvedOnly->setVisible(visible);
  mPreviousConflict->setVisible(visible);
  mNextConflict->setVisible(visible);
}

void DoubleTreeWidget::selectAdjacentConflict(int direction) {
  QList<QModelIndex> conflicts;
  TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  std::function<void(const QModelIndex &)> collect =
      [&](const QModelIndex &parent) {
        for (int row = 0; row < proxy->rowCount(parent); ++row) {
          QModelIndex index = proxy->index(row, 0, parent);
          int patchIndex = index.data(DiffTreeModel::PatchIndexRole).toInt();
          if (patchIndex >= 0 && mDiff.patch(patchIndex).isConflicted())
            conflicts.append(index);
          collect(index);
        }
      };
  collect(QModelIndex());

  if (conflicts.isEmpty())
    return;

  QModelIndexList selected = unstagedFiles->selectionModel()->selectedIndexes();
  QModelIndex current = selected.isEmpty() ? QModelIndex() : selected.first();
  int currentIndex = -1;
  for (int i = 0; i < conflicts.size(); ++i) {
    if (conflicts.at(i) == current) {
      currentIndex = i;
      break;
    }
  }

  int targetIndex;
  if (currentIndex < 0)
    targetIndex = direction > 0 ? 0 : conflicts.size() - 1;
  else
    targetIndex =
        (currentIndex + direction + conflicts.size()) % conflicts.size();
  QModelIndex target = conflicts.at(targetIndex);

  stagedFiles->deselectAll();
  unstagedFiles->selectionModel()->setCurrentIndex(
      target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  unstagedFiles->scrollTo(target);
  openFileInspection();
}

void DoubleTreeWidget::storeSelection() {
  QModelIndexList indexes = stagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    mSelectedFile.filename = indexes.first().data(Qt::EditRole).toString();
    mSelectedFile.stagedModel = true;
    return;
  }

  indexes = unstagedFiles->selectionModel()->selectedIndexes();
  if (!indexes.isEmpty()) {
    mSelectedFile.filename = indexes.first().data(Qt::EditRole).toString();
    mSelectedFile.stagedModel = false;
    return;
  }
  mSelectedFile.filename = "";
}

void DoubleTreeWidget::loadSelection() {
  QModelIndex index;
  Qt::CheckState state;

  if (mSelectedFile.filename != "") {
    index = mDiffTreeModel->index(mSelectedFile.filename);

    if (!index.isValid()) {
      // If index is anymore valid, because of removed file,
      // select the parent if possible
      auto list = mSelectedFile.filename.split(
          QStringLiteral("/")); // TODO: check also on windows
      list.removeLast();
      while (!index.isValid() && !list.isEmpty()) {
        const QString s = list.join(QStringLiteral("/"));
        index = mDiffTreeModel->index(s);
        list.removeLast();
      }
    }
    state = static_cast<Qt::CheckState>(
        mDiffTreeModel->data(index, Qt::CheckStateRole).toInt());
  }

  if (!index.isValid() ||
      (mSelectedFile.stagedModel && state != Qt::CheckState::Checked) ||
      (!mSelectedFile.stagedModel && state != Qt::CheckState::Unchecked)) {
    mSelectedFile.filename = "";
    if (mDiffTreeModel->rowCount() > 0) {
      index = mDiffTreeModel->index(0, 0);
      git::Index::StagedState s = static_cast<git::Index::StagedState>(
          mDiffTreeModel->data(index, Qt::CheckStateRole).toInt());
      mSelectedFile.stagedModel = (s == git::Index::StagedState::Staged);
    }
  }

  bool ignoreSelectionChange = mIgnoreSelectionChange;
  mIgnoreSelectionChange = true;
  if (mSelectedFile.stagedModel) {
    TreeProxy *proxy = static_cast<TreeProxy *>(stagedFiles->model());
    index = proxy->mapFromSource(index);
    stagedFiles->selectionModel()->setCurrentIndex(index,
                                                   QItemSelectionModel::Select);
  } else {
    TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
    index = proxy->mapFromSource(index);
    unstagedFiles->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::Select);
  }
  mIgnoreSelectionChange = ignoreSelectionChange;
}

void DoubleTreeWidget::treeModelStateChanged(const QModelIndex &index,
                                             int checkState) {
  Q_UNUSED(index)
  Q_UNUSED(checkState)

  // clear editor and disable diffView when no item is selected
  QModelIndexList stagedSelections =
      stagedFiles->selectionModel()->selectedIndexes();
  if (stagedSelections.count())
    return;

  QModelIndexList unstagedSelections =
      unstagedFiles->selectionModel()->selectedIndexes();
  if (unstagedSelections.count())
    return;

  mDiffView->enable(false);
  mEditor->clear();
}

void DoubleTreeWidget::collapseCountChanged(int count) {
  TreeView *view = static_cast<TreeView *>(QObject::sender());

  if (view == stagedFiles)
    collapseButtonStagedFiles->setState(count == 0);
  else
    collapseButtonUnstagedFiles->setState(count == 0);
}

void DoubleTreeWidget::filesSelected(const QModelIndexList &indexes) {
  if (mIgnoreSelectionChange)
    return;

  QObject *obj = QObject::sender();
  if (obj && !indexes.isEmpty()) {
    mIgnoreSelectionChange = true;
    TreeView *treeview = static_cast<TreeView *>(obj);
    if (treeview == stagedFiles) {
      unstagedFiles->deselectAll();
    } else if (treeview == unstagedFiles) {
      stagedFiles->deselectAll();
    }
    mIgnoreSelectionChange = false;
  }

  QModelIndexList selected = stagedFiles->selectionModel()->selectedIndexes();
  selected.append(unstagedFiles->selectionModel()->selectedIndexes());
  if (selected.isEmpty()) {
    mDiffView->enable(false);
    mEditor->clear();
    mFileInspectionClosed = true;
    RepoView::parentView(this)->setFileInspectionVisible(false);
    return;
  }

  if (!RepoView::parentView(this)->isFileInspectionVisible())
    return;

  loadEditorContent(selected);
}

void DoubleTreeWidget::openFileInspection() {
  QModelIndexList selected = stagedFiles->selectionModel()->selectedIndexes();
  selected.append(unstagedFiles->selectionModel()->selectedIndexes());
  if (selected.isEmpty())
    return;

  mFileInspectionClosed = false;
  loadEditorContent(selected);
  RepoView::parentView(this)->setFileInspectionVisible(true);
}

void DoubleTreeWidget::closeFileInspection() {
  bool ignoreSelectionChange = mIgnoreSelectionChange;
  mIgnoreSelectionChange = true;
  stagedFiles->deselectAll();
  unstagedFiles->deselectAll();
  mSelectedFile.filename.clear();
  mFileInspectionClosed = true;
  mEditor->clear();
  mDiffView->enable(false);
  mIgnoreSelectionChange = ignoreSelectionChange;
  RepoView::parentView(this)->setFileInspectionVisible(false);
}

void DoubleTreeWidget::loadEditorContent(const QModelIndexList &indexes) {
  QString name;
  git::Blob blob;
  git::Commit commit;
  bool unresolvedConflict = false;

  if (indexes.count() == 1) {
    RepoView *view = RepoView::parentView(this);
    name = indexes.first().data(Qt::EditRole).toString();
    QList<git::Commit> commits = view->commits();
    commit = !commits.isEmpty() ? commits.first() : git::Commit();
    int idx = mDiff.isValid() ? mDiff.indexOf(name) : -1;
    unresolvedConflict = idx >= 0 && mDiff.patch(idx).isConflicted();
    blob = idx < 0 ? commit.blob(name)
                   : view->repo().lookupBlob(mDiff.id(idx, git::Diff::NewFile));
  }

  mBlameButton->setEnabled(!unresolvedConflict);
  mBlameButton->setToolTip(unresolvedConflict
                               ? tr("Blame is unavailable until this conflict "
                                    "is resolved.")
                               : tr("Show Blame Editor"));
  if (unresolvedConflict) {
    mEditor->clear();
    mFileView->setCurrentWidget(mDiffView);
    mDiffButton->setChecked(true);
    stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
    unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
  } else {
    mEditor->load(name, blob, std::move(commit));
  }

  mDiffView->enable(true);
  mDiffView->updateFiles();
}

void DoubleTreeWidget::toggleCollapseStagedFiles() {
  if (collapseButtonStagedFiles->toggleState())
    stagedFiles->expandAll();
  else
    stagedFiles->collapseAll();
}

void DoubleTreeWidget::toggleCollapseUnstagedFiles() {
  if (collapseButtonUnstagedFiles->toggleState())
    unstagedFiles->expandAll();
  else
    unstagedFiles->collapseAll();
}

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
#include "DiffView/FileWidget.h"
#include "git/Index.h"
#include "git/Config.h"
#include "git/Patch.h"
#include "util/PerformanceTrace.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
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

QIcon diffModeIcon(Settings::DiffMode mode) {
  QPixmap pixmap(18, 18);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setPen(QPen(QColor(145, 155, 170), 1));
  painter.drawRect(1, 2, 15, 13);
  if (mode == Settings::DiffMode::Split) {
    painter.drawLine(8, 2, 8, 15);
    for (int y : {5, 8, 11}) {
      painter.drawLine(3, y, 6, y);
      painter.drawLine(10, y, 14, y);
    }
  } else {
    if (mode == Settings::DiffMode::Hunk)
      painter.drawLine(2, 5, 15, 5);
    const int start = mode == Settings::DiffMode::Hunk ? 8 : 5;
    for (int y = start; y <= 12; y += 3)
      painter.drawLine(4, y, 13, y);
  }
  return QIcon(pixmap);
}

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

  SegmentedButton *diffModes = new SegmentedButton(this);
  QToolButton *inlineMode = new QToolButton(this);
  inlineMode->setObjectName("InlineDiffMode");
  inlineMode->setIcon(diffModeIcon(Settings::DiffMode::Inline));
  diffModes->addButton(inlineMode, tr("Inline complete-file view"), true);
  QToolButton *hunkMode = new QToolButton(this);
  hunkMode->setObjectName("HunkDiffMode");
  hunkMode->setIcon(diffModeIcon(Settings::DiffMode::Hunk));
  diffModes->addButton(hunkMode, tr("Hunk view"), true);
  QToolButton *splitMode = new QToolButton(this);
  splitMode->setObjectName("SplitDiffMode");
  splitMode->setIcon(diffModeIcon(Settings::DiffMode::Split));
  diffModes->addButton(splitMode, tr("Split view"), true);
  const QList<QToolButton *> modeButtons = {inlineMode, hunkMode, splitMode};
  modeButtons.at(static_cast<int>(Settings::instance()->diffMode()))
      ->setChecked(true);
  connect(diffModes->buttonGroup(), &QButtonGroup::idClicked, this,
          [this](int id) {
            Settings::instance()->setDiffMode(
                static_cast<Settings::DiffMode>(id));
            mDiffView->rebuildPresentations();
          });

  QToolButton *ignoreWhitespace = new QToolButton(this);
  ignoreWhitespace->setObjectName("IgnoreEdgeWhitespace");
  ignoreWhitespace->setText(tr("WS"));
  ignoreWhitespace->setToolTip(
      tr("Ignore leading/trailing whitespace in Inline and Split views"));
  ignoreWhitespace->setCheckable(true);
  ignoreWhitespace->setChecked(
      Settings::instance()->isEdgeWhitespaceIgnored());
  connect(ignoreWhitespace, &QToolButton::toggled, this, [this](bool checked) {
    Settings::instance()->setEdgeWhitespaceIgnored(checked);
    mDiffView->rebuildPresentations();
  });

  QToolButton *wordWrap = new QToolButton(this);
  wordWrap->setObjectName("DiffWordWrap");
  wordWrap->setText(tr("Wrap"));
  wordWrap->setToolTip(tr("Word wrap"));
  wordWrap->setCheckable(true);
  wordWrap->setChecked(Settings::instance()->isTextEditorWrapLines());
  connect(wordWrap, &QToolButton::toggled, this, [](bool checked) {
    Settings::instance()->setTextEditorWrapLines(checked);
  });
  connect(Settings::instance(), &Settings::settingsChanged, this,
          [modeButtons, ignoreWhitespace, wordWrap] {
            Settings *settings = Settings::instance();
            modeButtons.at(static_cast<int>(settings->diffMode()))
                ->setChecked(true);
            ignoreWhitespace->setChecked(
                settings->isEdgeWhitespaceIgnored());
            wordWrap->setChecked(settings->isTextEditorWrapLines());
          });

  QAction *singleTree = setupAppearanceAction(
      "Single View", Setting::Id::ShowChangedFilesInSingleView);
  QAction *listView =
      setupAppearanceAction("List View", Setting::Id::ShowChangedFilesAsList);
  QAction *multiColumn = setupAppearanceAction(
      "Multi Column", Setting::Id::ShowChangedFilesMultiColumn, true);

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
  buttonLayout->addWidget(diffModes);
  buttonLayout->addWidget(ignoreWhitespace);
  buttonLayout->addWidget(wordWrap);
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
  mDiffButton->setChecked(true);
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
  mStagedFilesLabel = new QLabel(kStagedFiles);
  mStagedFilesLabel->setObjectName("StagedFilesLabel");
  hBoxLayout->addWidget(mStagedFilesLabel);
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
  mUnstagedCommitedFiles->setObjectName("UnstagedFilesLabel");
  hBoxLayout->addWidget(mUnstagedCommitedFiles);
  mConflictSummary = new QLabel(this);
  mConflictSummary->setObjectName("ConflictSummary");
  mConflictSummary->setVisible(false);
  hBoxLayout->addWidget(mConflictSummary);
  hBoxLayout->addStretch();
  mMarkAllResolved = new QPushButton(tr("Mark All Resolved"), this);
  mMarkAllResolved->setObjectName("MarkAllResolved");
  mMarkAllResolved->setStyleSheet(QStringLiteral(
      "QPushButton#MarkAllResolved {"
      "  background-color: #d6a321; color: #241a00;"
      "  border: 1px solid #b8860b; border-radius: 3px; padding: 4px 10px;"
      "  font-weight: 700;"
      "}"
      "QPushButton#MarkAllResolved:hover { background-color: #e7b53b; }"
      "QPushButton#MarkAllResolved:pressed {"
      "  background-color: #b8860b; color: #ffffff;"
      "}"
      "QPushButton#MarkAllResolved:disabled {"
      "  background-color: #756a4d; color: #ddd6c2; border-color: #756a4d;"
      "}"));
  mMarkAllResolved->setVisible(false);
  connect(mMarkAllResolved, &QPushButton::clicked, this, [this] {
    int conflicts = 0;
    for (int i = 0; i < mDiff.count(); ++i)
      conflicts += mDiff.patch(i).isConflicted();
    if (conflicts == 0 ||
        QMessageBox::warning(
            this, tr("Mark all files resolved?"),
            tr("The Current version will be kept for every conflicted file."),
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Ok)
      return;

    const QStringList failed = FileWidget::resolveAllConflicts(mDiff);
    if (!failed.isEmpty())
      QMessageBox::warning(this, tr("Some conflicts were not resolved"),
                           tr("These files changed or could not be saved:\n%1")
                               .arg(failed.join('\n')));
    RepoView::parentView(this)->refresh();
  });
  hBoxLayout->addWidget(mMarkAllResolved);
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
  connect(mFileView, &QStackedWidget::currentChanged, this, [this](int id) {
    mBlameButton->setChecked(id == Blame);
    mDiffButton->setChecked(id == Diff);
  });
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
        if (RepoView::parentView(this)->isFileInspectionVisible())
          scheduleEditorContentLoad();
      });
#else
  connect(
      viewGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
      [this, viewGroup](QAbstractButton *button) {
        const int id = viewGroup->id(button);
        mFileView->setCurrentIndex(id);
        // Change selection mode.
        if (id == Blame) {
          stagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::SingleSelection);
        } else {
          stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
          unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
        if (RepoView::parentView(this)->isFileInspectionVisible())
          scheduleEditorContentLoad();
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
          [this, repo](const QStringList &paths) {
            if (repo.state() != GIT_REPOSITORY_STATE_NONE) {
              for (const QString &path : paths) {
                const int index = mDiff.indexOf(path);
                if (index >= 0 && mDiff.patch(index).isConflicted() &&
                    !repo.index().hasConflict(path))
                  mResolvedConflictPaths.insert(path);
              }
            }
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

  mStatusSnapshotMode = false;
  mStatusSnapshot = git::WorkingTreeStatusSnapshot();
  mDiff = diff;
  RepoView *repoView = RepoView::parentView(this);
  if (repoView->repo().state() == GIT_REPOSITORY_STATE_NONE)
    mResolvedConflictPaths.clear();

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
  const bool conflictMode =
      diff.isValid() && diff.isStatusDiff() && diff.isConflicted();
  if (diff.isValid() && diff.isStatusDiff() && !conflictMode)
    mConflictAutoOpenEnabled = true;
  const bool showAllFiles = commitDiff && mShowAllFiles->isChecked();
  if (showAllFiles)
    model->setTree(repoView->tree(), diff);
  else
    model->setDiff(diff, mResolvedConflictPaths.values());
  stagedFiles->setRootIsDecorated(!listView);
  unstagedFiles->setRootIsDecorated(!listView);
  // mUnstagedCommitedFiles->setVisible(!singleTree);
  collapseButtonStagedFiles->setVisible(!listView);
  collapseButtonUnstagedFiles->setVisible(!listView);
  updateConflictUi();
  updateStageAllChangesButton();

  unstagedFiles->updateView(); // Must be before expandAll/collapseAll is done,
                               // otherwise the collapse counter is wrong
  stagedFiles->updateView();

  // If statusDiff, there exist no staged/unstaged, but only
  // the commited files must be shown
  if (!diff.isValid() || diff.isStatusDiff()) {
    if (!conflictMode)
      mUnstagedCommitedFiles->setText(singleTree ? kAllFiles : kUnstagedFiles);
    mUnstagedCommitedFiles->setEnabled(true);
    mShowAllFiles->setVisible(false);
    if (diff.isValid() && diff.count() < fileCountExpansionThreshold)
      stagedFiles->expandAll();
    else
      stagedFiles->collapseAll();

    proxy->enableFilter(conflictMode || !singleTree);
    mStagedWidget->setVisible(conflictMode || !singleTree);
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
  if (diff.isValid() && !mFileInspectionClosed && loadSelection()) {
    if (mDiff.isConflicted() && mConflictAutoOpenEnabled)
      repoView->setFileInspectionVisible(true);
    if (repoView->isFileInspectionVisible())
      scheduleEditorContentLoad();
  }

  mIgnoreSelectionChange = ignoreSelectionChange;

  DebugRefresh("finished, time: " << QDateTime::currentDateTime()
                                   << "Counter: " << mSetDiffCounter);
}

void DoubleTreeWidget::setWorkingTreeStatus(
    const git::WorkingTreeStatusSnapshot &status, const QString &file) {
  Q_UNUSED(file)

  PerformanceTrace::Span span(
      "detail", "DoubleTreeWidget::setWorkingTreeStatus",
      RepoView::parentView(this)->repo().dir(false).path(),
      {{"entries", status.entries().size()}});

  mSetDiffCounter++;
  bool ignoreSelectionChange = mIgnoreSelectionChange;
  mIgnoreSelectionChange = true;

  mDiff = git::Diff();
  mStatusSnapshot = status;
  mStatusSnapshotMode = status.isValid();

  storeSelection();

  TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  DiffTreeModel *model = static_cast<DiffTreeModel *>(proxy->sourceModel());

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

  model->enableListView(listView);
  model->setMultiColumn(multiColumn);
  model->setStatusSnapshot(status);
  stagedFiles->setRootIsDecorated(!listView);
  unstagedFiles->setRootIsDecorated(!listView);
  collapseButtonStagedFiles->setVisible(!listView);
  collapseButtonUnstagedFiles->setVisible(!listView);
  updateConflictUi();
  updateStageAllChangesButton();

  unstagedFiles->updateView();
  stagedFiles->updateView();
  mUnstagedCommitedFiles->setText(singleTree ? kAllFiles : kUnstagedFiles);
  mUnstagedCommitedFiles->setEnabled(true);
  mShowAllFiles->setVisible(false);
  proxy->enableFilter(!singleTree);
  mStagedWidget->setVisible(!singleTree);

  if (status.entries().size() < fileCountExpansionThreshold) {
    stagedFiles->expandAll();
    unstagedFiles->expandAll();
  } else {
    stagedFiles->collapseAll();
    unstagedFiles->collapseAll();
  }

  mEditor->clear();
  mDiffView->setDiff(git::Diff());

  if (status.isDirty() && !mFileInspectionClosed && loadSelection() &&
      RepoView::parentView(this)->isFileInspectionVisible())
    scheduleEditorContentLoad();

  mIgnoreSelectionChange = ignoreSelectionChange;
}

void DoubleTreeWidget::find() { mEditor->find(); }

void DoubleTreeWidget::findNext() { mEditor->findNext(); }

void DoubleTreeWidget::findPrevious() { mEditor->findPrevious(); }

void DoubleTreeWidget::cancelBackgroundTasks() { mEditor->cancelBlame(); }

void DoubleTreeWidget::updateStageAllChangesButton() {
  const bool statusDiff = mStatusSnapshotMode ||
                          (mDiff.isValid() && mDiff.isStatusDiff());
  const bool conflictMode = mDiff.isValid() && mDiff.isConflicted();
  mStageAllChanges->setVisible(statusDiff && !conflictMode);
  mStageAllChanges->setEnabled(statusDiff && !conflictMode &&
                               RepoView::parentView(this)->isStageEnabled());
}

void DoubleTreeWidget::updateConflictUi() {
  int unresolvedFiles = 0;
  int unresolvedBlocks = 0;
  int resolvedFiles = 0;
  if (mDiff.isValid() && mDiff.isStatusDiff()) {
    for (int i = 0; i < mDiff.count(); ++i) {
      git::Patch patch = mDiff.patch(i);
      if (patch.isConflicted()) {
        ++unresolvedFiles;
        unresolvedBlocks += patch.count();
      } else if (mDiff.index().isStaged(patch.name()) == git::Index::Staged) {
        ++resolvedFiles;
      }
    }
  }
  for (const QString &path : std::as_const(mResolvedConflictPaths)) {
    if (!mDiff.isValid() || mDiff.indexOf(path) < 0)
      ++resolvedFiles;
  }

  const bool conflictMode = unresolvedFiles > 0;
  static_cast<TreeProxy *>(stagedFiles->model())->setConflictMode(conflictMode);
  static_cast<TreeProxy *>(unstagedFiles->model())
      ->setConflictMode(conflictMode);
  mStagedFilesLabel->setText(conflictMode
                                 ? tr("Resolved Files (%1)").arg(resolvedFiles)
                                 : kStagedFiles);
  if (conflictMode)
    mUnstagedCommitedFiles->setText(
        tr("Conflicted Files (%1)").arg(unresolvedFiles));

  const bool conflictSessionComplete =
      unresolvedFiles == 0 && mConflictSessionTotal > 0;
  if (unresolvedFiles == 0) {
    mConflictSessionTotal = 0;
    mUnresolvedOnly->setChecked(false);
  } else {
    mConflictSessionTotal = qMax(mConflictSessionTotal, unresolvedFiles);
  }

  QString summary;
  int sessionResolvedFiles = mConflictSessionTotal - unresolvedFiles;
  if (sessionResolvedFiles > 0) {
    summary = tr("%1 of %2 resolved | %3 blocks remaining")
                  .arg(sessionResolvedFiles)
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
  mMarkAllResolved->setVisible(visible);
  mUnresolvedOnly->setVisible(false);
  mPreviousConflict->setVisible(visible);
  mNextConflict->setVisible(visible);

  if (conflictSessionComplete)
    RepoView::parentView(this)->setFileInspectionVisible(false);
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

bool DoubleTreeWidget::loadSelection() {
  QModelIndex index;
  Qt::CheckState state;

  const bool conflictMode = mDiff.isValid() && mDiff.isConflicted();
  if (conflictMode) {
    int start = 0;
    for (int i = 0; i < mDiff.count(); ++i) {
      if (mDiff.patch(i).name() == mSelectedFile.filename) {
        start = i + 1;
        if (mDiff.patch(i).isConflicted())
          start = i;
        break;
      }
    }
    for (int offset = 0; offset < mDiff.count(); ++offset) {
      const git::Patch patch = mDiff.patch((start + offset) % mDiff.count());
      if (patch.isConflicted()) {
        index = mDiffTreeModel->index(patch.name());
        mSelectedFile.filename = patch.name();
        mSelectedFile.stagedModel = false;
        break;
      }
    }
  }

  if (!conflictMode && mSelectedFile.filename != "") {
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

  if (!conflictMode &&
      (!index.isValid() ||
       (mSelectedFile.stagedModel && state != Qt::CheckState::Checked) ||
       (!mSelectedFile.stagedModel && state != Qt::CheckState::Unchecked))) {
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
  return index.isValid();
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

  const QString requestedName =
      indexes.size() == 1 ? indexes.first().data(Qt::EditRole).toString()
                          : QString();
  const QList<FileWidget *> files =
      mDiffView->widget()->findChildren<FileWidget *>();
  for (auto it = files.crbegin(); it != files.crend(); ++it) {
    FileWidget *file = *it;
    if (!file->hasUnsavedConflictOutput() || file->name() == requestedName)
      continue;
    if (QMessageBox::warning(
            this, tr("Discard unsaved Output?"),
            tr("The edited conflict Output has not been saved or staged."),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel) == QMessageBox::Discard)
      break;

    const QModelIndex source = mDiffTreeModel->index(file->name());
    const QModelIndex previous =
        static_cast<TreeProxy *>(unstagedFiles->model())->mapFromSource(source);
    mIgnoreSelectionChange = true;
    stagedFiles->deselectAll();
    unstagedFiles->selectionModel()->setCurrentIndex(
        previous,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    mIgnoreSelectionChange = false;
    return;
  }

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
    ++mEditorLoadGeneration;
    mDiffView->enable(false);
    mEditor->clear();
    mFileInspectionClosed = true;
    RepoView::parentView(this)->setFileInspectionVisible(false);
    return;
  }

  if (!RepoView::parentView(this)->isFileInspectionVisible())
    return;

  scheduleEditorContentLoad();
}

void DoubleTreeWidget::openFileInspection() {
  QModelIndexList selected = stagedFiles->selectionModel()->selectedIndexes();
  selected.append(unstagedFiles->selectionModel()->selectedIndexes());
  if (selected.isEmpty())
    return;

  RepoView *view = RepoView::parentView(this);
  const bool alreadyVisible = view->isFileInspectionVisible();
  mConflictAutoOpenEnabled = true;
  mFileInspectionClosed = false;
  mBlameButton->setChecked(mFileView->currentIndex() == Blame);
  mDiffButton->setChecked(mFileView->currentIndex() == Diff);
  view->setFileInspectionVisible(true);
  if (!alreadyVisible)
    scheduleEditorContentLoad();
}

void DoubleTreeWidget::closeFileInspection() {
  QModelIndexList selected = stagedFiles->selectionModel()->selectedIndexes();
  selected.append(unstagedFiles->selectionModel()->selectedIndexes());
  const QString selectedName =
      selected.isEmpty() ? QString()
                         : selected.first().data(Qt::EditRole).toString();
  const QList<FileWidget *> files =
      mDiffView->widget()->findChildren<FileWidget *>();
  for (auto it = files.crbegin(); it != files.crend(); ++it) {
    if ((*it)->name() != selectedName)
      continue;
    if ((*it)->hasUnsavedConflictOutput() &&
        QMessageBox::warning(
            this, tr("Discard unsaved Output?"),
            tr("The edited conflict Output has not been saved or staged."),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Discard)
      return;
    break;
  }

  bool ignoreSelectionChange = mIgnoreSelectionChange;
  ++mEditorLoadGeneration;
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

void DoubleTreeWidget::scheduleEditorContentLoad() {
  const int generation = ++mEditorLoadGeneration;
  QTimer::singleShot(0, this, [this, generation] {
    RepoView *view = RepoView::parentView(this);
    if (generation != mEditorLoadGeneration || !view->isFileInspectionVisible())
      return;

    QModelIndexList selected = stagedFiles->selectionModel()->selectedIndexes();
    selected.append(unstagedFiles->selectionModel()->selectedIndexes());
    if (!selected.isEmpty())
      loadEditorContent(selected);
  });
}

void DoubleTreeWidget::loadEditorContent(const QModelIndexList &indexes) {
  QString name;
  bool unresolvedConflict = false;

  if (indexes.count() == 1) {
    name = indexes.first().data(Qt::EditRole).toString();
    int idx = mDiff.isValid() ? mDiff.indexOf(name) : -1;
    unresolvedConflict = idx >= 0 && mDiff.patch(idx).isConflicted();
  }

  const bool blameAvailable = indexes.count() == 1 && !unresolvedConflict;
  mBlameButton->setEnabled(blameAvailable);
  mBlameButton->setToolTip(unresolvedConflict
                               ? tr("Blame is unavailable until this conflict "
                                    "is resolved.")
                               : tr("Show Blame Editor"));
  if (!blameAvailable && mFileView->currentIndex() == Blame) {
    mEditor->clear();
    mFileView->setCurrentWidget(mDiffView);
    mDiffButton->setChecked(true);
    stagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
    unstagedFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
  }

  if (mFileView->currentIndex() == Blame) {
    RepoView *view = RepoView::parentView(this);
    const QList<git::Commit> commits = view->commits();
    git::Commit commit = !commits.isEmpty() ? commits.first() : git::Commit();
    const int idx = mDiff.isValid() ? mDiff.indexOf(name) : -1;
    git::Blob blob =
        idx < 0 ? commit.blob(name)
                : view->repo().lookupBlob(mDiff.id(idx, git::Diff::NewFile));
    mEditor->load(name, blob, std::move(commit));
    return;
  }

  mEditor->clear();
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

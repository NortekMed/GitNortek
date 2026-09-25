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
#include "StopTrackingDialog.h"
#include "TreeProxy.h"
#include "TreeView.h"
#include "Debug.h"
#include "RepoView.h"
#include "conf/Settings.h"
#include "DiffView/DiffView.h"
#include "DiffView/FileWidget.h"
#include "git/Index.h"
#include "git/Config.h"
#include "git/Patch.h"
#include "git/WorkingTreeStatus.h"
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

void appendPath(QStringList &paths, const QString &path) {
  if (!path.isEmpty() && !paths.contains(path))
    paths.append(path);
}

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

QIcon redTrashIcon(const QStyle *style) {
  const QIcon source = style->standardIcon(QStyle::SP_TrashIcon);
  QIcon red;
  for (const QSize size : {QSize(16, 16), QSize(32, 32)}) {
    QPixmap pixmap = source.pixmap(size);
    if (pixmap.isNull())
      continue;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), QColor(210, 70, 70, 175));
    red.addPixmap(pixmap);
  }
  return red.isNull() ? source : red;
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
  // Top (primary file/diff view and optional blame controls).
  SegmentedButton *segmentedButton = new SegmentedButton(this);
  mFileButton = new QPushButton(tr("File View"), this);
  mFileButton->setObjectName("FileViewButton");
  segmentedButton->addButton(mFileButton, tr("Show File View"), true);

  mBlameButton = new QPushButton(tr("Blame"), this);
  mBlameButton->setObjectName("BlameViewButton");
  mBlameButton->setCheckable(true);
  mBlameButton->setToolTip(tr("Show blame annotations"));

  mDiffButton = new QPushButton(tr("Diff"), this);
  mDiffButton->setObjectName("DiffViewButton");
  segmentedButton->addButton(mDiffButton, tr("Show Diff View"), true);

  // Context button.
  ContextMenuButton *contextButton = new ContextMenuButton(this);
  QMenu *contextMenu = new QMenu(this);
  contextButton->setMenu(contextMenu);

  QToolButton *closeButton = new QToolButton(this);
  closeButton->setObjectName("CloseFileInspection");
  closeButton->setAccessibleName(tr("Close File View and Diff"));
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
  connect(
      diffModes->buttonGroup(), &QButtonGroup::idClicked, this, [this](int id) {
        Settings::instance()->setDiffMode(static_cast<Settings::DiffMode>(id));
        mDiffView->rebuildPresentations();
        scheduleDiffBlameRefresh();
      });

  QToolButton *ignoreWhitespace = new QToolButton(this);
  ignoreWhitespace->setObjectName("IgnoreEdgeWhitespace");
  ignoreWhitespace->setText(tr("WS"));
  ignoreWhitespace->setToolTip(
      tr("Ignore leading/trailing whitespace in Inline and Split views"));
  ignoreWhitespace->setCheckable(true);
  ignoreWhitespace->setChecked(Settings::instance()->isEdgeWhitespaceIgnored());
  connect(ignoreWhitespace, &QToolButton::toggled, this, [this](bool checked) {
    Settings::instance()->setEdgeWhitespaceIgnored(checked);
    mDiffView->rebuildPresentations();
    scheduleDiffBlameRefresh();
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
            ignoreWhitespace->setChecked(settings->isEdgeWhitespaceIgnored());
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
  buttonLayout->addWidget(mBlameButton);
  buttonLayout->addWidget(ignoreWhitespace);
  buttonLayout->addWidget(wordWrap);
  buttonLayout->addWidget(contextButton);
  buttonLayout->addWidget(closeButton);

  // Bottom (stacked file/diff view with an optional blame panel).
  QVBoxLayout *fileViewLayout = new QVBoxLayout();
  mFileView = new QStackedWidget(this);
  mEditor = new BlameEditor(repo, this);
  mEditor->setObjectName("FileViewEditor");
  mEditor->setBlameVisible(false);
  mDiffView = new DiffView(repo, this);
  mDiffBlameEditor = new BlameEditor(repo, this, true);
  mDiffBlameEditor->setObjectName("DiffBlameEditor");
  mDiffBlameEditor->setBlameVisible(false);
  mDiffBlameEditor->setVisible(false);
  connect(mDiffView, &DiffView::editorsChanged, this,
          &DoubleTreeWidget::scheduleDiffBlameRefresh);
  mFileView->addWidget(mEditor);
  mFileView->addWidget(mDiffView);

  fileViewLayout->addLayout(buttonLayout);
  QHBoxLayout *inspectionLayout = new QHBoxLayout();
  inspectionLayout->setContentsMargins(0, 0, 0, 0);
  inspectionLayout->addWidget(mDiffBlameEditor);
  inspectionLayout->addWidget(mFileView);
  fileViewLayout->addLayout(inspectionLayout);
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

  connect(mBlameButton, &QPushButton::toggled, this, [this](bool checked) {
    mEditor->setBlameVisible(checked);
    mDiffBlameEditor->setBlameVisible(checked);

    const bool showDiffBlame = checked && mFileView->currentIndex() == Diff &&
                               mBlameButton->isEnabled();
    mDiffBlameEditor->setVisible(showDiffBlame);
    if (!checked)
      mDiffBlameEditor->clear();

    if (checked && RepoView::parentView(this)->isFileInspectionVisible())
      scheduleEditorContentLoad();
  });

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

  mDiscardAllChanges = new QToolButton(this);
  mDiscardAllChanges->setObjectName("DiscardAllChangesButton");
  mDiscardAllChanges->setAccessibleName(tr("Discard All Changes"));
  mDiscardAllChanges->setToolTip(tr("Discard All Changes"));
  mDiscardAllChanges->setAutoRaise(true);
  mDiscardAllChanges->setIcon(redTrashIcon(style()));
  mDiscardAllChanges->setVisible(false);
  mDiscardAllChanges->setEnabled(false);

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
  collapseButtonUnstagedFiles =
      new StatePushButton(kCollapseAll, kExpandAll, this);
  const int headerButtonHeight =
      qMax(collapseButtonUnstagedFiles->sizeHint().height(),
           mDiscardAllChanges->sizeHint().height());
  mDiscardAllChanges->setFixedHeight(headerButtonHeight);
  mStageAllChanges->setFixedHeight(headerButtonHeight);
  collapseButtonUnstagedFiles->setFixedHeight(headerButtonHeight);
  hBoxLayout->insertWidget(0, mDiscardAllChanges);
  hBoxLayout->addWidget(mStageAllChanges);
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
  connect(mFileView, &QStackedWidget::currentChanged, this,
          [this, diffModes](int id) {
            mFileButton->setChecked(id == File);
            mDiffButton->setChecked(id == Diff);
            diffModes->setEnabled(id == Diff && mDiffButton->isEnabled());
            mDiffView->enable(id == Diff);
            if (mBlameButton->isChecked())
              mBlameButton->setChecked(false);
            mDiffBlameEditor->setVisible(id == Diff &&
                                         mBlameButton->isChecked() &&
                                         mBlameButton->isEnabled());

            mEditor->setBlameVisible(false);

            stagedFiles->setSelectionMode(
                id == File ? QAbstractItemView::SingleSelection
                           : QAbstractItemView::ExtendedSelection);
            unstagedFiles->setSelectionMode(
                id == File ? QAbstractItemView::SingleSelection
                           : QAbstractItemView::ExtendedSelection);
          });
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
  connect(viewGroup, QOverload<int>::of(&QButtonGroup::idClicked), this,
          [this](int id) {
            if (id == Diff && !mDiffButton->isEnabled())
              return;
            mFileView->setCurrentIndex(id);
            if (RepoView::parentView(this)->isFileInspectionVisible())
              scheduleEditorContentLoad();
          });
#else
  connect(viewGroup,
          QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
          [this, viewGroup](QAbstractButton *button) {
            const int id = viewGroup->id(button);
            if (id == Diff && !mDiffButton->isEnabled())
              return;
            mFileView->setCurrentIndex(id);
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
  connect(mDiscardAllChanges, &QToolButton::clicked, this,
          &DoubleTreeWidget::promptToDiscardAllChanges);
  connect(repoView, &RepoView::activityChanged, this,
          [this](bool) { updateStageAllChangesButton(); });
  connect(repoView, &RepoView::discardAllChangesPrepared, this,
          &DoubleTreeWidget::showDiscardAllChangesDialog);
  connect(repoView, &RepoView::stopTrackingPrepared, this,
          &DoubleTreeWidget::showStopTrackingDialog);
  connect(repoView, &RepoView::discardAllChangesFinished, this,
          [this](const git::WorkingTreeDiscardExecution &) {
            updateStageAllChangesButton();
          });
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
            const bool refreshStatus = mPendingStatusDiff.isValid();
            mPendingStatusDiff = git::Diff();
            if (repo.state() != GIT_REPOSITORY_STATE_NONE) {
              for (const QString &path : paths) {
                const int index = mDiff.indexOf(path);
                if (index >= 0 && mDiff.patch(index).isConflicted() &&
                    !repo.index().hasConflict(path))
                  mResolvedConflictPaths.insert(path);
              }
            }
            mDiffTreeModel->refresh(paths);
            if (refreshStatus) {
              QMetaObject::invokeMethod(
                  this, [this] { RepoView::parentView(this)->refresh(); },
                  Qt::QueuedConnection);
            }
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
  QStringList roots;
  QModelIndexList indexes = tree->selectionModel()->selectedIndexes();
  const auto diff = view->diff();
  if (!diff.isValid())
    return;

  const bool statusDiff = diff.isStatusDiff();
  foreach (const QModelIndex &index, indexes) {
    auto node = index.data(Qt::UserRole).value<Node *>();
    if (node)
      roots.append(node->path(true));

    addNodeToMenu(view->repo().index(), files, node, staged, statusDiff);
  }

  if (files.isEmpty())
    return;

  auto menu = new FileContextMenu(view, files, git::Index(), tree, roots,
                                  view->isWorkingTreeContext());
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

  FileContextMenu fileMenu(view, files, git::Index(), nullptr, files,
                           view->isWorkingTreeContext());
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

  if (mStatusSnapshotMode &&
      RepoView::parentView(this)->isFileInspectionVisible() &&
      mFileView->currentIndex() == Diff && mDiffView->reuseFiles(diff)) {
    mDiff = diff;
    mPendingStatusDiff = diff;
    return;
  }

  mPendingStatusDiff = git::Diff();
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

  const bool keepDiffBlamePanel =
      mBlameButton->isChecked() && mFileView->currentIndex() == Diff &&
      repoView->isFileInspectionVisible() && !mFileInspectionClosed;
  const QList<git::Commit> commits = repoView->commits();
  const git::Commit blameCommit =
      !commits.isEmpty() ? commits.first() : git::Commit();
  const bool preserveDiffBlame =
      keepDiffBlamePanel &&
      mDiffBlameEditor->hasBlameFor(mSelectedFile.filename, blameCommit);

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

  // Clear editors.
  mEditor->clear();
  if (!preserveDiffBlame) {
    mDiffBlameEditor->clear();
    if (!keepDiffBlamePanel)
      mDiffBlameEditor->setVisible(false);
  }

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

  mPendingStatusDiff = git::Diff();
  mDiff = git::Diff();
  mStatusSnapshot = status;
  mStatusSnapshotMode = status.isValid();

  storeSelection();

  const bool preserveDiffBlame =
      mBlameButton->isChecked() && mFileView->currentIndex() == Diff &&
      RepoView::parentView(this)->isFileInspectionVisible() &&
      !mFileInspectionClosed;

  TreeProxy *proxy = static_cast<TreeProxy *>(unstagedFiles->model());
  DiffTreeModel *model = static_cast<DiffTreeModel *>(proxy->sourceModel());
  model->setIgnoredPaths(
      RepoView::parentView(this)->stopTrackingIgnoredPaths());

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

  const bool inspectionVisible =
      RepoView::parentView(this)->isFileInspectionVisible();
  mEditor->clear();
  if (!preserveDiffBlame) {
    mDiffBlameEditor->clear();
    mDiffBlameEditor->setVisible(false);
  }
  if (!inspectionVisible)
    mDiffView->setDiff(git::Diff());

  if ((status.isDirty() || preserveDiffBlame) && !mFileInspectionClosed &&
      loadSelection() && inspectionVisible &&
      (mFileView->currentIndex() == File || mBlameButton->isChecked()))
    scheduleEditorContentLoad();

  mIgnoreSelectionChange = ignoreSelectionChange;
}

void DoubleTreeWidget::find() {
  if (mFileView->currentIndex() == File)
    mEditor->find();
  else if (mDiffBlameEditor->isVisible())
    mDiffBlameEditor->find();
}

void DoubleTreeWidget::findNext() {
  if (mFileView->currentIndex() == File)
    mEditor->findNext();
  else if (mDiffBlameEditor->isVisible())
    mDiffBlameEditor->findNext();
}

void DoubleTreeWidget::findPrevious() {
  if (mFileView->currentIndex() == File)
    mEditor->findPrevious();
  else if (mDiffBlameEditor->isVisible())
    mDiffBlameEditor->findPrevious();
}

void DoubleTreeWidget::cancelBackgroundTasks() {
  mEditor->cancelBlame();
  mDiffBlameEditor->cancelBlame();
}

void DoubleTreeWidget::updateStageAllChangesButton() {
  RepoView *view = RepoView::parentView(this);
  const bool statusDiff =
      mStatusSnapshotMode || (mDiff.isValid() && mDiff.isStatusDiff());
  const bool conflictMode = mDiff.isValid() && mDiff.isConflicted();
  const bool dirtyStatus =
      mStatusSnapshotMode
          ? mStatusSnapshot.isDirty()
          : mDiff.isValid() && mDiff.isStatusDiff() && mDiff.count() > 0;
  mStageAllChanges->setVisible(statusDiff && !conflictMode);
  mStageAllChanges->setEnabled(statusDiff && !conflictMode &&
                               view->isStageEnabled() &&
                               !view->hasBackgroundActivity());
  mDiscardAllChanges->setVisible(dirtyStatus);
  mDiscardAllChanges->setEnabled(dirtyStatus &&
                                 !view->isDiscardAllChangesActive() &&
                                 !view->hasBackgroundActivity());
}

void DoubleTreeWidget::promptToDiscardAllChanges() {
  QStringList tracked;
  QStringList untracked;
  RepoView *view = RepoView::parentView(this);
  if (!view || view->isDiscardAllChangesActive())
    return;

  const git::Commit head = view->repo().head().target();
  const bool hasHead = head.isValid();

  if (mStatusSnapshotMode) {
    for (const git::WorkingTreeStatusEntry &entry : mStatusSnapshot.entries()) {
      QStringList &paths =
          (!hasHead || entry.isUntracked()) ? untracked : tracked;
      appendPath(paths, entry.path);
      if (!entry.isUntracked())
        appendPath(paths, entry.oldPath);
    }
  } else if (mDiff.isValid() && mDiff.isStatusDiff()) {
    for (int i = 0; i < mDiff.count(); ++i) {
      git::Patch patch = mDiff.patch(i);
      QStringList &paths =
          (!hasHead || patch.isUntracked()) ? untracked : tracked;
      const QString path = patch.name();
      appendPath(paths, path);
      if (!patch.isUntracked()) {
        const QString oldPath = patch.name(git::Diff::OldFile);
        if (oldPath != path)
          appendPath(paths, oldPath);
      }
    }
  }

  if (tracked.isEmpty() && untracked.isEmpty())
    return;

  view->prepareDiscardAllChanges(tracked, untracked,
                                 hasHead ? head.id().toString() : QString());
}

void DoubleTreeWidget::showDiscardAllChangesDialog(
    const git::WorkingTreeDiscardPreparation &preparation) {
  RepoView *view = RepoView::parentView(this);
  if (!view || preparation.canceled)
    return;

  if (!preparation.error.isEmpty()) {
    QMessageBox *warning =
        new QMessageBox(QMessageBox::Warning, tr("Unable to prepare discard"),
                        preparation.error, QMessageBox::Ok, this);
    warning->setAttribute(Qt::WA_DeleteOnClose);
    warning->open();
    return;
  }

  const git::WorkingTreeDiscardPlan plan = preparation.plan;
  if (!plan.isDirty())
    return;
  const bool hasHead = !plan.headId.isEmpty();

  QMessageBox *dialog = new QMessageBox(
      QMessageBox::Warning, tr("Discard all changes?"),
      tr("Are you sure you want to discard all changes in the working "
         "directory?"),
      QMessageBox::Cancel, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setInformativeText(
      hasHead ? tr("Tracked paths will be restored from HEAD. Untracked paths "
                   "will be permanently deleted. This action cannot be undone.")
              : tr("All listed paths will be permanently deleted. This action "
                   "cannot be undone."));

  QString detailedText;
  if (!plan.trackedPaths.isEmpty())
    detailedText = tr("Tracked paths (restored from HEAD):\n%1")
                       .arg(plan.trackedPaths.join('\n'));
  if (!plan.untrackedPaths.isEmpty()) {
    if (!detailedText.isEmpty())
      detailedText += "\n\n";
    detailedText += tr("Untracked paths (permanently deleted):\n%1")
                        .arg(plan.untrackedPaths.join('\n'));
  }
  dialog->setDetailedText(detailedText);

  foreach (QAbstractButton *button, dialog->buttons()) {
    if (dialog->buttonRole(button) == QMessageBox::ActionRole) {
      button->click();
      break;
    }
  }

  QPushButton *discard =
      dialog->addButton(tr("Discard All Changes"), QMessageBox::AcceptRole);
  discard->setObjectName("DiscardButton");
  dialog->setDefaultButton(discard);
  const std::shared_ptr<bool> accepted = std::make_shared<bool>(false);
  connect(discard, &QPushButton::pressed, this,
          [accepted] { *accepted = true; });
  connect(discard, &QPushButton::clicked, this, [view, plan, accepted] {
    if (!view->executeDiscardAllChanges(plan))
      *accepted = false;
  });
  connect(dialog, &QDialog::finished, this, [view, plan, accepted] {
    if (!*accepted && view->isDiscardAllChangesAwaitingConfirmation())
      view->cancelDiscardAllChanges(plan.generation);
  });
  dialog->open();
}

void DoubleTreeWidget::showStopTrackingDialog(
    const git::WorkingTreeUntrackPreparation &preparation) {
  RepoView *view = RepoView::parentView(this);
  if (!view || preparation.canceled)
    return;

  if (!preparation.error.isEmpty()) {
    QMessageBox *warning = new QMessageBox(
        QMessageBox::Warning, tr("Unable to prepare stop tracking"),
        preparation.error, QMessageBox::Ok, this);
    warning->setAttribute(Qt::WA_DeleteOnClose);
    warning->open();
    return;
  }

  const git::WorkingTreeUntrackPlan plan = preparation.plan;
  if (plan.isEmpty())
    return;

  auto *dialog = new StopTrackingDialog(plan, this);
  connect(dialog, &QDialog::accepted, this, [view, dialog, plan] {
    view->executeStopTracking(plan, dialog->deleteTracked(),
                              dialog->deleteUntracked());
  });
  connect(dialog, &QDialog::finished, this, [view, plan](int result) {
    if (result != QDialog::Accepted &&
        view->isStopTrackingAwaitingConfirmation())
      view->cancelStopTracking(plan.generation);
  });
  dialog->open();
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
  mDiffBlameEditor->clear();
  mDiffBlameEditor->setVisible(false);
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
      mDiffView->widget() ? mDiffView->widget()->findChildren<FileWidget *>()
                          : QList<FileWidget *>();
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
    mFileButton->setEnabled(false);
    mDiffButton->setEnabled(false);
    mBlameButton->setEnabled(false);
    mBlameButton->setChecked(false);
    mDiffView->enable(false);
    mEditor->clear();
    mDiffBlameEditor->clear();
    mDiffBlameEditor->setVisible(false);
    mFileInspectionClosed = true;
    RepoView::parentView(this)->setFileInspectionVisible(false);
    return;
  }

  const bool committedDiff = mDiff.isValid() && !mDiff.isStatusDiff();
  const QString selectedName =
      selected.size() == 1 ? selected.first().data(Qt::EditRole).toString()
                           : QString();
  const int selectedPatchIndex =
      selected.size() == 1 ? mDiff.indexOf(selectedName) : -1;
  const bool unchangedCommittedFile =
      committedDiff && selected.size() == 1 && selectedPatchIndex < 0;
  const bool diffAvailable =
      selected.size() > 1 || !committedDiff || selectedPatchIndex >= 0;
  const bool unresolvedConflict =
      selectedPatchIndex >= 0 && mDiff.patch(selectedPatchIndex).isConflicted();

  mFileButton->setEnabled(selected.size() == 1);
  mDiffButton->setEnabled(diffAvailable);
  mBlameButton->setEnabled(selected.size() == 1 && !unresolvedConflict);
  if (unresolvedConflict)
    mBlameButton->setChecked(false);

  // Selecting a committed file chooses the useful presentation automatically.
  // A user can still switch a modified file to File View explicitly.
  if (unchangedCommittedFile || (selected.size() == 1 && diffAvailable &&
                                 (mStatusSnapshotMode || mDiff.isStatusDiff() ||
                                  mFileView->currentIndex() == File))) {
    mFileView->setCurrentIndex(unchangedCommittedFile ? File : Diff);
  } else if (selected.size() > 1 && mFileView->currentIndex() == File) {
    mFileView->setCurrentIndex(Diff);
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
  mFileButton->setChecked(mFileView->currentIndex() == File);
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
      mDiffView->widget() ? mDiffView->widget()->findChildren<FileWidget *>()
                          : QList<FileWidget *>();
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
  mDiffBlameEditor->clear();
  mDiffBlameEditor->setVisible(false);
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

void DoubleTreeWidget::scheduleDiffBlameRefresh() {
  if (!mBlameButton->isChecked() ||
      !RepoView::parentView(this)->isFileInspectionVisible() ||
      mDiffBlameRefreshPending)
    return;

  mDiffBlameRefreshPending = true;
  QTimer::singleShot(0, this, [this] {
    mDiffBlameRefreshPending = false;
    refreshDiffBlameEditor();
  });
}

void DoubleTreeWidget::refreshDiffBlameEditor() {
  if (!mBlameButton->isChecked() || mFileView->currentIndex() != Diff ||
      !mBlameButton->isEnabled())
    return;

  const QList<TextEditor *> editors = mDiffView->editors();
  if (editors.isEmpty())
    return;

  if (mDiffBlameEditor->editor() == editors.first())
    mDiffBlameEditor->refreshBlame();
  else
    mDiffBlameEditor->setEditor(editors.first(), true);
  mDiffBlameEditor->setVisible(true);
}

void DoubleTreeWidget::loadEditorContent(const QModelIndexList &indexes) {
  QString name;
  int idx = -1;
  bool unresolvedConflict = false;

  if (indexes.count() == 1) {
    name = indexes.first().data(Qt::EditRole).toString();
    idx = mDiff.isValid() ? mDiff.indexOf(name) : -1;
    unresolvedConflict = idx >= 0 && mDiff.patch(idx).isConflicted();
  }

  const bool committedDiff = mDiff.isValid() && !mDiff.isStatusDiff();
  const bool unchangedCommittedFile =
      committedDiff && indexes.count() == 1 && idx < 0;
  const bool diffAvailable = indexes.count() > 1 || !committedDiff || idx >= 0;

  mFileButton->setEnabled(indexes.count() == 1);
  mDiffButton->setEnabled(diffAvailable);
  const bool blameAvailable = indexes.count() == 1 && !unresolvedConflict;
  mBlameButton->setEnabled(blameAvailable);
  mBlameButton->setToolTip(unresolvedConflict
                               ? tr("Blame is unavailable until this conflict "
                                    "is resolved.")
                               : tr("Show blame annotations"));
  if (!blameAvailable)
    mBlameButton->setChecked(false);

  if (unchangedCommittedFile && mFileView->currentIndex() == Diff)
    mFileView->setCurrentWidget(mEditor);

  RepoView *view = RepoView::parentView(this);
  const QList<git::Commit> commits = view->commits();
  git::Commit commit = !commits.isEmpty() ? commits.first() : git::Commit();
  git::Blob blob;
  if (indexes.count() == 1) {
    if (idx < 0) {
      blob = commit.blob(name);
    } else if (mDiff.isValid()) {
      blob = view->repo().lookupBlob(mDiff.id(idx, git::Diff::NewFile));
    }
  }

  if (mFileView->currentIndex() == File) {
    mDiffView->enable(false);
    mDiffBlameEditor->clear();
    mDiffBlameEditor->setVisible(false);
    if (indexes.count() == 1)
      mEditor->load(name, blob, std::move(commit));
    else
      mEditor->clear();
    return;
  }

  mEditor->clear();
  // The status snapshot is only a lightweight tree model. Keep the current
  // diff visible until the asynchronously generated full status diff arrives.
  mDiffView->enable(true);
  if (mStatusSnapshotMode) {
    if (mPendingStatusDiff.isValid()) {
      git::Diff pending = mPendingStatusDiff;
      mPendingStatusDiff = git::Diff();
      mStatusSnapshotMode = false;
      setDiff(pending);
    } else {
      return;
    }
  } else {
    mDiffView->updateFiles();
  }

  if (mBlameButton->isChecked() && blameAvailable) {
    const QList<TextEditor *> editors = mDiffView->editors();
    if (!editors.isEmpty()) {
      mDiffBlameEditor->setEditor(editors.first());
      mDiffBlameEditor->setBlameVisible(true);
      mDiffBlameEditor->setVisible(true);
      mDiffBlameEditor->load(name, blob, commit);
    } else {
      mDiffBlameEditor->clear();
      mDiffBlameEditor->setVisible(false);
    }
  } else {
    mDiffBlameEditor->clear();
    mDiffBlameEditor->setVisible(false);
  }
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

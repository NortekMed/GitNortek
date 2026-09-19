#include "Test.h"
#include "ui/DiffView/DiffView.h"
#include "ui/DiffView/FileWidget.h"
#include "ui/BlameEditor.h"
#include "ui/CommitList.h"
#include "ui/MainWindow.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/StatePushButton.h"
#include "ui/TreeView.h"
#include "ui/TreeProxy.h"
#include "ui/FileContextMenu.h"
#include "conf/Settings.h"
#include "editor/TextEditor.h"

#include <algorithm>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

using namespace Test;
using namespace QTest;

#define INIT_REPO(repoPath, /* bool */ useTempDir)                             \
  QString path = Test::extractRepository(repoPath, useTempDir);                \
  QVERIFY(!path.isEmpty());                                                    \
  auto repo = git::Repository::open(path);                                     \
  QVERIFY(repo.isValid());                                                     \
  Test::initRepo(repo);                                                        \
  MainWindow window(repo);                                                     \
  window.show();                                                               \
  QVERIFY(QTest::qWaitForWindowExposed(&window));                              \
                                                                               \
  RepoView *repoView = window.currentView();

static void disableListView(TreeView &treeView, RepoView &repoView) {
  auto treeProxy = dynamic_cast<TreeProxy *>(treeView.model());
  QVERIFY(treeProxy);

  auto diffTreeModel = dynamic_cast<DiffTreeModel *>(treeProxy->sourceModel());
  QVERIFY(diffTreeModel);

  diffTreeModel->enableListView(false);
  Settings::instance()->setValue(Setting::Id::ShowChangedFilesAsList, false);
  repoView.refresh();
}

class TestTreeView : public QObject {
  Q_OBJECT

private slots:
  void restoreStagedFileAfterCommit();
  void discardFiles();
  void fileMergeCrash();
  void committedFileInspection();
  void dirtySubmoduleAndStagedSubmodule();
  void conflictedAndStagedFile();
  void stageAllChangesButton();
  void discardAllChangesButton();
  void discardAllChangesInUnbornRepository();
  void externalRefreshPreservesSelection();
  void externalRefreshKeepsEditorContent();
  void externalRefreshPreservesViewport();

private:
};

void TestTreeView::restoreStagedFileAfterCommit() {
  INIT_REPO("TreeViewCollapseCount.zip", true);

  // Check for a single file called "test".
  RepoView *view = window.currentView();
  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  {
    auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
    QVERIFY(unstagedTree);
    disableListView(*unstagedTree, *view);
    QAbstractItemModel *unstagedModel = unstagedTree->model();
    // Wait for refresh
    auto timeout = Timeout(10000, "Repository didn't refresh in time");
    while (unstagedModel->rowCount() < 1)
      qWait(300);

    QCOMPARE(unstagedModel->rowCount(), 2);
    auto folder = unstagedModel->index(0, 0);
    auto subfolder = unstagedModel->index(0, 0, folder);
    auto file_txt = unstagedModel->index(0, 0, subfolder);
    QCOMPARE(unstagedModel->data(file_txt).toString(), QString("file.txt"));
    unstagedTree->selectionModel()->select(file_txt,
                                           QItemSelectionModel::Select);

    // Click on the check box. --> Stage file.txt
    mouseClick(unstagedTree->viewport(), Qt::LeftButton,
               Qt::KeyboardModifiers(),
               unstagedTree->checkRect(file_txt).center());
  }

  refresh(view, true);

  auto stagedTree = doubleTree->findChild<TreeView *>("Staged");
  stagedTree->expandAll();

  QAbstractItemModel *stagedModel = stagedTree->model();
  QVERIFY(stagedTree);
  {
    QCOMPARE(stagedModel->rowCount(), 1);
    auto folder = stagedModel->index(0, 0);
    auto subfolder = stagedModel->index(0, 0, folder);
    auto file_txt = stagedModel->index(0, 0, subfolder);
    QCOMPARE(stagedModel->data(file_txt).toString(), QString("file.txt"));

    // Select file
    stagedTree->selectionModel()->clearSelection();
    stagedTree->selectionModel()->select(file_txt, QItemSelectionModel::Select);
  }

  QTextEdit *editor = view->findChild<QTextEdit *>("MessageEditor");
  QVERIFY(editor);
  editor->setText("conflicting commit b");
  view->commit();

  // The application should not crash!
}

void TestTreeView::discardFiles() {
  // staging single files and discard files afterwards. It should not discard
  // not selected files Discarding a folder in staged treeview should only
  // delete the staged files, but not the unstaged files in that folder!

  INIT_REPO("TestRepository.zip", false);

  git::Commit commit =
      repo.lookupCommit("5c61b24e236310ad4a8a64f7cd1ccc968f1eec20");
  QVERIFY(commit);

  // modifying all files
  QHash<QString, QString> fileContent{
      {"file.txt", "Modified file"},
      {"file2.txt", "Modified file2"},
      {"folder1/file.txt", "Modified file in folder1"},
      {"folder1/file2.txt", "Modified file2 in folder1"},
      {"GittyupTestRepo/README.md", "Modified readme in submodule"},
  };
  {
    QHashIterator<QString, QString> i(fileContent);
    while (i.hasNext()) {
      i.next();
      QFile file(repo.workdir().filePath(i.key()));
      QVERIFY(file.exists());
      QVERIFY(file.open(QFile::WriteOnly));
      file.write(i.value().toLatin1());
    }
  }

  // refresh repo
  refresh(repoView);

  // let the changes settle
  QApplication::processEvents();

  // Check for a single file called "test".
  RepoView *view = window.currentView();
  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  // stage folder1/file.txt
  {
    auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
    QVERIFY(unstagedTree);
    QAbstractItemModel *unstagedModel = unstagedTree->model();
    // Wait for refresh
    auto timeout = Timeout(10000, "Repository didn't refresh in time");
    while (unstagedModel->rowCount() < 1)
      qWait(300);

    QCOMPARE(unstagedModel->rowCount(), 4);
    auto folder1 = unstagedModel->index(3, 0);
    auto file_txt = unstagedModel->index(0, 0, folder1);
    QCOMPARE(unstagedModel->data(file_txt).toString(), QString("file.txt"));
    unstagedTree->selectionModel()->select(file_txt,
                                           QItemSelectionModel::Select);

    // Click on the check box. --> Stage file.txt
    mouseClick(unstagedTree->viewport(), Qt::LeftButton,
               Qt::KeyboardModifiers(),
               unstagedTree->checkRect(file_txt).center());
  }

  refresh(view, true);

  auto stagedTree = doubleTree->findChild<TreeView *>("Staged");
  stagedTree->expandAll();

  // discard staged folder1
  QAbstractItemModel *stagedModel = stagedTree->model();
  QVERIFY(stagedTree);
  {
    QCOMPARE(stagedModel->rowCount(), 1);
    auto folder1 = stagedModel->index(0, 0);
    QCOMPARE(stagedModel->data(folder1).toString(), QString("folder1"));

    // Select file
    stagedTree->selectionModel()->clearSelection();
    stagedTree->selectionModel()->select(folder1, QItemSelectionModel::Select);
  }

  DoubleTreeWidget::showFileContextMenu(QPoint(), repoView, stagedTree, true);

  auto *menu = doubleTree->findChild<FileContextMenu *>();
  QVERIFY(menu);
  QCOMPARE(menu->mFiles.count(), 1);
  // only folder1/file.txt shall get discarded.
  // folder1/file2.txt shall not discarded!
  QCOMPARE(menu->mFiles.at(0), "folder1/file.txt");

  // From here on everything is tested in TestFileContextMenu
}

void TestTreeView::fileMergeCrash() {
  INIT_REPO("CrashMerge.zip", false);

  git::Reference otherBranch = repo.lookupRef("refs/heads/otherBranch");
  QVERIFY(otherBranch);

  git::Reference master =
      repo.lookupRef(QString("refs/heads/%1").arg("master"));
  QVERIFY(master);

  QCOMPARE(repo.head().name(), "master");

  repoView->merge(RepoView::Merge, otherBranch);

  // Diff is in a conflicted state
  git::Diff diff = repo.diffIndexToWorkdir();
  QVERIFY(diff.isConflicted());

  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  doubleTree->fileCountExpansionThreshold = 5;
  auto stagedTree = doubleTree->findChild<TreeView *>("Staged");
  QVERIFY(stagedTree);
  auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(unstagedTree);

  QAbstractItemModel *stagedModel = stagedTree->model();

  // Wait for refresh
  while (stagedModel->rowCount() < 3)
    qWait(300);

  QAbstractItemModel *unstagedModel = unstagedTree->model();
  QCOMPARE(unstagedModel->rowCount(), 1);

  unstagedTree->expandAll();

  QModelIndex index = unstagedModel->index(0, 0); // common
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // src
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // main
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // java
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // com
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // something
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // common
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // configs
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // security_config
  QVERIFY(index.isValid());
  index = unstagedModel->index(0, 0, index); // File_security_config
  QVERIFY(index.isValid());

  unstagedTree->selectionModel()->select(
      index, QItemSelectionModel::SelectionFlag::Select);
  QVERIFY(QMetaObject::invokeMethod(unstagedTree, "fileSelectionRequested"));

  auto diffView = repoView->findChild<DiffView *>();
  QVERIFY(diffView);

  QCheckBox *incoming = nullptr;
  QTRY_VERIFY(
      (incoming = diffView->findChild<QCheckBox *>("ConflictIncomingBlock_0")));
  mouseClick(incoming, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(), 0);

  QToolButton *markResolved = nullptr;
  QTRY_VERIFY((markResolved = diffView->widget()->findChild<QToolButton *>(
                   "ConflictMarkResolved")));
  mouseClick(markResolved, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             0);

  // should not crash
}

void TestTreeView::committedFileInspection() {
  Settings::instance()->setDiffMode(Settings::DiffMode::Inline);
  INIT_REPO("TestRepository.zip", false);

  QStackedWidget *primaryView =
      repoView->findChild<QStackedWidget *>("RepositoryPrimaryView");
  QWidget *fileInspection =
      repoView->findChild<QWidget *>("FileInspectionView");
  CommitList *commits = repoView->findChild<CommitList *>();
  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  auto committedFiles = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(primaryView);
  QVERIFY(fileInspection);
  QVERIFY(commits);
  QVERIFY(doubleTree);
  QVERIFY(committedFiles);
  auto *diffView = repoView->findChild<DiffView *>();
  QVERIFY(diffView);

  QModelIndex commitIndex;
  for (int row = 0; row < commits->model()->rowCount(); ++row) {
    QModelIndex candidate = commits->model()->index(row, 0);
    if (candidate.data(CommitList::CommitRole).isValid()) {
      commitIndex = candidate;
      break;
    }
  }
  QVERIFY(commitIndex.isValid());
  commits->selectionModel()->select(commitIndex,
                                    QItemSelectionModel::ClearAndSelect);

  QTRY_VERIFY(committedFiles->model()->rowCount() > 0);
  QVERIFY(primaryView->currentWidget() != fileInspection);
  const QModelIndexList committedFileIndexes = committedFiles->model()->match(
      committedFiles->model()->index(0, 0), Qt::EditRole, QString("file.txt"),
      1, Qt::MatchExactly | Qt::MatchRecursive);
  QVERIFY(!committedFileIndexes.isEmpty());
  committedFiles->selectionModel()->clearSelection();
  committedFiles->selectionModel()->select(committedFileIndexes.first(),
                                           QItemSelectionModel::Select);
  QVERIFY(primaryView->currentWidget() != fileInspection);
  bool eventLoopAdvanced = false;
  QTimer::singleShot(0, [&eventLoopAdvanced] { eventLoopAdvanced = true; });
  QVERIFY(QMetaObject::invokeMethod(committedFiles, "fileSelectionRequested"));
  QCOMPARE(primaryView->currentWidget(), fileInspection);
  QVERIFY(!diffView->widget()->findChild<FileWidget *>());
  QTRY_VERIFY(eventLoopAdvanced);
  QPointer<FileWidget> initialFile;
  QTRY_VERIFY((initialFile = diffView->widget()->findChild<FileWidget *>()));

  eventLoopAdvanced = false;
  QTimer::singleShot(0, [&eventLoopAdvanced] {
    QTimer::singleShot(0, [&eventLoopAdvanced] { eventLoopAdvanced = true; });
  });
  QVERIFY(QMetaObject::invokeMethod(committedFiles, "fileSelectionRequested"));
  QTRY_VERIFY(eventLoopAdvanced);
  QVERIFY(initialFile);
  QVERIFY(!initialFile->isHidden());

  doubleTree->closeFileInspection();
  QVERIFY(!repoView->isFileInspectionVisible());
  QVERIFY(initialFile);
  committedFiles->selectionModel()->setCurrentIndex(
      committedFileIndexes.first(),
      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  QVERIFY(QMetaObject::invokeMethod(committedFiles, "fileSelectionRequested"));
  QVERIFY(repoView->isFileInspectionVisible());
  eventLoopAdvanced = false;
  QTimer::singleShot(0, [&eventLoopAdvanced] {
    QTimer::singleShot(0, [&eventLoopAdvanced] { eventLoopAdvanced = true; });
  });
  QTRY_VERIFY(eventLoopAdvanced);
  QCOMPARE(diffView->widget()->findChild<FileWidget *>(), initialFile.data());

  auto *diffButton = doubleTree->mDiffButton;
  auto *inlineMode = repoView->findChild<QToolButton *>("InlineDiffMode");
  auto *hunkMode = repoView->findChild<QToolButton *>("HunkDiffMode");
  auto *splitMode = repoView->findChild<QToolButton *>("SplitDiffMode");
  QVERIFY(diffButton);
  QVERIFY(inlineMode);
  QVERIFY(hunkMode);
  QVERIFY(splitMode);
  QVERIFY(diffView);
  QVERIFY(diffButton->isChecked());
  QVERIFY(inlineMode->isChecked());
  auto *blameButton = doubleTree->mBlameButton;
  auto *blameEditor = doubleTree->mEditor;
  QVERIFY(blameButton);
  QVERIFY(blameEditor);
  const QString selectedFile =
      committedFileIndexes.first().data(Qt::EditRole).toString();
  QVERIFY(blameButton->isEnabled());
  QVERIFY(blameEditor->name() != selectedFile);
  mouseClick(blameButton, Qt::LeftButton);
  QTRY_COMPARE(doubleTree->mFileView->currentWidget(), blameEditor);
  QTRY_COMPARE(blameEditor->name(), selectedFile);
  mouseClick(diffButton, Qt::LeftButton);
  auto fileForEditor = [](QWidget *widget) {
    while (widget && !qobject_cast<FileWidget *>(widget))
      widget = widget->parentWidget();
    return qobject_cast<FileWidget *>(widget);
  };
  const QList<TextEditor *> initialEditors = diffView->editors();
  QVERIFY(!initialEditors.isEmpty());
  auto *fileWidget = fileForEditor(initialEditors.first());
  QVERIFY(fileWidget);

  auto verifySelection = [&] {
    const QModelIndexList selected =
        committedFiles->selectionModel()->selectedIndexes();
    QVERIFY(!selected.isEmpty());
    QCOMPARE(selected.first().data(Qt::EditRole).toString(), selectedFile);
    QCOMPARE(primaryView->currentWidget(), fileInspection);
  };
  auto hasVisiblePresentation = [fileWidget](const QString &name) {
    for (QWidget *view : fileWidget->findChildren<QWidget *>(name)) {
      if (view->isVisible())
        return true;
    }
    return false;
  };

  mouseClick(hunkMode, Qt::LeftButton);
  QCOMPARE(Settings::instance()->diffMode(), Settings::DiffMode::Hunk);
  verifySelection();
  QVERIFY(!diffView->editors().isEmpty());
  QCOMPARE(fileForEditor(diffView->editors().first()), fileWidget);
  mouseClick(splitMode, Qt::LeftButton);
  QCOMPARE(Settings::instance()->diffMode(), Settings::DiffMode::Split);
  verifySelection();
  QCOMPARE(diffView->editors().size(), 2);
  QCOMPARE(fileForEditor(diffView->editors().first()), fileWidget);
  QTRY_VERIFY(hasVisiblePresentation("SplitFileDiff"));
  mouseClick(inlineMode, Qt::LeftButton);
  QCOMPARE(Settings::instance()->diffMode(), Settings::DiffMode::Inline);
  verifySelection();
  QCOMPARE(diffView->editors().size(), 1);
  QCOMPARE(fileForEditor(diffView->editors().first()), fileWidget);
  QTRY_VERIFY(hasVisiblePresentation("InlineFileDiff"));

  QToolButton *close =
      repoView->findChild<QToolButton *>("CloseFileInspection");
  QVERIFY(close);
  close->click();
  QVERIFY(primaryView->currentWidget() != fileInspection);
  QVERIFY(committedFiles->selectionModel()->selectedIndexes().isEmpty());
}

void TestTreeView::dirtySubmoduleAndStagedSubmodule() {
  INIT_REPO("DirtySubmoduleUnstagedTree.zip", false);

  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto stagedTree = doubleTree->findChild<TreeView *>("Staged");
  QVERIFY(stagedTree);
  auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(unstagedTree);

  {
    QAbstractItemModel *stagedModel = stagedTree->model();
    QTRY_COMPARE(stagedModel->rowCount(), 1);
    QModelIndex index = stagedModel->index(0, 0); // submodules folder
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "submodules");

    QCOMPARE(stagedModel->rowCount(index), 1);
    index = stagedModel->index(0, 0, index); // submodule1
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "submodule1");
  }

  {
    QAbstractItemModel *unstagedModel = unstagedTree->model();
    QTRY_COMPARE(unstagedModel->rowCount(), 1);
    QModelIndex index = unstagedModel->index(0, 0); // submodules folder
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "submodules");

    QCOMPARE(unstagedModel->rowCount(index), 1);
    index = unstagedModel->index(0, 0, index); // submodule2
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "submodule2");
  }
}

void TestTreeView::conflictedAndStagedFile() {
  INIT_REPO("ConflictedAndStagedFile.zip", false);

  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto stagedTree = doubleTree->findChild<TreeView *>("Staged");
  QVERIFY(stagedTree);
  auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(unstagedTree);

  {
    QAbstractItemModel *stagedModel = stagedTree->model();
    QCOMPARE(stagedModel->rowCount(), 1);
    QModelIndex index = stagedModel->index(0, 0); // "folder" folder
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "folder");

    QCOMPARE(stagedModel->rowCount(index), 1);
    index = stagedModel->index(0, 0, index);
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "NotConflictedFile.txt");
  }

  {
    QAbstractItemModel *unstagedModel = unstagedTree->model();
    QCOMPARE(unstagedModel->rowCount(), 1);
    QModelIndex index = unstagedModel->index(0, 0); // "folder" folder
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "folder");

    QCOMPARE(unstagedModel->rowCount(index), 1);
    index = unstagedModel->index(0, 0, index);
    QVERIFY(index.isValid());
    QCOMPARE(index.data(), "conflictedFile.txt");
  }

  auto *unresolvedOnly = doubleTree->findChild<QCheckBox *>("UnresolvedOnly");
  QVERIFY(unresolvedOnly);
  QVERIFY(!unresolvedOnly->isVisible());

  auto *conflictedLabel = doubleTree->findChild<QLabel *>("UnstagedFilesLabel");
  auto *resolvedLabel = doubleTree->findChild<QLabel *>("StagedFilesLabel");
  QVERIFY(conflictedLabel);
  QVERIFY(resolvedLabel);
  QVERIFY(conflictedLabel->text().startsWith("Conflicted Files"));
  QVERIFY(resolvedLabel->text().startsWith("Resolved Files"));

  stagedTree->deselectAll();
  unstagedTree->deselectAll();
  auto *next = doubleTree->findChild<QToolButton *>("NextConflict");
  QVERIFY(next);
  mouseClick(next, Qt::LeftButton);
  QTRY_COMPARE(doubleTree->selectedFile(), QString("conflictedFile.txt"));

  auto *blame = doubleTree->mBlameButton;
  auto *diff = doubleTree->mDiffButton;
  QVERIFY(blame);
  QVERIFY(diff);
  QTRY_VERIFY(!blame->isEnabled());
  QVERIFY(blame->toolTip().contains("unavailable"));
  QVERIFY(diff->isChecked());
  QCOMPARE(stagedTree->selectionMode(), QAbstractItemView::ExtendedSelection);
  QCOMPARE(unstagedTree->selectionMode(), QAbstractItemView::ExtendedSelection);
}

void TestTreeView::stageAllChangesButton() {
  INIT_REPO("TestRepository.zip", false);

  QFile file(repo.workdir().filePath("file.txt"));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  QCOMPARE(file.write("Stage all changes test\n"), 23);
  file.close();
  refresh(repoView);

  auto *doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto *button = doubleTree->findChild<QPushButton *>("StageAllChangesButton");
  QVERIFY(button);
  QCOMPARE(button, doubleTree->mStageAllChanges);

  auto *unstagedLayout = qobject_cast<QVBoxLayout *>(
      doubleTree->collapseButtonUnstagedFiles->parentWidget()->layout());
  QVERIFY(unstagedLayout);
  auto *headerLayout =
      qobject_cast<QHBoxLayout *>(unstagedLayout->itemAt(0)->layout());
  QVERIFY(headerLayout);
  QCOMPARE(headerLayout->indexOf(button) + 1,
           headerLayout->indexOf(doubleTree->collapseButtonUnstagedFiles));

  QTRY_VERIFY(button->isVisible());
  QTRY_VERIFY(button->isEnabled());
  QTRY_COMPARE(button->height(),
               doubleTree->collapseButtonUnstagedFiles->height());
  QCOMPARE(button->isEnabled(), repoView->isStageEnabled());

  mouseClick(button, Qt::LeftButton);
  QTRY_COMPARE(repo.diffIndexToWorkdir().count(), 0);
  QTRY_VERIFY(!button->isEnabled());
  QCOMPARE(button->isEnabled(), repoView->isStageEnabled());

  auto *commits = repoView->findChild<CommitList *>();
  QVERIFY(commits);
  QModelIndex commitIndex;
  for (int row = 0; row < commits->model()->rowCount(); ++row) {
    QModelIndex candidate = commits->model()->index(row, 0);
    if (candidate.data(CommitList::CommitRole).isValid()) {
      commitIndex = candidate;
      break;
    }
  }
  QVERIFY(commitIndex.isValid());
  commits->selectionModel()->select(commitIndex,
                                    QItemSelectionModel::ClearAndSelect);
  QTRY_VERIFY(!button->isVisible());
}

void TestTreeView::discardAllChangesButton() {
  INIT_REPO("TestRepository.zip", false);

  QFile modified(repo.workdir().filePath("file.txt"));
  QVERIFY(modified.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QVERIFY(modified.write("discarded modification\n") > 0);
  modified.close();

  const QString deletedPath = repo.workdir().filePath("file2.txt");
  QVERIFY(QFile::remove(deletedPath));

  const QString untrackedPath = repo.workdir().filePath("discard-all.txt");
  QFile untracked(untrackedPath);
  QVERIFY(untracked.open(QIODevice::WriteOnly));
  QVERIFY(untracked.write("remove me\n") > 0);
  untracked.close();

  const QString submoduleTrackedPath =
      repo.workdir().filePath("GittyupTestRepo/README.md");
  QFile submoduleTracked(submoduleTrackedPath);
  QVERIFY(submoduleTracked.open(QIODevice::ReadOnly));
  const QByteArray submoduleOriginal = submoduleTracked.readAll();
  submoduleTracked.close();
  QVERIFY(submoduleTracked.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QVERIFY(submoduleTracked.write("discarded submodule modification\n") > 0);
  submoduleTracked.close();

  const QString submoduleUntrackedPath =
      repo.workdir().filePath("GittyupTestRepo/discard-all.txt");
  QFile submoduleUntracked(submoduleUntrackedPath);
  QVERIFY(submoduleUntracked.open(QIODevice::WriteOnly));
  QVERIFY(submoduleUntracked.write("remove me too\n") > 0);
  submoduleUntracked.close();

  refresh(repoView);

  auto *doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto *discard =
      doubleTree->findChild<QToolButton *>("DiscardAllChangesButton");
  auto *stage = doubleTree->findChild<QPushButton *>("StageAllChangesButton");
  QVERIFY(discard);
  QVERIFY(stage);
  QVERIFY(doubleTree->collapseButtonUnstagedFiles);
  QCOMPARE(discard->height(),
           doubleTree->collapseButtonUnstagedFiles->height());

  auto *unstagedLayout = qobject_cast<QVBoxLayout *>(
      doubleTree->collapseButtonUnstagedFiles->parentWidget()->layout());
  QVERIFY(unstagedLayout);
  auto *headerLayout =
      qobject_cast<QHBoxLayout *>(unstagedLayout->itemAt(0)->layout());
  QVERIFY(headerLayout);
  QVERIFY(headerLayout->indexOf(discard) < headerLayout->indexOf(stage));
  QCOMPARE(headerLayout->indexOf(stage) - headerLayout->indexOf(discard), 2);

  QTRY_VERIFY(discard->isVisible());
  QTRY_VERIFY(discard->isEnabled());
  mouseClick(discard, Qt::LeftButton);

  auto *dialog = repoView->findChild<QMessageBox *>();
  QVERIFY(dialog);
  const QString details = dialog->detailedText();
  QVERIFY(details.contains("file.txt"));
  QVERIFY(details.contains("file2.txt"));
  QVERIFY(details.contains("discard-all.txt"));
  QVERIFY(details.contains("GittyupTestRepo/README.md"));
  QVERIFY(details.contains("GittyupTestRepo/discard-all.txt"));

  auto *cancel = dialog->button(QMessageBox::Cancel);
  QVERIFY(cancel);
  mouseClick(cancel, Qt::LeftButton);
  QTRY_VERIFY(!dialog->isVisible());

  QVERIFY(modified.open(QIODevice::ReadOnly));
  QCOMPARE(modified.readAll(), QByteArray("discarded modification\n"));
  QVERIFY(!QFile::exists(deletedPath));
  QVERIFY(QFile::exists(untrackedPath));

  mouseClick(discard, Qt::LeftButton);
  dialog = repoView->findChild<QMessageBox *>();
  QVERIFY(dialog);
  auto *accept = dialog->findChild<QPushButton *>("DiscardButton");
  QVERIFY(accept);
  mouseClick(accept, Qt::LeftButton);

  QTRY_VERIFY(QFile::exists(deletedPath));
  QTRY_VERIFY(!QFile::exists(untrackedPath));
  QTRY_VERIFY(!QFile::exists(submoduleUntrackedPath));
  QTRY_VERIFY([&] {
    QFile restored(deletedPath);
    return restored.open(QIODevice::ReadOnly) &&
           restored.readAll() == QByteArray("file2.txt\n");
  }());
  QTRY_VERIFY([&] {
    QFile restored(modified.fileName());
    return restored.open(QIODevice::ReadOnly) &&
           restored.readAll() == QByteArray("File.txt\n");
  }());
  QTRY_VERIFY([&] {
    QFile restored(submoduleTrackedPath);
    return restored.open(QIODevice::ReadOnly) &&
           restored.readAll() == submoduleOriginal;
  }());
  QTRY_VERIFY(!discard->isEnabled());
}

void TestTreeView::discardAllChangesInUnbornRepository() {
  Test::ScratchRepository repo;

  const QString stagedPath = repo->workdir().filePath("staged.txt");
  QFile staged(stagedPath);
  QVERIFY(staged.open(QIODevice::WriteOnly));
  QVERIFY(staged.write("staged\n") > 0);
  staged.close();
  repo->index().setStaged({"staged.txt"}, true, false);

  const QString untrackedPath = repo->workdir().filePath("untracked.txt");
  QFile untracked(untrackedPath);
  QVERIFY(untracked.open(QIODevice::WriteOnly));
  QVERIFY(untracked.write("untracked\n") > 0);
  untracked.close();

  MainWindow window(repo);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  RepoView *repoView = window.currentView();
  QVERIFY(repoView);
  refresh(repoView);

  auto *doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto *discard =
      doubleTree->findChild<QToolButton *>("DiscardAllChangesButton");
  QVERIFY(discard);
  QTRY_VERIFY(discard->isEnabled());

  mouseClick(discard, Qt::LeftButton);
  auto *dialog = repoView->findChild<QMessageBox *>();
  QVERIFY(dialog);
  QVERIFY(dialog->detailedText().contains("staged.txt"));
  QVERIFY(dialog->detailedText().contains("untracked.txt"));

  auto *accept = dialog->findChild<QPushButton *>("DiscardButton");
  QVERIFY(accept);
  mouseClick(accept, Qt::LeftButton);

  QTRY_VERIFY(!QFile::exists(stagedPath));
  QTRY_VERIFY(!QFile::exists(untrackedPath));
  QTRY_VERIFY(!discard->isEnabled());
}

void TestTreeView::externalRefreshPreservesSelection() {
  INIT_REPO("TestRepository.zip", true);

  auto *commits = repoView->findChild<CommitList *>();
  QVERIFY(commits);
  commits->cancelStatus();
  refresh(repoView, false);

  QModelIndex commitIndex;
  for (int row = 0; row < commits->model()->rowCount(); ++row) {
    QModelIndex candidate = commits->model()->index(row, 0);
    if (candidate.data(CommitList::CommitRole).isValid()) {
      commitIndex = candidate;
      break;
    }
  }
  QVERIFY(commitIndex.isValid());
  commits->selectionModel()->select(commitIndex,
                                    QItemSelectionModel::ClearAndSelect);
  QString selectedCommit = commits->selectedRange();
  QVERIFY(!selectedCommit.isEmpty());

  int detailRefreshes = 0;
  int invalidDetails = 0;
  connect(commits, &CommitList::diffSelected,
          [&detailRefreshes, &invalidDetails](const git::Diff &diff) {
            ++detailRefreshes;
            if (!diff.isValid())
              ++invalidDetails;
          });

  QSignalSpy cleanStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!cleanStatus.isEmpty());
  QCOMPARE(commits->selectedRange(), selectedCommit);
  QCOMPARE(invalidDetails, 0);
  QCOMPARE(detailRefreshes, 0);

  QFile file(repo.workdir().filePath("file.txt"));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  QCOMPARE(file.write("External refresh test\n"), 22);
  file.close();
  refresh(repoView);
  QCOMPARE(commits->selectedRange(), QString("status"));

  auto *doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);
  auto *unstagedFiles = doubleTree->findChild<TreeView *>("Unstaged");
  auto *diffView = repoView->findChild<DiffView *>();
  QVERIFY(unstagedFiles);
  QVERIFY(diffView);
  QTRY_VERIFY(unstagedFiles->model()->rowCount() > 0);
  const QModelIndexList files = unstagedFiles->model()->match(
      unstagedFiles->model()->index(0, 0), Qt::EditRole, QString("file.txt"), 1,
      Qt::MatchExactly | Qt::MatchRecursive);
  QVERIFY(!files.isEmpty());
  unstagedFiles->selectionModel()->setCurrentIndex(
      files.first(),
      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  bool eventLoopAdvanced = false;
  QTimer::singleShot(0, [&eventLoopAdvanced] { eventLoopAdvanced = true; });
  QVERIFY(QMetaObject::invokeMethod(unstagedFiles, "fileSelectionRequested"));
  QVERIFY(repoView->isFileInspectionVisible());
  QVERIFY(!diffView->widget()->findChild<FileWidget *>());
  QTRY_VERIFY(eventLoopAdvanced);
  FileWidget *visibleFile = nullptr;
  QTRY_VERIFY((visibleFile = diffView->widget()->findChild<FileWidget *>()));
  QVERIFY(visibleFile);
  QCOMPARE(visibleFile->name(), QString("file.txt"));
  QVERIFY(!visibleFile->editors().isEmpty());
  QVERIFY(visibleFile->editors().first()->length() > 0);
  QPointer<FileWidget> outgoingFile = visibleFile;
  diffView->updateFiles();
  QVERIFY(outgoingFile);
  QCOMPARE(diffView->widget()->findChild<FileWidget *>(), outgoingFile.data());

  detailRefreshes = 0;
  invalidDetails = 0;
  QSignalSpy dirtyStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!dirtyStatus.isEmpty());
  QCOMPARE(commits->selectedRange(), QString("status"));
  QCOMPARE(detailRefreshes, 1);
  QCOMPARE(invalidDetails, 0);
  QCOMPARE(doubleTree->selectedFile(), QString("file.txt"));
  visibleFile = diffView->widget()->findChild<FileWidget *>();
  QVERIFY(visibleFile);
  QCOMPARE(visibleFile->name(), QString("file.txt"));
  QVERIFY(!visibleFile->editors().isEmpty());
  QVERIFY(visibleFile->editors().first()->length() > 0);
}

void TestTreeView::externalRefreshKeepsEditorContent() {
  INIT_REPO("TestRepository.zip", true);

  auto *commits = repoView->findChild<CommitList *>();
  QVERIFY(commits);
  commits->cancelStatus();
  refresh(repoView, false);
  int detailRefreshes = 0;
  connect(commits, &CommitList::diffSelected,
          [&detailRefreshes](const git::Diff &) { ++detailRefreshes; });

  QModelIndex commitIndex;
  for (int row = 0; row < commits->model()->rowCount(); ++row) {
    QModelIndex candidate = commits->model()->index(row, 0);
    if (candidate.data(CommitList::CommitRole).isValid()) {
      commitIndex = candidate;
      break;
    }
  }
  QVERIFY(commitIndex.isValid());
  commits->selectionModel()->select(commitIndex,
                                    QItemSelectionModel::ClearAndSelect);

  QFile file(repo.workdir().filePath("file.txt"));
  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  QVERIFY(file.write("Initial external refresh test\n") > 0);
  file.close();
  refresh(repoView);

  auto *doubleTree = repoView->findChild<DoubleTreeWidget *>();
  auto *unstagedFiles = repoView->findChild<TreeView *>("Unstaged");
  auto *diffView = repoView->findChild<DiffView *>();
  QVERIFY(doubleTree);
  QVERIFY(unstagedFiles);
  QVERIFY(diffView);
  QTRY_VERIFY(unstagedFiles->model()->rowCount() > 0);

  const QModelIndexList files = unstagedFiles->model()->match(
      unstagedFiles->model()->index(0, 0), Qt::EditRole, QString("file.txt"), 1,
      Qt::MatchExactly | Qt::MatchRecursive);
  QVERIFY(!files.isEmpty());
  unstagedFiles->selectionModel()->setCurrentIndex(
      files.first(),
      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  QVERIFY(QMetaObject::invokeMethod(unstagedFiles, "fileSelectionRequested"));
  QTRY_VERIFY(repoView->isFileInspectionVisible());

  FileWidget *visibleFile = nullptr;
  QTRY_VERIFY((visibleFile = diffView->widget()->findChild<FileWidget *>()));
  QVERIFY(!visibleFile->editors().isEmpty());
  QVERIFY(visibleFile->editors().first()->length() > 0);

  QPointer<FileWidget> outgoingFile = visibleFile;
  bool statusSelectionObserved = false;
  bool outgoingFileVisibleDuringStatusRefresh = false;
  connect(commits, &CommitList::statusSelected,
          [&statusSelectionObserved, &outgoingFile,
           &outgoingFileVisibleDuringStatusRefresh](
              const git::WorkingTreeStatusSnapshot &, const QString &, bool) {
            statusSelectionObserved = true;
            outgoingFileVisibleDuringStatusRefresh =
                outgoingFile && !outgoingFile->isHidden();
          });

  QVERIFY(file.open(QFile::WriteOnly | QFile::Truncate));
  QVERIFY(file.write("Updated external refresh test\n") > 0);
  file.close();
  QSignalSpy statusChanged(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(statusSelectionObserved);
  QVERIFY(outgoingFileVisibleDuringStatusRefresh);
  QTRY_VERIFY(!statusChanged.isEmpty());

  QTRY_VERIFY((visibleFile = diffView->widget()->findChild<FileWidget *>()));
  QCOMPARE(visibleFile->name(), QString("file.txt"));
  QVERIFY(!visibleFile->editors().isEmpty());
  QVERIFY(visibleFile->editors().first()->length() > 0);
  QTRY_VERIFY([visibleFile] {
    const QList<TextEditor *> editors = visibleFile->editors();
    return std::any_of(
        editors.cbegin(), editors.cend(), [](TextEditor *editor) {
          return editor->text().contains("Updated external refresh test");
        });
  }());

  QPointer<FileWidget> stableFile = visibleFile;
  detailRefreshes = 0;
  QSignalSpy unchangedStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!unchangedStatus.isEmpty());
  QTRY_COMPARE(detailRefreshes, 1);
  QCOMPARE(diffView->widget()->findChild<FileWidget *>(), stableFile.data());

  QFile unrelated(repo.workdir().filePath("unrelated.txt"));
  QVERIFY(unrelated.open(QFile::WriteOnly | QFile::Truncate));
  QVERIFY(unrelated.write("Unrelated external refresh test\n") > 0);
  unrelated.close();

  detailRefreshes = 0;
  QSignalSpy unrelatedStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!unrelatedStatus.isEmpty());
  QTRY_COMPARE(detailRefreshes, 1);
  QCOMPARE(diffView->widget()->findChild<FileWidget *>(), stableFile.data());

  auto *stageButton = visibleFile->findChild<QPushButton *>("StageFileButton");
  QVERIFY(stageButton);
  QSignalSpy stagedStatus(repoView, &RepoView::statusChanged);
  stageButton->click();
  QTRY_VERIFY(!stagedStatus.isEmpty());
  QTRY_VERIFY((visibleFile = diffView->widget()->findChild<FileWidget *>()));
  QCOMPARE(visibleFile->header()->check()->checkState(), Qt::Checked);
}

void TestTreeView::externalRefreshPreservesViewport() {
  INIT_REPO("TestRepository.zip", true);

  auto *commits = repoView->findChild<CommitList *>();
  QVERIFY(commits);
  QTRY_COMPARE(commits->selectedRange(), repo.head().target().id().toString());
  commits->cancelStatus();
  refresh(repoView, false);

  commits->setFixedHeight(80);
  commits->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  while (commits->model()->canFetchMore(QModelIndex()))
    commits->model()->fetchMore(QModelIndex());
  QTRY_VERIFY(commits->verticalScrollBar()->maximum() > 0);

  QModelIndex selectedIndex;
  for (int row = 0; row < commits->model()->rowCount(); ++row) {
    QModelIndex index = commits->model()->index(row, 0);
    if (index.data(CommitList::CommitRole).isValid()) {
      selectedIndex = index;
      break;
    }
  }
  QVERIFY(selectedIndex.isValid());
  commits->selectionModel()->select(selectedIndex,
                                    QItemSelectionModel::ClearAndSelect);
  const QString selectedCommit = commits->selectedRange();

  commits->verticalScrollBar()->setValue(
      commits->verticalScrollBar()->maximum() / 2);
  auto topCommit = [commits] {
    QModelIndex index = commits->indexAt(commits->viewport()->rect().topLeft());
    return index.data(CommitList::CommitRole)
        .value<git::Commit>()
        .id()
        .toString();
  };
  const QString viewportCommit = topCommit();
  QVERIFY(!viewportCommit.isEmpty());

  QSignalSpy cleanStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!cleanStatus.isEmpty());
  QCOMPARE(commits->selectedRange(), selectedCommit);
  QCOMPARE(topCommit(), viewportCommit);

  QFile file(repo.workdir().filePath("viewport-refresh.txt"));
  QVERIFY(file.open(QFile::WriteOnly));
  file.close();
  QSignalSpy dirtyStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!dirtyStatus.isEmpty());
  QCOMPARE(commits->selectedRange(), selectedCommit);
  QCOMPARE(topCommit(), viewportCommit);

  QVERIFY(QFile::remove(file.fileName()));
  QSignalSpy cleanFinalStatus(repoView, &RepoView::statusChanged);
  emit repo.notifier()->workdirChanged();
  QTRY_VERIFY(!cleanFinalStatus.isEmpty());
  QCOMPARE(commits->selectedRange(), selectedCommit);
  QCOMPARE(topCommit(), viewportCommit);
}

TEST_MAIN(TestTreeView)

#include "TreeView.moc"

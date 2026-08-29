#include "Test.h"
#include "ui/DiffView/DiffView.h"
#include "ui/DiffView/FileWidget.h"
#include "ui/CommitList.h"
#include "ui/MainWindow.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/StatePushButton.h"
#include "ui/TreeView.h"
#include "ui/TreeProxy.h"
#include "ui/FileContextMenu.h"
#include "conf/Settings.h"
#include "editor/TextEditor.h"

#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextEdit>
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
  void externalRefreshPreservesSelection();

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

  QCheckBox *incoming =
      diffView->findChild<QCheckBox *>("ConflictIncomingBlock_0");
  QVERIFY(incoming);
  mouseClick(incoming, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(), 0);

  QToolButton *markResolved =
      diffView->widget()->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(markResolved);
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
  committedFiles->selectionModel()->clearSelection();
  committedFiles->selectionModel()->select(committedFiles->model()->index(0, 0),
                                           QItemSelectionModel::Select);
  QVERIFY(primaryView->currentWidget() != fileInspection);
  QVERIFY(QMetaObject::invokeMethod(committedFiles, "fileSelectionRequested"));
  QCOMPARE(primaryView->currentWidget(), fileInspection);

  auto *diffButton = repoView->findChild<QPushButton *>("DiffViewButton");
  auto *inlineMode = repoView->findChild<QToolButton *>("InlineDiffMode");
  auto *hunkMode = repoView->findChild<QToolButton *>("HunkDiffMode");
  auto *splitMode = repoView->findChild<QToolButton *>("SplitDiffMode");
  auto *diffView = repoView->findChild<DiffView *>();
  QVERIFY(diffButton);
  QVERIFY(inlineMode);
  QVERIFY(hunkMode);
  QVERIFY(splitMode);
  QVERIFY(diffView);
  QVERIFY(diffButton->isChecked());
  QVERIFY(inlineMode->isChecked());
  auto fileForEditor = [](QWidget *widget) {
    while (widget && !qobject_cast<FileWidget *>(widget))
      widget = widget->parentWidget();
    return qobject_cast<FileWidget *>(widget);
  };
  const QList<TextEditor *> initialEditors = diffView->editors();
  QVERIFY(!initialEditors.isEmpty());
  auto *fileWidget = fileForEditor(initialEditors.first());
  QVERIFY(fileWidget);

  const QString selectedFile =
      committedFiles->selectionModel()->selectedIndexes().first().data(
          Qt::EditRole).toString();
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
  QVERIFY(!blame->isEnabled());
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
  QVERIFY(QMetaObject::invokeMethod(unstagedFiles, "fileSelectionRequested"));
  FileWidget *visibleFile = diffView->widget()->findChild<FileWidget *>();
  QVERIFY(visibleFile);
  QCOMPARE(visibleFile->name(), QString("file.txt"));
  QVERIFY(!visibleFile->editors().isEmpty());
  QVERIFY(visibleFile->editors().first()->length() > 0);

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

TEST_MAIN(TestTreeView)

#include "TreeView.moc"

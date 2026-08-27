//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Shane Gramlich
//

#include "qtsupport.h"
#include "Test.h"
#include "ui/MainWindow.h"
#include "ui/DetailView.h"
#include "ui/DiffView/DiffView.h"
#include "ui/DiffView/FileWidget.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/RepoView.h"
#include "ui/TreeView.h"
#include <QCheckBox>
#include <QFile>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <functional>

using namespace Test;
using namespace QTest;

class TestMerge : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void firstCommit();
  void secondCommit();
  void thirdCommit();
  void mergeConflict();
  void resolve();
  void fileLevelConflicts();
  void cleanupTestCase();

private:
  int inputDelay = 0;
  int closeDelay = 0;

  ScratchRepository mRepo;
  MainWindow *mWindow = nullptr;
  QString mMainBranch;
};

void TestMerge::initTestCase() {
  mMainBranch = mRepo->unbornHeadName();
  mWindow = new MainWindow(mRepo);
  mWindow->show();
  QVERIFY(qWaitForWindowExposed(mWindow));
}

void TestMerge::firstCommit() {
  // Add file and refresh.
  QFile file(mRepo->workdir().filePath("test"));
  QVERIFY(file.open(QFile::WriteOnly));
  QTextStream(&file) << "Resolver test\n"
                        "First shared line\n"
                        "First base choice\n"
                        "Spacer 1\n"
                        "Spacer 2\n"
                        "Spacer 3\n"
                        "Spacer 4\n"
                        "Spacer 5\n"
                        "Spacer 6\n"
                        "Spacer 7\n"
                        "Spacer 8\n"
                        "Second shared line\n"
                        "Second base choice\n"
                        "Resolver end\n";

  RepoView *view = mWindow->currentView();
  refresh(view);

  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  QAbstractItemModel *model = files->model();
  QCOMPARE(model->rowCount(), 1);

  // Click on the check box.
  QModelIndex index = model->index(0, 0);
  mouseClick(files->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
             files->checkRect(index).center());

  // Commit and refresh.
  QTextEdit *editor = view->findChild<QTextEdit *>("MessageEditor");
  QVERIFY(editor);

  editor->setText("base commit");
  view->commit();
  refresh(view, false);
}

void TestMerge::secondCommit() {
  RepoView *view = mWindow->currentView();
  git::Branch branch = mRepo->createBranch("branch2", mRepo->head().target());
  QVERIFY(branch.isValid());

  view->checkout(branch);
  QCOMPARE(mRepo->head().name(), QString("branch2"));

  QFile file(mRepo->workdir().filePath("test"));
  QVERIFY(file.open(QFile::WriteOnly));
  QTextStream(&file) << "Resolver test\n"
                        "First shared line\n"
                        "FIRST INCOMING\n"
                        "Spacer 1\n"
                        "Spacer 2\n"
                        "Spacer 3\n"
                        "Spacer 4\n"
                        "Spacer 5\n"
                        "Spacer 6\n"
                        "Spacer 7\n"
                        "Spacer 8\n"
                        "Second shared line\n"
                        "SECOND INCOMING\n"
                        "Resolver end\n";

  refresh(view);

  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  QAbstractItemModel *model = files->model();
  QCOMPARE(model->rowCount(), 1);

  // Click on the check box.
  QModelIndex index = model->index(0, 0);
  mouseClick(files->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
             files->checkRect(index).center());

  // Commit and refresh.
  QTextEdit *editor = view->findChild<QTextEdit *>("MessageEditor");
  QVERIFY(editor);

  editor->setText("conflicting commit b");
  view->commit();
  refresh(view, false);
}

void TestMerge::thirdCommit() {
  RepoView *view = mWindow->currentView();
  git::Reference ref =
      mRepo->lookupRef(QString("refs/heads/%1").arg(mMainBranch));
  QVERIFY(ref);

  view->checkout(ref);
  QCOMPARE(mRepo->head().name(), mMainBranch);

  QFile file(mRepo->workdir().filePath("test"));
  QVERIFY(file.open(QFile::WriteOnly));
  QTextStream(&file) << "Resolver test\n"
                        "First shared line\n"
                        "FIRST CURRENT\n"
                        "Spacer 1\n"
                        "Spacer 2\n"
                        "Spacer 3\n"
                        "Spacer 4\n"
                        "Spacer 5\n"
                        "Spacer 6\n"
                        "Spacer 7\n"
                        "Spacer 8\n"
                        "Second shared line\n"
                        "SECOND CURRENT\n"
                        "Resolver end\n";

  refresh(view);

  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  QAbstractItemModel *model = files->model();
  QCOMPARE(model->rowCount(), 1);

  // Click on the check box.
  QModelIndex index = model->index(0, 0);
  mouseClick(files->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
             files->checkRect(index).center());

  // Commit and refresh.
  QTextEdit *editor = view->findChild<QTextEdit *>("MessageEditor");
  QVERIFY(editor);

  editor->setText("conflicting commit a");
  view->commit();
  refresh(view, false);
}

void TestMerge::mergeConflict() {
  RepoView *view = mWindow->currentView();
  git::Reference master =
      mRepo->lookupRef(QString("refs/heads/%1").arg(mMainBranch));
  QVERIFY(master);

  git::Reference branch2 = mRepo->lookupRef("refs/heads/branch2");
  QVERIFY(branch2);

  QCOMPARE(mRepo->head().name(), mMainBranch);

  view->merge(RepoView::Merge, branch2);

  // Diff is in a conflicted state
  git::Diff diff = mRepo->diffIndexToWorkdir();
  QVERIFY(diff.isConflicted());
}

void TestMerge::resolve() {
  RepoView *view = mWindow->currentView();
  DiffView *diffView = view->findChild<DiffView *>();

  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  // Wait for refresh
  QAbstractItemModel *model = files->model();
  qWait(1000); // Because before the merge, there is already an item in the
               // unstaged model
  while (model->rowCount() < 1)
    qWait(300);

  QModelIndex file = files->model()->index(0, 0);
  files->selectionModel()->select(file, QItemSelectionModel::Select);
  QVERIFY(QMetaObject::invokeMethod(files, "fileSelectionRequested"));

  FileWidget *fileWidget = diffView->findChild<FileWidget *>();
  QVERIFY(fileWidget);
  QCOMPARE(fileWidget->hunks().size(), 0);

  QToolButton *currentLine =
      fileWidget->findChild<QToolButton *>("ConflictCurrentBubble_0_0");
  QCheckBox *firstIncoming =
      fileWidget->findChild<QCheckBox *>("ConflictIncomingBlock_0");
  QCheckBox *secondIncoming =
      fileWidget->findChild<QCheckBox *>("ConflictIncomingBlock_1");
  QVERIFY(currentLine);
  QVERIFY(firstIncoming);
  QVERIFY(secondIncoming);

  QToolButton *markResolved =
      fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(markResolved);
  QVERIFY(markResolved->isEnabled());

  mouseClick(currentLine, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);
  mouseClick(firstIncoming, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);

  QTimer::singleShot(0, [] {
    if (auto *message =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      message->button(QMessageBox::Cancel)->click();
  });
  mouseClick(markResolved, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);
  QVERIFY(mRepo->index().hasConflicts());

  mouseClick(secondIncoming, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);
  QVERIFY(markResolved->isEnabled());

  QPlainTextEdit *result =
      fileWidget->findChild<QPlainTextEdit *>("ConflictResult");
  QVERIFY(result);
  QVERIFY(result->toPlainText().indexOf("FIRST CURRENT") <
          result->toPlainText().indexOf("FIRST INCOMING"));
  QString editedResult = result->toPlainText();
  editedResult.replace("Resolver end", "Manual result edit\nResolver end");
  result->setPlainText(editedResult);
  mouseClick(currentLine, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);
  QCOMPARE(result->toPlainText(), editedResult);
  QVERIFY(markResolved->isEnabled());

  QFile external(mRepo->workdir().filePath("externally-staged"));
  QVERIFY(external.open(QFile::WriteOnly));
  QCOMPARE(external.write("preserve me\n"), 12);
  external.close();
  QProcess git;
  git.setWorkingDirectory(mRepo->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "externally-staged"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  mouseClick(markResolved, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);

  QTRY_VERIFY(!mRepo->index().hasConflicts());
  QCOMPARE(mRepo->index().isStaged("test"), git::Index::Staged);
  QCOMPARE(mRepo->index().isStaged("externally-staged"), git::Index::Staged);

  QFile resolved(mRepo->workdir().filePath("test"));
  QVERIFY(resolved.open(QFile::ReadOnly));
  const QByteArray expected = "Resolver test\n"
                              "First shared line\n"
                              "FIRST CURRENT\n"
                              "FIRST INCOMING\n"
                              "Spacer 1\n"
                              "Spacer 2\n"
                              "Spacer 3\n"
                              "Spacer 4\n"
                              "Spacer 5\n"
                              "Spacer 6\n"
                              "Spacer 7\n"
                              "Spacer 8\n"
                              "Second shared line\n"
                              "SECOND INCOMING\n"
                              "Manual result edit\n"
                              "Resolver end\n";
  QCOMPARE(resolved.readAll(), expected);

  // Commit and refresh.
  QTextEdit *editor = view->findChild<QTextEdit *>("MessageEditor");
  QVERIFY(editor);

  editor->setText("conflicts resolved");
  view->commit();
  refresh(view, false);

  // Diff is not in a conflicted state
  git::Diff diff = mRepo->diffIndexToWorkdir();
  QVERIFY(!diff.isConflicted());
}

void TestMerge::fileLevelConflicts() {
  RepoView *view = mWindow->currentView();
  auto runGit = [this](const QStringList &arguments) {
    QProcess git;
    git.setWorkingDirectory(mRepo->workdir().path());
    git.start(GIT_EXECUTABLE, arguments);
    if (!git.waitForFinished())
      return -1;
    return git.exitCode();
  };
  auto writeFile = [this](const QString &name, const QByteArray &content) {
    QFile file(mRepo->workdir().filePath(name));
    if (!file.open(QFile::WriteOnly | QFile::Truncate))
      return false;
    return file.write(content) == content.size();
  };

  QVERIFY(writeFile("binary-conflict.bin", QByteArray("\0base", 5)));
  QVERIFY(writeFile("current-deleted.txt", "base\n"));
  QVERIFY(writeFile("incoming-deleted.txt", "base\n"));
  QCOMPARE(runGit({"add", "binary-conflict.bin", "current-deleted.txt",
                   "incoming-deleted.txt"}),
           0);
  QCOMPARE(runGit({"commit", "-m", "add file-level conflict fixtures"}), 0);
  QCOMPARE(runGit({"checkout", "-b", "file-conflicts"}), 0);

  QVERIFY(writeFile("binary-conflict.bin", QByteArray("\0incoming", 9)));
  QVERIFY(writeFile("current-deleted.txt", "incoming modified\n"));
  QCOMPARE(runGit({"rm", "incoming-deleted.txt"}), 0);
  QCOMPARE(runGit({"add", "binary-conflict.bin", "current-deleted.txt"}), 0);
  QCOMPARE(runGit({"commit", "-m", "modify incoming fixtures"}), 0);
  QCOMPARE(runGit({"checkout", mMainBranch}), 0);

  QVERIFY(writeFile("binary-conflict.bin", QByteArray("\0current", 8)));
  QVERIFY(writeFile("incoming-deleted.txt", "current modified\n"));
  QCOMPARE(runGit({"add", "binary-conflict.bin"}), 0);
  QCOMPARE(runGit({"add", "incoming-deleted.txt"}), 0);
  QCOMPARE(runGit({"rm", "current-deleted.txt"}), 0);
  QCOMPARE(runGit({"commit", "-m", "modify and delete current fixtures"}), 0);
  QCOMPARE(runGit({"merge", "file-conflicts"}), 1);

  mRepo->index().read();
  refresh(view);
  QVERIFY(mRepo->index().hasConflicts());

  auto *doubleTree = view->findChild<DoubleTreeWidget *>();
  auto *files = doubleTree->findChild<TreeView *>("Unstaged");
  auto *diffView = view->findChild<DiffView *>();
  QVERIFY(files);
  QVERIFY(diffView);

  auto findFile = [](QAbstractItemModel *model, const QString &name) {
    std::function<QModelIndex(const QModelIndex &)> find =
        [&](const QModelIndex &parent) -> QModelIndex {
      for (int row = 0; row < model->rowCount(parent); ++row) {
        QModelIndex index = model->index(row, 0, parent);
        if (index.data(Qt::EditRole).toString() == name)
          return index;
        QModelIndex child = find(index);
        if (child.isValid())
          return child;
      }
      return QModelIndex();
    };
    return find(QModelIndex());
  };
  auto openFile = [&](const QString &name) {
    QModelIndex index = findFile(files->model(), name);
    if (!index.isValid())
      return static_cast<FileWidget *>(nullptr);
    files->selectionModel()->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QMetaObject::invokeMethod(files, "fileSelectionRequested");
    return diffView->widget()->findChild<FileWidget *>();
  };

  FileWidget *fileWidget = openFile("binary-conflict.bin");
  QVERIFY(fileWidget);
  QVERIFY(fileWidget->findChild<QWidget *>("FileConflictResolver"));
  auto *incoming =
      fileWidget->findChild<QToolButton *>("ConflictFileIncomingChoice");
  auto *markResolved =
      fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(incoming);
  QVERIFY(markResolved);
  QVERIFY(!markResolved->isEnabled());
  mouseClick(incoming, Qt::LeftButton);
  QVERIFY(markResolved->isEnabled());
  mouseClick(markResolved, Qt::LeftButton);

  QTRY_COMPARE(mRepo->index().isStaged("binary-conflict.bin"),
               git::Index::Staged);
  QFile binary(mRepo->workdir().filePath("binary-conflict.bin"));
  QVERIFY(binary.open(QFile::ReadOnly));
  QCOMPARE(binary.readAll(), QByteArray("\0incoming", 9));

  QTRY_VERIFY(!findFile(files->model(), "binary-conflict.bin").isValid());
  QTRY_VERIFY(findFile(files->model(), "current-deleted.txt").isValid());
  fileWidget = openFile("current-deleted.txt");
  QVERIFY(fileWidget);
  auto *current =
      fileWidget->findChild<QToolButton *>("ConflictFileCurrentChoice");
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(current);
  QVERIFY(markResolved);
  mouseClick(current, Qt::LeftButton);
  QVERIFY(markResolved->isEnabled());
  mouseClick(markResolved, Qt::LeftButton);

  QTRY_VERIFY(!findFile(files->model(), "current-deleted.txt").isValid());
  QVERIFY(!QFileInfo::exists(mRepo->workdir().filePath("current-deleted.txt")));

  QTRY_VERIFY(findFile(files->model(), "incoming-deleted.txt").isValid());
  fileWidget = openFile("incoming-deleted.txt");
  QVERIFY(fileWidget);
  incoming = fileWidget->findChild<QToolButton *>("ConflictFileIncomingChoice");
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(incoming);
  QVERIFY(markResolved);
  mouseClick(incoming, Qt::LeftButton);
  QVERIFY(markResolved->isEnabled());
  mouseClick(markResolved, Qt::LeftButton);

  QTRY_VERIFY(!mRepo->index().hasConflicts());
  QVERIFY(
      !QFileInfo::exists(mRepo->workdir().filePath("incoming-deleted.txt")));
}

void TestMerge::cleanupTestCase() {
  qWait(closeDelay);
  mWindow->close();
}

TEST_MAIN(TestMerge)

#include "merge.moc"

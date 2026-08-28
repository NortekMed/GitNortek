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
#include "git/Commit.h"
#include "ui/MainWindow.h"
#include "ui/DetailView.h"
#include "ui/DiffTreeModel.h"
#include "ui/DiffView/DiffView.h"
#include "ui/DiffView/FileWidget.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/RepoView.h"
#include "ui/TreeView.h"
#include <QCheckBox>
#include <QFile>
#include <QLabel>
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

  auto *commit = view->findChild<QPushButton *>("CommitButton");
  auto *abort = view->findChild<QPushButton *>("AbortMergeButton");
  QVERIFY(commit);
  QVERIFY(abort);
  QCOMPARE(commit->text(), QString("Commit and Merge"));
  QVERIFY(commit->styleSheet().contains("#36c96b"));
  QVERIFY(!commit->isEnabled());
  QTRY_VERIFY(abort->isVisible());
  QVERIFY(abort->isEnabled());
  QVERIFY(abort->styleSheet().contains("#c93c4a"));
  auto *markAll = view->findChild<QPushButton *>("MarkAllResolved");
  QVERIFY(markAll);
  QVERIFY(markAll->styleSheet().contains("#d6a321"));
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
  QVERIFY(currentLine->parentWidget()->styleSheet().contains(
      "rgba(45, 164, 78, 0.18)"));

  QPlainTextEdit *result =
      fileWidget->findChild<QPlainTextEdit *>("ConflictResult");
  QCheckBox *currentMaster =
      fileWidget->findChild<QCheckBox *>("ConflictCurrentPanelMaster");
  QCheckBox *incomingMaster =
      fileWidget->findChild<QCheckBox *>("ConflictIncomingPanelMaster");
  QVERIFY(result);
  QVERIFY(currentMaster);
  QVERIFY(incomingMaster);
  QVERIFY2(result->toPlainText().contains("First base choice"),
           qPrintable(result->toPlainText()));
  QVERIFY(result->toPlainText().contains("Second base choice"));

  mouseClick(currentMaster, Qt::LeftButton);
  QCOMPARE(currentMaster->checkState(), Qt::Checked);
  QVERIFY(result->toPlainText().contains("FIRST CURRENT"));
  QVERIFY(result->toPlainText().contains("SECOND CURRENT"));
  const QColor currentResultColor(45, 164, 78, 46);
  const QColor incomingResultColor(9, 105, 218, 46);
  auto hasResultColor = [result](const QColor &color) {
    for (const QTextEdit::ExtraSelection &selection :
         result->extraSelections()) {
      if (selection.format.background().color() == color)
        return true;
    }
    return false;
  };
  QVERIFY(hasResultColor(currentResultColor));
  QVERIFY(!hasResultColor(incomingResultColor));
  mouseClick(incomingMaster, Qt::LeftButton);
  QCOMPARE(incomingMaster->checkState(), Qt::Checked);
  QVERIFY(hasResultColor(currentResultColor));
  QVERIFY(hasResultColor(incomingResultColor));
  QVERIFY(result->toPlainText().indexOf("FIRST CURRENT") <
          result->toPlainText().indexOf("FIRST INCOMING"));
  mouseClick(currentMaster, Qt::LeftButton);
  mouseClick(incomingMaster, Qt::LeftButton);
  QCOMPARE(currentMaster->checkState(), Qt::Unchecked);
  QCOMPARE(incomingMaster->checkState(), Qt::Unchecked);
  QVERIFY(result->toPlainText().contains("First base choice"));

  QToolButton *markResolved =
      fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(markResolved);
  QVERIFY(markResolved->isEnabled());
  QVERIFY(markResolved->styleSheet().contains("#36c96b"));

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
  QTRY_VERIFY(!view->isFileInspectionVisible());
  QCOMPARE(mRepo->index().isStaged("test"), git::Index::Staged);
  QCOMPARE(mRepo->index().isStaged("externally-staged"), git::Index::Staged);

  // Resolved paths remain staged when inspecting a historical diff.
  git::Diff historicalDiff = mRepo->head().target().diff();
  QVERIFY(historicalDiff.isValid());
  QVERIFY(!historicalDiff.isStatusDiff());
  DiffTreeModel historicalModel(mRepo);
  historicalModel.setDiff(historicalDiff, {"test"});
  QModelIndex historicalFile = historicalModel.index(0, 0);
  QVERIFY(historicalFile.isValid());
  QCOMPARE(historicalFile.data(Qt::CheckStateRole).value<Qt::CheckState>(),
           Qt::Checked);

  auto *commit = view->findChild<QPushButton *>("CommitButton");
  QVERIFY(commit);
  QTextEdit *editor = view->findChild<QTextEdit *>("MessageEditor");
  QVERIFY(editor);
  editor->clear();
  QVERIFY(!commit->isEnabled());
  editor->setText("   \n");
  QVERIFY(!commit->isEnabled());
  editor->setText("conflicts resolved");
  QVERIFY(commit->isEnabled());

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

  // Commit and refresh from the branch view.
  mouseClick(commit, Qt::LeftButton);
  QTRY_VERIFY(!view->isFileInspectionVisible());
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
  auto readFile = [this](const QString &name) {
    QFile file(mRepo->workdir().filePath(name));
    if (!file.open(QFile::ReadOnly))
      return QByteArray();
    return file.readAll();
  };
  auto largeContent = [](const QByteArray &conflictLine) {
    QByteArray content;
    content.reserve(7500 * 16);
    for (int line = 0; line < 7500; ++line) {
      if (line == 3750)
        content.append(conflictLine).append('\n');
      else
        content.append("unchanged line ")
            .append(QByteArray::number(line))
            .append('\n');
    }
    return content;
  };

  QVERIFY(writeFile("binary-conflict.bin", QByteArray("\0base", 5)));
  QVERIFY(writeFile(".gitattributes", "*.bin binary\nchunks.txt -text\n"));
  QVERIFY(writeFile("attribute-binary.bin", "base text\n"));
  QVERIFY(writeFile("chunks.txt", "base chunk\r\n"));
  QVERIFY(writeFile("large-conflict.txt", largeContent("base choice")));
  QVERIFY(writeFile("current-deleted.txt", "base\n"));
  QVERIFY(writeFile("incoming-deleted.txt", "base\n"));
  QCOMPARE(runGit({"add", ".gitattributes", "attribute-binary.bin",
                   "binary-conflict.bin", "chunks.txt", "current-deleted.txt",
                   "incoming-deleted.txt", "large-conflict.txt"}),
           0);
  QCOMPARE(runGit({"commit", "-m", "add file-level conflict fixtures"}), 0);
  QCOMPARE(runGit({"checkout", "-b", "file-conflicts"}), 0);

  QVERIFY(writeFile("binary-conflict.bin", QByteArray("\0incoming", 9)));
  QVERIFY(writeFile("attribute-binary.bin", "incoming text\n"));
  QVERIFY(writeFile("both-empty.txt", "incoming added\n"));
  QVERIFY(writeFile("chunks.txt", "incoming chunk\r\n"));
  QVERIFY(writeFile("current-deleted.txt", "incoming modified\r\n"));
  QVERIFY(writeFile("large-conflict.txt", largeContent("incoming choice")));
  QCOMPARE(runGit({"rm", "incoming-deleted.txt"}), 0);
  QCOMPARE(runGit({"add", "attribute-binary.bin", "binary-conflict.bin",
                   "both-empty.txt", "chunks.txt", "current-deleted.txt",
                   "large-conflict.txt"}),
           0);
  QCOMPARE(runGit({"commit", "-m", "modify incoming fixtures"}), 0);
  QCOMPARE(runGit({"checkout", mMainBranch}), 0);

  QVERIFY(writeFile("binary-conflict.bin", QByteArray("\0current", 8)));
  QVERIFY(writeFile("attribute-binary.bin", "current text\n"));
  QVERIFY(writeFile("both-empty.txt", "current added\n"));
  QVERIFY(writeFile("chunks.txt", "current chunk\r\n"));
  QVERIFY(writeFile("large-conflict.txt", largeContent("current choice")));
  QVERIFY(writeFile("incoming-deleted.txt", "current modified\n"));
  QCOMPARE(runGit({"add", "attribute-binary.bin", "binary-conflict.bin",
                   "both-empty.txt", "chunks.txt", "large-conflict.txt"}),
           0);
  QCOMPARE(runGit({"add", "incoming-deleted.txt"}), 0);
  QCOMPARE(runGit({"rm", "current-deleted.txt"}), 0);
  QCOMPARE(runGit({"commit", "-m", "modify and delete current fixtures"}), 0);
  QCOMPARE(runGit({"merge", "file-conflicts"}), 1);

  mRepo->index().read();
  refresh(view);
  QVERIFY(mRepo->index().hasConflicts());

  auto *doubleTree = view->findChild<DoubleTreeWidget *>();
  auto *files = doubleTree->findChild<TreeView *>("Unstaged");
  auto *resolvedFiles = doubleTree->findChild<TreeView *>("Staged");
  auto *diffView = view->findChild<DiffView *>();
  QVERIFY(files);
  QVERIFY(resolvedFiles);
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
    const QList<FileWidget *> widgets =
        diffView->widget()->findChildren<FileWidget *>();
    for (auto it = widgets.crbegin(); it != widgets.crend(); ++it) {
      if ((*it)->name() == name)
        return *it;
    }
    return static_cast<FileWidget *>(nullptr);
  };

  FileWidget *fileWidget = openFile("both-empty.txt");
  QVERIFY(fileWidget);
  auto *result = fileWidget->findChild<QPlainTextEdit *>("ConflictResult");
  auto *markResolved =
      fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(result);
  QVERIFY(markResolved);
  QVERIFY(!fileWidget->findChild<QToolButton *>("ConflictExternalMerge")
               ->isHidden());
  QCOMPARE(result->toPlainText(), QString());
  result->setPlainText("unsaved draft");
  QTimer::singleShot(0, [] {
    if (auto *message =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      message->button(QMessageBox::Cancel)->click();
  });
  files->selectionModel()->setCurrentIndex(
      findFile(files->model(), "binary-conflict.bin"),
      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  QCOMPARE(files->currentIndex().data(Qt::EditRole).toString(),
           QString("both-empty.txt"));
  QTimer::singleShot(0, [] {
    if (auto *message =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      message->button(QMessageBox::Cancel)->click();
  });
  doubleTree->closeFileInspection();
  QVERIFY(view->isFileInspectionVisible());
  result->clear();
  QVERIFY(!fileWidget->hasUnsavedConflictOutput());
  QPointer<FileWidget> previousFile = fileWidget;
  QTimer::singleShot(0, [] {
    if (auto *message =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      for (QPushButton *button : message->findChildren<QPushButton *>()) {
        if (button->text() == "Save Output") {
          button->click();
          return;
        }
      }
    }
  });
  mouseClick(markResolved, Qt::LeftButton);
  QTRY_COMPARE(mRepo->index().isStaged("both-empty.txt"), git::Index::Staged);
  QCOMPARE(QFileInfo(mRepo->workdir().filePath("both-empty.txt")).size(), 0);
  QTRY_VERIFY(!findFile(files->model(), "both-empty.txt").isValid());
  QTRY_VERIFY(previousFile.isNull());
  QTRY_VERIFY(files->currentIndex().isValid());
  QVERIFY(files->currentIndex().data(Qt::EditRole).toString() !=
          "both-empty.txt");

  QTRY_VERIFY(findFile(files->model(), "chunks.txt").isValid());
  fileWidget = openFile("chunks.txt");
  QVERIFY(fileWidget);
  QCOMPARE(fileWidget->name(), QString("chunks.txt"));
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(markResolved);
  QVERIFY(markResolved->isEnabled());
  previousFile = fileWidget;
  QTimer::singleShot(0, [] {
    if (auto *message =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      for (QPushButton *button : message->findChildren<QPushButton *>()) {
        if (button->text() == "Create and Stage Conflict Chunks") {
          button->click();
          return;
        }
      }
    }
  });
  mouseClick(markResolved, Qt::LeftButton);
  QTRY_COMPARE(mRepo->index().isStaged("chunks.txt"), git::Index::Staged);
  QVERIFY(!mRepo->index().conflict("chunks.txt").isValid());
  QFile chunks(mRepo->workdir().filePath("chunks.txt"));
  QVERIFY(chunks.open(QFile::ReadOnly));
  const QByteArray chunkOutput = chunks.readAll();
  const QByteArray expectedChunks = "<<<<<<< Current\r\n"
                                    "current chunk\r\n"
                                    "||||||| Base\r\n"
                                    "base chunk\r\n"
                                    "=======\r\n"
                                    "incoming chunk\r\n"
                                    ">>>>>>> Incoming\r\n";
  QCOMPARE(chunkOutput, expectedChunks);
  QTRY_VERIFY(!findFile(files->model(), "chunks.txt").isValid());
  QTRY_VERIFY(previousFile.isNull());

  QTRY_VERIFY(findFile(files->model(), "attribute-binary.bin").isValid());
  git::Diff attributedDiff = mRepo->diffIndexToWorkdir();
  bool foundAttributedPatch = false;
  for (int i = 0; i < attributedDiff.count(); ++i) {
    const git::Patch patch = attributedDiff.patch(i);
    if (patch.name() == "attribute-binary.bin") {
      foundAttributedPatch = true;
      QVERIFY(!patch.conflictBlocks().isEmpty());
      break;
    }
  }
  QVERIFY(foundAttributedPatch);
  fileWidget = openFile("attribute-binary.bin");
  QVERIFY(fileWidget);
  QVERIFY(fileWidget->findChild<QWidget *>("ConflictResolver"));
  auto *currentMaster =
      fileWidget->findChild<QCheckBox *>("ConflictCurrentPanelMaster");
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(currentMaster);
  QVERIFY(markResolved);
  mouseClick(currentMaster, Qt::LeftButton);
  mouseClick(markResolved, Qt::LeftButton);
  QTRY_VERIFY(!findFile(files->model(), "attribute-binary.bin").isValid());
  QTRY_VERIFY(
      findFile(resolvedFiles->model(), "attribute-binary.bin").isValid());
  QCOMPARE(readFile("attribute-binary.bin"), QByteArray("current text\n"));

  QTRY_VERIFY(findFile(files->model(), "large-conflict.txt").isValid());
  fileWidget = openFile("large-conflict.txt");
  QVERIFY(fileWidget);
  auto *largeResolver = fileWidget->findChild<QWidget *>("ConflictResolver");
  QVERIFY(largeResolver);
  result = fileWidget->findChild<QPlainTextEdit *>("ConflictResult");
  currentMaster =
      fileWidget->findChild<QCheckBox *>("ConflictCurrentPanelMaster");
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(result);
  QVERIFY(currentMaster);
  QVERIFY(markResolved);
  QCOMPARE(result->toPlainText(),
           QString::fromUtf8(largeContent("base choice")));
  QVERIFY(largeResolver->findChildren<QWidget *>("ConflictSourceOmittedLines")
              .size() >= 2);
  QVERIFY(largeResolver->findChildren<QToolButton *>().size() < 50);
  mouseClick(currentMaster, Qt::LeftButton);
  QVERIFY(result->toPlainText().contains("current choice"));
  mouseClick(markResolved, Qt::LeftButton);
  QTRY_VERIFY(!findFile(files->model(), "large-conflict.txt").isValid());
  QCOMPARE(readFile("large-conflict.txt"), largeContent("current choice"));

  QTRY_VERIFY(findFile(files->model(), "binary-conflict.bin").isValid());
  fileWidget = openFile("binary-conflict.bin");
  QVERIFY(fileWidget);
  QVERIFY(fileWidget->findChild<QWidget *>("FileConflictResolver"));
  auto *current =
      fileWidget->findChild<QToolButton *>("ConflictFileCurrentChoice");
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(current);
  QVERIFY(markResolved);
  QVERIFY(markResolved->isEnabled());
  auto *outputInfo = fileWidget->findChild<QLabel *>("FileConflictOutputInfo");
  QVERIFY(outputInfo);
  QVERIFY(outputInfo->text().contains("Binary output"));
  mouseClick(current, Qt::LeftButton);
  QVERIFY(markResolved->isEnabled());
  mouseClick(markResolved, Qt::LeftButton);

  QTRY_COMPARE(mRepo->index().isStaged("binary-conflict.bin"),
               git::Index::Staged);
  QFile binary(mRepo->workdir().filePath("binary-conflict.bin"));
  QVERIFY(binary.open(QFile::ReadOnly));
  QCOMPARE(binary.readAll(), QByteArray("\0current", 8));

  QTRY_VERIFY(!findFile(files->model(), "binary-conflict.bin").isValid());
  QTRY_VERIFY(
      findFile(resolvedFiles->model(), "binary-conflict.bin").isValid());
  QTRY_VERIFY(findFile(files->model(), "current-deleted.txt").isValid());
  fileWidget = openFile("current-deleted.txt");
  QVERIFY(fileWidget);
  auto *incoming =
      fileWidget->findChild<QToolButton *>("ConflictFileIncomingChoice");
  markResolved = fileWidget->findChild<QToolButton *>("ConflictMarkResolved");
  QVERIFY(incoming);
  QVERIFY(markResolved);
  mouseClick(incoming, Qt::LeftButton);
  QVERIFY(markResolved->isEnabled());
  mouseClick(markResolved, Qt::LeftButton);

  QTRY_VERIFY(!findFile(files->model(), "current-deleted.txt").isValid());
  QCOMPARE(readFile("current-deleted.txt"), QByteArray("incoming modified\n"));

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
  QTRY_VERIFY(!view->isFileInspectionVisible());
  QVERIFY(
      !QFileInfo::exists(mRepo->workdir().filePath("incoming-deleted.txt")));

  QCOMPARE(runGit({"merge", "--abort"}), 0);
  QCOMPARE(runGit({"merge", "file-conflicts"}), 1);
  mRepo->index().read();

  MainWindow conflictedWindow(mRepo, nullptr, Qt::WindowFlags(), false);
  RepoView *conflictedView = conflictedWindow.currentView();
  auto *conflictedTree = conflictedView->findChild<DoubleTreeWidget *>();
  QVERIFY(conflictedTree);
  QTRY_VERIFY(conflictedTree->setDiffCounter() > 0);
  QVERIFY(mRepo->index().hasConflicts());
  QVERIFY(!conflictedView->isFileInspectionVisible());
  const uint32_t initialDiffCount = conflictedTree->setDiffCounter();
  conflictedView->refresh(false);
  QTRY_VERIFY(conflictedTree->setDiffCounter() > initialDiffCount);
  QVERIFY(!conflictedView->isFileInspectionVisible());

  git::Diff bulkDiff = mRepo->diffIndexToWorkdir();
  QVERIFY(FileWidget::resolveAllConflicts(bulkDiff).isEmpty());
  QVERIFY(!mRepo->index().hasConflicts());
  QCOMPARE(readFile("both-empty.txt"), QByteArray("current added\n"));
  QCOMPARE(readFile("chunks.txt"), QByteArray("current chunk\r\n"));
  QCOMPARE(readFile("large-conflict.txt"), largeContent("current choice"));
  QCOMPARE(readFile("binary-conflict.bin"), QByteArray("\0current", 8));
  QVERIFY(!QFileInfo::exists(mRepo->workdir().filePath("current-deleted.txt")));
  QCOMPARE(readFile("incoming-deleted.txt"), QByteArray("current modified\n"));

  auto *abort = view->findChild<QPushButton *>("AbortMergeButton");
  QVERIFY(abort);
  view->setFileInspectionVisible(true);
  QVERIFY(view->isFileInspectionVisible());
  mouseClick(abort, Qt::LeftButton);
  QTRY_COMPARE(mRepo->state(), GIT_REPOSITORY_STATE_NONE);
  QVERIFY(!view->isFileInspectionVisible());
}

void TestMerge::cleanupTestCase() {
  qWait(closeDelay);
  mWindow->close();
}

TEST_MAIN(TestMerge)

#include "merge.moc"

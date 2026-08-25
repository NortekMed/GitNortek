//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Shane Gramlich
//

#include "Test.h"
#include "git/Config.h"
#include "log/LogEntry.h"
#include "log/LogView.h"
#include "ui/ConfigKeys.h"
#include "ui/CommitList.h"
#include "ui/DetailView.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "ui/TreeView.h"
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>

using namespace Test;
using namespace QTest;

class TestLineEndings : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void createFile();
  void commitFile();
  void overwriteFile();
  void dirtyStatusWhenStatusRowHidden();
  void testLineEndings();
  void repositoryDiagnostics();
  void cleanupTestCase();

private:
  int inputDelay = 0;
  int closeDelay = 0;

  ScratchRepository mRepo;
  MainWindow *mWindow = nullptr;
  RepoView *mRepoView = nullptr;
};

void TestLineEndings::initTestCase() {
  mWindow = new MainWindow(mRepo);
  mWindow->show();
  QVERIFY(qWaitForWindowActive(mWindow));
  mRepoView = mWindow->currentView();
}

void TestLineEndings::createFile() {
  QDir workingDirectory = mRepo->workdir();
  QFile mFile(workingDirectory.filePath("test_file"));
  QVERIFY(mFile.open(QFile::WriteOnly));
  QTextStream(&mFile) << "git config --global core.autocrlf true";
  mFile.close();
  refresh(mRepoView);
}

void TestLineEndings::commitFile() {
  DetailView *detailView = mRepoView->findChild<DetailView *>();
  detailView->setCommitMessage(
      "Discarding file should not leave behind CRLF remnants on Windows");
  QList<QPushButton *> buttons = detailView->findChildren<QPushButton *>();
  QPushButton *stageAll = buttons.at(0);
  QPushButton *commit = buttons.at(2);
  mouseClick(stageAll, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);
  mouseClick(commit, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);
}

void TestLineEndings::overwriteFile() {
  QDir workingDirectory = mRepo->workdir();
  QFile file(workingDirectory.filePath("test_file"));
  QVERIFY(file.open(QFile::WriteOnly));
  QTextStream(&file) << "\n This change will be discarded";
  file.close();
  refresh(mRepoView);
}

void TestLineEndings::dirtyStatusWhenStatusRowHidden() {
  CommitList *commitList = mRepoView->findChild<CommitList *>();
  QVERIFY(commitList);

  git::Config config = mRepo->appConfig();
  config.setValue(ConfigKeys::kStatusKey, false);
  commitList->resetSettings();
  refresh(mRepoView, true);
  QVERIFY(commitList->status().isValid());
  QVERIFY(commitList->model()
              ->index(0, 0)
              .data(CommitList::DiffRole)
              .value<git::Diff>()
              .isValid());

  config.setValue(ConfigKeys::kStatusKey, true);
  commitList->resetSettings();
}

void TestLineEndings::testLineEndings() {
  auto doubleTree = mRepoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  QModelIndex file = files->model()->index(0, 0);
  files->selectionModel()->select(file, QItemSelectionModel::Select);
  QVERIFY(QMetaObject::invokeMethod(files, "fileSelectionRequested"));

  int inputDelay = this->inputDelay;
  auto mRepoView = this->mRepoView;
  QTimer::singleShot(100, [inputDelay, mRepoView] {
    QMessageBox *popup = mRepoView->findChild<QMessageBox *>();
    QVERIFY(popup);

    QList<QAbstractButton *> buttons = popup->buttons();
    QAbstractButton *accept = buttons.at(0);
    QVERIFY(accept);

    mouseClick(accept, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
               inputDelay);
  });

  QToolButton *button = mRepoView->findChild<QToolButton *>("DiscardButton");
  mouseClick(button, Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(),
             inputDelay);

  CommitList *commitList = mRepoView->findChild<CommitList *>();
  QVERIFY(!commitList->status().isValid());
}

void TestLineEndings::repositoryDiagnostics() {
  mRepoView->reportDiagnostics();

  LogView *log = mRepoView->findChild<LogView *>();
  QVERIFY(log);
  const QList<LogEntry *> entries = log->findChildren<LogEntry *>();
  QStringList titles;
  for (LogEntry *entry : entries)
    titles.append(entry->title());

  QVERIFY(titles.contains("$ pwd"));
  QVERIFY(titles.contains("$ git rev-parse --show-toplevel"));
  QVERIFY(titles.contains("$ git status --porcelain=v1 --branch"));
  QVERIFY(titles.contains("$ git rev-parse HEAD"));
}

void TestLineEndings::cleanupTestCase() {
  qWait(closeDelay);
  mWindow->close();
}

TEST_MAIN(TestLineEndings)

#include "line_endings.moc"

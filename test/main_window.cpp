//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Test.h"
#include "ui/CommitList.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QProcess>

using namespace Test;
using namespace QTest;

class TestMainWindow : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void show();
  void preserveSelectionAfterRemoteUpdate();
  void cleanupTestCase();

private:
  QTemporaryDir mRemoteDir;
  ScratchRepository mRepo;
  MainWindow *mWindow = nullptr;
};

void TestMainWindow::initTestCase() {
  QProcess git;
  git.start(GIT_EXECUTABLE, {"init", "--bare", mRemoteDir.path()});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git.setWorkingDirectory(mRepo->workdir().path());
  for (int i = 1; i <= 3; ++i) {
    git.start(GIT_EXECUTABLE,
              {"commit", "--allow-empty", "-m", QString("commit %1").arg(i)});
    QVERIFY(git.waitForFinished());
    QCOMPARE(git.exitCode(), 0);
  }

  QVERIFY(mRepo->addRemote("origin", mRemoteDir.path()).isValid());

  mWindow = new MainWindow(mRepo);
}

void TestMainWindow::show() {
  mWindow->show();
  QVERIFY(qWaitForWindowActive(mWindow));
}

void TestMainWindow::preserveSelectionAfterRemoteUpdate() {
  RepoView *view = mWindow->currentView();
  CommitList *commits = view->findChild<CommitList *>();
  QVERIFY(commits);

  git::Commit selected = mRepo->head().target().parents().first();
  QVERIFY(selected.isValid());
  QVERIFY(commits->selectRange(selected.id().toString()));
  QCOMPARE(commits->selectedRange(), selected.id().toString());

  git::Reference head = mRepo->head();
  const QString remoteName = QString("refs/remotes/origin/%1").arg(head.name());
  bool notified = false;
  connect(mRepo->notifier(), &git::RepositoryNotifier::referenceUpdated,
          [&notified, remoteName](const git::Reference &ref,
                                  bool restoreSelection) {
            if (ref.qualifiedName() == remoteName) {
              QVERIFY(restoreSelection);
              notified = true;
            }
          });

  view->push(mRepo->lookupRemote("origin"), head);

  QTRY_VERIFY_WITH_TIMEOUT(notified, 10000);
  QCOMPARE(mRepo->lookupRef(remoteName).target().id(), head.target().id());

  QCOMPARE(commits->selectedRange(), selected.id().toString());
}

void TestMainWindow::cleanupTestCase() { mWindow->close(); }

TEST_MAIN(TestMainWindow)

#include "main_window.moc"

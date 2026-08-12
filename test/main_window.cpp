//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Test.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "ui/CommitList.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "ui/SideBar.h"
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeView>

using namespace Test;
using namespace QTest;

class TestMainWindow : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void show();
  void preserveSelectionAfterRemoteUpdate();
  void invalidRecentRepository();
  void cleanupTestCase();

private:
  QTemporaryDir mRemoteDir;
  QTemporaryDir mInvalidRepoDir;
  ScratchRepository mRepo;
  QStringList mRecentRepositories;
  MainWindow *mWindow = nullptr;
};

void TestMainWindow::initTestCase() {
  mRecentRepositories = QSettings().value("recent").toStringList();

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

void TestMainWindow::invalidRecentRepository() {
  QString path = mInvalidRepoDir.path();
  QVERIFY(QDir(path).mkdir(".git"));
  RecentRepositories::instance()->add(path);

  auto contains = [path] {
    RecentRepositories *repos = RecentRepositories::instance();
    for (int i = 0; i < repos->count(); ++i) {
      if (repos->repository(i)->gitpath() == path)
        return true;
    }
    return false;
  };
  QVERIFY(contains());

  auto clickButton = [](const QString &text) {
    QTimer::singleShot(0, [text] {
      QMessageBox *dialog =
          qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      QVERIFY(dialog);
      foreach (QPushButton *button, dialog->findChildren<QPushButton *>()) {
        if (button->text() == text) {
          button->click();
          return;
        }
      }
      QFAIL("Expected recent repository action was not found");
    });
  };

  clickButton("Keep");
  QVERIFY(!MainWindow::open(path, true,
                            MainWindow::OpenSource::RecentRepository));
  QVERIFY(contains());

  clickButton("OK");
  QVERIFY(!MainWindow::open(path));
  QVERIFY(contains());

  mWindow->setSideBarVisible(true);
  QVERIFY(mWindow->isSideBarVisible());
  SideBar *sideBar = mWindow->findChild<SideBar *>();
  QVERIFY(sideBar);
  QTreeView *recentTree = sideBar->findChild<QTreeView *>();
  QVERIFY(recentTree);
  QModelIndex recentRoot = recentTree->model()->index(1, 0);
  QModelIndex recent = recentTree->model()->index(0, 0, recentRoot);
  QCOMPARE(recent.data(Qt::UserRole).toString(), path);

  clickButton("Remove From Recent");
  QVERIFY(QMetaObject::invokeMethod(recentTree, "doubleClicked",
                                    Q_ARG(QModelIndex, recent)));
  QVERIFY(!contains());
  QVERIFY(mWindow->isSideBarVisible());
}

void TestMainWindow::cleanupTestCase() {
  mWindow->close();
  QSettings().setValue("recent", mRecentRepositories);
}

TEST_MAIN(TestMainWindow)

#include "main_window.moc"

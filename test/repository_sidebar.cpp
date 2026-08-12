//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "git/Branch.h"
#include "git/TagRef.h"
#include "host/Account.h"
#include "host/Accounts.h"
#include "ui/Footer.h"
#include "ui/MainWindow.h"
#include "ui/RepositoryNavigator.h"
#include "ui/RepositoryNavigatorModel.h"
#include "ui/RepoView.h"
#include "ui/SideBar.h"
#include "ui/TabWidget.h"
#include <QAbstractItemModelTester>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>

using namespace QTest;

namespace {

enum SideBarRole {
  PathRole = Qt::UserRole,
  TabRole,
  RecentRole,
  AccountRole,
  AccountKindRole
};

} // namespace

class TestRepositorySideBar : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void chooserModel();
  void recentRemoval();
  void accountRows();
  void navigatorModel();
  void navigatorView();
  void activeRepositoryBinding();
  void cleanupTestCase();

private:
  MainWindow *mWindow = nullptr;
  QTreeView *mTree = nullptr;
  Footer *mFooter = nullptr;
  Test::ScratchRepository mRepo;
};

void TestRepositorySideBar::initTestCase() {
  QCOMPARE(RecentRepositories::instance()->count(), 0);
  QCOMPARE(Accounts::instance()->count(), 0);

  mWindow = new MainWindow(git::Repository());
  SideBar *sideBar = mWindow->findChild<SideBar *>();
  QVERIFY(sideBar);

  mTree = sideBar->findChild<QTreeView *>("RepositoryTree");
  mFooter = sideBar->findChild<Footer *>("RepositoryFooter");
  QVERIFY(mTree);
  QVERIFY(mFooter);
}

void TestRepositorySideBar::chooserModel() {
  QAbstractItemModel *model = mTree->model();
  QCOMPARE(model->rowCount(), 3);

  QModelIndex open = model->index(0, 0);
  QCOMPARE(open.data().toString(), QString("open"));
  QCOMPARE(model->rowCount(open), 1);
  QCOMPARE(model->index(0, 0, open).data().toString(), QString("none"));

  QModelIndex recent = model->index(1, 0);
  QCOMPARE(recent.data().toString(), QString("recent"));
  QCOMPARE(model->rowCount(recent), 1);
  QCOMPARE(model->index(0, 0, recent).data().toString(), QString("none"));

  QModelIndex remote = model->index(2, 0);
  QCOMPARE(remote.data().toString(), QString("remote"));
  QCOMPARE(model->rowCount(remote), Account::NUM_KINDS);
  for (int row = 0; row < Account::NUM_KINDS; ++row) {
    Account::Kind kind = static_cast<Account::Kind>(row);
    QModelIndex provider = model->index(row, 0, remote);
    QCOMPARE(provider.data().toString(), Account::name(kind));
    QCOMPARE(provider.data(AccountKindRole).value<Account::Kind>(), kind);
    QVERIFY(!provider.data(AccountRole).value<Account *>());
  }
}

void TestRepositorySideBar::recentRemoval() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  RecentRepositories *recent = RecentRepositories::instance();
  QSignalSpy removed(recent, &RecentRepositories::repositoryRemoved);
  recent->add(dir.path());
  QCOMPARE(recent->count(), 1);

  QAbstractItemModel *model = mTree->model();
  QModelIndex recentRoot = model->index(1, 0);
  QModelIndex row = model->index(0, 0, recentRoot);
  QCOMPARE(row.data(PathRole).toString(), dir.path());
  QVERIFY(row.data(RecentRole).value<RecentRepository *>());

  mTree->selectionModel()->setCurrentIndex(
      row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  QToolButton *remove = mFooter->findChild<QToolButton *>("Remove");
  QVERIFY(remove);
  QVERIFY(remove->isEnabled());
  QVERIFY(QMetaObject::invokeMethod(mFooter, "minusClicked"));

  QCOMPARE(removed.count(), 1);
  QCOMPARE(recent->count(), 0);
  recentRoot = model->index(1, 0);
  QCOMPARE(model->index(0, 0, recentRoot).data().toString(), QString("none"));
}

void TestRepositorySideBar::accountRows() {
  Accounts *accounts = Accounts::instance();
  Account *account = accounts->createAccount(Account::GitLab, "offline-test");
  QVERIFY(account);
  QCOMPARE(accounts->count(), 1);

  QAbstractItemModel *model = mTree->model();
  QModelIndex remote = model->index(2, 0);
  QCOMPARE(model->rowCount(remote), 1);

  QModelIndex row = model->index(0, 0, remote);
  QCOMPARE(row.data().toString(), QString("offline-test"));
  QCOMPARE(row.data(AccountRole).value<Account *>(), account);
  QCOMPARE(row.data(AccountKindRole).value<Account::Kind>(), Account::GitLab);
  QCOMPARE(model->rowCount(row), 0);

  accounts->removeAccount(account);
  QCOMPARE(accounts->count(), 0);
  remote = model->index(2, 0);
  QCOMPARE(model->rowCount(remote), Account::NUM_KINDS);
}

void TestRepositorySideBar::navigatorModel() {
  QFile file(mRepo->workdir().filePath("tracked.txt"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("initial\n"), qint64(8));
  file.close();

  QProcess git;
  git.setWorkingDirectory(mRepo->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "tracked.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "initial"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"update-ref", "refs/remotes/origin/main", "HEAD"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git::Commit head = mRepo->head().target();
  QVERIFY(head.isValid());
  QVERIFY(mRepo->createBranch("feature", head).isValid());
  QVERIFY(mRepo->createTag(head, "v1").isValid());

  QVERIFY(file.open(QIODevice::Append));
  QCOMPARE(file.write("change\n"), qint64(7));
  file.close();
  QVERIFY(mRepo->stash("sidebar stash").isValid());

  RepositoryNavigatorModel model;
  QAbstractItemModelTester tester(
      &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
  model.setRepository(mRepo);

  QCOMPARE(model.rowCount(),
           static_cast<int>(RepositoryNavigatorModel::Section::Count));
  const QStringList sections = {"Local",         "Remote", "Stashes",
                                "Cloud Patches", "Pull Requests",
                                "GitHub Issues", "Teams",  "Tags",
                                "Submodules"};
  for (int row = 0; row < sections.size(); ++row)
    QCOMPARE(model.index(row, 0).data().toString(), sections.at(row));

  for (RepositoryNavigatorModel::Section section :
       {RepositoryNavigatorModel::Section::CloudPatches,
        RepositoryNavigatorModel::Section::PullRequests,
        RepositoryNavigatorModel::Section::GitHubIssues,
        RepositoryNavigatorModel::Section::Teams}) {
    QModelIndex index = model.sectionIndex(section);
    QCOMPARE(index.data(RepositoryNavigatorModel::CountRole).toInt(), 0);
    QVERIFY(!index.data(RepositoryNavigatorModel::AvailableRole).toBool());
    QVERIFY(!(model.flags(index) & Qt::ItemIsEnabled));
  }

  QModelIndex local =
      model.sectionIndex(RepositoryNavigatorModel::Section::Local);
  QCOMPARE(model.rowCount(local), 2);
  int current = 0;
  for (int row = 0; row < model.rowCount(local); ++row) {
    QModelIndex branch = model.index(row, 0, local);
    QVERIFY(branch.data(RepositoryNavigatorModel::ReferenceRole)
                .value<git::Reference>()
                .isValid());
    current += branch.data(RepositoryNavigatorModel::CurrentRole).toBool();
  }
  QCOMPARE(current, 1);

  QModelIndex remotes =
      model.sectionIndex(RepositoryNavigatorModel::Section::Remote);
  QCOMPARE(model.rowCount(remotes), 1);
  QCOMPARE(model.index(0, 0, remotes).data().toString(),
           QString("origin/main"));

  QVERIFY(mRepo->createBranch("notified", head).isValid());
  QTRY_COMPARE(model.rowCount(local), 3);

  QModelIndex stashes =
      model.sectionIndex(RepositoryNavigatorModel::Section::Stashes);
  QCOMPARE(model.rowCount(stashes), 1);
  QModelIndex stash = model.index(0, 0, stashes);
  QCOMPARE(stash.data(RepositoryNavigatorModel::StashIndexRole).toInt(), 0);
  QVERIFY(stash.data(RepositoryNavigatorModel::CommitRole)
              .value<git::Commit>()
              .isValid());

  QModelIndex tags =
      model.sectionIndex(RepositoryNavigatorModel::Section::Tags);
  QCOMPARE(model.rowCount(tags), 1);
  QCOMPARE(model.index(0, 0, tags).data().toString(), QString("v1"));

  model.clear();
  QCOMPARE(model.rowCount(), sections.size());
  QCOMPARE(model.rowCount(model.sectionIndex(
               RepositoryNavigatorModel::Section::Local)),
           0);
}

void TestRepositorySideBar::navigatorView() {
  RepositoryNavigator navigator;
  QTreeView *view = navigator.view();
  QVERIFY(view);
  QCOMPARE(view->objectName(), QString("RepositoryNavigationTree"));
  QVERIFY(view->focusPolicy() != Qt::NoFocus);

  RepositoryNavigatorModel *model = navigator.model();
  QModelIndex local =
      model->sectionIndex(RepositoryNavigatorModel::Section::Local);
  QModelIndex cloud =
      model->sectionIndex(RepositoryNavigatorModel::Section::CloudPatches);
  QVERIFY(view->isExpanded(local));
  QVERIFY(!view->isExpanded(cloud));

  view->collapse(local);
  QCoreApplication::processEvents();
  QCOMPARE(QSettings()
               .value("sidebar/repositoryNavigator/expanded/Local")
               .toBool(),
           false);

  navigator.setRepository(mRepo);
  local = model->sectionIndex(RepositoryNavigatorModel::Section::Local);
  QVERIFY(!view->isExpanded(local));
}

void TestRepositorySideBar::activeRepositoryBinding() {
  MainWindow window(mRepo);
  SideBar *sideBar = window.findChild<SideBar *>();
  RepositoryNavigator *navigator =
      sideBar->findChild<RepositoryNavigator *>("RepositoryNavigator");
  QSplitter *content =
      sideBar->findChild<QSplitter *>("RepositorySidebarContent");
  QVERIFY(navigator);
  QVERIFY(content);
  QCOMPARE(content->count(), 2);
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           mRepo->dir(false).path());

  Test::ScratchRepository second;
  RepoView *secondView = window.addTab(second);
  QVERIFY(secondView);
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           second->dir(false).path());

  QVERIFY(window.tabWidget()->closeTab(secondView));
  QTRY_COMPARE(window.count(), 1);
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           mRepo->dir(false).path());

  RepoView *view = window.currentView();
  QModelIndex local = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Local);
  QModelIndex feature;
  for (int row = 0; row < navigator->model()->rowCount(local); ++row) {
    QModelIndex candidate = navigator->model()->index(row, 0, local);
    if (candidate.data().toString() == "feature") {
      feature = candidate;
      break;
    }
  }
  QVERIFY(feature.isValid());

  QSignalSpy referenceSelected(view, &RepoView::referenceSelected);
  QVERIFY(QMetaObject::invokeMethod(navigator->view(), "clicked",
                                    Q_ARG(QModelIndex, feature)));
  QTRY_COMPARE(referenceSelected.count(), 1);
  QCOMPARE(referenceSelected.first().first().value<git::Reference>().name(),
           QString("feature"));

  QVERIFY(QMetaObject::invokeMethod(navigator->view(), "doubleClicked",
                                    Q_ARG(QModelIndex, feature)));
  QTRY_COMPARE(mRepo->head().name(), QString("feature"));

  QVERIFY(window.tabWidget()->closeTab(window.currentView()));
  QTRY_COMPARE(window.count(), 0);
  QVERIFY(!navigator->model()->repository().isValid());
}

void TestRepositorySideBar::cleanupTestCase() { mWindow->close(); }

int main(int argc, char *argv[]) {
  QTemporaryDir settings;
  if (!settings.isValid())
    return 1;

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  qunsetenv("GITTYUP_OAUTH");

  Application::setInTest();
  int testArgc = argc;
  auto app = Test::createApp(argc, argv);
  TestRepositorySideBar test;
  return QTest::qExec(&test, testArgc, argv);
}

#include "repository_sidebar.moc"

//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "host/Account.h"
#include "host/Accounts.h"
#include "ui/Footer.h"
#include "ui/MainWindow.h"
#include "ui/SideBar.h"
#include <QSettings>
#include <QSignalSpy>
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
  void cleanupTestCase();

private:
  MainWindow *mWindow = nullptr;
  QTreeView *mTree = nullptr;
  Footer *mFooter = nullptr;
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

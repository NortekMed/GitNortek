#include "Test.h"

#include "dialogs/CloneDialog.h"
#include "dialogs/StartDialog.h"
#include "host/Accounts.h"
#include "host/Repository.h"
#include "ui/Footer.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

namespace {

void clearAccounts() {
  Accounts *accounts = Accounts::instance();
  while (accounts->count() > 0)
    accounts->removeAccount(0);

  QSettings settings;
  settings.remove("accounts");
  settings.remove("remote");
  settings.remove("start/remote/fullname");
  settings.sync();
}

} // namespace

class TestStartDialog : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();
  void emptyAccountListHasNoProviderPlaceholders();
  void selectingAccountShowsItsRepositories();
  void cloneDefaultsToSshAndHomeDirectory();
  void unclonedRepositoryCanBeClonedAndMappedRepositoryOpens();
};

void TestStartDialog::init() { clearAccounts(); }

void TestStartDialog::cleanup() { clearAccounts(); }

void TestStartDialog::emptyAccountListHasNoProviderPlaceholders() {
  StartDialog dialog;
  auto *accountList = dialog.findChild<QListView *>("RemoteAccounts");
  auto *repositoryList = dialog.findChild<QListView *>("RemoteRepositories");
  auto *accountFooter = dialog.findChild<Footer *>("AccountFooter");
  auto *cloneOtherRepository =
      dialog.findChild<QPushButton *>("CloneOtherRepository");

  QVERIFY(accountList);
  QVERIFY(repositoryList);
  QVERIFY(accountFooter);
  QVERIFY(cloneOtherRepository);
  QCOMPARE(accountList->model()->rowCount(), 0);
  QCOMPARE(repositoryList->model()->rowCount(), 0);

  QToolButton *addAccount = accountFooter->findChild<QToolButton *>("Add");
  QVERIFY(addAccount);
  QVERIFY(addAccount->isEnabled());

  QCOMPARE(cloneOtherRepository->text(), QString("Clone other repository"));
  QVERIFY(!cloneOtherRepository->menu());
  for (QPushButton *button : dialog.findChildren<QPushButton *>())
    QVERIFY(button->text() != "View Getting Started Video");

  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  cloneOtherRepository->click();
  QTRY_VERIFY(qobject_cast<CloneDialog *>(QApplication::activeModalWidget()));
  auto *cloneDialog =
      qobject_cast<CloneDialog *>(QApplication::activeModalWidget());
  QVERIFY(cloneDialog);
  cloneDialog->reject();
}

void TestStartDialog::selectingAccountShowsItsRepositories() {
  Accounts *accounts = Accounts::instance();
  Account *github = accounts->createAccount(Account::GitHub, "alice");
  github->addRepository("first", "alice/first");
  github->addRepository("second", "alice/second");

  Account *gitlab = accounts->createAccount(Account::GitLab, "bob");
  gitlab->addRepository("third", "bob/third");

  StartDialog dialog;
  auto *accountList = dialog.findChild<QListView *>("RemoteAccounts");
  auto *repositoryList = dialog.findChild<QListView *>("RemoteRepositories");
  QVERIFY(accountList);
  QVERIFY(repositoryList);

  QCOMPARE(accountList->model()->rowCount(), 2);
  QCOMPARE(accountList->currentIndex().data().toString(), QString("alice"));
  QCOMPARE(repositoryList->model()->rowCount(), 2);
  QCOMPARE(repositoryList->model()->index(0, 0).data().toString(),
           QString("first"));

  QModelIndex gitlabIndex = accountList->model()->index(1, 0);
  accountList->selectionModel()->setCurrentIndex(
      gitlabIndex, QItemSelectionModel::ClearAndSelect);

  QCOMPARE(repositoryList->model()->rowCount(), 1);
  QCOMPARE(repositoryList->model()->index(0, 0).data().toString(),
           QString("third"));
}

void TestStartDialog::cloneDefaultsToSshAndHomeDirectory() {
  Account *account =
      Accounts::instance()->createAccount(Account::GitHub, "alice");
  Repository *repo = account->addRepository("project", "alice/project");
  repo->setUrl(Repository::Https, "https://github.com/alice/project.git");
  repo->setUrl(Repository::Ssh, "git@github.com:alice/project.git");

  QSettings().setValue("repo/path", "/tmp/previous-clone-folder");

  CloneDialog dialog(CloneDialog::Clone, nullptr, repo);
  auto *protocol = dialog.findChild<QComboBox *>("RepositoryProtocol");
  auto *url = dialog.findChild<QLineEdit *>("RemoteUrl");
  QVERIFY(protocol);
  QVERIFY(url);
  QCOMPARE(protocol->currentData().toInt(), int(Repository::Ssh));
  QCOMPARE(url->text(), repo->url(Repository::Ssh));

  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  dialog.next();
  QTRY_COMPARE(dialog.currentId(), 1);
  auto *directory = dialog.findChild<QLineEdit *>("RepositoryDirectory");
  QVERIFY(directory);
  QCOMPARE(directory->text(), QDir::homePath());
}

void TestStartDialog::unclonedRepositoryCanBeClonedAndMappedRepositoryOpens() {
  Account *account =
      Accounts::instance()->createAccount(Account::GitHub, "alice");
  Repository *repo = account->addRepository("project", "alice/project");
  repo->setUrl(Repository::Https, "https://github.com/alice/project.git");

  StartDialog dialog;
  auto *repositoryList = dialog.findChild<QListView *>("RemoteRepositories");
  auto *buttonBox = dialog.findChild<QDialogButtonBox *>();
  QVERIFY(repositoryList);
  QVERIFY(buttonBox);

  QModelIndex repoIndex = repositoryList->model()->index(0, 0);
  repositoryList->selectionModel()->setCurrentIndex(
      repoIndex, QItemSelectionModel::ClearAndSelect);
  QPushButton *open = buttonBox->button(QDialogButtonBox::Open);
  QVERIFY(open);
  QVERIFY(open->isEnabled());
  QCOMPARE(open->text(), QString("Clone"));

  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  open->click();
  QTRY_VERIFY(qobject_cast<CloneDialog *>(QApplication::activeModalWidget()));
  auto *cloneDialog =
      qobject_cast<CloneDialog *>(QApplication::activeModalWidget());
  QVERIFY(cloneDialog);
  cloneDialog->reject();
  QTRY_VERIFY(!QApplication::activeModalWidget());

  const QPoint repoCenter = repositoryList->visualRect(repoIndex).center();
  QVERIFY(repositoryList->visualRect(repoIndex).isValid());
  QTest::mouseClick(repositoryList->viewport(), Qt::LeftButton, Qt::NoModifier,
                    repoCenter);
  QTest::mouseDClick(repositoryList->viewport(), Qt::LeftButton, Qt::NoModifier,
                     repoCenter);
  QTRY_VERIFY(qobject_cast<CloneDialog *>(QApplication::activeModalWidget()));
  cloneDialog = qobject_cast<CloneDialog *>(QApplication::activeModalWidget());
  QVERIFY(cloneDialog);
  cloneDialog->reject();

  QTemporaryDir localClone;
  QVERIFY(localClone.isValid());
  account->setRepositoryPath(0, localClone.path());
  QCOMPARE(open->text(), QString("Open"));
}

int main(int argc, char *argv[]) {
  QTemporaryDir settingsDirectory;
  if (!settingsDirectory.isValid())
    return 1;

  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     settingsDirectory.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settingsDirectory.path());

  QTEST_SET_MAIN_SOURCE_PATH
  return Test::runTest<TestStartDialog>(argc, argv);
}

#include "start_dialog.moc"

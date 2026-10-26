#include "Test.h"
#include "dialogs/PublishRepositoryDialog.h"
#include "git/Remote.h"
#include "host/Account.h"
#include "host/Accounts.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

namespace {

void clearAccounts() {
  Accounts *accounts = Accounts::instance();
  while (accounts->count() > 0)
    accounts->removeAccount(0);

  QSettings settings;
  settings.remove("accounts");
  settings.sync();
}

QPushButton *publishButton(PublishRepositoryDialog *dialog) {
  return dialog->findChild<QPushButton *>("CreateRepositoryAndPush");
}

} // namespace

class TestPublishRepository : public QObject {
  Q_OBJECT

private slots:
  void init();
  void cleanup();
  void defaultsAndPrivateOnlyHost();
  void publicVisibilityForGitHub();
  void noAccountOffersManualRemote();
  void existingRemoteNameIsRejected();
  void createOnlyUsesSelectedHostAndProtocol();

private:
  Test::ScratchRepository mRepo;
};

void TestPublishRepository::init() { clearAccounts(); }

void TestPublishRepository::cleanup() { clearAccounts(); }

void TestPublishRepository::defaultsAndPrivateOnlyHost() {
  Accounts::instance()->createAccount(Account::Beanstalk, "alice");
  PublishRepositoryDialog dialog(mRepo);

  auto *account = dialog.findChild<QComboBox *>("PublishAccount");
  auto *destination = dialog.findChild<QComboBox *>("PublishNamespace");
  auto *name = dialog.findChild<QLineEdit *>("PublishRepositoryName");
  auto *remote = dialog.findChild<QLineEdit *>("PublishRemoteName");
  auto *visibility = dialog.findChild<QComboBox *>("PublishVisibility");
  auto *protocol = dialog.findChild<QComboBox *>("PublishProtocol");
  QVERIFY(account);
  QVERIFY(destination);
  QVERIFY(name);
  QVERIFY(remote);
  QVERIFY(visibility);
  QVERIFY(protocol);

  QCOMPARE(account->count(), 1);
  QCOMPARE(destination->count(), 1);
  QCOMPARE(remote->text(), QStringLiteral("origin"));
  QVERIFY(!name->text().isEmpty());
  QCOMPARE(visibility->count(), 1);
  QCOMPARE(visibility->currentData().toBool(), false);
  QCOMPARE(protocol->currentData().toInt(), int(Repository::Ssh));
  QVERIFY(publishButton(&dialog));
  QVERIFY(publishButton(&dialog)->isEnabled());

  QSignalSpy publishRequested(&dialog,
                              &PublishRepositoryDialog::publishRequested);
  QVERIFY(publishRequested.isValid());
  publishButton(&dialog)->click();
  QCOMPARE(publishRequested.count(), 1);
}

void TestPublishRepository::publicVisibilityForGitHub() {
  Accounts::instance()->createAccount(Account::GitHub, "alice");
  PublishRepositoryDialog dialog(mRepo);

  auto *visibility = dialog.findChild<QComboBox *>("PublishVisibility");
  auto *destination = dialog.findChild<QComboBox *>("PublishNamespace");
  QVERIFY(visibility);
  QVERIFY(destination);
  QCOMPARE(visibility->count(), 2);
  QCOMPARE(visibility->itemData(0).toBool(), false);
  QCOMPARE(visibility->itemData(1).toBool(), true);
  QVERIFY(destination->count() >= 1);
}

void TestPublishRepository::noAccountOffersManualRemote() {
  PublishRepositoryDialog dialog(mRepo);
  auto *account = dialog.findChild<QComboBox *>("PublishAccount");
  auto *addRemote = dialog.findChild<QPushButton *>("AddExistingRemote");
  QVERIFY(account);
  QVERIFY(addRemote);
  QVERIFY(!dialog.account());
  QVERIFY(publishButton(&dialog));
  QVERIFY(!publishButton(&dialog)->isEnabled());

  QSignalSpy addRemoteRequested(&dialog,
                                &PublishRepositoryDialog::addRemoteRequested);
  QVERIFY(addRemoteRequested.isValid());
  addRemote->click();
  QCOMPARE(addRemoteRequested.count(), 1);
}

void TestPublishRepository::existingRemoteNameIsRejected() {
  QVERIFY(mRepo->addRemote("origin", "https://example.com/alice/project.git")
              .isValid());
  Accounts::instance()->createAccount(Account::GitHub, "alice");
  PublishRepositoryDialog dialog(mRepo);
  QVERIFY(publishButton(&dialog));
  QVERIFY(!publishButton(&dialog)->isEnabled());
}

void TestPublishRepository::createOnlyUsesSelectedHostAndProtocol() {
  Account *github =
      Accounts::instance()->createAccount(Account::GitHub, "alice");
  Accounts::instance()->createAccount(Account::GitLab, "bob");

  PublishRepositoryDialog dialog(mRepo, nullptr,
                                 PublishRepositoryDialog::CreateOnly,
                                 Account::GitHub, Repository::Https);
  auto *accounts = dialog.findChild<QComboBox *>("PublishAccount");
  auto *protocol = dialog.findChild<QComboBox *>("PublishProtocol");
  auto *remoteName = dialog.findChild<QLineEdit *>("PublishRemoteName");
  auto *addRemote = dialog.findChild<QPushButton *>("AddExistingRemote");
  auto *create = dialog.findChild<QPushButton *>("CreateRepository");
  QVERIFY(accounts);
  QVERIFY(protocol);
  QVERIFY(remoteName);
  QVERIFY(addRemote);
  QVERIFY(create);

  QCOMPARE(dialog.windowTitle(), QStringLiteral("Create Repository"));
  QCOMPARE(accounts->count(), 1);
  QCOMPARE(dialog.account(), github);
  QCOMPARE(protocol->currentData().toInt(), int(Repository::Https));
  QVERIFY(remoteName->isHidden());
  QVERIFY(addRemote->isHidden());
  QVERIFY(create->isEnabled());

  QSignalSpy createRequested(
      &dialog, &PublishRepositoryDialog::createRepositoryRequested);
  QSignalSpy publishRequested(&dialog,
                              &PublishRepositoryDialog::publishRequested);
  QVERIFY(createRequested.isValid());
  QVERIFY(publishRequested.isValid());
  create->click();
  QCOMPARE(createRequested.count(), 1);
  QCOMPARE(publishRequested.count(), 0);
  dialog.complete();
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
  return Test::runTest<TestPublishRepository>(argc, argv);
}

#include "PublishRepository.moc"

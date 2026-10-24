#include "Test.h"

#include "cred/CredentialHelper.h"
#include "dialogs/ExternalToolsModel.h"
#include "dialogs/SettingsDialog.h"
#include "git/Config.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

namespace {

void answerNextConfirmation(QMessageBox::StandardButton answer) {
  QTimer::singleShot(0, [answer] {
    auto *message =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (message)
      message->done(answer);
  });
}

} // namespace

class TestSettingsDialog : public QObject {
  Q_OBJECT

private:
  QString mConfigPath;

private slots:
  void initTestCase();
  void init();
  void cleanup();
  void cleanupTestCase();
  void uncheckedCredentialHelperShowsTemporaryCache();
  void credentialHelperChangesRequireConfirmation();
  void globalTextEditsConfirmOnCommit();
  void diffAndToolChangesRequireConfirmation();
  void externalToolEditsRequireConfirmation();
};

void TestSettingsDialog::initTestCase() {
  mConfigPath = QString::fromLocal8Bit(qgetenv("GIT_CONFIG_GLOBAL"));
  QVERIFY(!mConfigPath.isEmpty());
}

void TestSettingsDialog::init() {
  QFile::remove(mConfigPath);
  QFile configFile(mConfigPath);
  QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));
  QTextStream out(&configFile);
  out << "[user]\n\tname = Original User\n\temail = original@example.com\n";
  configFile.close();
  QVERIFY(git::Config::global().isValid());
}

void TestSettingsDialog::cleanup() { delete CredentialHelper::instance(); }

void TestSettingsDialog::cleanupTestCase() {}

void TestSettingsDialog::uncheckedCredentialHelperShowsTemporaryCache() {
  SettingsDialog dialog(SettingsDialog::General);
  auto *enabled =
      dialog.findChild<QCheckBox *>("GlobalCredentialHelperEnabled");
  auto *store = dialog.findChild<QComboBox *>("GlobalCredentialHelperStore");
  QVERIFY(enabled);
  QVERIFY(store);

  QVERIFY(!enabled->isChecked());
  QVERIFY(!store->isEnabled());
  QCOMPARE(store->currentData().toString(), QStringLiteral("cache"));
  QVERIFY(store->itemText(store->findData("cache"))
              .contains("memory", Qt::CaseInsensitive));

  int plaintextIndex = store->findData("git-credential-store");
  QVERIFY(plaintextIndex >= 0);
  QVERIFY(store->itemText(plaintextIndex)
              .contains("unencrypted", Qt::CaseInsensitive));
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QString());
}

void TestSettingsDialog::credentialHelperChangesRequireConfirmation() {
  SettingsDialog dialog(SettingsDialog::General);
  auto *enabled =
      dialog.findChild<QCheckBox *>("GlobalCredentialHelperEnabled");
  auto *store = dialog.findChild<QComboBox *>("GlobalCredentialHelperStore");
  QVERIFY(enabled);
  QVERIFY(store);

  answerNextConfirmation(QMessageBox::No);
  enabled->click();
  QVERIFY(!enabled->isChecked());
  QVERIFY(!store->isEnabled());
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QString());

  answerNextConfirmation(QMessageBox::Yes);
  enabled->click();
  QVERIFY(enabled->isChecked());
  QVERIFY(store->isEnabled());
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QStringLiteral("cache"));

  const int plaintextIndex = store->findData("git-credential-store");
  QVERIFY(plaintextIndex >= 0);
  store->setCurrentIndex(plaintextIndex);
  answerNextConfirmation(QMessageBox::No);
  QVERIFY(QMetaObject::invokeMethod(store, "activated", Qt::DirectConnection,
                                    Q_ARG(int, plaintextIndex)));
  QCOMPARE(store->currentData().toString(), QStringLiteral("cache"));
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QStringLiteral("cache"));

  store->setCurrentIndex(plaintextIndex);
  answerNextConfirmation(QMessageBox::Yes);
  QVERIFY(QMetaObject::invokeMethod(store, "activated", Qt::DirectConnection,
                                    Q_ARG(int, plaintextIndex)));
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QStringLiteral("git-credential-store"));

  answerNextConfirmation(QMessageBox::No);
  enabled->click();
  QVERIFY(enabled->isChecked());
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QStringLiteral("git-credential-store"));

  answerNextConfirmation(QMessageBox::Yes);
  enabled->click();
  QVERIFY(!enabled->isChecked());
  QVERIFY(!store->isEnabled());
  QCOMPARE(store->currentData().toString(), QStringLiteral("cache"));
  QCOMPARE(git::Config::global().value<QString>("credential.helper"),
           QString());
}

void TestSettingsDialog::globalTextEditsConfirmOnCommit() {
  SettingsDialog dialog(SettingsDialog::General);
  auto *name = dialog.findChild<QLineEdit *>("GlobalGitUserName");
  QVERIFY(name);

  name->setText("New User");
  QCOMPARE(git::Config::global().value<QString>("user.name"),
           QStringLiteral("Original User"));

  answerNextConfirmation(QMessageBox::No);
  QVERIFY(
      QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection));
  QCOMPARE(name->text(), QStringLiteral("Original User"));
  QCOMPARE(git::Config::global().value<QString>("user.name"),
           QStringLiteral("Original User"));

  name->setText("Accepted User");
  answerNextConfirmation(QMessageBox::Yes);
  QVERIFY(
      QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection));
  QCOMPARE(git::Config::global().value<QString>("user.name"),
           QStringLiteral("Accepted User"));
}

void TestSettingsDialog::diffAndToolChangesRequireConfirmation() {
  {
    SettingsDialog dialog(SettingsDialog::Diff);
    auto *context = dialog.findChild<QSpinBox *>("GlobalGitDiffContext");
    QVERIFY(context);
    context->setValue(7);
    QCOMPARE(git::Config::global().value<int>("diff.context", 3), 3);

    answerNextConfirmation(QMessageBox::No);
    QVERIFY(QMetaObject::invokeMethod(context, "editingFinished",
                                      Qt::DirectConnection));
    QCOMPARE(context->value(), 3);
    QCOMPARE(git::Config::global().value<int>("diff.context", 3), 3);

    context->setValue(8);
    answerNextConfirmation(QMessageBox::Yes);
    QVERIFY(QMetaObject::invokeMethod(context, "editingFinished",
                                      Qt::DirectConnection));
    QCOMPARE(git::Config::global().value<int>("diff.context", 3), 8);
  }

  {
    SettingsDialog dialog(SettingsDialog::Tools);
    auto *backup = dialog.findChild<QCheckBox *>("GlobalGitMergeKeepBackup");
    QVERIFY(backup);

    answerNextConfirmation(QMessageBox::No);
    backup->click();
    QVERIFY(!backup->isChecked());
    QVERIFY(!git::Config::global().value<bool>("mergetool.keepBackup"));

    answerNextConfirmation(QMessageBox::Yes);
    backup->click();
    QVERIFY(backup->isChecked());
    QVERIFY(git::Config::global().value<bool>("mergetool.keepBackup"));
  }
}

void TestSettingsDialog::externalToolEditsRequireConfirmation() {
  git::Config config = git::Config::global();
  QVERIFY(
      config.setValue("difftool.example.cmd", QStringLiteral("old-command")));

  QDialog parent;
  ExternalToolsModel model("diff", false, &parent);
  int row = -1;
  for (int i = 0; i < model.rowCount(); ++i) {
    if (model.index(i, ExternalToolsModel::Name).data().toString() ==
        "example") {
      row = i;
      break;
    }
  }
  QVERIFY(row >= 0);

  QModelIndex command = model.index(row, ExternalToolsModel::Command);
  answerNextConfirmation(QMessageBox::No);
  QVERIFY(!model.setData(command, "rejected-command", Qt::EditRole));
  QCOMPARE(git::Config::global().value<QString>("difftool.example.cmd"),
           QStringLiteral("old-command"));

  command = model.index(row, ExternalToolsModel::Command);
  answerNextConfirmation(QMessageBox::Yes);
  QVERIFY(model.setData(command, "accepted-command", Qt::EditRole));
  QCOMPARE(git::Config::global().value<QString>("difftool.example.cmd"),
           QStringLiteral("accepted-command "));
}

int main(int argc, char *argv[]) {
  QTemporaryDir tempDir;
  if (!tempDir.isValid())
    return 1;

  const QByteArray oldGlobalConfig = qgetenv("GIT_CONFIG_GLOBAL");
  const QByteArray oldNoSystem = qgetenv("GIT_CONFIG_NOSYSTEM");
  const QByteArray oldHome = qgetenv("HOME");
  const QByteArray oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
  const QString configPath = tempDir.filePath(".gitconfig");
  const QString xdgPath = tempDir.filePath("xdg");
  QDir().mkpath(xdgPath);

  QFile configFile(configPath);
  if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text))
    return 1;
  configFile.close();

  qputenv("GIT_CONFIG_GLOBAL", configPath.toUtf8());
  qputenv("GIT_CONFIG_NOSYSTEM", "1");
  qputenv("HOME", tempDir.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", xdgPath.toUtf8());

  int result = Test::runTest<TestSettingsDialog>(argc, argv);

  if (oldGlobalConfig.isNull())
    qunsetenv("GIT_CONFIG_GLOBAL");
  else
    qputenv("GIT_CONFIG_GLOBAL", oldGlobalConfig);
  if (oldNoSystem.isNull())
    qunsetenv("GIT_CONFIG_NOSYSTEM");
  else
    qputenv("GIT_CONFIG_NOSYSTEM", oldNoSystem);
  if (oldHome.isNull())
    qunsetenv("HOME");
  else
    qputenv("HOME", oldHome);
  if (oldXdgConfigHome.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldXdgConfigHome);

  return result;
}
#include "SettingsDialog.moc"

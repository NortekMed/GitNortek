#include "Test.h"

#include "cred/CredentialHelper.h"
#include "cred/GitCredential.h"
#include <QFile>
#include <QTemporaryDir>

class TestCredentialHelper : public QObject {
  Q_OBJECT

private:
  QTemporaryDir mTempDir;
  QByteArray mConfigGlobal;
  QByteArray mConfigNoSystem;

private slots:
  void initTestCase();
  void configuredHelperUsesGitCredential();
  void scopedHelperOverridesGlobalHelper();
  void nonMatchingUrlUsesGlobalHelper();
  void cleanupTestCase();
};

void TestCredentialHelper::initTestCase() {
  QVERIFY(mTempDir.isValid());
  mConfigGlobal = qgetenv("GIT_CONFIG_GLOBAL");
  mConfigNoSystem = qgetenv("GIT_CONFIG_NOSYSTEM");

  QFile config(mTempDir.filePath("config"));
  QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Text));
  QTextStream out(&config);
  out << "[credential]\n";
  out << "\thelper = \"!echo username=global; echo password=global; :\"\n";
  out << "[credential \"https://github.com\"]\n";
  out << "\thelper =\n";
  out << "\thelper = \"!echo username=github; echo password=github; :\"\n";
  config.close();

  qputenv("GIT_CONFIG_GLOBAL", config.fileName().toUtf8());
  qputenv("GIT_CONFIG_NOSYSTEM", "1");
}

void TestCredentialHelper::configuredHelperUsesGitCredential() {
  QVERIFY(dynamic_cast<GitCredential *>(CredentialHelper::instance()));
}

void TestCredentialHelper::scopedHelperOverridesGlobalHelper() {
  GitCredential helper;
  QString username;
  QString password;

  QVERIFY(helper.get("https://github.com/NortekMed/GitNortek.git", username,
                     password));
  QCOMPARE(username, "github");
  QCOMPARE(password, "github");
}

void TestCredentialHelper::nonMatchingUrlUsesGlobalHelper() {
  GitCredential helper;
  QString username;
  QString password;

  QVERIFY(helper.get("https://gitlab.com/NortekMed/GitNortek.git", username,
                     password));
  QCOMPARE(username, "global");
  QCOMPARE(password, "global");
}

void TestCredentialHelper::cleanupTestCase() {
  delete CredentialHelper::instance();
  if (mConfigGlobal.isNull())
    qunsetenv("GIT_CONFIG_GLOBAL");
  else
    qputenv("GIT_CONFIG_GLOBAL", mConfigGlobal);
  if (mConfigNoSystem.isNull())
    qunsetenv("GIT_CONFIG_NOSYSTEM");
  else
    qputenv("GIT_CONFIG_NOSYSTEM", mConfigNoSystem);
}

TEST_MAIN(TestCredentialHelper)
#include "CredentialHelper.moc"

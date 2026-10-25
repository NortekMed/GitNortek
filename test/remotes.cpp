//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "dialogs/RemoteConfigDialog.h"
#include "git/Config.h"
#include "git/Reference.h"
#include "git/Remote.h"
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QToolButton>

using namespace Test;
using namespace QTest;

namespace {

class RecordingCallbacks : public git::Remote::Callbacks {
public:
  using git::Remote::Callbacks::Callbacks;

  bool url(QString &url) override {
    resolvedUrls.append(url);
    return true;
  }

  QStringList resolvedUrls;
};

bool runGit(const QString &workingDirectory, const QStringList &arguments,
            QByteArray *output = nullptr) {
  QProcess git;
  if (!workingDirectory.isEmpty())
    git.setWorkingDirectory(workingDirectory);
  git.start(QStringLiteral(GIT_EXECUTABLE), arguments);
  if (!git.waitForFinished() || git.exitCode() != 0)
    return false;
  if (output)
    *output = git.readAllStandardOutput().trimmed();
  return true;
}

} // namespace

class TestRemotes : public QObject {
  Q_OBJECT

private slots:
  void dialogAddsAndEditsRemote();
  void validatesAndRenamesRemote();
  void configuredPushRefspecsAndExplicitOverride();
};

void TestRemotes::dialogAddsAndEditsRemote() {
  ScratchRepository repository;
  QTemporaryDir remoteDir;
  QVERIFY(remoteDir.isValid());

  RemoteConfigDialog blankAdd(repository);
  auto *blankName =
      blankAdd.findChild<QLineEdit *>(QStringLiteral("RemoteName"));
  auto *blankFetchUrl =
      blankAdd.findChild<QLineEdit *>(QStringLiteral("RemoteFetchUrl"));
  auto *blankPushUrl =
      blankAdd.findChild<QLineEdit *>(QStringLiteral("RemotePushUrl"));
  auto *pushUrlHint =
      blankAdd.findChild<QLabel *>(QStringLiteral("RemotePushUrlHint"));
  auto *refspecToggle =
      blankAdd.findChild<QToolButton *>(QStringLiteral("RemoteRefspecsToggle"));
  auto *advanced =
      blankAdd.findChild<QWidget *>(QStringLiteral("RemoteAdvancedSettings"));
  QVERIFY(blankName);
  QVERIFY(blankFetchUrl);
  QVERIFY(blankPushUrl);
  QVERIFY(pushUrlHint);
  QVERIFY(refspecToggle);
  QVERIFY(advanced);
  QVERIFY(blankName->text().isEmpty());
  QVERIFY(blankName->placeholderText().contains(QStringLiteral("origin")));
  QVERIFY(blankFetchUrl->text().isEmpty());
  QVERIFY(
      blankFetchUrl->placeholderText().contains(QStringLiteral("github.com")));
  QVERIFY(blankPushUrl->text().isEmpty());
  QVERIFY(
      blankPushUrl->placeholderText().contains(QStringLiteral("github.com")));
  QVERIFY(pushUrlHint->text().contains(QStringLiteral("fetch URL")));
  QVERIFY(!refspecToggle->isChecked());
  QVERIFY(advanced->isHidden());

  RemoteConfigDialog add(repository, git::Remote(), QStringLiteral("origin"));
  auto *name = add.findChild<QLineEdit *>(QStringLiteral("RemoteName"));
  auto *fetchUrl = add.findChild<QLineEdit *>(QStringLiteral("RemoteFetchUrl"));
  auto *pushUrl = add.findChild<QLineEdit *>(QStringLiteral("RemotePushUrl"));
  auto *fetchSpecs =
      add.findChild<QPlainTextEdit *>(QStringLiteral("RemoteFetchRefspecs"));
  auto *pushSpecs =
      add.findChild<QPlainTextEdit *>(QStringLiteral("RemotePushRefspecs"));
  QVERIFY(name);
  QVERIFY(fetchUrl);
  QVERIFY(pushUrl);
  QVERIFY(fetchSpecs);
  QVERIFY(pushSpecs);

  fetchUrl->setText(remoteDir.path());
  pushUrl->setText(remoteDir.path() + QStringLiteral("/push"));
  fetchSpecs->setPlainText(
      QStringLiteral("+refs/heads/*:refs/remotes/origin/*\n"
                     "+refs/tags/*:refs/tags/*"));
  pushSpecs->setPlainText(
      QStringLiteral("refs/heads/main:refs/heads/release/main\n"
                     "refs/heads/dev:refs/heads/release/dev"));
  add.accept();
  QCOMPARE(add.result(), int(QDialog::Accepted));

  git::Remote remote = repository->lookupRemote(QStringLiteral("origin"));
  QVERIFY(remote.isValid());
  QCOMPARE(remote.url(), remoteDir.path());
  QCOMPARE(remote.pushUrl(), remoteDir.path() + QStringLiteral("/push"));
  QCOMPARE(remote.fetchRefspecs(),
           (QStringList{QStringLiteral("+refs/heads/*:refs/remotes/origin/*"),
                        QStringLiteral("+refs/tags/*:refs/tags/*")}));
  QCOMPARE(
      remote.pushRefspecs(),
      (QStringList{QStringLiteral("refs/heads/main:refs/heads/release/main"),
                   QStringLiteral("refs/heads/dev:refs/heads/release/dev")}));

  RemoteConfigDialog edit(repository, remote);
  auto *editPushUrl =
      edit.findChild<QLineEdit *>(QStringLiteral("RemotePushUrl"));
  auto *editPushSpecs =
      edit.findChild<QPlainTextEdit *>(QStringLiteral("RemotePushRefspecs"));
  auto *editRefspecToggle =
      edit.findChild<QToolButton *>(QStringLiteral("RemoteRefspecsToggle"));
  auto *editAdvanced =
      edit.findChild<QWidget *>(QStringLiteral("RemoteAdvancedSettings"));
  QVERIFY(editPushUrl);
  QVERIFY(editPushSpecs);
  QVERIFY(editRefspecToggle);
  QVERIFY(editAdvanced);
  QVERIFY(editRefspecToggle->isChecked());
  QVERIFY(!editAdvanced->isHidden());
  editPushUrl->clear();
  editPushSpecs->clear();
  edit.accept();
  QCOMPARE(edit.result(), int(QDialog::Accepted));

  remote = repository->lookupRemote(QStringLiteral("origin"));
  QVERIFY(remote.pushUrl().isEmpty());
  QVERIFY(remote.effectivePushUrl() == remote.url());
  QVERIFY(remote.pushRefspecs().isEmpty());
}

void TestRemotes::validatesAndRenamesRemote() {
  ScratchRepository repository;
  QTemporaryDir remoteDir;
  QVERIFY(remoteDir.isValid());

  git::Remote remote =
      repository->addRemote(QStringLiteral("origin"), remoteDir.path());
  QVERIFY(remote.isValid());
  git::Remote other =
      repository->addRemote(QStringLiteral("backup"), remoteDir.path());
  QVERIFY(other.isValid());

  const QStringList originalFetchRefspecs = remote.fetchRefspecs();
  QVERIFY(!originalFetchRefspecs.isEmpty());
  git::Result invalidRefspec = remote.configure(
      QStringLiteral("origin"), remote.url(), QString(),
      {QStringLiteral("refs/heads/main:refs/heads/a..b")}, QStringList());
  QVERIFY(invalidRefspec.error());
  QCOMPARE(repository->lookupRemote(QStringLiteral("origin")).fetchRefspecs(),
           originalFetchRefspecs);

  git::Result invalidName =
      remote.configure(QStringLiteral("bad name"), remote.url(), QString(),
                       originalFetchRefspecs, QStringList());
  QVERIFY(invalidName.error());
  git::Result duplicateName =
      remote.configure(QStringLiteral("backup"), remote.url(), QString(),
                       originalFetchRefspecs, QStringList());
  QVERIFY(duplicateName.error());

  repository->gitConfig().setValue(QStringLiteral("branch.test.remote"),
                                   QStringLiteral("origin"));
  repository->gitConfig().setValue(QStringLiteral("branch.test.merge"),
                                   QStringLiteral("refs/heads/main"));
  git::Result renamed =
      remote.configure(QStringLiteral("renamed"), remote.url(), QString(),
                       originalFetchRefspecs, QStringList());
  QVERIFY2(!renamed.error(), qPrintable(renamed.errorString()));

  QVERIFY(!repository->lookupRemote(QStringLiteral("origin")).isValid());
  remote = repository->lookupRemote(QStringLiteral("renamed"));
  QVERIFY(remote.isValid());
  QCOMPARE(remote.fetchRefspecs(),
           QStringList{QStringLiteral("+refs/heads/*:refs/remotes/renamed/*")});
  QCOMPARE(repository->gitConfig().value<QString>(
               QStringLiteral("branch.test.remote")),
           QStringLiteral("renamed"));
  QCOMPARE(repository->gitConfig().value<QString>(
               QStringLiteral("branch.test.merge")),
           QStringLiteral("refs/heads/main"));
}

void TestRemotes::configuredPushRefspecsAndExplicitOverride() {
  ScratchRepository repository;
  QTemporaryDir fetchDir;
  QTemporaryDir pushDir;
  QVERIFY(fetchDir.isValid());
  QVERIFY(pushDir.isValid());
  QVERIFY(runGit(QString(), {QStringLiteral("init"), QStringLiteral("--bare"),
                             fetchDir.path()}));
  QVERIFY(runGit(QString(), {QStringLiteral("init"), QStringLiteral("--bare"),
                             pushDir.path()}));
  QVERIFY(runGit(repository->workdir().path(),
                 {QStringLiteral("commit"), QStringLiteral("--allow-empty"),
                  QStringLiteral("-m"), QStringLiteral("initial commit")}));

  git::Reference head = repository->head();
  QVERIFY(head.isValid());
  git::Remote remote =
      repository->addRemote(QStringLiteral("origin"), fetchDir.path());
  QVERIFY(remote.isValid());
  QString source = head.qualifiedName();
  QStringList pushRefspecs{
      source + QStringLiteral(":refs/heads/configured-one"),
      source + QStringLiteral(":refs/heads/configured-two")};
  git::Result configured =
      remote.configure(remote.name(), remote.url(), pushDir.path(),
                       remote.fetchRefspecs(), pushRefspecs);
  QVERIFY2(!configured.error(), qPrintable(configured.errorString()));
  remote = repository->lookupRemote(QStringLiteral("origin"));

  RecordingCallbacks callbacks(remote.effectivePushUrl(), repository);
  QCOMPARE(remote.effectivePushUrl(), pushDir.path());
  git::Result pushed =
      remote.push(&callbacks, head, QString(), false, false, true);
  QVERIFY2(!pushed.error(), qPrintable(pushed.errorString()));
  QVERIFY(!callbacks.resolvedUrls.isEmpty());
  QCOMPARE(callbacks.resolvedUrls.first(), pushDir.path());

  const QByteArray expected = head.target().id().toString().toUtf8();
  QByteArray pushRefs;
  QByteArray fetchRefs;
  QVERIFY(runGit(QString(),
                 {QStringLiteral("--git-dir"), pushDir.path(),
                  QStringLiteral("for-each-ref"),
                  QStringLiteral("--format=%(refname)")},
                 &pushRefs));
  QVERIFY(runGit(QString(),
                 {QStringLiteral("--git-dir"), fetchDir.path(),
                  QStringLiteral("for-each-ref"),
                  QStringLiteral("--format=%(refname)")},
                 &fetchRefs));
  const QString refsError =
      QStringLiteral("push URL refs: %1; fetch URL refs: %2; callback URLs: %3")
          .arg(QString::fromUtf8(pushRefs), QString::fromUtf8(fetchRefs),
               callbacks.resolvedUrls.join(QStringLiteral(", ")));
  QVERIFY2(pushRefs.contains("refs/heads/configured-one"),
           qPrintable(refsError));
  for (const QString &ref :
       {QStringLiteral("configured-one"), QStringLiteral("configured-two")}) {
    QByteArray actual;
    QVERIFY(runGit(QString(),
                   {QStringLiteral("--git-dir"), pushDir.path(),
                    QStringLiteral("rev-parse"),
                    QStringLiteral("refs/heads/") + ref},
                   &actual));
    QCOMPARE(actual, expected);
  }

  QVERIFY(fetchRefs.isEmpty());

  git::Result explicitPush =
      remote.push(&callbacks, head, QStringLiteral("refs/heads/override"));
  QVERIFY2(!explicitPush.error(), qPrintable(explicitPush.errorString()));
  QByteArray overrideRef;
  QVERIFY(runGit(QString(),
                 {QStringLiteral("--git-dir"), pushDir.path(),
                  QStringLiteral("rev-parse"),
                  QStringLiteral("refs/heads/override")},
                 &overrideRef));
  QCOMPARE(overrideRef, expected);
}

TEST_MAIN(TestRemotes)

#include "remotes.moc"

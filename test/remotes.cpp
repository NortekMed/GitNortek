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
#include "host/Accounts.h"
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSignalSpy>
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

class AccountCleanup {
public:
  ~AccountCleanup() {
    Accounts *accounts = Accounts::instance();
    for (Account *account : mAccounts) {
      if (accounts->indexOf(account) >= 0)
        accounts->removeAccount(account);
    }
  }

  Account *create(Account::Kind kind, const QString &username) {
    Account *account = Accounts::instance()->createAccount(kind, username);
    mAccounts.append(account);
    return account;
  }

private:
  QList<Account *> mAccounts;
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
  void dialogSelectsProviderAndPrefillsUrls();
  void validatesAndRenamesRemote();
  void configuredPushRefspecsAndExplicitOverride();
};

void TestRemotes::dialogAddsAndEditsRemote() {
  ScratchRepository repository;
  QTemporaryDir remoteDir;
  QVERIFY(remoteDir.isValid());

  RemoteConfigDialog blankAdd(repository);
  QVERIFY(blankAdd.width() >= blankAdd.sizeHint().width() * 2);
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
      !blankFetchUrl->placeholderText().contains(QStringLiteral("github.com")));
  QVERIFY(blankPushUrl->text().isEmpty());
  QVERIFY(
      !blankPushUrl->placeholderText().contains(QStringLiteral("github.com")));
  QVERIFY(pushUrlHint->text().contains(QStringLiteral("fetch URL")));
  QCOMPARE(blankAdd.service(), -1);
  QCOMPARE(blankAdd.protocol(), Repository::Ssh);
  auto *protocol =
      blankAdd.findChild<QComboBox *>(QStringLiteral("RemoteProtocol"));
  auto *fetchExample =
      blankAdd.findChild<QLabel *>(QStringLiteral("RemoteFetchUrlExample"));
  auto *pushExample =
      blankAdd.findChild<QLabel *>(QStringLiteral("RemotePushUrlExample"));
  QVERIFY(protocol);
  QVERIFY(fetchExample);
  QVERIFY(pushExample);
  QVERIFY(fetchExample->text().contains(QStringLiteral("<host>")));
  QVERIFY(pushExample->text().contains(QStringLiteral("<host>")));
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

void TestRemotes::dialogSelectsProviderAndPrefillsUrls() {
  ScratchRepository repository;
  AccountCleanup accountCleanup;
  Account *github = accountCleanup.create(Account::GitHub, "alice");
  Repository *hosted = github->addRepository(QStringLiteral("project"),
                                             QStringLiteral("alice/project"));
  hosted->setUrl(Repository::Https,
                 QStringLiteral("https://github.com/alice/project.git"));
  hosted->setUrl(Repository::Ssh,
                 QStringLiteral("git@github.com:alice/project.git"));
  Account *gitlab = accountCleanup.create(Account::GitLab, "bob");
  Repository *other = gitlab->addRepository(QStringLiteral("other"),
                                            QStringLiteral("bob/other"));
  other->setUrl(Repository::Https,
                QStringLiteral("https://gitlab.com/bob/other.git"));
  other->setUrl(Repository::Ssh,
                QStringLiteral("git@gitlab.com:bob/other.git"));

  RemoteConfigDialog dialog(repository, git::Remote(),
                            QStringLiteral("origin"));
  auto *githubButton =
      dialog.findChild<QToolButton *>(QStringLiteral("RemoteServiceGitHub"));
  auto *gitlabButton =
      dialog.findChild<QToolButton *>(QStringLiteral("RemoteServiceGitLab"));
  auto *protocol =
      dialog.findChild<QComboBox *>(QStringLiteral("RemoteProtocol"));
  auto *fetchUrl =
      dialog.findChild<QLineEdit *>(QStringLiteral("RemoteFetchUrl"));
  auto *pushUrl =
      dialog.findChild<QLineEdit *>(QStringLiteral("RemotePushUrl"));
  auto *fetchExample =
      dialog.findChild<QLabel *>(QStringLiteral("RemoteFetchUrlExample"));
  auto *accountMenu =
      dialog.findChild<QToolButton *>(QStringLiteral("RemoteAccountMenu"));
  QVERIFY(githubButton);
  QVERIFY(gitlabButton);
  QVERIFY(protocol);
  QVERIFY(fetchUrl);
  QVERIFY(pushUrl);
  QVERIFY(fetchExample);
  QVERIFY(accountMenu);
  QVERIFY(!githubButton->icon().isNull());
  QVERIFY(!gitlabButton->icon().isNull());

  githubButton->click();
  QCOMPARE(dialog.service(), int(Account::GitHub));
  QVERIFY(fetchExample->text().contains(QStringLiteral("github.com")));
  QVERIFY(!accountMenu->isHidden());

  QMenu *menu = accountMenu->menu();
  QVERIFY(menu);
  QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
  QMenu *githubMenu = nullptr;
  for (QAction *action : menu->actions()) {
    if (action->menu() && action->text().contains(QStringLiteral("alice")))
      githubMenu = action->menu();
  }
  QVERIFY(githubMenu);
  QCOMPARE(githubMenu->actions().size(), 1);
  githubMenu->actions().first()->trigger();
  QCOMPARE(fetchUrl->text(),
           QStringLiteral("git@github.com:alice/project.git"));
  QCOMPARE(pushUrl->text(), QStringLiteral("git@github.com:alice/project.git"));

  protocol->setCurrentIndex(protocol->findData(Repository::Https));
  QCOMPARE(fetchUrl->text(),
           QStringLiteral("https://github.com/alice/project.git"));
  QCOMPARE(pushUrl->text(),
           QStringLiteral("https://github.com/alice/project.git"));

  fetchUrl->setText(QStringLiteral("ssh://manual.example/project"));
  protocol->setCurrentIndex(protocol->findData(Repository::Ssh));
  QCOMPARE(fetchUrl->text(), QStringLiteral("ssh://manual.example/project"));
  QCOMPARE(pushUrl->text(), QStringLiteral("git@github.com:alice/project.git"));

  dialog.setCreatedRepositoryUrls(
      QStringLiteral("https://github.com/alice/new-project.git"),
      QStringLiteral("git@github.com:alice/new-project.git"),
      Repository::Https);
  auto *repositoryStatus =
      dialog.findChild<QLabel *>(QStringLiteral("RemoteRepositoryStatus"));
  QVERIFY(repositoryStatus);
  QVERIFY(repositoryStatus->text().contains(QStringLiteral("not been pushed")));
  QCOMPARE(protocol->currentData().toInt(), int(Repository::Https));
  QCOMPARE(fetchUrl->text(),
           QStringLiteral("https://github.com/alice/new-project.git"));
  QCOMPARE(pushUrl->text(),
           QStringLiteral("https://github.com/alice/new-project.git"));

  QSignalSpy createRequested(&dialog,
                             &RemoteConfigDialog::createRepositoryRequested);
  QVERIFY(createRequested.isValid());
  QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
  QAction *createAction = nullptr;
  for (QAction *action : menu->actions()) {
    if (action->text() == QStringLiteral("Create new repository…"))
      createAction = action;
  }
  QVERIFY(createAction);
  createAction->trigger();
  QCOMPARE(createRequested.count(), 1);
  QCOMPARE(createRequested.first().at(0).toInt(), int(Account::GitHub));
  QCOMPARE(createRequested.first().at(1).toInt(), int(Repository::Https));

  gitlabButton->click();
  QCOMPARE(dialog.service(), int(Account::GitLab));
  QVERIFY(fetchExample->text().contains(QStringLiteral("gitlab.com")));
  QCOMPARE(fetchUrl->text(),
           QStringLiteral("https://github.com/alice/new-project.git"));
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

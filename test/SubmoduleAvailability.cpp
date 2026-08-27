//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "git/Config.h"
#include "git/Reference.h"
#include "git/Submodule.h"
#include "git/SubmoduleAvailability.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>

namespace {

void runGit(const QString &directory, const QStringList &arguments) {
  QProcess git;
  git.setWorkingDirectory(directory);
  git.start(GIT_EXECUTABLE, arguments);
  QVERIFY2(git.waitForFinished(), qPrintable(git.errorString()));
  QVERIFY2(git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0,
           git.readAllStandardError().constData());
}

void commitFile(const QString &directory, const QString &contents,
                const QString &message) {
  QFile file(directory + "/file.txt");
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(contents.toUtf8());
  file.close();
  runGit(directory, {"add", "file.txt"});
  runGit(directory, {"commit", "-m", message});
}

} // namespace

class TestSubmoduleAvailability : public QObject {
  Q_OBJECT

private slots:
  void advertisedAndUnknownCommits();
};

void TestSubmoduleAvailability::advertisedAndUnknownCommits() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const QString remote = root.filePath("remote.git");
  const QString parentRemote = root.filePath("parent.git");
  const QString seed = root.filePath("seed");
  const QString parentPath = root.filePath("parent");

  QDir().mkpath(remote);
  QDir().mkpath(parentRemote);
  QDir().mkpath(seed);
  QDir().mkpath(parentPath);
  runGit(remote, {"init", "--bare"});
  runGit(parentRemote, {"init", "--bare"});
  runGit(seed, {"init"});
  runGit(seed, {"config", "user.name", "Test"});
  runGit(seed, {"config", "user.email", "test@example.com"});
  commitFile(seed, "one\n", "one");
  runGit(seed, {"branch", "-M", "main"});
  runGit(seed, {"remote", "add", "origin", remote});
  runGit(seed, {"push", "-u", "origin", "main"});

  runGit(parentPath, {"init"});
  runGit(parentPath, {"config", "user.name", "Test"});
  runGit(parentPath, {"config", "user.email", "test@example.com"});
  runGit(parentPath, {"-c", "protocol.file.allow=always", "submodule", "add",
                      "-b", "main", remote, "child"});
  runGit(parentPath, {"-c", "protocol.file.allow=always", "submodule", "add",
                      "-b", "main", remote, "child-ok"});
  runGit(parentPath, {"commit", "-m", "add child"});
  runGit(parentPath, {"remote", "add", "origin", parentRemote});
  runGit(parentPath, {"push", "-u", "origin", "HEAD"});

  git::Repository parent = git::Repository::open(parentPath);
  git::Repository publishedParent = git::Repository::open(parentRemote);
  QVERIFY(parent.isValid());
  QVERIFY(publishedParent.isValid());
  QCOMPARE(parent.submodules().size(), 2);
  git::Submodule checkedSubmodule = parent.lookupSubmodule("child");
  QVERIFY(checkedSubmodule.isInitialized());
  git::Repository checkedChild = checkedSubmodule.open();
  QVERIFY(checkedChild.isValid());
  checkedChild.gitConfig().setValue<QString>("http.followRedirects", "invalid");
  QVERIFY(git::SubmoduleAvailability::check(parent, parent.head().target())
              .isEmpty());
  QVERIFY(checkedChild.gitConfig().remove("http.followRedirects"));
  QVERIFY(git::SubmoduleAvailability::check(parent, parent.head().target())
              .isEmpty());
  const git::Id initialParent = parent.head().target().id();

  const QString child = parentPath + "/child";
  runGit(child, {"config", "user.name", "Test"});
  runGit(child, {"config", "user.email", "test@example.com"});
  commitFile(child, "two\n", "two");
  runGit(parentPath, {"add", "child"});
  runGit(parentPath, {"commit", "-m", "pin two"});

  QList<git::SubmoduleAvailability::Issue> issues =
      git::SubmoduleAvailability::check(parent, parent.head().target());
  QCOMPARE(issues.size(), 1);
  QCOMPARE(issues.first().reason,
           git::SubmoduleAvailability::Issue::NotAdvertised);
  QCOMPARE(issues.first().path, QString("child"));
  QVERIFY(!issues.first().message.contains("no error", Qt::CaseInsensitive));

  MainWindow window(parent);
  window.show();
  QVERIFY(QTest::qWaitForWindowActive(&window));
  RepoView *view = window.currentView();
  QVERIFY(view);
  view->push();
  QTRY_VERIFY_WITH_TIMEOUT(view->findChild<QMessageBox *>(), 10000);
  QMessageBox *dialog = view->findChild<QMessageBox *>();
  QCOMPARE(dialog->windowTitle(), QString("Submodule Commits May Be Missing"));
  QCOMPARE(dialog->detailedText().count("Pinned commit:"), 1);
  QVERIFY(dialog->detailedText().contains("child (child)"));
  QVERIFY(!dialog->detailedText().contains("child-ok"));
  QVERIFY(!dialog->detailedText().contains("no error", Qt::CaseInsensitive));
  dialog->button(QMessageBox::Cancel)->click();
  QTRY_VERIFY(!view->findChild<QMessageBox *>());
  QCOMPARE(publishedParent.head().target().id(), initialParent);

  view->push();
  QTRY_VERIFY_WITH_TIMEOUT(view->findChild<QMessageBox *>(), 10000);
  dialog = view->findChild<QMessageBox *>();
  QPushButton *override = nullptr;
  foreach (QPushButton *button, dialog->findChildren<QPushButton *>()) {
    if (button->text() == "Push Parent Anyway") {
      override = button;
      break;
    }
  }
  QVERIFY(override);
  override->click();
  QTRY_COMPARE_WITH_TIMEOUT(publishedParent.head().target().id(),
                            parent.head().target().id(), 10000);

  runGit(child, {"push", "origin", "HEAD:main"});
  commitFile(child, "three\n", "three");
  runGit(child, {"push", "origin", "HEAD:main"});
  QVERIFY(git::SubmoduleAvailability::check(parent, parent.head().target())
              .isEmpty());

  runGit(seed, {"pull", "--ff-only"});
  commitFile(seed, "four\n", "four");
  runGit(seed, {"push"});
  issues = git::SubmoduleAvailability::check(parent, parent.head().target());
  QVERIFY(issues.isEmpty());

  git::Submodule submodule = parent.lookupSubmodule("child");
  submodule.setUrl(root.filePath("missing.git"));
  issues = git::SubmoduleAvailability::check(parent, parent.head().target());
  QCOMPARE(issues.size(), 1);
  QCOMPARE(issues.first().reason,
           git::SubmoduleAvailability::Issue::RemoteError);
  QVERIFY(!issues.first().message.contains("http.followRedirects"));

  runGit(parentPath, {"submodule", "deinit", "-f", "child"});
  issues = git::SubmoduleAvailability::check(parent, parent.head().target());
  QCOMPARE(issues.size(), 1);
  QCOMPARE(issues.first().reason,
           git::SubmoduleAvailability::Issue::LocalError);
}

TEST_MAIN(TestSubmoduleAvailability)

#include "SubmoduleAvailability.moc"

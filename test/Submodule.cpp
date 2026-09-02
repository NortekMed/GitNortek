//
//          Copyright (c) 2022, Gittyup Team
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Martin Marmsoler
//

#include "Test.h"

#include "qtsupport.h"
#include "dialogs/CloneDialog.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "ui/RepoView.h"
#include "conf/Settings.h"
#include "git/Config.h"
#include "git/Submodule.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/TreeView.h"
#include "watcher/RepositoryWatcher.h"

#include <QToolButton>
#include <QMenu>
#include <QWizard>
#include <QLineEdit>
#include <QtConcurrent>

#define INIT_REPO(repoPath, /* bool */ useTempDir)                             \
  QString path = Test::extractRepository(repoPath, useTempDir);                \
  QVERIFY(!path.isEmpty());                                                    \
  auto repo = git::Repository::open(path);                                     \
  QVERIFY(repo.isValid());                                                     \
  Test::initRepo(repo);                                                        \
  MainWindow window(repo);                                                     \
  window.show();                                                               \
  QVERIFY(QTest::qWaitForWindowExposed(&window));                              \
                                                                               \
  RepoView *repoView = window.currentView();                                   \
  auto diff = repo.status(repo.index(), nullptr, false);

using namespace Test;
using namespace QTest;

namespace {

bool runGit(const QString &path, const QStringList &arguments) {
  QProcess git;
  git.setWorkingDirectory(path);
  git.start(GIT_EXECUTABLE, arguments);
  return git.waitForFinished() && git.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;

  file.write(contents);
  return true;
}

} // namespace

class TestSubmodule : public QObject {
  Q_OBJECT

private slots:
  void updateSubmoduleClone();
  void noUpdateSubmoduleClone();
  void openDoesNotInitializeSubmodules();
  void discardFile();
  void canceledStatusIsDiscarded();
  void cleanAfterBranchRename();
  void movedHeadDetected();
  void listsUninitializedSubmodules();
  void excludesRemovedSubmodules();
  void listsDirtySubmodules();
  void removeSubmodule();
  void refuseRemovalWithGitmodulesChanges();

private:
};

void TestSubmodule::updateSubmoduleClone() {
  // Update submodules after cloning
  QString remote = Test::extractRepository("SubmoduleTest.zip", true);
  QCOMPARE(remote.isEmpty(), false);

  Settings *settings = Settings::instance();
  settings->setValue(Setting::Id::UpdateSubmodulesAfterPullAndClone, false);
  CloneDialog *d = new CloneDialog(CloneDialog::Kind::Clone);
  QVERIFY(!d->findChild<QWidget *>("CloneAdvancedOptions")->isHidden());
  QCOMPARE(d->field("updateSubmodules").toBool(), false);

  RepoView *view = nullptr;

  bool cloneFinished = false;
  QObject::connect(d, &CloneDialog::accepted, [d, &view, &cloneFinished] {
    cloneFinished = true;
    if (MainWindow *window =
            MainWindow::open(d->path(), true, MainWindow::OpenSource::Other,
                             d->updateSubmodules())) {
      view = window->currentView();
    }
  });

  QTemporaryDir tempdir;
  QVERIFY(tempdir.isValid());
  d->setField("url", remote);
  d->setField("name", "TestrepoSubmodule");
  d->setField("path", tempdir.path());
  d->setField("bare", "false");
  d->setField("updateSubmodules", true);
  d->page(2)->initializePage(); // start clone

  {
    auto timeout = Timeout(10e3, "Failed to clone");
    while (!cloneFinished)
      qWait(300);
  }

  QVERIFY(view);
  QTRY_VERIFY_WITH_TIMEOUT(
      [&view] {
        const QList<git::Submodule> submodules = view->repo().submodules();
        return submodules.count() == 1 && submodules.first().isInitialized();
      }(),
      10000);
  QCOMPARE(view->repo().submodules().count(), 1);
  for (const auto &s : view->repo().submodules()) {
    QVERIFY(s.isValid());
    QVERIFY(s.isInitialized());
  }
}

void TestSubmodule::noUpdateSubmoduleClone() {
  // Don't update submodules after cloning
  QString remote = Test::extractRepository("SubmoduleTest.zip", true);
  QCOMPARE(remote.isEmpty(), false);

  Settings *settings = Settings::instance();
  settings->setValue(Setting::Id::UpdateSubmodulesAfterPullAndClone, true);
  CloneDialog *d = new CloneDialog(CloneDialog::Kind::Clone);
  QCOMPARE(d->field("updateSubmodules").toBool(), true);

  RepoView *view = nullptr;

  bool cloneFinished = false;
  QObject::connect(d, &CloneDialog::accepted, [d, &view, &cloneFinished] {
    cloneFinished = true;
    if (MainWindow *window =
            MainWindow::open(d->path(), true, MainWindow::OpenSource::Other,
                             d->updateSubmodules())) {
      view = window->currentView();
    }
  });

  QTemporaryDir tempdir;
  QVERIFY(tempdir.isValid());
  d->setField("url", remote);
  d->setField("name", "TestrepoSubmodule");
  d->setField("path", tempdir.path());
  d->setField("bare", "false");
  d->setField("updateSubmodules", false);
  d->page(2)->initializePage(); // start clone

  {
    auto timeout = Timeout(10e3, "Failed to clone");
    while (!cloneFinished)
      qWait(300);
  }

  QVERIFY(view);
  QCOMPARE(view->repo().submodules().count(), 1);
  for (auto s : view->repo().submodules()) {
    QVERIFY(s.isValid());
    QCOMPARE(s.isInitialized(), false);

    git::Submodule::UpdateStatus status = s.checkForUpdates(nullptr);
    QCOMPARE(status.state, git::Submodule::UpdateStatus::NotTracked);
    QCOMPARE(status.message, QString("No submodule branch is configured."));

    s.setBranch("main");
    view->repo().invalidateSubmoduleCache();
    status = view->repo().lookupSubmodule(s.name()).checkForUpdates(nullptr);
    QCOMPARE(status.state, git::Submodule::UpdateStatus::Error);
    QCOMPARE(status.message,
             QString("The submodule repository is not initialized."));
  }
}

void TestSubmodule::openDoesNotInitializeSubmodules() {
  QString path = Test::extractRepository("SubmoduleTest.zip", true);
  QVERIFY(!path.isEmpty());
  git::Repository repo = git::Repository::open(path);
  QVERIFY(repo.isValid());
  Test::initRepo(repo);

  QList<git::Submodule> submodules = repo.submodules();
  QCOMPARE(submodules.size(), 1);
  git::Submodule submodule = submodules.first();
  QVERIFY(submodule.isValid());
  const QString name = submodule.name();
  submodule.deinitialize();
  repo.invalidateSubmoduleCache();
  submodule = repo.lookupSubmodule(name);
  QVERIFY(!submodule.isInitialized());

  const QString configKey =
      QString("submodule.%1.url").arg(submodule.name());
  QVERIFY(repo.gitConfig().value<QString>(configKey).isEmpty());

  Settings *settings = Settings::instance();
  bool updateAfterPullAndClone =
      settings->value(Setting::Id::UpdateSubmodulesAfterPullAndClone).toBool();
  auto restoreSetting = qScopeGuard([settings, updateAfterPullAndClone] {
    settings->setValue(Setting::Id::UpdateSubmodulesAfterPullAndClone,
                       updateAfterPullAndClone);
  });
  settings->setValue(Setting::Id::UpdateSubmodulesAfterPullAndClone, true);
  repo.appConfig().setValue("autoupdate.enable", true);

  MainWindow window(repo);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  qWait(1000);

  repo.invalidateSubmoduleCache();
  submodule = repo.lookupSubmodule(name);
  QVERIFY(submodule.isValid());
  QVERIFY(!submodule.isInitialized());
  QVERIFY(repo.gitConfig().value<QString>(configKey).isEmpty());

  QDir worktree(repo.workdir().filePath(submodule.path()));
  QVERIFY(worktree.entryList(QDir::AllEntries | QDir::Hidden |
                             QDir::NoDotAndDotDot)
              .isEmpty());
}

void TestSubmodule::discardFile() {
  // Discarding a file should not reset the submodule
  INIT_REPO("SubmoduleTest.zip", true);
  repoView->updateSubmodules(repo.submodules(), true, true);

  qWait(1000); // Not needed if the test is long enough and the fetch operation
               // finishes

  QCOMPARE(repo.submodules().count(), 1);
  for (const auto &submodule : repo.submodules())
    QVERIFY(submodule.isInitialized());

  {
    QFile file(repo.workdir().filePath("README.md"));
    QVERIFY(file.open(QFile::WriteOnly));
    QTextStream(&file) << "Changing readme of main repository" << Qt::endl;
    file.close();
  }

  {
    QFile file(repo.workdir().filePath("GittyupTestRepo/README.md"));
    QVERIFY(file.open(QFile::WriteOnly));
    QTextStream(&file) << "Changing content of submodule readme" << Qt::endl;
    file.close();
  }

  auto doubleTree = repoView->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  // Select head
  // Does not work
  // repoView->selectHead();
  // repoView->selectFirstCommit();

  refresh(repoView); // Do a refresh to simulate selecting the working directory
                     // entry in the commit list

  {
    // wait for refresh!
    auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
    QVERIFY(unstagedTree);
    QAbstractItemModel *unstagedModel = unstagedTree->model();
    auto timeout = Timeout(10000, "Repository didn't refresh in time");
    while (unstagedModel->rowCount() < 2 ||
           unstagedModel->data(unstagedModel->index(1, 0)) != "README.md")
      qWait(300);
  }

  {
    auto unstagedTree = doubleTree->findChild<TreeView *>("Unstaged");
    QVERIFY(unstagedTree);
    QAbstractItemModel *unstagedModel = unstagedTree->model();

    QCOMPARE(unstagedModel->rowCount(), 2);
    auto submodule = unstagedModel->index(0, 0);
    auto readme = unstagedModel->index(1, 0);
    QCOMPARE(unstagedModel->data(readme).toString(), QString("README.md"));

    unstagedTree->discard(readme, true);
  }

  QFile file(repo.workdir().filePath("GittyupTestRepo/README.md"));
  QVERIFY(file.open(QFile::ReadOnly));
  QCOMPARE(file.readAll(), "Changing content of submodule readme\n");
}

void TestSubmodule::canceledStatusIsDiscarded() {
  class CancelCallbacks : public git::Diff::Callbacks {
  public:
    bool progress(const QString &, const QString &) override {
      called = true;
      return false;
    }

    bool called = false;
  } callbacks;

  ScratchRepository repo;
  QFile file(repo->workdir().filePath("untracked.txt"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("untracked\n");
  file.close();

  git::Diff status = repo->status(repo->index(), &callbacks, false);
  QVERIFY(callbacks.called);
  QVERIFY(!status.isValid());
}

void TestSubmodule::cleanAfterBranchRename() {
  ScratchRepository child;
  QFile childFile(child->workdir().filePath("child.txt"));
  QVERIFY(childFile.open(QIODevice::WriteOnly));
  childFile.write("child\n");
  childFile.close();

  QProcess git;
  git.setWorkingDirectory(child->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "child.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  ScratchRepository parent;
  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"-c", "protocol.file.allow=always", "submodule", "add",
             child->workdir().path(), "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "add submodule"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  parent->appConfig().setValue("autofetch.enable", false);
  QCOMPARE(parent->submodules().size(), 1);
  QVERIFY(!parent->status(parent->index(), nullptr, false).isValid());

  MainWindow window(parent);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  RepoView *view = window.currentView();
  refresh(view, false);

  QSignalSpy statusChanged(view, &RepoView::statusChanged);
  git::Branch renamed = git::Branch(parent->head()).rename("contextual");
  QVERIFY(renamed.isValid());
  QCOMPARE(git::Branch(parent->head()).name(), QString("contextual"));
  qWait(100);
  QVERIFY(statusChanged.isEmpty());

  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE, {"status", "--short"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  QVERIFY(git.readAllStandardOutput().isEmpty());

  // Make the watcher-driven refresh deterministic even if Git did not need to
  // rewrite index metadata on this filesystem.
  emit parent->notifier()->workdirChanged();
  QTRY_VERIFY_WITH_TIMEOUT(!statusChanged.isEmpty(), 10000);
  for (const QList<QVariant> &args : statusChanged)
    QVERIFY(!args.first().toBool());

  git::Repository repo = parent;
  for (int i = 0; i < 20; ++i) {
    git::Index index = parent->index();
    index.read();
    QFuture<git::Diff> future = QtConcurrent::run(
        [repo, index] { return repo.status(index, nullptr, false); });
    while (!future.isFinished()) {
      parent->invalidateSubmoduleCache();
      QCOMPARE(parent->submodules().size(), 1);
    }
    QVERIFY(!future.result().isValid());
  }
}

void TestSubmodule::movedHeadDetected() {
  ScratchRepository child;
  QFile childFile(child->workdir().filePath("child.txt"));
  QVERIFY(childFile.open(QIODevice::WriteOnly));
  childFile.write("first\n");
  childFile.close();

  QProcess git;
  git.setWorkingDirectory(child->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "child.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "first"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  ScratchRepository parent;
  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"-c", "protocol.file.allow=always", "submodule", "add",
             child->workdir().path(), "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "add submodule"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  // Prime the repository-level submodule cache at the recorded commit.
  QCOMPARE(parent->submodules().size(), 1);

#ifdef Q_OS_LINUX
  RepositoryWatcher watcher(parent);
  QSignalSpy workdirChanged(parent->notifier(),
                            &git::RepositoryNotifier::workdirChanged);
  qWait(100);
#endif

  git.setWorkingDirectory(parent->workdir().filePath("child"));
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "second"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

#ifdef Q_OS_LINUX
  QTRY_VERIFY_WITH_TIMEOUT(!workdirChanged.isEmpty(), 5000);
#endif

  git::Diff status = parent->status(parent->index(), nullptr, false);
  QVERIFY(status.isValid());
  QCOMPARE(status.count(), 1);
  QCOMPARE(status.name(0), QString("child"));
  QCOMPARE(status.status(0), GIT_DELTA_MODIFIED);

  git::Submodule submodule = parent->submodules().first();
  git::Id oldId = submodule.headId();
  git::Id stagedId = submodule.open().head().target().id();
  parent->index().setStaged({"child"}, true);

  git.setWorkingDirectory(parent->workdir().filePath("child"));
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "third"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git::Id newId = submodule.open().head().target().id();
  QString message = QString("Update child from %1 to %2:\n- %2 third")
                        .arg(oldId.toString().left(7),
                             newId.toString().left(7));
  QVERIFY(!parent->commitSubmodule(submodule, newId, message).isValid());
  QCOMPARE(parent->lookupSubmodule("child").indexId(), stagedId);
  parent->index().setStaged({"child"}, false);

  QFile parentFile(parent->workdir().filePath("parent.txt"));
  QVERIFY(parentFile.open(QIODevice::WriteOnly));
  parentFile.write("unrelated\n");
  parentFile.close();
  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "parent.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  submodule = parent->submodules().first();
  QVERIFY(!parent->commitSubmodule(submodule, oldId, message).isValid());
  git::Commit commit = parent->commitSubmodule(submodule, newId, message);
  QVERIFY(commit.isValid());
  QCOMPARE(commit.message().trimmed(), message);
  QCOMPARE(commit.tree().id("child"), newId);
  QVERIFY(commit.tree().id("parent.txt").isNull());

  status = parent->status(parent->index(), nullptr, false);
  QVERIFY(status.isValid());
  QCOMPARE(status.count(), 1);
  QCOMPARE(status.name(0), QString("parent.txt"));
  QVERIFY(parent->index().isTracked("parent.txt"));
}

void TestSubmodule::listsUninitializedSubmodules() {
  ScratchRepository child;
  QVERIFY(writeFile(child->workdir().filePath("child.txt"), "child\n"));
  QVERIFY(runGit(child->workdir().path(), {"add", "child.txt"}));
  QVERIFY(runGit(child->workdir().path(), {"commit", "-m", "child"}));

  ScratchRepository parent;
  QVERIFY(runGit(parent->workdir().path(),
                 {"-c", "protocol.file.allow=always", "submodule", "add",
                  child->workdir().path(), "child"}));
  QVERIFY(runGit(parent->workdir().path(), {"commit", "-m", "add child"}));
  QVERIFY(runGit(parent->workdir().path(),
                 {"submodule", "deinit", "-f", "child"}));

  parent->invalidateSubmoduleCache();
  QList<git::Submodule> submodules = parent->submodules();
  QCOMPARE(submodules.count(), 1);
  QCOMPARE(submodules.first().name(), QString("child"));
  QVERIFY(!submodules.first().isInitialized());
}

void TestSubmodule::excludesRemovedSubmodules() {
  ScratchRepository child;
  QVERIFY(writeFile(child->workdir().filePath("child.txt"), "child\n"));
  QVERIFY(runGit(child->workdir().path(), {"add", "child.txt"}));
  QVERIFY(runGit(child->workdir().path(), {"commit", "-m", "child"}));

  ScratchRepository parent;
  QVERIFY(runGit(parent->workdir().path(),
                 {"-c", "protocol.file.allow=always", "submodule", "add",
                  child->workdir().path(), "child"}));
  QVERIFY(runGit(parent->workdir().path(), {"commit", "-m", "add child"}));
  QVERIFY(runGit(parent->workdir().path(), {"rm", "-f", "child"}));

  parent->invalidateSubmoduleCache();
  QCOMPARE(parent->submodules().count(), 0);
}

void TestSubmodule::listsDirtySubmodules() {
  ScratchRepository child;
  QVERIFY(writeFile(child->workdir().filePath("child.txt"), "child\n"));
  QVERIFY(runGit(child->workdir().path(), {"add", "child.txt"}));
  QVERIFY(runGit(child->workdir().path(), {"commit", "-m", "child"}));

  ScratchRepository parent;
  QVERIFY(runGit(parent->workdir().path(),
                 {"-c", "protocol.file.allow=always", "submodule", "add",
                  child->workdir().path(), "child"}));
  QVERIFY(runGit(parent->workdir().path(), {"commit", "-m", "add child"}));
  QVERIFY(writeFile(parent->workdir().filePath("child/dirty.txt"), "dirty\n"));

  parent->invalidateSubmoduleCache();
  QList<git::Submodule> submodules = parent->submodules();
  QCOMPARE(submodules.count(), 1);
  QCOMPARE(submodules.first().name(), QString("child"));
  QVERIFY(submodules.first().isInitialized());
}

void TestSubmodule::removeSubmodule() {
  ScratchRepository child;
  QFile childFile(child->workdir().filePath("child.txt"));
  QVERIFY(childFile.open(QIODevice::WriteOnly));
  childFile.write("child\n");
  childFile.close();

  QProcess git;
  git.setWorkingDirectory(child->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "child.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  ScratchRepository parent;
  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"-c", "protocol.file.allow=always", "submodule", "add",
             child->workdir().path(), "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "add submodule"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git::Submodule submodule = parent->submodules().first();
  QVERIFY(submodule.isValid());
  const QString worktree = parent->workdir().filePath("child");
  const QString cache = parent->dir().filePath("modules/child");
  QVERIFY(QFileInfo::exists(worktree));
  QVERIFY(QFileInfo::exists(cache));

  git::Result result = git::Submodule::remove(parent, submodule);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(!QFileInfo::exists(worktree));
  QVERIFY(!QFileInfo::exists(cache));
  QCOMPARE(parent->submodules().count(), 0);
  QVERIFY(parent->gitConfig().value<QString>("submodule.child.url").isEmpty());

  git.start(GIT_EXECUTABLE, {"diff", "--cached", "--name-status"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  const QByteArray staged = git.readAllStandardOutput();
  QVERIFY(staged.contains("M\t.gitmodules"));
  QVERIFY(staged.contains("D\tchild"));
}

void TestSubmodule::refuseRemovalWithGitmodulesChanges() {
  ScratchRepository child;
  QFile childFile(child->workdir().filePath("child.txt"));
  QVERIFY(childFile.open(QIODevice::WriteOnly));
  childFile.write("child\n");
  childFile.close();

  QProcess git;
  git.setWorkingDirectory(child->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "child.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  ScratchRepository parent;
  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"-c", "protocol.file.allow=always", "submodule", "add",
             child->workdir().path(), "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "add submodule"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  QFile modules(parent->workdir().filePath(".gitmodules"));
  QVERIFY(modules.open(QIODevice::Append));
  modules.write("# keep this change\n");
  modules.close();

  git::Submodule submodule = parent->submodules().first();
  git::Result result = git::Submodule::remove(parent, submodule);
  QVERIFY(!result);
  QVERIFY(result.errorString().contains("existing changes"));
  QVERIFY(QFileInfo::exists(parent->workdir().filePath("child")));
  QVERIFY(QFileInfo::exists(parent->dir().filePath("modules/child")));
  QCOMPARE(parent->submodules().count(), 1);
}

TEST_MAIN(TestSubmodule)

#include "Submodule.moc"

//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "Test.h"

#include "git/WorkingTreeUntrack.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <algorithm>

namespace {

bool runGit(const QString &path, const QStringList &arguments) {
  QProcess process;
  QStringList command = {QStringLiteral("-C"), path};
  command.append(arguments);
  process.start(QStringLiteral("git"), command);
  return process.waitForFinished() &&
         process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return QByteArray();
  return file.readAll();
}

} // namespace

class TestWorkingTreeUntrack : public QObject {
  Q_OBJECT

private slots:
  void preparesFolderAndKeepsFilesWhenDeletionIsDisabled();
  void deletesTrackedAndUntrackedFilesSeparately();
  void rejectsFilesChangedAfterPreparation();
  void preservesGeneratedIgnoreFileWhenDeletingTrackedPaths();
  void preservesUntrackedIgnoreFileWhenDeletingPaths();
  void protectsNestedRepositories();
#ifndef Q_OS_WIN
  void refusesDeletionThroughSymlinkedParent();
#endif
};

void TestWorkingTreeUntrack::
    preparesFolderAndKeepsFilesWhenDeletionIsDisabled() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  const QString folderPath = repo.workdir().filePath("folder");
  QVERIFY(QDir().mkpath(folderPath));
  QVERIFY(writeFile(repo.workdir().filePath(".gitignore"),
                    "/folder/ignored.txt\n"));
  QVERIFY(writeFile(QDir(folderPath).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(repositoryPath, {"add", "."}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));
  QVERIFY(writeFile(QDir(folderPath).filePath("untracked.txt"), "new\n"));
  QVERIFY(writeFile(QDir(folderPath).filePath("ignored.txt"), "ignored\n"));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath, {"folder"}, canceled);
  QVERIFY(preparation.isValid());
  QCOMPARE(preparation.plan.trackedPaths, QStringList({"folder/tracked.txt"}));
  QCOMPARE(preparation.plan.untrackedPaths,
           QStringList({"folder/untracked.txt"}));
  QCOMPARE(preparation.plan.ignoredPaths, QStringList({"folder/ignored.txt"}));
  QCOMPARE(preparation.plan.ignorePatterns, QStringList({"/folder/"}));

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, false, false,
                                       canceled);
  QVERIFY(execution.isValid());
  QVERIFY(execution.indexWritten);
  QVERIFY(execution.ignoreWritten);
  QVERIFY(QFile::exists(QDir(folderPath).filePath("tracked.txt")));
  QVERIFY(QFile::exists(QDir(folderPath).filePath("untracked.txt")));
  QVERIFY(QFile::exists(QDir(folderPath).filePath("ignored.txt")));
  QVERIFY(
      readFile(repo.workdir().filePath(".gitignore")).contains("/folder/\n"));

  const git::Repository reopened = git::Repository::open(repositoryPath);
  QVERIFY(reopened.index().pathsUnder({"folder"}).isEmpty());
}

void TestWorkingTreeUntrack::deletesTrackedAndUntrackedFilesSeparately() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  const QString folderPath = repo.workdir().filePath("folder");
  QVERIFY(QDir().mkpath(folderPath));
  QVERIFY(writeFile(QDir(folderPath).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(repositoryPath, {"add", "."}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));
  QVERIFY(writeFile(QDir(folderPath).filePath("untracked.txt"), "new\n"));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath, {"folder"}, canceled);
  QVERIFY(preparation.isValid());

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, true, true, canceled);
  QVERIFY(execution.isValid());
  QCOMPARE(execution.deletedTrackedPaths, QStringList({"folder/tracked.txt"}));
  QCOMPARE(execution.deletedUntrackedPaths,
           QStringList({"folder/untracked.txt"}));
  QVERIFY(!QFile::exists(QDir(folderPath).filePath("tracked.txt")));
  QVERIFY(!QFile::exists(QDir(folderPath).filePath("untracked.txt")));
  QVERIFY(!QFile::exists(folderPath));
}

void TestWorkingTreeUntrack::rejectsFilesChangedAfterPreparation() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "initial\n"));
  QVERIFY(runGit(repositoryPath, {"add", "tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath, {"tracked.txt"},
                                       canceled);
  QVERIFY(preparation.isValid());
  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "changed\n"));

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, true, false, canceled);
  QVERIFY(!execution.isValid());
  QVERIFY(!execution.error.isEmpty());
  QVERIFY(QFile::exists(repo.workdir().filePath("tracked.txt")));
  QCOMPARE(readFile(repo.workdir().filePath("tracked.txt")),
           QByteArray("changed\n"));
}

void TestWorkingTreeUntrack::
    preservesGeneratedIgnoreFileWhenDeletingTrackedPaths() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  QVERIFY(writeFile(repo.workdir().filePath(".gitignore"), "existing\n"));
  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(repositoryPath, {"add", "."}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath, {".gitignore"},
                                       canceled);
  QVERIFY(preparation.isValid());
  QCOMPARE(preparation.plan.preservedTrackedPaths, QStringList({".gitignore"}));

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, true, false, canceled);
  QVERIFY(execution.isValid());
  QVERIFY(QFile::exists(repo.workdir().filePath(".gitignore")));
  QVERIFY(readFile(repo.workdir().filePath(".gitignore"))
              .contains("/.gitignore\n"));
  QVERIFY(git::Repository::open(repositoryPath)
              .index()
              .pathsUnder({".gitignore"})
              .isEmpty());
}

void TestWorkingTreeUntrack::preservesUntrackedIgnoreFileWhenDeletingPaths() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  QVERIFY(writeFile(repo.workdir().filePath(".gitignore"), "existing\n"));
  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(repositoryPath, {"add", "tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath,
                                       {"tracked.txt", ".gitignore"}, canceled);
  QVERIFY(preparation.isValid());
  QCOMPARE(preparation.plan.preservedUntrackedPaths,
           QStringList({".gitignore"}));

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, true, true, canceled);
  QVERIFY(execution.isValid());
  QVERIFY(!QFile::exists(repo.workdir().filePath("tracked.txt")));
  QVERIFY(QFile::exists(repo.workdir().filePath(".gitignore")));
  QVERIFY(readFile(repo.workdir().filePath(".gitignore"))
              .contains("/.gitignore\n"));
}

void TestWorkingTreeUntrack::protectsNestedRepositories() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  const QString folderPath = repo.workdir().filePath("folder");
  const QString nestedPath = QDir(folderPath).filePath("nested");
  QVERIFY(QDir().mkpath(folderPath));
  QVERIFY(writeFile(QDir(folderPath).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(repositoryPath, {"add", "folder/tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));
  QVERIFY(runGit(repositoryPath, {"init", "--bare", "folder/nested"}));
  QVERIFY(writeFile(QDir(folderPath).filePath("untracked.txt"), "new\n"));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath, {"folder"}, canceled);
  QVERIFY(preparation.isValid());
  QVERIFY(preparation.plan.protectedPaths.contains("folder/nested"));
  QVERIFY(std::none_of(
      preparation.plan.untrackedPaths.cbegin(),
      preparation.plan.untrackedPaths.cend(),
      [](const QString &path) { return path.startsWith("folder/nested/"); }));

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, true, true, canceled);
  QVERIFY(execution.isValid());
  QVERIFY(QDir(nestedPath).exists());
  QVERIFY(!QFile::exists(QDir(folderPath).filePath("tracked.txt")));
  QVERIFY(!QFile::exists(QDir(folderPath).filePath("untracked.txt")));
}

#ifndef Q_OS_WIN
void TestWorkingTreeUntrack::refusesDeletionThroughSymlinkedParent() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();
  const QString folderPath = repo.workdir().filePath("link");
  QVERIFY(QDir().mkpath(folderPath));
  QVERIFY(writeFile(QDir(folderPath).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(repositoryPath, {"add", "link/tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));

  const QString outsidePath = repo.workdir().filePath("outside");
  QVERIFY(QDir().mkpath(outsidePath));
  QVERIFY(writeFile(QDir(outsidePath).filePath("tracked.txt"), "outside\n"));
  QVERIFY(QDir(folderPath).removeRecursively());
  QVERIFY(QFile::link(outsidePath, folderPath));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeUntrackPreparation preparation =
      git::WorkingTreeUntrack::prepare(repositoryPath, {"link"}, canceled);
  QVERIFY(preparation.isValid());

  const git::WorkingTreeUntrackExecution execution =
      git::WorkingTreeUntrack::execute(preparation.plan, true, false, canceled);
  QVERIFY(!execution.isValid());
  QVERIFY(execution.failedPaths.contains("link/tracked.txt"));
  QVERIFY(QFileInfo(folderPath).isSymLink());
  QCOMPARE(readFile(QDir(outsidePath).filePath("tracked.txt")),
           QByteArray("outside\n"));
}
#endif

TEST_MAIN(TestWorkingTreeUntrack)

#include "WorkingTreeUntrack.moc"

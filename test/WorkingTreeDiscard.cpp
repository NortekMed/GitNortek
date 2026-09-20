//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "Test.h"

#include "git/Reference.h"
#include "git/WorkingTreeDiscard.h"
#include "git/WorkingTreeStatus.h"

#include <QFile>
#include <QProcess>

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

} // namespace

class TestWorkingTreeDiscard : public QObject {
  Q_OBJECT

private slots:
  void preservesPathsAddedAfterPreparation();
  void handlesUnbornRepository();
  void rejectsChangedHeadBeforeExecution();
  void rejectsChangedTrackedStatusBeforeExecution();
  void restoresSubmoduleHeads();
};

void TestWorkingTreeDiscard::preservesPathsAddedAfterPreparation() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "initial\n"));
  QVERIFY(writeFile(repo.workdir().filePath("deleted.txt"), "restore me\n"));
  QVERIFY(runGit(repositoryPath, {"add", "tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"add", "deleted.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "modified\n"));
  QVERIFY(QFile::remove(repo.workdir().filePath("deleted.txt")));
  QVERIFY(writeFile(repo.workdir().filePath("planned.txt"), "remove\n"));
  const QString headId = repo.head().target().id().toString();
  const auto canceled = std::make_shared<std::atomic_bool>(false);

  const git::WorkingTreeDiscardPreparation preparation =
      git::WorkingTreeDiscard::prepare(repositoryPath, headId,
                                       {"tracked.txt", "deleted.txt"},
                                       {"planned.txt"}, canceled);
  QVERIFY(preparation.isValid());
  QVERIFY(preparation.plan.isDirty());

  QVERIFY(writeFile(repo.workdir().filePath("late.txt"), "keep\n"));
  const git::WorkingTreeDiscardExecution execution =
      git::WorkingTreeDiscard::execute(preparation.plan, canceled);
  QVERIFY(execution.isValid());

  QFile tracked(repo.workdir().filePath("tracked.txt"));
  QVERIFY(tracked.open(QIODevice::ReadOnly));
  QCOMPARE(tracked.readAll(), QByteArray("initial\n"));
  QFile deleted(repo.workdir().filePath("deleted.txt"));
  QVERIFY(deleted.open(QIODevice::ReadOnly));
  QCOMPARE(deleted.readAll(), QByteArray("restore me\n"));
  QVERIFY(!QFile::exists(repo.workdir().filePath("planned.txt")));
  QVERIFY(QFile::exists(repo.workdir().filePath("late.txt")));
}

void TestWorkingTreeDiscard::handlesUnbornRepository() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();

  QVERIFY(writeFile(repo.workdir().filePath("staged.txt"), "staged\n"));
  repo.index().setStaged({"staged.txt"}, true, false);
  QVERIFY(writeFile(repo.workdir().filePath("untracked.txt"), "new\n"));

  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeDiscardPreparation preparation =
      git::WorkingTreeDiscard::prepare(repositoryPath, QString(), {},
                                       {"staged.txt", "untracked.txt"},
                                       canceled);
  QVERIFY(preparation.isValid());

  const git::WorkingTreeDiscardExecution execution =
      git::WorkingTreeDiscard::execute(preparation.plan, canceled);
  QVERIFY(execution.isValid());
  QVERIFY(!QFile::exists(repo.workdir().filePath("staged.txt")));
  QVERIFY(!QFile::exists(repo.workdir().filePath("untracked.txt")));
  QVERIFY(!git::WorkingTreeStatusSnapshot::scan(repositoryPath,
                                                git::WorkingTreeStatusOptions())
               .isDirty());
}

void TestWorkingTreeDiscard::rejectsChangedHeadBeforeExecution() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "initial\n"));
  QVERIFY(runGit(repositoryPath, {"add", "tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));
  const QString preparedHead = repo.head().target().id().toString();

  QVERIFY(writeFile(repo.workdir().filePath("planned.txt"), "remove\n"));
  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeDiscardPreparation preparation =
      git::WorkingTreeDiscard::prepare(repositoryPath, preparedHead, {},
                                       {"planned.txt"}, canceled);
  QVERIFY(preparation.isValid());

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "next\n"));
  QVERIFY(runGit(repositoryPath, {"add", "tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "next"}));

  const git::WorkingTreeDiscardExecution execution =
      git::WorkingTreeDiscard::execute(preparation.plan, canceled);
  QVERIFY(!execution.error.isEmpty());
  QVERIFY(QFile::exists(repo.workdir().filePath("planned.txt")));
}

void TestWorkingTreeDiscard::rejectsChangedTrackedStatusBeforeExecution() {
  Test::ScratchRepository scratch;
  git::Repository repo = scratch;
  const QString repositoryPath = repo.dir(false).path();

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "initial\n"));
  QVERIFY(runGit(repositoryPath, {"add", "tracked.txt"}));
  QVERIFY(runGit(repositoryPath, {"commit", "-m", "initial"}));

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "planned\n"));
  const QString headId = repo.head().target().id().toString();
  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeDiscardPreparation preparation =
      git::WorkingTreeDiscard::prepare(repositoryPath, headId, {"tracked.txt"},
                                       {}, canceled);
  QVERIFY(preparation.isValid());

  QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "keep\n"));
  const git::WorkingTreeDiscardExecution execution =
      git::WorkingTreeDiscard::execute(preparation.plan, canceled);
  QVERIFY(!execution.isValid());
  QVERIFY(!execution.error.isEmpty());

  QFile tracked(repo.workdir().filePath("tracked.txt"));
  QVERIFY(tracked.open(QIODevice::ReadOnly));
  QCOMPARE(tracked.readAll(), QByteArray("keep\n"));
}

void TestWorkingTreeDiscard::restoresSubmoduleHeads() {
  const QString repositoryPath =
      Test::extractRepository("DirtySubmoduleUnstagedTree.zip", true);
  QVERIFY(!repositoryPath.isEmpty());

  const git::Repository repo = git::Repository::open(repositoryPath);
  QVERIFY(repo.isValid());
  const QString headId = repo.head().target().id().toString();
  const auto canceled = std::make_shared<std::atomic_bool>(false);
  const git::WorkingTreeDiscardPreparation preparation =
      git::WorkingTreeDiscard::prepare(repositoryPath, headId, {}, {},
                                       canceled);
  QVERIFY(preparation.isValid());
  QVERIFY(preparation.plan.repositories.size() > 1);

  const git::WorkingTreeDiscardExecution execution =
      git::WorkingTreeDiscard::execute(preparation.plan, canceled);
  QVERIFY(execution.isValid());

  for (const git::WorkingTreeDiscardRepositoryPlan &repositoryPlan :
       preparation.plan.repositories) {
    if (repositoryPlan.targetId.isEmpty() ||
        repositoryPlan.targetId == repositoryPlan.headId)
      continue;

    const git::Repository child =
        git::Repository::open(repositoryPlan.repositoryPath);
    QVERIFY(child.isValid());
    QCOMPARE(child.head().target().id().toString(), repositoryPlan.targetId);
    QVERIFY(child.isHeadDetached());
  }
  QVERIFY(!git::WorkingTreeStatusSnapshot::scan(repositoryPath,
                                                git::WorkingTreeStatusOptions())
               .isDirty());
}

TEST_MAIN(TestWorkingTreeDiscard)

#include "WorkingTreeDiscard.moc"

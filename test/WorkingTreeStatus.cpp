//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
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
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}

git::WorkingTreeStatusSnapshot scan(const git::Repository &repo) {
  return git::WorkingTreeStatusSnapshot::scan(
      repo.dir(false).path(), git::WorkingTreeStatusOptions());
}

} // namespace

class TestWorkingTreeStatus : public QObject {
private slots:
  void cleanRepository() {
    Test::ScratchRepository scratch;
    git::Repository repo = scratch;

    git::WorkingTreeStatusSnapshot status = scan(repo);
    QVERIFY(status.isValid());
    QVERIFY(!status.isDirty());
    QVERIFY(!status.hasTrackedChanges());
    QVERIFY(status.untrackedPaths().isEmpty());
  }

  void untrackedFile() {
    Test::ScratchRepository scratch;
    git::Repository repo = scratch;
    QVERIFY(writeFile(repo.workdir().filePath("untracked.txt"), "untracked\n"));

    git::WorkingTreeStatusSnapshot status = scan(repo);
    QVERIFY(status.isValid());
    QVERIFY(status.isDirty());
    QVERIFY(!status.hasTrackedChanges());
    QCOMPARE(status.untrackedPaths(), QStringList({"untracked.txt"}));
  }

  void trackedChanges() {
    Test::ScratchRepository scratch;
    git::Repository repo = scratch;
    const QString path = repo.dir(false).path();
    QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "initial\n"));
    QVERIFY(runGit(path, {"add", "tracked.txt"}));
    QVERIFY(runGit(path, {"commit", "-m", "initial"}));

    QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "modified\n"));
    git::WorkingTreeStatusSnapshot modified = scan(repo);
    QVERIFY(modified.isValid());
    QVERIFY(modified.hasTrackedChanges());
    QCOMPARE(modified.entries().size(), 1);
    QVERIFY(modified.entries().constFirst().hasWorkdirChange());

    QVERIFY(runGit(path, {"add", "tracked.txt"}));
    git::WorkingTreeStatusSnapshot staged = scan(repo);
    QVERIFY(staged.isValid());
    QVERIFY(staged.hasTrackedChanges());
    QCOMPARE(staged.entries().size(), 1);
    QVERIFY(staged.entries().constFirst().hasIndexChange());
  }
};

TEST_MAIN(TestWorkingTreeStatus)

//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Remote.h"
#include "git/Result.h"
#include "git/Worktree.h"
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

QString canonicalPath(const QString &path) {
  return QFileInfo(path).canonicalFilePath();
}

git::Commit initialCommit(git::Repository &repo) {
  return repo.commit("initial");
}

} // namespace

class TestWorktree : public QObject {
  Q_OBJECT

private slots:
  void localWorktree();
  void remoteTrackingWorktree();
  void removeWorktree();
};

void TestWorktree::localWorktree() {
  QTemporaryDir sandbox;
  QVERIFY(sandbox.isValid());

  git::Repository repo =
      git::Repository::init(QDir(sandbox.path()).filePath("repo"));
  QVERIFY(repo.isValid());
  Test::initRepo(repo);
  git::Commit commit = initialCommit(repo);
  QVERIFY(commit.isValid());

  git::Branch feature = repo.createBranch("feature/with-slash", commit);
  QVERIFY(feature.isValid());
  QVERIFY(!feature.isCheckedOut());

  QString linkedPath = QDir(sandbox.path()).filePath("feature-tree");
  git::Result result;
  git::Repository linked = repo.createWorktree("feature-tree", linkedPath,
                                               feature, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(linked.isValid());
  QVERIFY(linked.isWorktree());
  QCOMPARE(canonicalPath(linked.dir(false).path()), canonicalPath(linkedPath));
  QCOMPARE(canonicalPath(linked.workdir().path()), canonicalPath(linkedPath));
  QCOMPARE(canonicalPath(linked.commonDir().path()),
           canonicalPath(repo.commonDir().path()));
  QCOMPARE(canonicalPath(linked.appDir().path()),
           canonicalPath(repo.appDir().path()));
  QVERIFY(feature.isCheckedOut());

  QList<git::Worktree> fromMain = repo.worktrees();
  QCOMPARE(fromMain.size(), 2);
  QVERIFY(fromMain.at(0).isMain());
  QVERIFY(fromMain.at(0).isCurrent());
  QCOMPARE(fromMain.at(0).name(), QString("Home"));
  QCOMPARE(fromMain.at(0).branch(), repo.head().name());
  QVERIFY(!fromMain.at(1).isMain());
  QVERIFY(!fromMain.at(1).isCurrent());
  QVERIFY(fromMain.at(1).isValid());
  QCOMPARE(fromMain.at(1).name(), QString("feature-tree"));
  QCOMPARE(fromMain.at(1).branch(), QString("feature/with-slash"));
  QCOMPARE(canonicalPath(fromMain.at(1).path()), canonicalPath(linkedPath));

  QList<git::Worktree> fromLinked = linked.worktrees();
  QCOMPARE(fromLinked.size(), 2);
  QVERIFY(!fromLinked.at(0).isCurrent());
  QVERIFY(fromLinked.at(1).isCurrent());

  git::Branch siblingBranch = repo.createBranch("sibling", commit);
  QVERIFY(siblingBranch.isValid());
  QString siblingPath = QDir(sandbox.path()).filePath("sibling-tree");
  git::Repository sibling = linked.createWorktree(
      "sibling-tree", siblingPath, siblingBranch, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(sibling.isValid());
  QCOMPARE(canonicalPath(sibling.commonDir().path()),
           canonicalPath(repo.commonDir().path()));

  QString duplicatePath = QDir(sandbox.path()).filePath("duplicate");
  git::Repository duplicate = repo.createWorktree("duplicate", duplicatePath,
                                                  feature, QString(), &result);
  QVERIFY(!duplicate.isValid());
  QVERIFY(!result);
  QVERIFY(!QFileInfo::exists(duplicatePath));

  QVERIFY(!repo.createBranch("feature/with-slash", commit, true).isValid());
  QVERIFY(!feature.rename("renamed-feature").isValid());
  feature.remove(true);
  QVERIFY(repo.lookupBranch("feature/with-slash", GIT_BRANCH_LOCAL).isValid());

  QVERIFY(repo.setHeadDetached(commit));
  QVERIFY(linked.setHeadDetached(commit));
  const QString detachedName = repo.head().name();
  QVERIFY(detachedName.startsWith("HEAD detached at "));
  QList<git::Worktree> detachedWorktrees = repo.worktrees();
  QCOMPARE(detachedWorktrees.first().branch(), detachedName);
  bool foundDetachedLinked = false;
  for (const git::Worktree &worktree : detachedWorktrees) {
    if (worktree.name() == QString("feature-tree")) {
      QCOMPARE(worktree.branch(), detachedName);
      foundDetachedLinked = true;
    }
  }
  QVERIFY(foundDetachedLinked);
}

void TestWorktree::remoteTrackingWorktree() {
  QTemporaryDir sandbox;
  QVERIFY(sandbox.isValid());

  git::Repository source =
      git::Repository::init(QDir(sandbox.path()).filePath("source"));
  QVERIFY(source.isValid());
  Test::initRepo(source);
  QVERIFY(initialCommit(source).isValid());
  QString sourceBranch = source.head().name();

  git::Repository repo =
      git::Repository::init(QDir(sandbox.path()).filePath("repo"));
  QVERIFY(repo.isValid());
  Test::initRepo(repo);
  QVERIFY(initialCommit(repo).isValid());

  git::Remote remote = repo.addRemote("origin", source.workdir().path());
  QVERIFY(remote.isValid());
  git::Remote::Callbacks callbacks(remote.url(), repo);
  git::Result fetch = remote.fetch(&callbacks);
  QVERIFY2(fetch, qPrintable(fetch.errorString()));

  git::Branch remoteBranch = repo.lookupBranch(
      QString("origin/%1").arg(sourceBranch), GIT_BRANCH_REMOTE);
  QVERIFY(remoteBranch.isValid());

  QVERIFY(QDir().mkpath(QDir(sandbox.path()).filePath("trees")));
  QString linkedPath = QDir(sandbox.path()).filePath("trees/topic-with-slash");
  git::Result result;
  git::Repository linked = repo.createWorktree(
      "remote-topic", linkedPath, remoteBranch, "topic/with-slash", &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(linked.isValid());
  QCOMPARE(linked.head().name(), QString("topic/with-slash"));

  git::Branch local = repo.lookupBranch("topic/with-slash", GIT_BRANCH_LOCAL);
  QVERIFY(local.isValid());
  QVERIFY(local.isCheckedOut());
  QVERIFY(local.upstream().isValid());
  QCOMPARE(local.upstream().qualifiedName(), remoteBranch.qualifiedName());

  QString rollbackPath = QDir(sandbox.path()).filePath("rollback");
  git::Repository failed = repo.createWorktree(
      "remote-topic", rollbackPath, remoteBranch, "rollback/topic", &result);
  QVERIFY(!failed.isValid());
  QVERIFY(!result);
  QVERIFY(!repo.lookupBranch("rollback/topic", GIT_BRANCH_LOCAL).isValid());
  QVERIFY(!QFileInfo::exists(rollbackPath));
  QCOMPARE(repo.worktrees().size(), 2);
  QVERIFY(linked.isValid());
}

void TestWorktree::removeWorktree() {
  QTemporaryDir sandbox;
  QVERIFY(sandbox.isValid());

  git::Repository repo =
      git::Repository::init(QDir(sandbox.path()).filePath("project"));
  QVERIFY(repo.isValid());
  Test::initRepo(repo);
  git::Commit commit = initialCommit(repo);
  QVERIFY(commit.isValid());

  git::Result statusResult;
  QVERIFY(!repo.hasWorkdirChanges(&statusResult));
  QVERIFY(statusResult);
  QVERIFY(!repo.removeWorktree(repo.worktrees().first()));

  const QString root = repo.workdir().path() + ".worktrees";
  QVERIFY(QDir().mkpath(root));
  git::Branch feature = repo.createBranch("feature", commit);
  QVERIFY(feature.isValid());
  const QString linkedPath = QDir(root).filePath("feature");
  git::Result result;
  git::Repository linked = repo.createWorktree(
      "feature", linkedPath, feature, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(linked.isValid());
  QVERIFY(!linked.hasWorkdirChanges(&statusResult));
  QVERIFY(statusResult);

  QFile untracked(QDir(linkedPath).filePath("untracked.txt"));
  QVERIFY(untracked.open(QIODevice::WriteOnly));
  QVERIFY(untracked.write("uncommitted\n") > 0);
  untracked.close();
  QVERIFY(linked.hasWorkdirChanges(&statusResult));
  QVERIFY(statusResult);

  git::Worktree worktree = repo.worktrees().last();
  result = repo.removeWorktree(worktree);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(!QFileInfo::exists(linkedPath));
  QVERIFY(!QFileInfo::exists(root));
  QCOMPARE(repo.worktrees().size(), 1);
  QVERIFY(repo.lookupBranch("feature", GIT_BRANCH_LOCAL).isValid());

  QVERIFY(QDir().mkpath(root));
  QFile sentinel(QDir(root).filePath("keep.txt"));
  QVERIFY(sentinel.open(QIODevice::WriteOnly));
  sentinel.close();
  git::Branch second = repo.createBranch("second", commit);
  QVERIFY(second.isValid());
  const QString secondPath = QDir(root).filePath("second");
  linked = repo.createWorktree("second", secondPath, second, QString(),
                               &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  worktree = repo.worktrees().last();
  result = repo.removeWorktree(worktree);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(!QFileInfo::exists(secondPath));
  QVERIFY(QFileInfo::exists(root));
  QVERIFY(QFileInfo::exists(sentinel.fileName()));
}

TEST_MAIN(TestWorktree)

#include "worktree.moc"

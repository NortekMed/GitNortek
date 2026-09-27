//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"

#include <QFile>

namespace {

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}

} // namespace

class TestRepositoryStash : public QObject {
  Q_OBJECT

private slots:
  void selectiveStash() {
    Test::ScratchRepository scratch;
    git::Repository repo = scratch;

    QVERIFY(writeFile(repo.workdir().filePath("selected.txt"), "initial\n"));
    QVERIFY(writeFile(repo.workdir().filePath("other.txt"), "initial\n"));
    QVERIFY(repo.index().setStaged({"selected.txt", "other.txt"}, true));
    QVERIFY(repo.commit("initial").isValid());

    QVERIFY(writeFile(repo.workdir().filePath("selected.txt"),
                      "selected change\n"));
    QVERIFY(writeFile(repo.workdir().filePath("other.txt"),
                      "other staged\n"));
    QVERIFY(repo.index().setStaged({"other.txt"}, true));
    QVERIFY(writeFile(repo.workdir().filePath("other.txt"),
                      "other change\n"));
    QVERIFY(writeFile(repo.workdir().filePath("selected-new.txt"),
                      "selected untracked\n"));
    QVERIFY(writeFile(repo.workdir().filePath("other-new.txt"),
                      "other untracked\n"));

    const git::Commit stash =
        repo.stash("selected files", true,
                   {"selected.txt", "selected-new.txt"});
    QVERIFY(stash.isValid());
    QCOMPARE(repo.stashes().size(), 1);

    QFile selected(repo.workdir().filePath("selected.txt"));
    QVERIFY(selected.open(QIODevice::ReadOnly));
    QCOMPARE(selected.readAll(), QByteArray("initial\n"));
    selected.close();
    QVERIFY(!QFile::exists(repo.workdir().filePath("selected-new.txt")));

    QFile other(repo.workdir().filePath("other.txt"));
    QVERIFY(other.open(QIODevice::ReadOnly));
    QCOMPARE(other.readAll(), QByteArray("other change\n"));
    other.close();
    QVERIFY(QFile::exists(repo.workdir().filePath("other-new.txt")));
    QCOMPARE(repo.index().isStaged("other.txt"), git::Index::PartiallyStaged);
    QVERIFY(!repo.index().isStaged("selected-new.txt"));

    // libgit2 requires a clean index before applying a stash. The selected
    // stash itself must still preserve the unrelated staged work.
    QVERIFY(repo.index().setStaged({"other.txt"}, false));
    QVERIFY(repo.applyStash(0));
    QVERIFY(selected.open(QIODevice::ReadOnly));
    QCOMPARE(selected.readAll(), QByteArray("selected change\n"));
    selected.close();
    QVERIFY(QFile::exists(repo.workdir().filePath("selected-new.txt")));
    QVERIFY(other.open(QIODevice::ReadOnly));
    QCOMPARE(other.readAll(), QByteArray("other change\n"));
    other.close();
    QVERIFY(QFile::exists(repo.workdir().filePath("other-new.txt")));

    QVERIFY(repo.dropStash(0));
  }
};

TEST_MAIN(TestRepositoryStash)

#include "RepositoryStash.moc"

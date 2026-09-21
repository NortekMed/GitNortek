//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "git/WorkingTreeStatus.h"
#include "ui/Badge.h"
#include "ui/DiffTreeModel.h"
#include "ui/TreeModel.h"
#include "ui/ViewDelegate.h"
#include <QHelpEvent>
#include <QFile>
#include <QProcess>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QToolTip>
#include <QTreeView>

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
  return git::WorkingTreeStatusSnapshot::scan(repo.dir(false).path(),
                                              git::WorkingTreeStatusOptions());
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

  void stopTrackedStatusUsesIgnoredLabel() {
    Test::ScratchRepository scratch;
    git::Repository repo = scratch;
    const QString path = repo.dir(false).path();
    QVERIFY(writeFile(repo.workdir().filePath("tracked.txt"), "tracked\n"));
    QVERIFY(runGit(path, {"add", "tracked.txt"}));
    QVERIFY(runGit(path, {"commit", "-m", "initial"}));
    QVERIFY(runGit(path, {"rm", "--cached", "tracked.txt"}));
    QVERIFY(writeFile(repo.workdir().filePath(".gitignore"), "/tracked.txt\n"));

    const git::WorkingTreeStatusSnapshot status = scan(repo);
    QVERIFY(status.isValid());

    DiffTreeModel model(repo);
    model.setStatusSnapshot(status);
    const QModelIndex index = model.index("tracked.txt");
    QVERIFY(index.isValid());
    QCOMPARE(model.data(index, DiffTreeModel::StatusRole).toString(),
             QStringLiteral("D"));
    QCOMPARE(model.data(index, Qt::ToolTipRole).toString(),
             repo.workdir().filePath("tracked.txt"));

    model.setIgnoredPaths({"tracked.txt"});
    model.setStatusSnapshot(status);
    const QModelIndex ignoredIndex = model.index("tracked.txt");
    QVERIFY(ignoredIndex.isValid());
    QCOMPARE(model.data(ignoredIndex, DiffTreeModel::StatusRole).toString(),
             QStringLiteral("I"));
  }

  void statusBadgeTooltipsPreserveFilePathTooltip() {
    const QList<QChar> statuses = {'I', 'D', 'M', 'A', '?',
                                   'R', '!', 'C', 'T', 'U'};
    for (const QChar status : statuses)
      QVERIFY(!Badge::statusTooltip(status).isEmpty());

    QStandardItemModel model;
    auto *item = new QStandardItem("tracked.txt");
    item->setData("M", TreeModel::StatusRole);
    item->setData("/repo/tracked.txt", Qt::ToolTipRole);
    model.appendRow(item);

    QTreeView view;
    view.setModel(&model);
    ViewDelegate delegate(&view);
    QStyleOptionViewItem option;
    option.initFrom(&view);
    option.rect = QRect(0, 0, 120, 24);
    option.font = view.font();
    const QModelIndex index = model.index(0, 0);

    QToolTip::hideText();
    QHelpEvent badgeEvent(QEvent::ToolTip, QPoint(110, 12), QPoint(110, 12));
    QVERIFY(delegate.helpEvent(&badgeEvent, &view, option, index));
    QCOMPARE(QToolTip::text(), Badge::statusTooltip('M'));

    QToolTip::hideText();
    QHelpEvent fileEvent(QEvent::ToolTip, QPoint(5, 12), QPoint(5, 12));
    QVERIFY(delegate.helpEvent(&fileEvent, &view, option, index));
    QCOMPARE(QToolTip::text(), QStringLiteral("/repo/tracked.txt"));
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

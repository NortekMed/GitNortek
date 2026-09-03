//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "conf/LocalWorkspace.h"
#include "conf/LocalWorkspaces.h"
#include "dialogs/DirectorySelectionDialog.h"
#include "dialogs/LocalWorkspaceDialog.h"
#include "git/Reference.h"
#include "ui/LocalRepositoryManagement.h"
#include "ui/LocalWorkspaceModel.h"
#include <QAbstractItemModelTester>
#include <QAbstractButton>
#include <QCryptographicHash>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHelpEvent>
#include <QIcon>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QThreadPool>
#include <QToolTip>
#include <QTreeView>

namespace {

void moveMouseTo(QTreeView *tree, const QModelIndex &index) {
  QWidget *viewport = tree->viewport();
  const QPoint position = tree->visualRect(index).center();
  QMouseEvent event(QEvent::MouseMove, position,
                    viewport->mapToGlobal(position), Qt::NoButton,
                    Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &event);
}

bool runGit(const QString &path, const QStringList &arguments) {
  QProcess process;
  QStringList command = {QStringLiteral("-C"), path};
  command.append(arguments);
  process.start(QStringLiteral("git"), command);
  return process.waitForFinished() && process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QString originCacheKey(const QString &path) {
  QString normalized = QDir(path).canonicalPath();
  if (normalized.isEmpty())
    normalized = QDir(path).absolutePath();
  const QByteArray hash =
      QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256)
          .toHex();
  return QStringLiteral("localRepositoryManagement/originSuccess/%1")
      .arg(QString::fromLatin1(hash));
}

QString originFailureKey(const QString &path) {
  QString key = originCacheKey(path);
  return key.replace(QStringLiteral("originSuccess"),
                     QStringLiteral("originFailure"));
}

QModelIndex workspaceIndex(QTreeView *tree, const QString &id) {
  for (int row = 0; row < tree->model()->rowCount(); ++row) {
    const QModelIndex index = tree->model()->index(row, 0);
    if (index.data(LocalWorkspaceModel::WorkspaceIdRole).toString() == id)
      return index;
  }
  return {};
}

} // namespace

class TestLocalWorkspaces : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void persistenceAndModel();
  void synchronizedDirectory();
  void manualRepositorySurvivesSynchronization();
  void addMultipleRepositories();
  void directorySelectionDialog();
  void readmeDetails();
  void managementInteraction();
  void managementRefreshesStaleOriginsWhileOpen();
  void managementChecksIndividualOriginFromContextMenu();
  void managementStartsOriginBatchForAllEligibleRepositories();
  void managementPreservesWorkspaceExpansion();
  void openWorkspaceConfirmation();
  void repositoryStatus();
  void cleanupTestCase();

private:
  void clearWorkspaces();

  QVariant mStoredWorkspaces;
  QVariantMap mStoredManagementSettings;
  bool mHadStoredWorkspaces = false;
};

void TestLocalWorkspaces::initTestCase() {
  QSettings settings;
  mHadStoredWorkspaces = settings.contains("localWorkspaces");
  mStoredWorkspaces = settings.value("localWorkspaces");
  settings.remove("localWorkspaces");
  settings.beginGroup("localRepositoryManagement");
  for (const QString &key : settings.allKeys())
    mStoredManagementSettings.insert(key, settings.value(key));
  settings.remove(QString());
  settings.endGroup();
}

void TestLocalWorkspaces::persistenceAndModel() {
  clearWorkspaces();
  Test::ScratchRepository repository;
  const git::Repository repo = repository;

  LocalWorkspace workspace;
  workspace.name = "Development";
  workspace.description = "Local projects";
  workspace.color = QColor("#336699");
  workspace.repositories.append(repo.dir(false).path());

  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  QSignalSpy changed(workspaces, &LocalWorkspaces::workspacesChanged);
  QString error;
  QVERIFY2(workspaces->add(workspace, &error), qPrintable(error));
  QCOMPARE(changed.count(), 1);
  QCOMPARE(workspaces->count(), 1);
  QVERIFY(QSettings().contains("localWorkspaces"));

  LocalWorkspace duplicate = workspace;
  duplicate.id = "duplicate";
  duplicate.name = "development";
  QVERIFY(!workspaces->add(duplicate, &error));

  LocalWorkspaceModel model;
  QAbstractItemModelTester tester(
      &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
  QCOMPARE(model.columnCount(), LocalWorkspaceModel::ColumnCount);
  QCOMPARE(model.headerData(LocalWorkspaceModel::RepositoryColumn,
                            Qt::Horizontal),
           QString("Repository"));
  QCOMPARE(model.headerData(LocalWorkspaceModel::BranchColumn, Qt::Horizontal),
           QString("Branch"));
  QCOMPARE(model.rowCount(), 1);
  const QModelIndex workspaceIndex = model.index(0, 0);
  QCOMPARE(model.rowCount(workspaceIndex), 1);
  const QModelIndex repositoryIndex = model.index(0, 0, workspaceIndex);
  QCOMPARE(model.parent(repositoryIndex), workspaceIndex);
  QCOMPARE(repositoryIndex.data(LocalWorkspaceModel::PathRole).toString(),
           repo.dir(false).path());
  const QModelIndex branchIndex =
      model.index(0, LocalWorkspaceModel::BranchColumn, workspaceIndex);
  QCOMPARE(branchIndex.data().toString(), repo.unbornHeadName());
  QCOMPARE(branchIndex.data(Qt::TextAlignmentRole).toInt(),
           int(Qt::AlignLeft | Qt::AlignVCenter));
  QVERIFY(!branchIndex.data(Qt::DecorationRole).value<QIcon>().isNull());
  const QModelIndex detailsIndex =
      model.index(0, LocalWorkspaceModel::DetailsColumn, workspaceIndex);
  QCOMPARE(detailsIndex.data(Qt::ToolTipRole).toString(),
           QString("show details"));
}

void TestLocalWorkspaces::synchronizedDirectory() {
  clearWorkspaces();
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QDir directory(root.path());
  QVERIFY(directory.mkdir("direct"));
  QVERIFY(directory.mkpath("outer/nested"));
  git::Repository direct = git::Repository::init(directory.filePath("direct"));
  git::Repository nested =
      git::Repository::init(directory.filePath("outer/nested"));
  QVERIFY(direct.isValid());
  QVERIFY(nested.isValid());

  LocalWorkspace workspace;
  workspace.name = "Synchronized";
  workspace.syncDirectory = root.path();
  workspace.syncEnabled = true;
  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  QString error;
  QVERIFY2(workspaces->add(workspace, &error), qPrintable(error));
  QVERIFY2(workspaces->rescanSynchronizedDirectory(workspace.id, &error),
           qPrintable(error));

  const LocalWorkspace *stored = workspaces->workspace(workspace.id);
  QVERIFY(stored);
  QCOMPARE(stored->repositories, QStringList({direct.dir(false).path()}));
  QCOMPARE(stored->synchronizedRepositories, stored->repositories);
  QVERIFY(stored->manualRepositories.isEmpty());
  QVERIFY(!stored->repositories.contains(nested.dir(false).path()));

  QStringList invalid;
  QStringList duplicates;
  QVERIFY2(workspaces->addRepositories(workspace.id, {direct.dir(false).path()},
                                       &invalid, &duplicates, &error),
           qPrintable(error));
  QVERIFY(invalid.isEmpty());
  QVERIFY(duplicates.isEmpty());
  QCOMPARE(workspaces->workspace(workspace.id)->manualRepositories,
           QStringList({direct.dir(false).path()}));

  LocalWorkspaceModel model;
  const QModelIndex workspaceIndex = model.index(0, 0);
  const QModelIndex removeIndex =
      model.index(0, LocalWorkspaceModel::RemoveColumn, workspaceIndex);
  QVERIFY(removeIndex.data(LocalWorkspaceModel::SynchronizedRole).toBool());
  QVERIFY(!removeIndex.flags().testFlag(Qt::ItemIsEnabled));

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(tree);
  const QModelIndex proxyWorkspace = tree->model()->index(0, 0);
  tree->setExpanded(proxyWorkspace, true);
  const QModelIndex proxyRemove = tree->model()->index(
      0, LocalWorkspaceModel::RemoveColumn, proxyWorkspace);
  QTRY_VERIFY(!tree->visualRect(proxyRemove).isEmpty());
  moveMouseTo(tree, proxyRemove);
  QVERIFY(tree->viewport()->cursor().shape() != Qt::PointingHandCursor);

  QVERIFY(directory.mkdir("created-later"));
  git::Repository createdLater =
      git::Repository::init(directory.filePath("created-later"));
  QVERIFY(createdLater.isValid());
  const QString createdLaterPath = createdLater.dir(false).path();
  QTRY_VERIFY_WITH_TIMEOUT(
      workspaces->workspace(workspace.id)->repositories.contains(
          createdLaterPath),
      3000);
}

void TestLocalWorkspaces::manualRepositorySurvivesSynchronization() {
  clearWorkspaces();
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QDir directory(root.path());
  QVERIFY(directory.mkdir("repository"));
  git::Repository repository =
      git::Repository::init(directory.filePath("repository"));
  QVERIFY(repository.isValid());
  const QString repositoryPath = repository.dir(false).path();

  LocalWorkspace workspace;
  workspace.name = "Manual and synchronized";
  workspace.repositories.append(repositoryPath);
  workspace.syncDirectory = root.path();
  workspace.syncEnabled = true;

  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  QString error;
  QVERIFY2(workspaces->add(workspace, &error), qPrintable(error));
  const LocalWorkspace *stored = workspaces->workspace(workspace.id);
  QVERIFY(stored);
  QCOMPARE(stored->repositories, QStringList({repositoryPath}));
  QCOMPARE(stored->manualRepositories, QStringList({repositoryPath}));
  QCOMPARE(stored->synchronizedRepositories, QStringList({repositoryPath}));
  LocalWorkspaceDialog dialog(*stored);
  QCOMPARE(dialog.workspace().manualRepositories,
           QStringList({repositoryPath}));
  QCheckBox *sync = dialog.findChild<QCheckBox *>(
      "LocalWorkspaceSyncEnabled");
  QVERIFY(sync);
  QVERIFY(sync->isChecked());
  sync->setChecked(false);

  LocalWorkspace updated = dialog.workspace();
  QVERIFY(!updated.syncEnabled);
  QCOMPARE(updated.syncDirectory, root.path());
  QVERIFY2(workspaces->update(updated, &error), qPrintable(error));
  stored = workspaces->workspace(workspace.id);
  QVERIFY(stored);
  QVERIFY(!stored->syncEnabled);
  QCOMPARE(stored->syncDirectory, root.path());
  QCOMPARE(stored->repositories, QStringList({repositoryPath}));
  QCOMPARE(stored->manualRepositories, QStringList({repositoryPath}));
  QCOMPARE(stored->synchronizedRepositories, QStringList({repositoryPath}));
  const QVariantList persisted = QSettings().value("localWorkspaces").toList();
  QCOMPARE(persisted.size(), 1);
  const QVariantMap persistedWorkspace = persisted.first().toMap();
  QCOMPARE(persistedWorkspace.value("syncEnabled").toBool(), false);
  QCOMPARE(persistedWorkspace.value("syncDirectory").toString(), root.path());
  QCOMPARE(persistedWorkspace.value("synchronizedRepositories").toStringList(),
           QStringList({repositoryPath}));

  QVERIFY(directory.mkdir("created-while-paused"));
  git::Repository createdWhilePaused =
      git::Repository::init(directory.filePath("created-while-paused"));
  QVERIFY(createdWhilePaused.isValid());
  QTest::qWait(700);
  QVERIFY(!workspaces->workspace(workspace.id)->repositories.contains(
      createdWhilePaused.dir(false).path()));

  updated = *workspaces->workspace(workspace.id);
  updated.syncEnabled = true;
  QVERIFY2(workspaces->update(updated, &error), qPrintable(error));
  stored = workspaces->workspace(workspace.id);
  QVERIFY(stored->syncEnabled);
  QVERIFY(stored->repositories.contains(createdWhilePaused.dir(false).path()));
}

void TestLocalWorkspaces::addMultipleRepositories() {
  clearWorkspaces();
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QDir directory(root.path());
  QVERIFY(directory.mkdir("first"));
  QVERIFY(directory.mkdir("second"));
  QVERIFY(directory.mkdir("invalid"));
  const git::Repository first =
      git::Repository::init(directory.filePath("first"));
  const git::Repository second =
      git::Repository::init(directory.filePath("second"));
  QVERIFY(first.isValid());
  QVERIFY(second.isValid());

  LocalWorkspace workspace;
  workspace.name = "Multiple repositories";
  workspace.repositories.append(first.dir(false).path());
  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  QString error;
  QVERIFY2(workspaces->add(workspace, &error), qPrintable(error));

  QSignalSpy changed(workspaces, &LocalWorkspaces::workspacesChanged);
  QStringList invalid;
  QStringList duplicates;
  QVERIFY2(workspaces->addRepositories(
               workspace.id,
               {first.dir(false).path(), second.dir(false).path(),
                directory.filePath("invalid"), second.dir(false).path()},
               &invalid, &duplicates, &error),
           qPrintable(error));
  QCOMPARE(changed.count(), 1);
  QCOMPARE(invalid, QStringList({directory.filePath("invalid")}));
  QCOMPARE(duplicates,
           QStringList({first.dir(false).path(), second.dir(false).path()}));
  QCOMPARE(workspaces->workspace(workspace.id)->repositories,
           QStringList({first.dir(false).path(), second.dir(false).path()}));
}

void TestLocalWorkspaces::directorySelectionDialog() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  QDir directory(root.path());
  QVERIFY(directory.mkdir("first"));
  QVERIFY(directory.mkdir("second"));

  DirectorySelectionDialog dialog("Select directories");
  QTreeView *tree = dialog.findChild<QTreeView *>("DirectorySelectionTree");
  QVERIFY(tree);
  QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
  QFileSystemModel *model = qobject_cast<QFileSystemModel *>(tree->model());
  QVERIFY(model);
  const QModelIndex rootIndex = model->setRootPath(root.path());
  tree->setRootIndex(rootIndex);
  QTRY_VERIFY(model->rowCount(rootIndex) >= 2);

  const QModelIndex first = model->index(directory.filePath("first"));
  const QModelIndex second = model->index(directory.filePath("second"));
  QVERIFY(first.isValid());
  QVERIFY(second.isValid());
  tree->selectionModel()->select(
      first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  tree->selectionModel()->select(
      second, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  QStringList selected = dialog.selectedDirectories();
  selected.sort();
  QStringList expected = {directory.filePath("first"),
                          directory.filePath("second")};
  expected.sort();
  QCOMPARE(selected, expected);

  QLineEdit *location = dialog.findChild<QLineEdit *>(
      "DirectorySelectionLocation");
  QPushButton *go = dialog.findChild<QPushButton *>("DirectorySelectionGo");
  QPushButton *select = dialog.findChild<QPushButton *>(
      "DirectorySelectionAccept");
  QVERIFY(location);
  QVERIFY(go);
  QVERIFY(select);
  tree->selectionModel()->clearSelection();
  location->setText(QDir::toNativeSeparators(directory.filePath("first")));
  QTRY_VERIFY(select->isEnabled());
  QCOMPARE(dialog.selectedDirectories(),
           QStringList({directory.filePath("first")}));
  location->setText(QDir::toNativeSeparators(directory.filePath("second")));
  QTest::mouseClick(go, Qt::LeftButton);
  QTRY_COMPARE(dialog.selectedDirectories(),
               QStringList({directory.filePath("second")}));
}

void TestLocalWorkspaces::readmeDetails() {
  clearWorkspaces();
  Test::ScratchRepository repository;
  const git::Repository repo = repository;
  const QString root = repo.dir(false).path();
  QFile readme(QDir(root).filePath("README.md"));
  QVERIFY(readme.open(QIODevice::WriteOnly));
  QVERIFY(readme.write("# Project Details\n\nWorking tree content.") > 0);
  readme.close();

  LocalWorkspace workspace;
  workspace.name = "README";
  workspace.repositories.append(root);
  QString error;
  QVERIFY2(LocalWorkspaces::instance()->add(workspace, &error),
           qPrintable(error));

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QTextBrowser *browser = management.findChild<QTextBrowser *>(
      "LocalRepositoryManagementReadme");
  QSplitter *splitter = management.findChild<QSplitter *>(
      "LocalRepositoryManagementSplitter");
  QWidget *details = management.findChild<QWidget *>(
      "LocalRepositoryManagementDetails");
  QVERIFY(tree);
  QVERIFY(browser);
  QVERIFY(splitter);
  QVERIFY(details);
  QVERIFY(!details->isVisible());
  QCOMPARE(tree->header()->sectionResizeMode(
               LocalWorkspaceModel::RepositoryColumn),
            QHeaderView::Stretch);
  QCOMPARE(tree->header()->sectionResizeMode(LocalWorkspaceModel::BranchColumn),
            QHeaderView::ResizeToContents);

  const QModelIndex workspaceIndex = tree->model()->index(0, 0);
  tree->setExpanded(workspaceIndex, true);
  const QModelIndex branchIndex = tree->model()->index(
      0, LocalWorkspaceModel::BranchColumn, workspaceIndex);
  QTRY_VERIFY(!branchIndex.data().toString().isEmpty());
  QVERIFY(tree->columnWidth(LocalWorkspaceModel::BranchColumn) >=
          tree->fontMetrics().horizontalAdvance(branchIndex.data().toString()));
  const QModelIndex repositoryIndex = tree->model()->index(
      0, LocalWorkspaceModel::RepositoryColumn, workspaceIndex);
  const QModelIndex detailsIndex = tree->model()->index(
      0, LocalWorkspaceModel::DetailsColumn, workspaceIndex);
  QVERIFY(detailsIndex.isValid());
  QTRY_VERIFY(!tree->visualRect(detailsIndex).isEmpty());
  moveMouseTo(tree, detailsIndex);
  QTRY_COMPARE(tree->viewport()->cursor().shape(), Qt::PointingHandCursor);
  QCOMPARE(tree->indexAt(tree->visualRect(repositoryIndex).center()),
           repositoryIndex);
  moveMouseTo(tree, repositoryIndex);
  QTRY_VERIFY(tree->viewport()->cursor().shape() != Qt::PointingHandCursor);
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    tree->visualRect(detailsIndex).center());
  QTRY_VERIFY(details->isVisible());
  QVERIFY(browser->toPlainText().contains("Project Details"));
  QVERIFY(browser->toPlainText().contains("Working tree content."));
  QCOMPARE(splitter->sizes().size(), 2);
  QTRY_VERIFY(qAbs(splitter->sizes().at(0) - splitter->sizes().at(1)) < 20);

  QVERIFY(QFile::remove(readme.fileName()));
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    tree->visualRect(detailsIndex).center());
  QTRY_VERIFY(browser->toPlainText().contains("no README.md"));
}

void TestLocalWorkspaces::managementInteraction() {
  clearWorkspaces();
  Test::ScratchRepository repository;
  const git::Repository repo = repository;
  const QString root = repo.dir(false).path();
  QVERIFY(writeFile(QDir(root).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(root, {"add", "tracked.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "tracked"}));
  const QString branch = repo.head().name();
  const QString upstream = QString("origin/%1").arg(branch);
  QVERIFY(runGit(root, {"remote", "add", "origin", root}));
  QVERIFY(runGit(root,
                 {"update-ref", QString("refs/remotes/%1").arg(upstream),
                  "HEAD"}));
  QVERIFY(runGit(root, {"branch", "--set-upstream-to", upstream, branch}));

  LocalWorkspace workspace;
  workspace.name = "Interaction";
  workspace.repositories.append(root);
  QString error;
  QVERIFY2(LocalWorkspaces::instance()->add(workspace, &error),
           qPrintable(error));

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QPushButton *check = management.findChild<QPushButton *>(
      "LocalRepositoryManagementCheckOrigin");
  QPushButton *expansion = management.findChild<QPushButton *>(
      "LocalRepositoryManagementExpansionToggle");
  QTimer *animation = management.findChild<QTimer *>(
      "LocalRepositoryManagementOriginAnimation");
  QVERIFY(tree);
  QVERIFY(check);
  QVERIFY(expansion);
  QVERIFY(animation);
  const QModelIndex workspaceIndex = tree->model()->index(0, 0);
  QTRY_VERIFY(!tree->visualRect(workspaceIndex).isEmpty());
  QVERIFY(!tree->isExpanded(workspaceIndex));
  QCOMPARE(expansion->text(), QString("Expand"));
  const QPoint workspacePosition =
      tree->visualRect(workspaceIndex).center();
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    workspacePosition);
  QVERIFY(tree->isExpanded(workspaceIndex));
  QCOMPARE(expansion->text(), QString("Collapse"));
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    workspacePosition);
  QVERIFY(!tree->isExpanded(workspaceIndex));
  QCOMPARE(expansion->text(), QString("Expand"));

  const QPoint disclosurePosition(
      tree->visualRect(workspaceIndex).left() - tree->indentation() / 2,
      tree->visualRect(workspaceIndex).center().y());
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    disclosurePosition);
  QVERIFY(tree->isExpanded(workspaceIndex));
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    disclosurePosition);
  QVERIFY(!tree->isExpanded(workspaceIndex));

  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    workspacePosition);
  QVERIFY(tree->isExpanded(workspaceIndex));
  bool editOpened = false;
  QTimer::singleShot(50, [&] {
    LocalWorkspaceDialog *dialog =
        qobject_cast<LocalWorkspaceDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    editOpened = true;
    dialog->reject();
  });
  QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                     workspacePosition);
  QTRY_VERIFY(editOpened);
  QVERIFY(!tree->isExpanded(workspaceIndex));
  QTest::mouseClick(expansion, Qt::LeftButton);
  QTRY_VERIFY(tree->isExpanded(workspaceIndex));
  QCOMPARE(expansion->text(), QString("Collapse"));

  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                    workspacePosition);
  QVERIFY(!tree->isExpanded(workspaceIndex));
  editOpened = false;
  QTimer::singleShot(50, [&] {
    LocalWorkspaceDialog *dialog =
        qobject_cast<LocalWorkspaceDialog *>(QApplication::activeModalWidget());
    if (!dialog)
      return;
    editOpened = true;
    dialog->reject();
  });
  QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                     workspacePosition);
  QTRY_VERIFY(editOpened);
  QVERIFY(tree->isExpanded(workspaceIndex));
  QTest::mouseClick(expansion, Qt::LeftButton);
  QTRY_VERIFY(!tree->isExpanded(workspaceIndex));
  QCOMPARE(expansion->text(), QString("Expand"));

  const QModelIndex remoteIndex = tree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, workspaceIndex);
  const QModelIndex changesIndex = tree->model()->index(
      0, LocalWorkspaceModel::ChangesColumn, workspaceIndex);
  QCOMPARE(remoteIndex.data(Qt::TextAlignmentRole).toInt(),
           int(Qt::AlignLeft | Qt::AlignVCenter));
  QCOMPARE(changesIndex.data(Qt::TextAlignmentRole).toInt(),
           int(Qt::AlignLeft | Qt::AlignVCenter));
  QSignalSpy selectedRepositoryChanged(
      &management, &LocalRepositoryManagement::selectedRepositoryChanged);
  tree->setCurrentIndex(remoteIndex);
  QCOMPARE(management.selectedRepositoryPath(), root);
  QCOMPARE(selectedRepositoryChanged.count(), 1);
  tree->setCurrentIndex(workspaceIndex);
  QVERIFY(management.selectedRepositoryPath().isEmpty());
  QCOMPARE(selectedRepositoryChanged.count(), 2);
  tree->setCurrentIndex(remoteIndex);
  QCOMPARE(management.selectedRepositoryPath(), root);
  QTRY_VERIFY(
      remoteIndex.data(LocalWorkspaceModel::OriginCheckEligibleRole).toBool());
  QVERIFY(!remoteIndex.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
  QCOMPARE(remoteIndex.data(Qt::ToolTipRole).toString(),
           QString("Waiting for origin check."));

  QSignalSpy started(&management,
                     &LocalRepositoryManagement::originCheckStarted);
  QSignalSpy finished(&management,
                      &LocalRepositoryManagement::originCheckFinished);
  QSignalSpy fetchStarted(&management,
                          &LocalRepositoryManagement::originFetchStarted);
  QSignalSpy fetchFinished(&management,
                           &LocalRepositoryManagement::originFetchFinished);
  bool activeStateObserved = false;
  bool inactiveStateObserved = false;
  connect(&management, &LocalRepositoryManagement::originFetchStarted,
          [&] {
            activeStateObserved =
                remoteIndex
                    .data(LocalWorkspaceModel::OriginFetchActiveRole)
                    .toBool() &&
                remoteIndex.data(Qt::ToolTipRole).toString() ==
                    QString("Synchronization is running.") &&
                animation->isActive();
          });
  connect(&management, &LocalRepositoryManagement::originFetchFinished,
          [&] {
            inactiveStateObserved =
                !remoteIndex
                     .data(LocalWorkspaceModel::OriginFetchActiveRole)
                     .toBool();
          });
  QVERIFY(runGit(root, {"remote", "set-url", "origin",
                        QDir(root).filePath("missing-origin")}));
  bool eventLoopAdvanced = false;
  QTimer::singleShot(0, &management,
                     [&] { eventLoopAdvanced = true; });
  QTest::mouseClick(check, Qt::LeftButton);
  QCOMPARE(started.count(), 1);
  QVERIFY(!eventLoopAdvanced);
  QTRY_VERIFY(eventLoopAdvanced);
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(fetchStarted.count(), 1);
  QCOMPARE(fetchFinished.count(), 1);
  QVERIFY(activeStateObserved);
  QVERIFY(inactiveStateObserved);
  QVERIFY(!animation->isActive());
  QVERIFY(!remoteIndex.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
  QVERIFY(remoteIndex.data(LocalWorkspaceModel::OriginCheckFailedRole).toBool());
  QCOMPARE(remoteIndex.data(Qt::ToolTipRole).toString(),
           QString("The last origin check failed."));
  QCOMPARE(finished.first().at(0).toInt(), 0);
  QCOMPARE(finished.first().at(1).toInt(), 1);
  QVERIFY(!check->isEnabled());
  QVERIFY(check->text().startsWith("Check origin ("));
  QTest::mouseClick(check, Qt::LeftButton);
  QCOMPARE(started.count(), 1);
  QVERIFY(QSettings().contains(originFailureKey(root)));

  LocalRepositoryManagement failedStateManagement;
  QTreeView *failedTree = failedStateManagement.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(failedTree);
  const QModelIndex failedWorkspace = failedTree->model()->index(0, 0);
  const QModelIndex failedRemote = failedTree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, failedWorkspace);
  QVERIFY(failedRemote.data(LocalWorkspaceModel::OriginCheckFailedRole).toBool());

  QVERIFY(runGit(root, {"remote", "set-url", "origin", root}));
  QSettings settings;
  settings.setValue("localRepositoryManagement/originLastAttempt",
                    QDateTime::currentDateTimeUtc().addSecs(-121));
  LocalRepositoryManagement successManagement;
  QPushButton *successCheck = successManagement.findChild<QPushButton *>(
      "LocalRepositoryManagementCheckOrigin");
  QTreeView *successTree = successManagement.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(successCheck);
  QVERIFY(successTree);
  const QModelIndex successWorkspace = successTree->model()->index(0, 0);
  const QModelIndex successRemote = successTree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, successWorkspace);
  QSignalSpy successFinished(
      &successManagement, &LocalRepositoryManagement::originCheckFinished);
  QTest::mouseClick(successCheck, Qt::LeftButton);
  QTRY_COMPARE(successFinished.count(), 1);
  QCOMPARE(successFinished.first().at(0).toInt(), 1);
  QCOMPARE(successFinished.first().at(1).toInt(), 0);
  QVERIFY(
      successRemote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
  QVERIFY(!successRemote.data(LocalWorkspaceModel::OriginCheckFailedRole)
               .toBool());
  QVERIFY(!settings.contains(originFailureKey(root)));

  settings.setValue("localRepositoryManagement/originLastAttempt",
                    QDateTime::currentDateTimeUtc().addSecs(-121));
  LocalRepositoryManagement freshManagement;
  QTreeView *freshTree = freshManagement.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(freshTree);
  const QModelIndex freshWorkspace = freshTree->model()->index(0, 0);
  const QModelIndex freshRemote = freshTree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, freshWorkspace);
  QSignalSpy freshStarted(
      &freshManagement, &LocalRepositoryManagement::originCheckStarted);
  freshManagement.checkOriginsIfStale();
  QVERIFY(freshRemote.data(LocalWorkspaceModel::OriginInitialPendingRole)
              .toBool());
  QCOMPARE(freshRemote.data(Qt::ToolTipRole).toString(),
           QString("Waiting for origin check."));
  freshManagement.show();
  QTRY_VERIFY(!freshRemote.data(LocalWorkspaceModel::OriginInitialPendingRole)
                   .toBool());
  QVERIFY(freshRemote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
  QTest::qWait(50);
  QCOMPARE(freshStarted.count(), 0);

  settings.setValue(originCacheKey(root),
                    QDateTime::currentDateTimeUtc().addSecs(-301));
  LocalRepositoryManagement staleManagement;
  QTreeView *staleTree = staleManagement.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(staleTree);
  const QModelIndex staleWorkspace = staleTree->model()->index(0, 0);
  const QModelIndex staleRemote = staleTree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, staleWorkspace);
  QVERIFY(!staleRemote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
  QTRY_VERIFY(
      staleRemote.data(LocalWorkspaceModel::OriginCheckEligibleRole).toBool());
  QCOMPARE(staleRemote.data(Qt::ToolTipRole).toString(),
           QString("Waiting for origin check."));
  staleManagement.show();
  QSignalSpy staleStarted(
      &staleManagement, &LocalRepositoryManagement::originCheckStarted);
  QSignalSpy staleFinished(
      &staleManagement, &LocalRepositoryManagement::originCheckFinished);
  staleManagement.checkOriginsIfStale();
  QVERIFY(staleRemote.data(LocalWorkspaceModel::OriginInitialPendingRole)
              .toBool());
  QTRY_COMPARE(staleStarted.count(), 1);
  QTRY_COMPARE(staleFinished.count(), 1);
}

void TestLocalWorkspaces::managementRefreshesStaleOriginsWhileOpen() {
  clearWorkspaces();
  Test::ScratchRepository repository;
  const git::Repository repo = repository;
  const QString root = repo.dir(false).path();
  QVERIFY(writeFile(QDir(root).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(root, {"add", "tracked.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "tracked"}));
  const QString branch = repo.head().name();
  const QString upstream = QString("origin/%1").arg(branch);
  QVERIFY(runGit(root, {"remote", "add", "origin", root}));
  QVERIFY(runGit(root,
                 {"update-ref", QString("refs/remotes/%1").arg(upstream),
                  "HEAD"}));
  QVERIFY(runGit(root, {"branch", "--set-upstream-to", upstream, branch}));

  LocalWorkspace workspace;
  workspace.name = "Automatic origin refresh";
  workspace.repositories.append(root);
  QString error;
  QVERIFY2(LocalWorkspaces::instance()->add(workspace, &error),
           qPrintable(error));

  QSettings settings;
  settings.setValue(originCacheKey(root), QDateTime::currentDateTimeUtc());
  settings.setValue("localRepositoryManagement/originLastAttempt",
                    QDateTime::currentDateTimeUtc().addSecs(-121));

  LocalRepositoryManagement management;
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QTimer *refresh = management.findChild<QTimer *>(
      "LocalRepositoryManagementRefreshTimer");
  QVERIFY(tree);
  QVERIFY(refresh);
  refresh->stop();
  const QModelIndex workspaceIndex = tree->model()->index(0, 0);
  const QModelIndex remote = tree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, workspaceIndex);
  QSignalSpy started(&management,
                     &LocalRepositoryManagement::originCheckStarted);
  QSignalSpy finished(&management,
                      &LocalRepositoryManagement::originCheckFinished);

  management.checkOriginsIfStale();
  QVERIFY(remote.data(LocalWorkspaceModel::OriginInitialPendingRole).toBool());
  management.show();
  QTRY_VERIFY(!remote.data(LocalWorkspaceModel::OriginInitialPendingRole)
                   .toBool());
  QVERIFY(remote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
  QCOMPARE(started.count(), 0);

  settings.setValue(originCacheKey(root),
                    QDateTime::currentDateTimeUtc().addSecs(-301));
  settings.setValue("localRepositoryManagement/originLastAttempt",
                    QDateTime::currentDateTimeUtc().addSecs(-121));
  QVERIFY(QMetaObject::invokeMethod(refresh, "timeout", Qt::DirectConnection));
  QTRY_COMPARE(started.count(), 1);
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(finished.first().at(0).toInt(), 1);
  QCOMPARE(finished.first().at(1).toInt(), 0);
  QVERIFY(remote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());

  QVERIFY(QMetaObject::invokeMethod(refresh, "timeout", Qt::DirectConnection));
  QTest::qWait(50);
  QCOMPARE(started.count(), 1);

  management.hide();
  settings.setValue(originCacheKey(root),
                    QDateTime::currentDateTimeUtc().addSecs(-301));
  settings.setValue("localRepositoryManagement/originLastAttempt",
                    QDateTime::currentDateTimeUtc().addSecs(-121));
  QVERIFY(QMetaObject::invokeMethod(refresh, "timeout", Qt::DirectConnection));
  QTest::qWait(50);
  QCOMPARE(started.count(), 1);
}

void TestLocalWorkspaces::managementChecksIndividualOriginFromContextMenu() {
  clearWorkspaces();
  Test::ScratchRepository repository;
  const git::Repository repo = repository;
  const QString root = repo.dir(false).path();
  QVERIFY(writeFile(QDir(root).filePath("tracked.txt"), "tracked\n"));
  QVERIFY(runGit(root, {"add", "tracked.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "tracked"}));
  const QString branch = repo.head().name();
  const QString upstream = QString("origin/%1").arg(branch);
  QVERIFY(runGit(root, {"remote", "add", "origin", root}));
  QVERIFY(runGit(root,
                 {"update-ref", QString("refs/remotes/%1").arg(upstream),
                  "HEAD"}));
  QVERIFY(runGit(root, {"branch", "--set-upstream-to", upstream, branch}));

  LocalWorkspace workspace;
  workspace.name = "Individual origin refresh";
  workspace.repositories.append(root);
  QString error;
  QVERIFY2(LocalWorkspaces::instance()->add(workspace, &error),
           qPrintable(error));

  QSettings settings;
  const QDateTime cooldownStart = QDateTime::currentDateTimeUtc();
  settings.setValue("localRepositoryManagement/originLastAttempt",
                    cooldownStart);

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(tree);
  const QModelIndex workspaceIndex = tree->model()->index(0, 0);
  tree->setExpanded(workspaceIndex, true);
  const QModelIndex remote = tree->model()->index(
      0, LocalWorkspaceModel::RemoteColumn, workspaceIndex);
  QTRY_VERIFY(!tree->visualRect(remote).isEmpty());
  QTRY_VERIFY(remote.data(LocalWorkspaceModel::OriginCheckEligibleRole)
                  .toBool());

  bool workspaceCheckFound = false;
  const QPoint workspacePosition = tree->visualRect(workspaceIndex).center();
  QTimer::singleShot(0, [&] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(menu);
    for (QAction *action : menu->actions())
      workspaceCheckFound = workspaceCheckFound || action->text() == "Check origin";
    menu->close();
  });
  QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                            Qt::DirectConnection,
                            Q_ARG(QPoint, workspacePosition));
  QVERIFY(!workspaceCheckFound);

  QSignalSpy started(&management,
                     &LocalRepositoryManagement::originCheckStarted);
  QSignalSpy finished(&management,
                      &LocalRepositoryManagement::originCheckFinished);
  auto triggerCheck = [&] {
    bool found = false;
    bool enabled = false;
    const QPoint position = tree->visualRect(remote).center();
    QTimer::singleShot(0, [&] {
      QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      QVERIFY(menu);
      for (QAction *action : menu->actions()) {
        if (action->text() != "Check origin")
          continue;
        found = true;
        enabled = action->isEnabled();
        if (enabled)
          action->trigger();
        break;
      }
      menu->close();
    });
    QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                              Qt::DirectConnection, Q_ARG(QPoint, position));
    QVERIFY(found);
    QVERIFY(enabled);
  };

  triggerCheck();
  QTRY_COMPARE(started.count(), 1);
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(finished.first().at(0).toInt(), 1);
  QCOMPARE(finished.first().at(1).toInt(), 0);
  QCOMPARE(settings.value("localRepositoryManagement/originLastAttempt")
               .toDateTime(),
           cooldownStart);
  QVERIFY(remote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());

  triggerCheck();
  QTRY_COMPARE(started.count(), 2);
  QTRY_COMPARE(finished.count(), 2);
  QCOMPARE(settings.value("localRepositoryManagement/originLastAttempt")
               .toDateTime(),
           cooldownStart);
}

void TestLocalWorkspaces::managementStartsOriginBatchForAllEligibleRepositories() {
  clearWorkspaces();
  Test::ScratchRepository first;
  Test::ScratchRepository second;
  const QList<git::Repository> repositories = {first, second};
  QStringList paths;
  for (const git::Repository &repository : repositories) {
    const QString path = repository.dir(false).path();
    QVERIFY(writeFile(QDir(path).filePath("tracked.txt"), "tracked\n"));
    QVERIFY(runGit(path, {"add", "tracked.txt"}));
    QVERIFY(runGit(path, {"commit", "-m", "tracked"}));
    const QString branch = repository.head().name();
    const QString upstream = QString("origin/%1").arg(branch);
    QVERIFY(runGit(path, {"remote", "add", "origin", path}));
    QVERIFY(runGit(path,
                   {"update-ref", QString("refs/remotes/%1").arg(upstream),
                    "HEAD"}));
    QVERIFY(runGit(path, {"branch", "--set-upstream-to", upstream, branch}));
    paths.append(path);
  }

  LocalWorkspace workspace;
  workspace.name = "Concurrent origin batch";
  workspace.repositories = paths;
  QString error;
  QVERIFY2(LocalWorkspaces::instance()->add(workspace, &error),
           qPrintable(error));

  QSettings settings;
  settings.setValue("localRepositoryManagement/originLastAttempt",
                    QDateTime::currentDateTimeUtc().addSecs(-121));

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QPushButton *check = management.findChild<QPushButton *>(
      "LocalRepositoryManagementCheckOrigin");
  QTimer *animation = management.findChild<QTimer *>(
      "LocalRepositoryManagementOriginAnimation");
  QThreadPool *pool = management.findChild<QThreadPool *>(
      "LocalRepositoryManagementOriginCheckPool");
  QVERIFY(tree);
  QVERIFY(check);
  QVERIFY(animation);
  QVERIFY(pool);
  QCOMPARE(pool->maxThreadCount(), 4);
  const QModelIndex workspaceIndex = tree->model()->index(0, 0);
  tree->setExpanded(workspaceIndex, true);
  QList<QModelIndex> remoteIndexes;
  for (int row = 0; row < paths.size(); ++row) {
    const QModelIndex remote = tree->model()->index(
        row, LocalWorkspaceModel::RemoteColumn, workspaceIndex);
    remoteIndexes.append(remote);
    QTRY_VERIFY(remote.data(LocalWorkspaceModel::OriginCheckEligibleRole)
                    .toBool());
  }

  QSignalSpy started(&management,
                     &LocalRepositoryManagement::originCheckStarted);
  QSignalSpy finished(&management,
                      &LocalRepositoryManagement::originCheckFinished);
  QSignalSpy fetchStarted(&management,
                          &LocalRepositoryManagement::originFetchStarted);
  QSignalSpy fetchFinished(&management,
                           &LocalRepositoryManagement::originFetchFinished);
  bool allActiveAtStart = false;
  bool checkingAtStart = false;
  bool animationAtStart = false;
  connect(&management, &LocalRepositoryManagement::originCheckStarted, [&] {
    allActiveAtStart = true;
    for (const QModelIndex &remote : remoteIndexes) {
      allActiveAtStart =
          allActiveAtStart &&
          remote.data(LocalWorkspaceModel::OriginFetchActiveRole).toBool();
    }
    checkingAtStart = !check->isEnabled() && check->text() == "Checking...";
    animationAtStart = animation->isActive();
  });

  QTest::mouseClick(check, Qt::LeftButton);
  QTRY_COMPARE(started.count(), 1);
  QCOMPARE(started.first().at(0).toInt(), paths.size());
  QVERIFY(allActiveAtStart);
  QVERIFY(checkingAtStart);
  QVERIFY(animationAtStart);
  QCOMPARE(fetchStarted.count(), paths.size());
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(finished.first().at(0).toInt(), paths.size());
  QCOMPARE(finished.first().at(1).toInt(), 0);
  QCOMPARE(fetchFinished.count(), paths.size());
  for (const QModelIndex &remote : remoteIndexes) {
    QVERIFY(remote.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool());
    QVERIFY(!remote.data(LocalWorkspaceModel::OriginFetchActiveRole).toBool());
  }
  QVERIFY(!animation->isActive());
}

void TestLocalWorkspaces::managementPreservesWorkspaceExpansion() {
  clearWorkspaces();
  Test::ScratchRepository first;
  Test::ScratchRepository second;
  Test::ScratchRepository third;
  const QString firstPath = git::Repository(first).dir(false).path();
  const QString secondPath = git::Repository(second).dir(false).path();
  const QString thirdPath = git::Repository(third).dir(false).path();

  LocalWorkspace primary;
  primary.name = "Primary expansion";
  primary.repositories = {firstPath, secondPath};
  LocalWorkspace secondary;
  secondary.name = "Secondary expansion";
  secondary.repositories = {thirdPath};
  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  QString error;
  QVERIFY2(workspaces->add(primary, &error), qPrintable(error));
  QVERIFY2(workspaces->add(secondary, &error), qPrintable(error));

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QLineEdit *search = management.findChild<QLineEdit *>(
      "LocalRepositoryManagementSearch");
  QPushButton *expansion = management.findChild<QPushButton *>(
      "LocalRepositoryManagementExpansionToggle");
  QVERIFY(tree);
  QVERIFY(search);
  QVERIFY(expansion);
  QModelIndex primaryIndex = workspaceIndex(tree, primary.id);
  QModelIndex secondaryIndex = workspaceIndex(tree, secondary.id);
  QVERIFY(primaryIndex.isValid());
  QVERIFY(secondaryIndex.isValid());
  tree->setExpanded(primaryIndex, true);
  tree->setExpanded(secondaryIndex, true);

  search->setText(primary.name);
  QTRY_COMPARE(tree->model()->rowCount(), 1);
  primaryIndex = workspaceIndex(tree, primary.id);
  QVERIFY(tree->isExpanded(primaryIndex));
  QVERIFY2(workspaces->removeRepository(primary.id, firstPath, &error),
           qPrintable(error));
  primaryIndex = workspaceIndex(tree, primary.id);
  QVERIFY(primaryIndex.isValid());
  QVERIFY(tree->isExpanded(primaryIndex));

  search->clear();
  QTRY_COMPARE(tree->model()->rowCount(), 2);
  primaryIndex = workspaceIndex(tree, primary.id);
  secondaryIndex = workspaceIndex(tree, secondary.id);
  QVERIFY(tree->isExpanded(primaryIndex));
  QVERIFY(tree->isExpanded(secondaryIndex));

  search->setText(primary.name);
  QTRY_COMPARE(tree->model()->rowCount(), 1);
  QTest::mouseClick(expansion, Qt::LeftButton);
  search->clear();
  QTRY_COMPARE(tree->model()->rowCount(), 2);
  QVERIFY(!tree->isExpanded(workspaceIndex(tree, primary.id)));
  QVERIFY(!tree->isExpanded(workspaceIndex(tree, secondary.id)));
  QTest::mouseClick(expansion, Qt::LeftButton);
  QVERIFY(tree->isExpanded(workspaceIndex(tree, primary.id)));
  QVERIFY(tree->isExpanded(workspaceIndex(tree, secondary.id)));

  QVERIFY2(workspaces->removeRepository(primary.id, secondPath, &error),
           qPrintable(error));
  primaryIndex = workspaceIndex(tree, primary.id);
  secondaryIndex = workspaceIndex(tree, secondary.id);
  QVERIFY(!tree->isExpanded(primaryIndex));
  QVERIFY(tree->isExpanded(secondaryIndex));

  QVERIFY2(workspaces->addRepository(primary.id, firstPath, &error),
           qPrintable(error));
  primaryIndex = workspaceIndex(tree, primary.id);
  QVERIFY(!tree->isExpanded(primaryIndex));
  QVERIFY(tree->isExpanded(workspaceIndex(tree, secondary.id)));
}

void TestLocalWorkspaces::openWorkspaceConfirmation() {
  clearWorkspaces();
  Test::ScratchRepository first;
  Test::ScratchRepository second;
  const QString firstPath = git::Repository(first).dir(false).path();
  const QString secondPath = git::Repository(second).dir(false).path();

  LocalWorkspace workspace;
  workspace.name = "Open confirmation";
  workspace.repositories = {firstPath, secondPath};
  LocalWorkspace empty;
  empty.name = "Empty workspace";
  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  QString error;
  QVERIFY2(workspaces->add(workspace, &error), qPrintable(error));
  QVERIFY2(workspaces->add(empty, &error), qPrintable(error));

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(tree);
  QSignalSpy opened(&management,
                    &LocalRepositoryManagement::openWorkspaceRequested);

  const auto triggerOpen = [&](bool accept) {
    bool messageSeen = false;
    QString messageText;
    const QModelIndex index = workspaceIndex(tree, workspace.id);
    const QPoint position = tree->visualRect(index).center();
    QTimer::singleShot(0, [&] {
      QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      QVERIFY(menu);
      QAction *open = nullptr;
      for (QAction *action : menu->actions()) {
        if (action->text() == QString("Open Workspace")) {
          open = action;
          break;
        }
      }
      QVERIFY(open);
      QTimer::singleShot(0, [&] {
        QMessageBox *message =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(message);
        messageSeen = true;
        messageText = message->text();
        if (!accept) {
          message->button(QMessageBox::Cancel)->click();
          return;
        }
        for (QAbstractButton *button : message->buttons()) {
          if (button->text().remove('&') == QString("Open Workspace")) {
            button->click();
            return;
          }
        }
        QFAIL("Open Workspace confirmation button not found");
      });
      open->trigger();
      menu->close();
    });
    QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                              Qt::DirectConnection, Q_ARG(QPoint, position));
    QVERIFY(messageSeen);
    QCOMPARE(messageText, QString("This will open 2 repositories."));
  };

  triggerOpen(false);
  QCOMPARE(opened.count(), 0);
  triggerOpen(true);
  QCOMPARE(opened.count(), 1);
  QCOMPARE(opened.first().at(0).toStringList(),
           QStringList({firstPath, secondPath}));

  bool openFound = false;
  bool openEnabled = true;
  const QModelIndex emptyIndex = workspaceIndex(tree, empty.id);
  const QPoint emptyPosition = tree->visualRect(emptyIndex).center();
  QTimer::singleShot(0, [&] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(menu);
    for (QAction *action : menu->actions()) {
      if (action->text() == QString("Open Workspace")) {
        openFound = true;
        openEnabled = action->isEnabled();
      }
    }
    menu->close();
  });
  QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                            Qt::DirectConnection,
                            Q_ARG(QPoint, emptyPosition));
  QVERIFY(openFound);
  QVERIFY(!openEnabled);
}

void TestLocalWorkspaces::repositoryStatus() {
  clearWorkspaces();
  Test::ScratchRepository repository;
  const git::Repository repo = repository;
  const QString root = repo.dir(false).path();
  QVERIFY(writeFile(QDir(root).filePath("modified.txt"), "base\n"));
  QVERIFY(writeFile(QDir(root).filePath("removed.txt"), "base\n"));
  QVERIFY(writeFile(QDir(root).filePath("rename.txt"), "base\n"));
  QVERIFY(runGit(root, {"add", "."}));
  QVERIFY(runGit(root, {"commit", "-m", "initial"}));

  const QString branch = repo.head().name();
  QVERIFY(!branch.isEmpty());
  const QString upstream = QString("origin/%1").arg(branch);

  LocalWorkspace workspace;
  workspace.name = "Status";
  workspace.repositories.append(root);
  QString error;
  QVERIFY2(LocalWorkspaces::instance()->add(workspace, &error),
           qPrintable(error));

  LocalWorkspaceModel model;
  const QModelIndex workspaceIndex = model.index(0, 0);
  QModelIndex remoteIndex =
      model.index(0, LocalWorkspaceModel::RemoteColumn, workspaceIndex);
  QModelIndex changesIndex =
      model.index(0, LocalWorkspaceModel::ChangesColumn, workspaceIndex);
  QTRY_VERIFY(changesIndex.data(LocalWorkspaceModel::StatusReadyRole).toBool());
  QCOMPARE(remoteIndex.data(LocalWorkspaceModel::TrackingReadyRole).toBool(),
           false);
  QCOMPARE(
      remoteIndex.data(LocalWorkspaceModel::OriginCheckEligibleRole).toBool(),
      false);

  QVERIFY(runGit(root, {"remote", "add", "origin", root}));
  QVERIFY(runGit(root,
                 {"update-ref", QString("refs/remotes/%1").arg(upstream),
                  "HEAD"}));
  QVERIFY(runGit(root, {"branch", "--set-upstream-to", upstream, branch}));
  model.refreshRepositories();
  QTRY_VERIFY(remoteIndex.data(LocalWorkspaceModel::TrackingReadyRole).toBool());
  QVERIFY(
      remoteIndex.data(LocalWorkspaceModel::OriginCheckEligibleRole).toBool());
  QCOMPARE(remoteIndex.data(LocalWorkspaceModel::AheadRole).toInt(), 0);
  QCOMPARE(remoteIndex.data(LocalWorkspaceModel::BehindRole).toInt(), 0);
  QCOMPARE(remoteIndex.data(LocalWorkspaceModel::UpstreamRole).toString(),
           upstream);

  QVERIFY(writeFile(QDir(root).filePath("ahead.txt"), "ahead\n"));
  QVERIFY(runGit(root, {"add", "ahead.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "ahead"}));
  model.refreshRepositories();
  QTRY_COMPARE(remoteIndex.data(LocalWorkspaceModel::AheadRole).toInt(), 1);
  QCOMPARE(remoteIndex.data(LocalWorkspaceModel::BehindRole).toInt(), 0);

  QVERIFY(runGit(root, {"checkout", "-b", "remote-future"}));
  QVERIFY(writeFile(QDir(root).filePath("remote.txt"), "remote\n"));
  QVERIFY(runGit(root, {"add", "remote.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "remote"}));
  QVERIFY(runGit(root,
                 {"update-ref", QString("refs/remotes/%1").arg(upstream),
                  "HEAD"}));
  QVERIFY(runGit(root, {"checkout", branch}));
  model.refreshRepositories();
  QTRY_COMPARE(remoteIndex.data(LocalWorkspaceModel::BehindRole).toInt(), 1);
  QCOMPARE(remoteIndex.data(LocalWorkspaceModel::AheadRole).toInt(), 0);

  QVERIFY(writeFile(QDir(root).filePath("local.txt"), "local\n"));
  QVERIFY(runGit(root, {"add", "local.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "local"}));
  model.refreshRepositories();
  QTRY_COMPARE(remoteIndex.data(LocalWorkspaceModel::AheadRole).toInt(), 1);
  QTRY_COMPARE(remoteIndex.data(LocalWorkspaceModel::BehindRole).toInt(), 1);

  QVERIFY(writeFile(QDir(root).filePath("modified.txt"), "local conflict\n"));
  QVERIFY(runGit(root, {"add", "modified.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "local conflict"}));
  QVERIFY(runGit(root, {"checkout", "remote-future"}));
  QVERIFY(writeFile(QDir(root).filePath("modified.txt"), "remote conflict\n"));
  QVERIFY(runGit(root, {"add", "modified.txt"}));
  QVERIFY(runGit(root, {"commit", "-m", "remote conflict"}));
  QVERIFY(runGit(root, {"checkout", branch}));
  QVERIFY(!runGit(root, {"merge", "remote-future"}));
  model.refreshRepositories();
  QTRY_COMPARE(changesIndex.data(LocalWorkspaceModel::ConflictedRole).toInt(), 1);
  QVERIFY(runGit(root, {"merge", "--abort"}));

  QVERIFY(writeFile(QDir(root).filePath("modified.txt"), "changed\n"));
  QVERIFY(writeFile(QDir(root).filePath("added.txt"), "added\n"));
  QVERIFY(runGit(root, {"add", "added.txt"}));
  QVERIFY(QFile::remove(QDir(root).filePath("removed.txt")));
  QVERIFY(runGit(root, {"mv", "rename.txt", "renamed.txt"}));
  QVERIFY(writeFile(QDir(root).filePath("untracked.txt"), "untracked\n"));
  QVERIFY(writeFile(QDir(root).filePath("staged-then-removed.txt"), "staged\n"));
  QVERIFY(runGit(root, {"add", "staged-then-removed.txt"}));
  QVERIFY(QFile::remove(QDir(root).filePath("staged-then-removed.txt")));
  model.refreshRepositories();
  QTRY_COMPARE(changesIndex.data(LocalWorkspaceModel::ModifiedRole).toInt(), 1);
  QCOMPARE(changesIndex.data(LocalWorkspaceModel::AddedRole).toInt(), 3);
  QCOMPARE(changesIndex.data(LocalWorkspaceModel::RemovedRole).toInt(), 3);
  QCOMPARE(changesIndex.data(LocalWorkspaceModel::UntrackedRole).toInt(), 1);
  QCOMPARE(changesIndex.data(LocalWorkspaceModel::ConflictedRole).toInt(), 0);

  LocalRepositoryManagement management;
  management.resize(1000, 600);
  management.show();
  QTreeView *tree = management.findChild<QTreeView *>(
      "LocalRepositoryManagementTree");
  QVERIFY(tree);
  const QModelIndex proxyWorkspace = tree->model()->index(0, 0);
  tree->setExpanded(proxyWorkspace, true);
  const QModelIndex proxyChanges = tree->model()->index(
      0, LocalWorkspaceModel::ChangesColumn, proxyWorkspace);
  QTRY_COMPARE(proxyChanges.data(LocalWorkspaceModel::ModifiedRole).toInt(), 1);
  QTRY_VERIFY(!tree->visualRect(proxyChanges).isEmpty());
  const QPoint position =
      tree->visualRect(proxyChanges).topLeft() + QPoint(8, 8);
  QHelpEvent tooltipEvent(QEvent::ToolTip, position,
                          tree->viewport()->mapToGlobal(position));
  QApplication::sendEvent(tree->viewport(), &tooltipEvent);
  QTRY_COMPARE(QToolTip::text(), QString("1 modified file"));
}

void TestLocalWorkspaces::cleanupTestCase() {
  clearWorkspaces();
  QSettings settings;
  if (mHadStoredWorkspaces)
    settings.setValue("localWorkspaces", mStoredWorkspaces);
  else
    settings.remove("localWorkspaces");
  settings.remove("localRepositoryManagement");
  settings.beginGroup("localRepositoryManagement");
  for (auto it = mStoredManagementSettings.cbegin();
       it != mStoredManagementSettings.cend(); ++it)
    settings.setValue(it.key(), it.value());
  settings.endGroup();
}

void TestLocalWorkspaces::clearWorkspaces() {
  LocalWorkspaces *workspaces = LocalWorkspaces::instance();
  while (workspaces->count() > 0) {
    const QString id = workspaces->workspace(0)->id;
    QVERIFY(workspaces->remove(id));
  }
}

TEST_MAIN(TestLocalWorkspaces)

#include "local_workspaces.moc"

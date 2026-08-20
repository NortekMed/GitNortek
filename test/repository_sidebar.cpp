//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "conf/Setting.h"
#include "conf/Settings.h"
#include "git/Branch.h"
#include "git/Config.h"
#include "git/TagRef.h"
#include "host/Account.h"
#include "host/Accounts.h"
#include "ui/CommitList.h"
#include "ui/ConfigKeys.h"
#include "ui/Footer.h"
#include "ui/MainWindow.h"
#include "ui/RepositoryNavigator.h"
#include "ui/RepositoryNavigatorModel.h"
#include "ui/RepoView.h"
#include "ui/SideBar.h"
#include "ui/TabWidget.h"
#include <QAbstractItemModelTester>
#include <QContextMenuEvent>
#include <QFile>
#include <QFontMetrics>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

using namespace QTest;

namespace {

enum SideBarRole {
  PathRole = Qt::UserRole,
  TabRole,
  RecentRole,
  AccountRole,
  AccountKindRole
};

QList<QPair<QString, bool>> contextMenuItems(QTreeView *view,
                                             const QModelIndex &index) {
  QList<QPair<QString, bool>> items;
  view->scrollTo(index);
  QPoint point = view->visualRect(index).center();
  QTimer::singleShot(0, [&items] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    for (QAction *action : menu->actions()) {
      if (!action->isSeparator())
        items.append({action->text(), action->isEnabled()});
    }
    menu->close();
  });
  QMetaObject::invokeMethod(view, "customContextMenuRequested",
                            Qt::DirectConnection, Q_ARG(QPoint, point));
  return items;
}

QStringList menuTexts(const QList<QPair<QString, bool>> &items) {
  QStringList texts;
  for (const auto &item : items)
    texts.append(item.first);
  return texts;
}

QStringList contextMenuItems(CommitList *view, const QModelIndex &index) {
  QStringList items;
  view->scrollTo(index);
  QPoint viewportPoint = view->visualRect(index).center();
  QPoint globalPoint = view->viewport()->mapToGlobal(viewportPoint);
  QTimer::singleShot(0, [&items] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    for (QAction *action : menu->actions()) {
      if (!action->isSeparator())
        items.append(action->text());
    }
    menu->close();
  });
  QContextMenuEvent event(QContextMenuEvent::Mouse, viewportPoint, globalPoint);
  QApplication::sendEvent(view->viewport(), &event);
  return items;
}

} // namespace

class TestRepositorySideBar : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void chooserModel();
  void recentRemoval();
  void accountRows();
  void navigatorModel();
  void navigatorView();
  void activeRepositoryBinding();
  void stashInteraction();
  void submoduleInteraction();
  void cleanupTestCase();

private:
  MainWindow *mWindow = nullptr;
  QTreeView *mTree = nullptr;
  Footer *mFooter = nullptr;
  Test::ScratchRepository mRepo;
};

void TestRepositorySideBar::initTestCase() {
  QCOMPARE(RecentRepositories::instance()->count(), 0);
  QCOMPARE(Accounts::instance()->count(), 0);

  mWindow = new MainWindow(git::Repository());
  SideBar *sideBar = mWindow->findChild<SideBar *>();
  QVERIFY(sideBar);

  mTree = sideBar->findChild<QTreeView *>("RepositoryTree");
  mFooter = sideBar->findChild<Footer *>("RepositoryFooter");
  QVERIFY(mTree);
  QVERIFY(mFooter);
}

void TestRepositorySideBar::chooserModel() {
  QAbstractItemModel *model = mTree->model();
  QCOMPARE(model->rowCount(), 3);

  QModelIndex open = model->index(0, 0);
  QCOMPARE(open.data().toString(), QString("open"));
  QCOMPARE(model->rowCount(open), 1);
  QCOMPARE(model->index(0, 0, open).data().toString(), QString("none"));

  QModelIndex recent = model->index(1, 0);
  QCOMPARE(recent.data().toString(), QString("recent"));
  QCOMPARE(model->rowCount(recent), 1);
  QCOMPARE(model->index(0, 0, recent).data().toString(), QString("none"));

  QModelIndex remote = model->index(2, 0);
  QCOMPARE(remote.data().toString(), QString("remote"));
  QCOMPARE(model->rowCount(remote), Account::NUM_KINDS);
  for (int row = 0; row < Account::NUM_KINDS; ++row) {
    Account::Kind kind = static_cast<Account::Kind>(row);
    QModelIndex provider = model->index(row, 0, remote);
    QCOMPARE(provider.data().toString(), Account::name(kind));
    QCOMPARE(provider.data(AccountKindRole).value<Account::Kind>(), kind);
    QVERIFY(!provider.data(AccountRole).value<Account *>());
  }
}

void TestRepositorySideBar::recentRemoval() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  RecentRepositories *recent = RecentRepositories::instance();
  QSignalSpy removed(recent, &RecentRepositories::repositoryRemoved);
  recent->add(dir.path());
  QCOMPARE(recent->count(), 1);

  QAbstractItemModel *model = mTree->model();
  QModelIndex recentRoot = model->index(1, 0);
  QModelIndex row = model->index(0, 0, recentRoot);
  QCOMPARE(row.data(PathRole).toString(), dir.path());
  QVERIFY(row.data(RecentRole).value<RecentRepository *>());

  mTree->selectionModel()->setCurrentIndex(
      row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  QToolButton *remove = mFooter->findChild<QToolButton *>("Remove");
  QVERIFY(remove);
  QVERIFY(remove->isEnabled());
  QVERIFY(QMetaObject::invokeMethod(mFooter, "minusClicked"));

  QCOMPARE(removed.count(), 1);
  QCOMPARE(recent->count(), 0);
  recentRoot = model->index(1, 0);
  QCOMPARE(model->index(0, 0, recentRoot).data().toString(), QString("none"));
}

void TestRepositorySideBar::accountRows() {
  Accounts *accounts = Accounts::instance();
  Account *account = accounts->createAccount(Account::GitLab, "offline-test");
  QVERIFY(account);
  QCOMPARE(accounts->count(), 1);

  QAbstractItemModel *model = mTree->model();
  QModelIndex remote = model->index(2, 0);
  QCOMPARE(model->rowCount(remote), 1);

  QModelIndex row = model->index(0, 0, remote);
  QCOMPARE(row.data().toString(), QString("offline-test"));
  QCOMPARE(row.data(AccountRole).value<Account *>(), account);
  QCOMPARE(row.data(AccountKindRole).value<Account::Kind>(), Account::GitLab);
  QCOMPARE(model->rowCount(row), 0);

  accounts->removeAccount(account);
  QCOMPARE(accounts->count(), 0);
  remote = model->index(2, 0);
  QCOMPARE(model->rowCount(remote), Account::NUM_KINDS);
}

void TestRepositorySideBar::navigatorModel() {
  QFile file(mRepo->workdir().filePath("tracked.txt"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("initial\n"), qint64(8));
  file.close();

  QProcess git;
  git.setWorkingDirectory(mRepo->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "tracked.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "initial"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"update-ref", "refs/remotes/origin/main", "HEAD"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git::Commit head = mRepo->head().target();
  QVERIFY(head.isValid());
  QVERIFY(mRepo->createBranch("feature", head).isValid());
  QVERIFY(mRepo->createTag(head, "v1").isValid());

  QVERIFY(file.open(QIODevice::Append));
  QCOMPARE(file.write("change\n"), qint64(7));
  file.close();
  QVERIFY(mRepo->stash("sidebar stash").isValid());

  git::Branch main = mRepo->head();
  git::Branch upstream = mRepo->lookupBranch("origin/main", GIT_BRANCH_REMOTE);
  QVERIFY(upstream.isValid());
  main.setUpstream(upstream);

  RepositoryNavigatorModel model;
  QAbstractItemModelTester tester(
      &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
  model.setRepository(mRepo);

  QCOMPARE(model.rowCount(),
           static_cast<int>(RepositoryNavigatorModel::Section::Count));
  const QStringList sections = {"Local",         "Remote", "Stashes",
                                "Cloud Patches", "Pull Requests",
                                "GitHub Issues", "Teams",  "Tags",
                                "Submodules"};
  for (int row = 0; row < sections.size(); ++row)
    QCOMPARE(model.index(row, 0).data().toString(), sections.at(row));

  for (RepositoryNavigatorModel::Section section :
       {RepositoryNavigatorModel::Section::CloudPatches,
        RepositoryNavigatorModel::Section::PullRequests,
        RepositoryNavigatorModel::Section::GitHubIssues,
        RepositoryNavigatorModel::Section::Teams}) {
    QModelIndex index = model.sectionIndex(section);
    QCOMPARE(index.data(RepositoryNavigatorModel::CountRole).toInt(), 0);
    QVERIFY(!index.data(RepositoryNavigatorModel::AvailableRole).toBool());
    QVERIFY(!(model.flags(index) & Qt::ItemIsEnabled));
  }

  QModelIndex local =
      model.sectionIndex(RepositoryNavigatorModel::Section::Local);
  QCOMPARE(model.rowCount(local), 2);
  int current = 0;
  bool foundTracking = false;
  for (int row = 0; row < model.rowCount(local); ++row) {
    QModelIndex branch = model.index(row, 0, local);
    QVERIFY(branch.data(RepositoryNavigatorModel::ReferenceRole)
                .value<git::Reference>()
                .isValid());
    current += branch.data(RepositoryNavigatorModel::CurrentRole).toBool();
    if (branch.data().toString() == main.name()) {
      QCOMPARE(branch.data(RepositoryNavigatorModel::AheadRole).toInt(), 0);
      QCOMPARE(branch.data(RepositoryNavigatorModel::BehindRole).toInt(), 0);
      foundTracking = true;
    } else {
      QVERIFY(!branch.data(RepositoryNavigatorModel::AheadRole).isValid());
      QVERIFY(!branch.data(RepositoryNavigatorModel::BehindRole).isValid());
    }
  }
  QCOMPARE(current, 1);
  QVERIFY(foundTracking);

  QModelIndex remotes =
      model.sectionIndex(RepositoryNavigatorModel::Section::Remote);
  QCOMPARE(model.rowCount(remotes), 1);
  QCOMPARE(model.index(0, 0, remotes).data().toString(),
           QString("origin/main"));

  QVERIFY(mRepo->createBranch("notified", head).isValid());
  QTRY_COMPARE(model.rowCount(local), 3);

  QModelIndex stashes =
      model.sectionIndex(RepositoryNavigatorModel::Section::Stashes);
  QCOMPARE(model.rowCount(stashes), 1);
  QModelIndex stash = model.index(0, 0, stashes);
  QCOMPARE(stash.data(RepositoryNavigatorModel::StashIndexRole).toInt(), 0);
  QVERIFY(stash.data(RepositoryNavigatorModel::CommitRole)
              .value<git::Commit>()
              .isValid());

  QModelIndex tags =
      model.sectionIndex(RepositoryNavigatorModel::Section::Tags);
  QCOMPARE(model.rowCount(tags), 1);
  QCOMPARE(model.index(0, 0, tags).data().toString(), QString("v1"));

  model.clear();
  QCOMPARE(model.rowCount(), sections.size());
  QCOMPARE(model.rowCount(model.sectionIndex(
               RepositoryNavigatorModel::Section::Local)),
           0);
}

void TestRepositorySideBar::navigatorView() {
  RepositoryNavigator navigator;
  QTreeView *view = navigator.view();
  QVERIFY(view);
  QCOMPARE(view->objectName(), QString("RepositoryNavigationTree"));
  QVERIFY(view->focusPolicy() != Qt::NoFocus);
  QVERIFY(view->itemDelegate());
  QCOMPARE(navigator.model()->index(0, 0).data().toString(), QString("Local"));
  QCOMPARE(view->itemDelegate()->sizeHint(QStyleOptionViewItem(),
                                          navigator.model()->index(0, 0))
               .height(),
           28);

  RepositoryNavigatorModel *model = navigator.model();
  QModelIndex local =
      model->sectionIndex(RepositoryNavigatorModel::Section::Local);
  QModelIndex cloud =
      model->sectionIndex(RepositoryNavigatorModel::Section::CloudPatches);
  QVERIFY(view->isExpanded(local));
  QVERIFY(!view->isExpanded(cloud));

  view->collapse(local);
  QCoreApplication::processEvents();
  QCOMPARE(QSettings()
               .value("sidebar/repositoryNavigator/expanded/Local")
               .toBool(),
           false);

  navigator.setRepository(mRepo);
  local = model->sectionIndex(RepositoryNavigatorModel::Section::Local);
  QVERIFY(!view->isExpanded(local));
}

void TestRepositorySideBar::activeRepositoryBinding() {
  MainWindow window(mRepo);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  SideBar *sideBar = window.findChild<SideBar *>();
  RepositoryNavigator *navigator =
      sideBar->findChild<RepositoryNavigator *>("RepositoryNavigator");
  QSplitter *content =
      sideBar->findChild<QSplitter *>("RepositorySidebarContent");
  QVERIFY(navigator);
  QVERIFY(content);
  QCOMPARE(content->count(), 2);
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           mRepo->dir(false).path());

  RepositoryNavigatorModel *model = navigator->model();
  QModelIndex local =
      model->sectionIndex(RepositoryNavigatorModel::Section::Local);
  navigator->view()->setExpanded(local, true);
  QModelIndex current;
  QModelIndex other;
  for (int row = 0; row < model->rowCount(local); ++row) {
    QModelIndex index = model->index(row, 0, local);
    if (index.data(RepositoryNavigatorModel::CurrentRole).toBool())
      current = index;
    else
      other = index;
  }
  QVERIFY(current.isValid());
  QVERIFY(other.isValid());

  QList<QPair<QString, bool>> currentItems =
      contextMenuItems(navigator->view(), current);
  QCOMPARE(menuTexts(currentItems),
           QStringList({"Checkout", "Rename", "Delete", "Merge...",
                        "Rebase...", "Squash..."}));
  QCOMPARE(currentItems.at(0).second, false);
  QCOMPARE(currentItems.at(1).second, false);
  QCOMPARE(currentItems.at(2).second, false);

  QList<QPair<QString, bool>> otherItems =
      contextMenuItems(navigator->view(), other);
  QCOMPARE(menuTexts(otherItems), menuTexts(currentItems));
  QVERIFY(otherItems.at(0).second);
  QVERIFY(otherItems.at(1).second);
  QVERIFY(otherItems.at(2).second);

  QModelIndex remotes =
      model->sectionIndex(RepositoryNavigatorModel::Section::Remote);
  navigator->view()->setExpanded(remotes, true);
  QModelIndex remote = model->index(0, 0, remotes);
  QList<QPair<QString, bool>> remoteItems =
      contextMenuItems(navigator->view(), remote);
  QCOMPARE(menuTexts(remoteItems),
           QStringList({"Checkout", "New Local Branch", "Merge...",
                        "Rebase...", "Squash..."}));
  for (const auto &item : remoteItems)
    QVERIFY(item.second);

  Test::ScratchRepository second;
  RepoView *secondView = window.addTab(second);
  QVERIFY(secondView);
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           second->dir(false).path());

  QVERIFY(window.tabWidget()->closeTab(secondView));
  QTRY_COMPARE(window.count(), 1);
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           mRepo->dir(false).path());

  RepoView *view = window.currentView();
  local = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Local);
  QModelIndex feature;
  for (int row = 0; row < navigator->model()->rowCount(local); ++row) {
    QModelIndex candidate = navigator->model()->index(row, 0, local);
    if (candidate.data().toString() == "feature") {
      feature = candidate;
      break;
    }
  }
  QVERIFY(feature.isValid());

  QSignalSpy referenceSelected(view, &RepoView::referenceSelected);
  QVERIFY(QMetaObject::invokeMethod(navigator->view(), "clicked",
                                    Q_ARG(QModelIndex, feature)));
  QTRY_COMPARE(referenceSelected.count(), 1);
  QCOMPARE(referenceSelected.first().first().value<git::Reference>().name(),
           QString("feature"));

  QVERIFY(QMetaObject::invokeMethod(navigator->view(), "doubleClicked",
                                    Q_ARG(QModelIndex, feature)));
  QTRY_COMPARE(mRepo->head().name(), QString("feature"));

  QVERIFY(window.tabWidget()->closeTab(window.currentView()));
  QTRY_COMPARE(window.count(), 0);
  QVERIFY(!navigator->model()->repository().isValid());
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void TestRepositorySideBar::stashInteraction() {
  const QString headerStateKey = "commit/columns/headerStateV10";
  QSettings columnSettings;
  bool hadHeaderState = columnSettings.contains(headerStateKey);
  QByteArray headerState = columnSettings.value(headerStateKey).toByteArray();
  auto restoreHeaderState = qScopeGuard([hadHeaderState, headerStateKey,
                                         headerState] {
    QSettings settings;
    if (hadHeaderState)
      settings.setValue(headerStateKey, headerState);
    else
      settings.remove(headerStateKey);
  });
  columnSettings.remove(headerStateKey);

  Test::ScratchRepository repo;
  QFile file(repo->workdir().filePath("stash.txt"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write("initial\n");
  file.close();

  QProcess git;
  git.setWorkingDirectory(repo->workdir().path());
  git.start(GIT_EXECUTABLE, {"add", "stash.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "stash base"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"commit", "--allow-empty", "-m", "stash tip"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  QString mainBranch = repo->head().name();
  git.start(GIT_EXECUTABLE, {"checkout", "-b", "graph-side", "HEAD~1"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"commit", "--allow-empty", "-m", "graph side"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"checkout", mainBranch});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"merge", "--no-ff", "graph-side", "-m", "graph merge"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  QVERIFY(file.open(QIODevice::Append));
  file.write("change\n");
  file.close();
  QVERIFY(repo->stash("interaction stash").isValid());
  QVERIFY(file.open(QIODevice::Append));
  file.write("second change\n");
  file.close();
  QVERIFY(repo->stash("second interaction stash").isValid());

  git::Config config = repo->appConfig();
  config.setValue(ConfigKeys::kRefsKey,
                  static_cast<int>(CommitList::RefsFilter::AllRefs));
  config.setValue(ConfigKeys::kStatusKey, false);

  MainWindow window(repo);
  CommitList *commitList = window.currentView()->findChild<CommitList *>();
  QVERIFY(commitList);
  commitList->setMinimumWidth(900);
  window.resize(1600, 900);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);

  QAbstractItemModel *graphModel = commitList->model();

  bool compact = Settings::instance()
                     ->value(Setting::Id::ShowCommitsInCompactMode)
                     .toBool();
  auto restoreCompactMode = qScopeGuard([compact] {
    Settings::instance()->setValue(Setting::Id::ShowCommitsInCompactMode,
                                   compact);
  });
  Settings::instance()->setValue(Setting::Id::ShowCommitsInCompactMode, true);
  commitList->resetSettings();
  QCoreApplication::processEvents();
  QCOMPARE(commitList->sizeHintForRow(0), 28);

  QHeaderView *header = commitList->findChild<QHeaderView *>();
  QVERIFY(header);
  QVERIFY(header->isVisible());
  QVERIFY(header->sectionsMovable());
  QCOMPARE(header->count(), 6);
  QCOMPARE(header->length(), header->width());
  QTRY_COMPARE(header->sectionSize(0),
               qBound(55, header->width() * 19 / 100, 360));
  QFont compactFont = commitList->font();
  if (compactFont.pointSizeF() > 1.0)
    compactFont.setPointSizeF(compactFont.pointSizeF() - 1.0);
  else if (compactFont.pixelSize() > 1)
    compactFont.setPixelSize(compactFont.pixelSize() - 1);
  QCOMPARE(header->font().pointSizeF(), compactFont.pointSizeF());
  QCOMPARE(header->font().pixelSize(), compactFont.pixelSize());

  QFontMetrics compactMetrics(compactFont, commitList);
  const QString shaCharacters = "0123456789abcdef";
  int maxCharacter = 0;
  int maxPairAdjustment = 0;
  for (QChar first : shaCharacters) {
    int firstWidth = compactMetrics.horizontalAdvance(first);
    maxCharacter = qMax(maxCharacter, firstWidth);
    for (QChar second : shaCharacters) {
      int pairWidth =
          compactMetrics.horizontalAdvance(QString(first) + second);
      int secondWidth = compactMetrics.horizontalAdvance(second);
      maxPairAdjustment = qMax(
          maxPairAdjustment, pairWidth - firstWidth - secondWidth);
    }
  }
  int shaMinimum = 7 * maxCharacter + 6 * maxPairAdjustment + 16;
  QCOMPARE(header->sectionSize(5), shaMinimum);
  header->resizeSection(5, shaMinimum - 10);
  QCOMPARE(header->sectionSize(5), shaMinimum);
  QCOMPARE(header->length(), header->width());

  int referencesWidth = header->sectionSize(0);
  int summaryWidth = header->sectionSize(2);
  header->resizeSection(0, referencesWidth + 20);
  QCOMPARE(header->sectionSize(0), referencesWidth + 20);
  QCOMPARE(header->sectionSize(2), summaryWidth - 20);
  QCOMPARE(header->length(), header->width());

  QToolButton *columnOptions = nullptr;
  for (QToolButton *button :
       commitList->findChildren<QToolButton *>(QString(),
                                                Qt::FindDirectChildrenOnly)) {
    if (button->menu()) {
      columnOptions = button;
      break;
    }
  }
  QVERIFY(columnOptions);
  QAction *referencesAction = columnOptions->menu()->actions().constFirst();
  referencesAction->setChecked(true);
  referencesAction->trigger();
  QVERIFY(header->isSectionHidden(0));
  referencesAction->trigger();
  QVERIFY(!header->isSectionHidden(0));

  while (graphModel->canFetchMore(QModelIndex()))
    graphModel->fetchMore(QModelIndex());
  constexpr int dotSegment = 0;
  constexpr int leftOutSegment = 6;
  constexpr int rightOutSegment = 8;
  constexpr int firstMergeSegment = 9;
  constexpr int mergeLeftInSegment = 10;
  constexpr int mergeLeftOutSegment = 11;
  constexpr int mergeRightInSegment = 12;
  constexpr int mergeRightOutSegment = 13;
  constexpr int firstForkSegment = 14;
  constexpr int forkLeftInSegment = 15;
  constexpr int forkLeftOutSegment = 16;
  constexpr int forkRightInSegment = 17;
  constexpr int forkRightOutSegment = 18;
  bool mergeUsesSidePort = false;
  QColor mergeNodeColor;
  QColor mergeEdgeColor;
  QColor mergeTargetColor;
  QList<QColor> mergeColors;
  bool stashUsesSourceColor = false;
  QColor divergenceColor;
  QList<QColor> forkColors;
  bool divergenceContinuesStraight = false;
  bool forkStartsAtParent = false;
  bool forkReachesDeferredLane = false;
  int mergeRow = -1;
  int mergeTargetColumn = -1;
  for (int row = 0; row < graphModel->rowCount(); ++row) {
    QModelIndex index = graphModel->index(row, 0);
    git::Commit commit = index.data(CommitList::CommitRole).value<git::Commit>();
    bool targetMerge = commit.isValid() && commit.summary() == "graph merge";
    bool targetDivergence =
        commit.isValid() && commit.summary() == "graph side";
    bool targetParent = commit.isValid() && commit.summary() == "stash base";
    bool stash = index.data(CommitList::StashIndexRole).isValid();
    QColor stashNodeColor;
    QList<QColor> stashEdgeColors;
    QVariantList columns = index.data(CommitList::GraphRole).toList();
    QVariantList colorColumns =
        index.data(CommitList::GraphColorRole).toList();
    QVariantList styleColumns =
        index.data(CommitList::GraphStyleRole).toList();
    int dotColumn = -1;
    for (int column = 0; column < columns.size(); ++column) {
      if (columns.at(column).toList().contains(dotSegment)) {
        dotColumn = column;
        break;
      }
    }
    for (int column = 0; column < columns.size(); ++column) {
      QVariantList segments = columns.at(column).toList();
      QVariantList colors = colorColumns.at(column).toList();
      QVariantList styles = styleColumns.at(column).toList();
      bool hasDot = segments.contains(dotSegment);
      for (int segment = 0; segment < segments.size(); ++segment) {
        int type = segments.at(segment).toInt();
        if (targetMerge && type == dotSegment)
          mergeNodeColor = colors.at(segment).value<QColor>();
        if (targetDivergence && type == dotSegment)
          divergenceColor = colors.at(segment).value<QColor>();
        if (targetDivergence && hasDot && type == 3)
          divergenceContinuesStraight = true;
        if (targetMerge &&
            (type == mergeLeftOutSegment || type == mergeRightOutSegment)) {
          mergeUsesSidePort = true;
          mergeEdgeColor = colors.at(segment).value<QColor>();
        }
        if (targetMerge &&
            (type == mergeLeftInSegment || type == mergeRightInSegment)) {
          mergeTargetColor = colors.at(segment).value<QColor>();
          mergeRow = row;
          mergeTargetColumn = column;
        }
        if (targetMerge && type >= firstMergeSegment)
          mergeColors.append(colors.at(segment).value<QColor>());
        if (targetParent && type >= firstForkSegment) {
          forkColors.append(colors.at(segment).value<QColor>());
          if (type == forkLeftOutSegment || type == forkRightOutSegment) {
            QCOMPARE(column, dotColumn);
            forkStartsAtParent = true;
          }
          if (type == forkLeftInSegment || type == forkRightInSegment) {
            QVERIFY(column != dotColumn);
            forkReachesDeferredLane = true;
          }
        }
        if (stash && type == dotSegment)
          stashNodeColor = colors.at(segment).value<QColor>();
        if (stash && styles.at(segment).toInt() == Qt::DotLine && type >= 4 &&
            type < firstMergeSegment) {
          stashEdgeColors.append(colors.at(segment).value<QColor>());
        }
      }
      if (hasDot && stash) {
        QColor stashEdgeColor;
        for (int segment = 0; segment < segments.size(); ++segment) {
          int type = segments.at(segment).toInt();
          if (type == leftOutSegment || type == rightOutSegment)
            stashEdgeColor = colors.at(segment).value<QColor>();
        }
        if (stashEdgeColor.isValid()) {
          QCOMPARE(stashEdgeColor, stashNodeColor);
          stashUsesSourceColor = true;
        }
        for (const QVariant &segment : segments)
          QVERIFY(segment.toInt() < firstMergeSegment);
      }
    }
    if (stash) {
      QVERIFY(stashNodeColor.isValid());
      for (const QColor &color : stashEdgeColors)
        QCOMPARE(color, stashNodeColor);
    }
    if (targetMerge)
      QVERIFY(commit.parents().size() > 1);
  }
  QVERIFY(mergeUsesSidePort);
  QVERIFY(mergeNodeColor.isValid());
  QVERIFY(mergeTargetColor.isValid());
  QCOMPARE(mergeEdgeColor, mergeTargetColor);
  for (const QColor &color : mergeColors)
    QCOMPARE(color, mergeTargetColor);
  QVERIFY(mergeRow + 1 < graphModel->rowCount());
  QVariantList nextColorColumns = graphModel->index(mergeRow + 1, 0)
                                      .data(CommitList::GraphColorRole)
                                      .toList();
  QVERIFY(mergeTargetColumn < nextColorColumns.size());
  QVariantList nextColors = nextColorColumns.at(mergeTargetColumn).toList();
  QVERIFY(!nextColors.isEmpty());
  QCOMPARE(mergeTargetColor, nextColors.constFirst().value<QColor>());
  QVERIFY(stashUsesSourceColor);
  QVERIFY(divergenceColor.isValid());
  QVERIFY(divergenceContinuesStraight);
  QVERIFY(forkStartsAtParent);
  QVERIFY(forkReachesDeferredLane);
  QVERIFY(!forkColors.isEmpty());
  for (const QColor &color : forkColors)
    QCOMPARE(color, divergenceColor);

  int laneWidth = qMax(compactMetrics.ascent(), 20);
  int graphMinimum = 50;
  for (int row = 0; row < graphModel->rowCount(); ++row) {
    int lanes = graphModel->index(row, 0)
                    .data(CommitList::GraphRole)
                    .toList()
                    .size();
    graphMinimum = qMax(graphMinimum, lanes * laneWidth);
  }
  QVERIFY(header->sectionSize(1) >= graphMinimum);
  header->resizeSection(1, graphMinimum - 10);
  QCOMPARE(header->sectionSize(1), graphMinimum);

  QStandardItemModel wideGraph(1, 1);
  QVariantList wideColumns;
  for (int lane = 0; lane < 60; ++lane)
    wideColumns.append(QVariant(QVariantList()));
  int referencesWidthBeforeGraphGrowth = header->sectionSize(0);
  int authorWidthBeforeGraphGrowth = header->sectionSize(3);
  int dateWidthBeforeGraphGrowth = header->sectionSize(4);
  int idWidthBeforeGraphGrowth = header->sectionSize(5);
  QModelIndex wideIndex = wideGraph.index(0, 0);
  wideGraph.setData(wideIndex, wideColumns, CommitList::GraphRole);
  wideGraph.setData(wideIndex, wideColumns, CommitList::GraphColorRole);
  wideGraph.setData(wideIndex, wideColumns, CommitList::GraphStyleRole);
  commitList->setModel(&wideGraph);
  QCoreApplication::processEvents();
  QCOMPARE(header->sectionSize(0), referencesWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(2), header->minimumSectionSize());
  QCOMPARE(header->sectionSize(3), authorWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(4), dateWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(5), idWidthBeforeGraphGrowth);
  QVERIFY(commitList->horizontalScrollBar()->maximum() > 0);
  commitList->horizontalScrollBar()->setValue(
      commitList->horizontalScrollBar()->maximum());
  QCOMPARE(header->offset(), commitList->horizontalScrollBar()->value());
  commitList->setModel(graphModel);
  QCoreApplication::processEvents();
  QCOMPARE(commitList->horizontalScrollBar()->maximum(), 0);

  Settings::instance()->setValue(Setting::Id::ShowCommitsInCompactMode, compact);
  commitList->resetSettings();

  auto graphStashes = [graphModel] {
    while (graphModel->canFetchMore(QModelIndex()))
      graphModel->fetchMore(QModelIndex());

    QModelIndexList result;
    for (int row = 0; row < graphModel->rowCount(); ++row) {
      QModelIndex index = graphModel->index(row, 0);
      if (index.data(CommitList::StashIndexRole).isValid())
        result.append(index);
    }
    return result;
  };
  QTRY_COMPARE(graphStashes().size(), 2);

  const QList<git::Commit> expectedStashes = repo->stashes();
  QSet<git::Id> auxiliaryCommits;
  for (const git::Commit &expected : expectedStashes) {
    const QList<git::Commit> parents = expected.parents();
    for (int i = 1; i < parents.size(); ++i)
      auxiliaryCommits.insert(parents.at(i).id());
  }

  for (const QModelIndex &index : graphStashes()) {
    int stashIndex = index.data(CommitList::StashIndexRole).toInt();
    git::Commit commit = index.data(CommitList::CommitRole).value<git::Commit>();
    QCOMPARE(commit.id(), expectedStashes.at(stashIndex).id());
    QCOMPARE(index.data(CommitList::GraphNodeRole)
                 .value<CommitList::GraphNode>(),
             CommitList::GraphNode::Stash);

    bool dotted = false;
    const QVariantList columns =
        index.data(CommitList::GraphStyleRole).toList();
    for (const QVariant &column : columns) {
      for (const QVariant &style : column.toList())
        dotted |= style.toInt() == Qt::DotLine;
    }
    QVERIFY(dotted);
  }

  for (int row = 0; row < graphModel->rowCount(); ++row) {
    git::Commit commit = graphModel->index(row, 0)
                             .data(CommitList::CommitRole)
                             .value<git::Commit>();
    QVERIFY(!commit.isValid() || !auxiliaryCommits.contains(commit.id()));
  }

  git::Id stashBase = expectedStashes.first().parents().first().id();
  QModelIndex baseIndex;
  for (int row = 0; row < graphModel->rowCount(); ++row) {
    QModelIndex index = graphModel->index(row, 0);
    git::Commit commit = index.data(CommitList::CommitRole).value<git::Commit>();
    if (commit.isValid() && commit.id() == stashBase) {
      baseIndex = index;
      break;
    }
  }
  QVERIFY(baseIndex.isValid());
  QSet<int> baseStyles;
  for (const QVariant &column :
       baseIndex.data(CommitList::GraphStyleRole).toList()) {
    for (const QVariant &style : column.toList())
      baseStyles.insert(style.toInt());
  }
  QVERIFY(baseStyles.contains(Qt::DotLine));
  QVERIFY(baseStyles.contains(Qt::SolidLine));

  bool passedHead = false;
  git::Id head = repo->head().target().id();
  for (int row = 0; row < graphModel->rowCount(); ++row) {
    QModelIndex index = graphModel->index(row, 0);
    git::Commit commit =
        index.data(CommitList::CommitRole).value<git::Commit>();
    if (passedHead) {
      for (const QVariant &column :
           index.data(CommitList::GraphColorRole).toList()) {
        for (const QVariant &color : column.toList())
          QVERIFY(color.value<QColor>() != QColor(Qt::gray));
      }
    }
    if (commit.isValid() && commit.id() == head)
      passedHead = true;
  }
  QVERIFY(passedHead);

  QCOMPARE(contextMenuItems(commitList, graphStashes().first()),
           QStringList({"Apply", "Pop", "Drop"}));

  config.setValue(ConfigKeys::kRefsKey,
                  static_cast<int>(CommitList::RefsFilter::SelectedRef));
  commitList->resetSettings();
  QTRY_COMPARE(graphStashes().size(), 0);
  config.setValue(ConfigKeys::kRefsKey,
                  static_cast<int>(CommitList::RefsFilter::AllRefs));
  commitList->resetSettings();
  QTRY_COMPARE(graphStashes().size(), 2);

  QModelIndex stashes = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Stashes);
  QCOMPARE(navigator->model()->rowCount(stashes), 2);
  QModelIndex stash = navigator->model()->index(0, 0, stashes);
  git::Commit expected =
      stash.data(RepositoryNavigatorModel::CommitRole).value<git::Commit>();
  QVERIFY(expected.isValid());

  QVERIFY(QMetaObject::invokeMethod(navigator->view(), "clicked",
                                    Q_ARG(QModelIndex, stash)));
  QTRY_VERIFY(!window.currentView()->commits().isEmpty());
  QCOMPARE(window.currentView()->commits().first().id(), expected.id());

  window.currentView()->dropStash(0);
  QTRY_COMPARE(navigator->model()->rowCount(stashes), 1);
  QTRY_COMPARE(graphStashes().size(), 1);
}

void TestRepositorySideBar::submoduleInteraction() {
  Test::ScratchRepository child;
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

  Test::ScratchRepository parent;
  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"-c", "protocol.file.allow=always", "submodule", "add",
             child->workdir().path(), "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  QString childBranch = child->head().name();
  git.start(GIT_EXECUTABLE,
            {"submodule", "set-branch", "--branch", childBranch, "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"add", ".gitmodules"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "add submodule"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  QVERIFY(childFile.open(QIODevice::Append));
  childFile.write("origin change\n");
  childFile.close();
  git.setWorkingDirectory(child->workdir().path());
  git.start(GIT_EXECUTABLE, {"commit", "-am", "origin change"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  Settings::instance()->setValue(Setting::Id::OpenSubmodulesInTabs, true);
  MainWindow window(parent);
  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);

  QModelIndex submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  navigator->view()->setExpanded(submodules, true);
  QCOMPARE(navigator->model()->rowCount(submodules), 1);
  QModelIndex submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PathRole).toString(),
           QString("child"));
  QVERIFY(submodule.data(RepositoryNavigatorModel::InitializedRole).toBool());
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PinnedAheadRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PinnedBehindRole).toInt(),
           0);
  QVERIFY(!submodule.data(RepositoryNavigatorModel::OriginAheadRole).isValid());
  QVERIFY(
      !submodule.data(RepositoryNavigatorModel::OriginBehindRole).isValid());
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginStateRole)
               .value<RepositoryNavigatorModel::OriginState>(),
           RepositoryNavigatorModel::OriginState::Pending);
  QVERIFY(submodule.data(Qt::ToolTipRole).toString().contains(
      "matches the commit recorded by the parent repository"));
  QVERIFY(submodule.data(Qt::ToolTipRole).toString().contains(
      "Waiting for a submodule update check"));
  QString tooltip = submodule.data(Qt::ToolTipRole).toString();
  QVERIFY(tooltip.startsWith("<qt>"));
  QVERIFY(tooltip.contains("color:#36c96b"));
  QVERIFY(tooltip.contains("Left icon: Pin summary."));
  QVERIFY(tooltip.contains("Pin delta"));
  QVERIFY(tooltip.contains("Origin delta"));

  git::Submodule selected =
      submodule.data(RepositoryNavigatorModel::SubmoduleRole)
          .value<git::Submodule>();
  git::Submodule::UpdateStatus synchronized;
  synchronized.name = "child";
  synchronized.branch = childBranch;
  synchronized.targetId = selected.workdirId();
  navigator->model()->setSubmoduleUpdateStatuses({synchronized});
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginStateRole)
               .value<RepositoryNavigatorModel::OriginState>(),
           RepositoryNavigatorModel::OriginState::Ready);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginAheadRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginBehindRole).toInt(),
           0);
  navigator->model()->setSubmoduleUpdateStatuses({});

  RepoView *parentView = window.currentView();
  bool statusesChanged = false;
  connect(parentView, &RepoView::submoduleUpdateStatusesChanged,
          [&statusesChanged] { statusesChanged = true; });
  parentView->checkSubmoduleUpdates();
  QTRY_VERIFY(statusesChanged);
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginAheadRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginBehindRole).toInt(),
           1);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginStateRole)
               .value<RepositoryNavigatorModel::OriginState>(),
           RepositoryNavigatorModel::OriginState::Ready);

  git::Submodule::UpdateStatus failed;
  failed.name = "child";
  failed.branch = childBranch;
  failed.state = git::Submodule::UpdateStatus::Error;
  failed.message = "test failure";
  navigator->model()->setSubmoduleUpdateStatuses({failed});
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginStateRole)
               .value<RepositoryNavigatorModel::OriginState>(),
           RepositoryNavigatorModel::OriginState::Failed);
  QVERIFY(submodule.data(Qt::ToolTipRole).toString().contains(
      "comparison failed - test failure"));
  navigator->model()->setSubmoduleUpdateStatuses(
      parentView->submoduleUpdateStatuses());

  QFile localFile(parent->workdir().filePath("child/local.txt"));
  QVERIFY(localFile.open(QIODevice::WriteOnly));
  localFile.write("local change\n");
  localFile.close();
  git.setWorkingDirectory(parent->workdir().filePath("child"));
  git.start(GIT_EXECUTABLE, {"add", "local.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "local change"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  parent->invalidateSubmoduleCache();
  navigator->model()->refresh();
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PinnedAheadRole).toInt(),
           1);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PinnedBehindRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginAheadRole).toInt(),
           1);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginBehindRole).toInt(),
           1);
  QVERIFY(submodule.data(Qt::ToolTipRole).toString().contains(
      "have diverged (1 commit ahead, 1 commit behind)"));

  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"submodule", "set-branch", "--default", "child"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  parent->invalidateSubmoduleCache();
  navigator->model()->refresh();
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginStateRole)
               .value<RepositoryNavigatorModel::OriginState>(),
           RepositoryNavigatorModel::OriginState::Hidden);
  QVERIFY(!submodule.data(RepositoryNavigatorModel::OriginAheadRole).isValid());
  QVERIFY(submodule.data(Qt::ToolTipRole).toString().contains(
      "Not shown because no remote branch is configured"));

  QList<QPair<QString, bool>> menu =
      contextMenuItems(navigator->view(), submodule);
  QCOMPARE(menuTexts(menu),
           QStringList({"Open", "Update", "Modify...",
                        "Delete Submodule..."}));
  for (const auto &item : menu)
    QVERIFY(item.second);

  selected = submodule.data(RepositoryNavigatorModel::SubmoduleRole)
                 .value<git::Submodule>();
  parentView->promptToDeleteSubmodule(selected);
  QTRY_VERIFY(QApplication::activeModalWidget());
  QMessageBox *confirmation =
      qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
  QVERIFY(confirmation);
  QCOMPARE(confirmation->windowTitle(), QString("Delete Submodule?"));
  QVERIFY(confirmation->defaultButton() ==
          confirmation->button(QMessageBox::Cancel));
  confirmation->button(QMessageBox::Cancel)->click();
  QVERIFY(QFileInfo::exists(parent->workdir().filePath("child")));

  QFile dirty(parent->workdir().filePath("child/child.txt"));
  QVERIFY(dirty.open(QIODevice::Append));
  dirty.write("dirty\n");
  dirty.close();
  parentView->promptToDeleteSubmodule(selected);
  QTRY_VERIFY(QApplication::activeModalWidget());
  confirmation =
      qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
  QVERIFY(confirmation);
  QVERIFY(confirmation->informativeText().contains("uncommitted changes"));
  confirmation->button(QMessageBox::Cancel)->click();

  QVERIFY(QMetaObject::invokeMethod(navigator->view(), "doubleClicked",
                                    Q_ARG(QModelIndex, submodule)));
  QTRY_COMPARE(window.count(), 2);
  QCOMPARE(window.currentView()->repo().workdir().path(),
           parent->workdir().filePath("child"));
}

void TestRepositorySideBar::cleanupTestCase() { mWindow->close(); }

int main(int argc, char *argv[]) {
  QTemporaryDir settings;
  if (!settings.isValid())
    return 1;

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  qunsetenv("GITTYUP_OAUTH");

  Application::setInTest();
  int testArgc = argc;
  auto app = Test::createApp(argc, argv);
  TestRepositorySideBar test;
  return QTest::qExec(&test, testArgc, argv);
}

#include "repository_sidebar.moc"

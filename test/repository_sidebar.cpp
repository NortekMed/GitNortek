//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "app/Application.h"
#include "app/Theme.h"
#include "conf/Setting.h"
#include "conf/Settings.h"
#include "dialogs/WorktreeDialog.h"
#include "git/Branch.h"
#include "git/Config.h"
#include "git/Result.h"
#include "git/TagRef.h"
#include "git/Tree.h"
#include "ui/CommitList.h"
#include "ui/ConfigKeys.h"
#include "ui/Footer.h"
#include "ui/FontUtils.h"
#include "ui/MainWindow.h"
#include "ui/RepositoryNavigator.h"
#include "ui/RepositoryNavigatorModel.h"
#include "ui/ReferenceList.h"
#include "ui/RepoView.h"
#include "ui/SearchField.h"
#include "ui/SideBar.h"
#include "ui/StatePushButton.h"
#include "ui/TabWidget.h"
#include "ui/ToolBar.h"
#include "ui/WorktreeIcon.h"
#include <QAbstractButton>
#include <QAbstractItemModelTester>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QFile>
#include <QFontMetrics>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLayout>
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
#include <QSplitterHandle>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

using namespace QTest;

namespace {

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

bool triggerContextMenuItem(QTreeView *view, const QModelIndex &index,
                            const QString &text) {
  bool triggered = false;
  view->scrollTo(index);
  QPoint point = view->visualRect(index).center();
  QTimer::singleShot(0, [&triggered, text] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    for (QAction *action : menu->actions()) {
      if (action->text() == text) {
        triggered = true;
        menu->close();
        action->trigger();
        return;
      }
    }
    menu->close();
  });
  QMetaObject::invokeMethod(view, "customContextMenuRequested",
                            Qt::DirectConnection, Q_ARG(QPoint, point));
  return triggered;
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

QStringList contextSubmenuItems(CommitList *view, const QModelIndex &index,
                                const QString &submenuText) {
  QStringList items;
  view->scrollTo(index);
  QPoint viewportPoint = view->visualRect(index).center();
  QPoint globalPoint = view->viewport()->mapToGlobal(viewportPoint);
  QTimer::singleShot(0, [&items, submenuText] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    for (QAction *action : menu->actions()) {
      if (action->text() != submenuText || !action->menu())
        continue;
      for (QAction *submenuAction : action->menu()->actions())
        items.append(submenuAction->text());
      break;
    }
    menu->close();
  });
  QContextMenuEvent event(QContextMenuEvent::Mouse, viewportPoint, globalPoint);
  QApplication::sendEvent(view->viewport(), &event);
  return items;
}

QModelIndex commitIndex(QAbstractItemModel *model, const git::Id &id) {
  for (int row = 0; row < model->rowCount(); ++row) {
    QModelIndex index = model->index(row, 0);
    git::Commit commit =
        index.data(CommitList::CommitRole).value<git::Commit>();
    if (commit.isValid() && commit.id() == id)
      return index;
  }
  return QModelIndex();
}

QColor graphNodeColor(const QModelIndex &index, int role) {
  constexpr int dotSegment = 0;
  QVariantList columns = index.data(CommitList::GraphRole).toList();
  QVariantList colorColumns = index.data(role).toList();
  for (int column = 0; column < columns.size(); ++column) {
    QVariantList segments = columns.at(column).toList();
    int dot = segments.indexOf(dotSegment);
    if (dot >= 0)
      return colorColumns.at(column).toList().at(dot).value<QColor>();
  }
  return QColor();
}

QColor branchBadgeColor(CommitList *view, QHeaderView *header,
                        const QModelIndex &index) {
  view->scrollTo(index, QAbstractItemView::PositionAtCenter);
  QCoreApplication::processEvents();
  QPoint sample(view->visualRect(index).x() + 8 +
                    header->sectionPosition(CommitList::ReferencesColumn) + 6,
                view->visualRect(index).center().y());
  return view->viewport()->grab().toImage().pixelColor(sample);
}

bool referenceBadgesContainColor(CommitList *view, QHeaderView *header,
                                 const QModelIndex &index,
                                 const QColor &color) {
  view->scrollTo(index, QAbstractItemView::PositionAtCenter);
  QCoreApplication::processEvents();
  QImage image = view->viewport()->grab().toImage();
  QRect row = view->visualRect(index);
  QRect refs(
      row.x() + 8 + header->sectionPosition(CommitList::ReferencesColumn),
      row.y(), header->sectionSize(CommitList::ReferencesColumn), row.height());
  refs = refs.intersected(image.rect());
  for (int y = refs.top(); y <= refs.bottom(); ++y) {
    for (int x = refs.left(); x <= refs.right(); ++x) {
      if (image.pixelColor(x, y) == color)
        return true;
    }
  }
  return false;
}

bool isGreenBranchColor(const QColor &color) {
  QColor hsv = color.toHsv();
  int hue = hsv.hsvHue();
  return hue >= 75 && hue <= 175 && hsv.hsvSaturation() >= 20;
}

int runGit(const git::Repository &repo, const QStringList &arguments) {
  QProcess git;
  git.setWorkingDirectory(repo.workdir().path());
  git.start(GIT_EXECUTABLE, arguments);
  if (!git.waitForFinished())
    return -1;
  return git.exitCode();
}

bool writeFile(const git::Repository &repo, const QString &name,
               const QByteArray &contents) {
  QFile file(repo.workdir().filePath(name));
  if (!file.open(QIODevice::WriteOnly))
    return false;
  return file.write(contents) == contents.size();
}

QPushButton *messageButton(QMessageBox *dialog, const QString &text) {
  for (QAbstractButton *button : dialog->buttons()) {
    if (button->text() == text)
      return qobject_cast<QPushButton *>(button);
  }
  return nullptr;
}

} // namespace

class TestRepositorySideBar : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void sidebarVisibility();
  void navigatorModel();
  void navigatorView();
  void submoduleExpansionSizing();
  void githubIssuesModel();
  void githubIssuesRemoteFilter();
  void activeRepositoryBinding();
  void checkoutKeepsNonConflictingChanges();
  void checkoutConflictStash();
  void checkoutConflictCommit();
  void branchGraphColors();
  void stashInteraction();
  void historyPrefetch();
  void tagPushToOrigin();
  void submoduleInteraction();
  void submoduleInitialization();
  void worktreeSubmoduleInitialization();
  void worktreeDeletion();
  void worktreeTabs();
  void cleanupTestCase();

private:
  MainWindow *mWindow = nullptr;
  Footer *mFooter = nullptr;
  Test::ScratchRepository mRepo;
};

void TestRepositorySideBar::initTestCase() {
  mWindow = new MainWindow(git::Repository());
  SideBar *sideBar = mWindow->findChild<SideBar *>();
  QVERIFY(sideBar);

  mFooter = sideBar->findChild<Footer *>("RepositoryFooter");
  QVERIFY(mFooter);
  QVERIFY(!sideBar->findChild<QTreeView *>("RepositoryTree"));
  QVERIFY(!sideBar->findChild<QSplitter *>("RepositorySidebarContent"));
  QToolButton *add = mFooter->findChild<QToolButton *>("Add");
  QToolButton *remove = mFooter->findChild<QToolButton *>("Remove");
  QToolButton *options = mFooter->findChild<QToolButton *>("Options");
  QVERIFY(add);
  QVERIFY(remove);
  QVERIFY(options);
  QVERIFY(add->isVisibleTo(mFooter));
  QVERIFY(!remove->isVisibleTo(mFooter));
  QVERIFY(!options->isVisibleTo(mFooter));
  QVERIFY(add->menu());
  QStringList actions;
  for (QAction *action : add->menu()->actions())
    actions.append(action->text());
  QCOMPARE(actions, QStringList({"Clone Repository", "Open Existing Repository",
                                 "Initialize New Repository"}));
}

void TestRepositorySideBar::sidebarVisibility() {
  QSettings settings;
  bool hadSidebarSetting = settings.contains("sidebar");
  QVariant sidebarSetting = settings.value("sidebar");
  auto restoreSidebarSetting = qScopeGuard([hadSidebarSetting, sidebarSetting] {
    QSettings settings;
    if (hadSidebarSetting)
      settings.setValue("sidebar", sidebarSetting);
    else
      settings.remove("sidebar");
  });
  settings.setValue("sidebar", false);

  MainWindow window{git::Repository()};
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  QVERIFY(window.isSideBarVisible());
  QSplitter *splitter = qobject_cast<QSplitter *>(window.tabWidget()->parent());
  QVERIFY(splitter);
  QVERIFY(splitter->sizes().constFirst() > 0);

  QToolButton *sidebarButton = nullptr;
  for (QToolButton *button : window.findChildren<QToolButton *>()) {
    if (button->toolTip() == "Show repository sidebar") {
      sidebarButton = button;
      break;
    }
  }
  QVERIFY(sidebarButton);
  sidebarButton->click();
  QVERIFY(!window.isSideBarVisible());
  sidebarButton->click();
  QVERIFY(window.isSideBarVisible());
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
  QVERIFY(mRepo->addRemote("origin", mRepo->workdir().path()).isValid());
  git.start(GIT_EXECUTABLE, {"update-ref", "refs/remotes/origin/main", "HEAD"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"update-ref", "refs/remotes/origin/NOTHEAD", "HEAD"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"symbolic-ref", "refs/remotes/origin/HEAD",
                             "refs/remotes/origin/main"});
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
  QVERIFY(mRepo->lookupRef("refs/remotes/origin/HEAD").isRemoteHead());
  QVERIFY(!mRepo->lookupRef("refs/remotes/origin/NOTHEAD").isRemoteHead());
  main.setUpstream(upstream);

  RepositoryNavigatorModel model;
  QAbstractItemModelTester tester(
      &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
  model.setRepository(mRepo);

  QCOMPARE(model.rowCount(),
           static_cast<int>(RepositoryNavigatorModel::Section::Count));
  const QStringList sections = {
      "Local",         "Remote",        "Worktrees", "Stashes", "Cloud Patches",
      "Pull Requests", "GitHub Issues", "Teams",     "Tags",    "Submodules"};
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
  QVERIFY(local.data(RepositoryNavigatorModel::AvailableRole).toBool());
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
  QCOMPARE(model.rowCount(remotes), 2);
  QVERIFY(remotes.data(RepositoryNavigatorModel::AvailableRole).toBool());
  QStringList remoteNames;
  for (int row = 0; row < model.rowCount(remotes); ++row)
    remoteNames.append(model.index(row, 0, remotes).data().toString());
  QVERIFY(remoteNames.contains("origin/main"));
  QVERIFY(remoteNames.contains("origin/NOTHEAD"));
  QVERIFY(!remoteNames.contains("origin/HEAD"));

  QModelIndex worktrees =
      model.sectionIndex(RepositoryNavigatorModel::Section::Worktrees);
  QCOMPARE(model.rowCount(worktrees), 1);
  QVERIFY(worktrees.data(RepositoryNavigatorModel::AvailableRole).toBool());
  QModelIndex home = model.index(0, 0, worktrees);
  QCOMPARE(home.data().toString(), main.name());
  QCOMPARE(static_cast<RepositoryNavigatorModel::ItemKind>(
               home.data(RepositoryNavigatorModel::ItemKindRole).toInt()),
           RepositoryNavigatorModel::ItemKind::Worktree);
  QCOMPARE(home.data(RepositoryNavigatorModel::PathRole).toString(),
           mRepo->workdir().path());
  QVERIFY(home.data(RepositoryNavigatorModel::CurrentRole).toBool());
  QVERIFY(home.data(RepositoryNavigatorModel::MainWorktreeRole).toBool());
  QVERIFY(home.data(RepositoryNavigatorModel::AvailableRole).toBool());

  QVERIFY(mRepo->createBranch("notified", head).isValid());
  QTRY_COMPARE(model.rowCount(local), 3);

  QModelIndex stashes =
      model.sectionIndex(RepositoryNavigatorModel::Section::Stashes);
  QCOMPARE(model.rowCount(stashes), 1);
  QVERIFY(stashes.data(RepositoryNavigatorModel::AvailableRole).toBool());
  QModelIndex stash = model.index(0, 0, stashes);
  QCOMPARE(stash.data(RepositoryNavigatorModel::StashIndexRole).toInt(), 0);
  QVERIFY(stash.data(RepositoryNavigatorModel::CommitRole)
              .value<git::Commit>()
              .isValid());

  QModelIndex tags =
      model.sectionIndex(RepositoryNavigatorModel::Section::Tags);
  QCOMPARE(model.rowCount(tags), 1);
  QVERIFY(tags.data(RepositoryNavigatorModel::AvailableRole).toBool());
  QCOMPARE(model.index(0, 0, tags).data().toString(), QString("v1"));

  QModelIndex submodules =
      model.sectionIndex(RepositoryNavigatorModel::Section::Submodules);
  QCOMPARE(model.rowCount(submodules), 0);
  QVERIFY(!submodules.data(RepositoryNavigatorModel::AvailableRole).toBool());

  model.clear();
  QCOMPARE(model.rowCount(), sections.size());
  QCOMPARE(model.rowCount(
               model.sectionIndex(RepositoryNavigatorModel::Section::Local)),
           0);

  Test::ScratchRepository emptyRepo;
  RepositoryNavigatorModel emptyModel;
  emptyModel.setRepository(emptyRepo);
  for (RepositoryNavigatorModel::Section section :
       {RepositoryNavigatorModel::Section::Local,
        RepositoryNavigatorModel::Section::Remote,
        RepositoryNavigatorModel::Section::Stashes,
        RepositoryNavigatorModel::Section::Tags,
        RepositoryNavigatorModel::Section::Submodules}) {
    QModelIndex index = emptyModel.sectionIndex(section);
    QCOMPARE(emptyModel.rowCount(index), 0);
    QVERIFY(!index.data(RepositoryNavigatorModel::AvailableRole).toBool());
  }
  QModelIndex emptyWorktrees =
      emptyModel.sectionIndex(RepositoryNavigatorModel::Section::Worktrees);
  QCOMPARE(emptyModel.rowCount(emptyWorktrees), 1);
  QVERIFY(
      emptyWorktrees.data(RepositoryNavigatorModel::AvailableRole).toBool());
}

void TestRepositorySideBar::navigatorView() {
  const QStringList collapsibleSections = {
      "Local",        "Remote", "Worktrees", "Stashes",
      "GitHubIssues", "Tags",   "Submodules"};
  QStringList settingKeys;
  for (const QString &section : collapsibleSections)
    settingKeys.append("sidebar/repositoryNavigator/expanded/" + section);
  QSettings settings;
  QSet<QString> existingKeys;
  QHash<QString, QVariant> settingValues;
  for (const QString &key : settingKeys) {
    if (settings.contains(key)) {
      existingKeys.insert(key);
      settingValues.insert(key, settings.value(key));
    }
  }
  auto restoreExpansion =
      qScopeGuard([existingKeys, settingKeys, settingValues] {
        QSettings settings;
        for (const QString &key : settingKeys) {
          if (existingKeys.contains(key))
            settings.setValue(key, settingValues.value(key));
          else
            settings.remove(key);
        }
      });
  for (const QString &section : collapsibleSections)
    settings.setValue("sidebar/repositoryNavigator/expanded/" + section, true);
  settings.setValue("sidebar/repositoryNavigator/expanded/GitHubIssues", false);

  QWidget host;
  RepositoryNavigator navigator(&host);
  QVBoxLayout *hostLayout = new QVBoxLayout(&host);
  hostLayout->setContentsMargins(0, 0, 0, 0);
  hostLayout->addWidget(&navigator);
  QTreeView *view =
      navigator.sectionView(RepositoryNavigatorModel::Section::Local);
  QVERIFY(view);
  QCOMPARE(view->objectName(), QString("RepositoryNavigationLocalView"));
  QVERIFY(view->focusPolicy() != Qt::NoFocus);
  QVERIFY(view->itemDelegate());
  QFont bodyFont = view->font();
  bodyFont.setPointSize(FontUtils::pointSize(bodyFont) + 2);
  navigator.setBodyFont(bodyFont);
  QCOMPARE(FontUtils::pointSize(view->font()), FontUtils::pointSize(bodyFont));
  RepositoryNavigatorModel *model = navigator.model();
  QModelIndex local =
      model->sectionIndex(RepositoryNavigatorModel::Section::Local);
  QCOMPARE(view->rootIndex(), local);
  QCOMPARE(view->verticalScrollMode(), QAbstractItemView::ScrollPerPixel);
  for (RepositoryNavigatorModel::Section section :
       {RepositoryNavigatorModel::Section::Local,
        RepositoryNavigatorModel::Section::Remote,
        RepositoryNavigatorModel::Section::Worktrees,
        RepositoryNavigatorModel::Section::Stashes,
        RepositoryNavigatorModel::Section::GitHubIssues,
        RepositoryNavigatorModel::Section::Tags,
        RepositoryNavigatorModel::Section::Submodules}) {
    QTreeView *sectionView = navigator.sectionView(section);
    QVERIFY(sectionView);
    QCOMPARE(sectionView->rootIndex(), model->sectionIndex(section));
    QVERIFY(sectionView->verticalScrollBar() != view->verticalScrollBar() ||
            section == RepositoryNavigatorModel::Section::Local);
  }
  QVERIFY(
      !navigator.sectionView(RepositoryNavigatorModel::Section::CloudPatches));
  for (const QString &section : QStringList(
           {"Local", "Remote", "Worktrees", "Stashes", "CloudPatches",
            "PullRequests", "GitHubIssues", "Teams", "Tags", "Submodules"})) {
    QVERIFY(navigator.findChild<QWidget *>("RepositoryNavigation" + section +
                                           "Icon"));
  }
  QSplitter *splitter =
      navigator.findChild<QSplitter *>("RepositorySectionSplitter");
  QVERIFY(splitter);
  QCOMPARE(splitter->orientation(), Qt::Vertical);
  QCOMPARE(splitter->count(),
           static_cast<int>(RepositoryNavigatorModel::Section::Count));
  QWidget *actionBar = navigator.findChild<QWidget *>(
      "RepositoryNavigationActionBar", Qt::FindDirectChildrenOnly);
  QVERIFY(actionBar);
  StatePushButton *expandCollapseAll = actionBar->findChild<StatePushButton *>(
      "RepositoryNavigationExpandCollapseAll");
  QVERIFY(expandCollapseAll);
  QCOMPARE(navigator.layout()->itemAt(0)->widget(), actionBar);
  QCOMPARE(navigator.layout()->itemAt(1)->widget(), splitter);
  QCOMPARE(expandCollapseAll->text(), QString("Expand all"));
  QCOMPARE(expandCollapseAll->accessibleName(), QString("Expand all"));
  QVERIFY(!expandCollapseAll->isEnabled());
  host.resize(320, 700);
  host.show();
  QVERIFY(qWaitForWindowExposed(&host));

  const QStringList availableSections = {"Local", "Worktrees"};
  QToolButton *localToggle =
      navigator.findChild<QToolButton *>("RepositoryNavigationLocalToggle");
  QVERIFY(localToggle);
  QCOMPARE(localToggle->accessibleName(), QString("Toggle Local"));
  QToolButton *worktreeAdd =
      navigator.findChild<QToolButton *>("RepositoryNavigationWorktreesAdd");
  QVERIFY(worktreeAdd);
  QVERIFY(!worktreeAdd->isEnabled());
  QToolButton *issuesToggle = navigator.findChild<QToolButton *>(
      "RepositoryNavigationGitHubIssuesToggle");
  QVERIFY(issuesToggle);
  QVERIFY(!issuesToggle->isEnabled());
  QVERIFY(!issuesToggle->isChecked());
  QToolButton *stashesToggle =
      navigator.findChild<QToolButton *>("RepositoryNavigationStashesToggle");
  QWidget *stashesHeader =
      navigator.findChild<QWidget *>("RepositoryNavigationStashesHeader");
  QWidget *stashesPanel =
      navigator.findChild<QWidget *>("RepositoryNavigationStashesPanel");
  QVERIFY(stashesToggle);
  QVERIFY(stashesHeader);
  QVERIFY(stashesPanel);
  QVERIFY(!stashesToggle->isEnabled());
  QVERIFY(!stashesToggle->isChecked());
  QCOMPARE(stashesToggle->arrowType(), Qt::RightArrow);

  Test::ScratchRepository emptyRepo;
  git::Repository localRepo = emptyRepo;
  Test::initRepo(localRepo);
  git::Commit initial = localRepo.commit("initial");
  QVERIFY(initial.isValid());
  for (int branch = 1; branch < 5; ++branch)
    QVERIFY(localRepo.createBranch(QString("branch-%1").arg(branch), initial)
                .isValid());
  navigator.setRepository(localRepo);
  QVERIFY(expandCollapseAll->isEnabled());
  QCOMPARE(expandCollapseAll->text(), QString("Collapse all"));
  for (const QString &section :
       QStringList({"Remote", "Stashes", "CloudPatches", "PullRequests",
                    "GitHubIssues", "Teams", "Tags", "Submodules"})) {
    QToolButton *toggle = navigator.findChild<QToolButton *>(
        "RepositoryNavigation" + section + "Toggle");
    QWidget *header = navigator.findChild<QWidget *>("RepositoryNavigation" +
                                                     section + "Header");
    QWidget *panel = navigator.findChild<QWidget *>("RepositoryNavigation" +
                                                    section + "Panel");
    QVERIFY(toggle);
    QVERIFY(header);
    QVERIFY(panel);
    QVERIFY(!toggle->isEnabled());
    QVERIFY(!toggle->isChecked());
    QCOMPARE(toggle->arrowType(), Qt::RightArrow);
    QTest::mouseClick(header, Qt::LeftButton, Qt::NoModifier,
                      header->rect().center());
    QVERIFY(!toggle->isChecked());
    QCOMPARE(panel->height(), header->height());
  }

  expandCollapseAll->click();
  QCoreApplication::processEvents();
  QCOMPARE(expandCollapseAll->text(), QString("Expand all"));
  QTRY_VERIFY(splitter->maximumHeight() <
              navigator.height() - actionBar->height());
  QTRY_COMPARE(splitter->y(), actionBar->geometry().bottom() + 1);
  QTRY_VERIFY(splitter->geometry().bottom() < navigator.rect().bottom());
  for (const QString &section : availableSections) {
    QToolButton *toggle = navigator.findChild<QToolButton *>(
        "RepositoryNavigation" + section + "Toggle");
    QVERIFY(toggle);
    QVERIFY(!toggle->isChecked());
    QCOMPARE(QSettings()
                 .value("sidebar/repositoryNavigator/expanded/" + section)
                 .toBool(),
             false);
  }
  QVERIFY(!issuesToggle->isChecked());

  QWidget *localPanel =
      navigator.findChild<QWidget *>("RepositoryNavigationLocalPanel");
  QWidget *localHeader =
      navigator.findChild<QWidget *>("RepositoryNavigationLocalHeader");
  QWidget *lastPanel =
      navigator.findChild<QWidget *>("RepositoryNavigationSubmodulesPanel");
  QVERIFY(localPanel);
  QVERIFY(localHeader);
  QVERIFY(lastPanel);
  QTRY_COMPARE(localPanel->y(), 0);
  QTRY_COMPARE(lastPanel->geometry().bottom(), splitter->rect().bottom());

  localToggle->click();
  QTRY_COMPARE(splitter->height(), splitter->maximumHeight());
  QTRY_VERIFY(splitter->maximumHeight() <
              navigator.height() - actionBar->height());
  QTRY_VERIFY(splitter->geometry().bottom() < navigator.rect().bottom());
  QTRY_VERIFY(localPanel->height() > localHeader->height());
  QTreeView *localView =
      navigator.sectionView(RepositoryNavigatorModel::Section::Local);
  QVERIFY(localView);
  QModelIndex localRoot = localView->rootIndex();
  QCOMPARE(navigator.model()->rowCount(localRoot), 5);
  QTRY_COMPARE(localView->verticalScrollBar()->maximum(), 0);
  QModelIndex fifthBranch = navigator.model()->index(4, 0, localRoot);
  QTRY_VERIFY(localView->visualRect(fifthBranch).bottom() <=
              localView->viewport()->rect().bottom());
  QWidget *localSpacer =
      navigator.findChild<QWidget *>("RepositoryNavigationLocalSpacer");
  QVERIFY(localSpacer);
  QCOMPARE(localSpacer->height(), 24);
  QTRY_COMPARE(localPanel->height(), localHeader->height() +
                                         localView->height() +
                                         localSpacer->height());

  QImage headerImage(localHeader->size(), QImage::Format_ARGB32_Premultiplied);
  headerImage.fill(Qt::transparent);
  localHeader->render(&headerImage);
  QCOMPARE(headerImage.pixelColor(localHeader->width() / 2, 0),
           localHeader->palette()
               .color(QPalette::Active, QPalette::Text)
               .darker(200));

  QLabel *localTitle =
      navigator.findChild<QLabel *>("RepositoryNavigationLocalTitle");
  QVERIFY(localTitle);
  QCOMPARE(localTitle->foregroundRole(), QPalette::Text);
  QCOMPARE(localTitle->palette().color(QPalette::Inactive, QPalette::Text),
           localView->palette().color(QPalette::Active, QPalette::Text));

  QSplitterHandle *menuDivider = splitter->handle(1);
  QVERIFY(menuDivider);
  QImage dividerImage(menuDivider->size(), QImage::Format_ARGB32_Premultiplied);
  dividerImage.fill(Qt::transparent);
  menuDivider->render(&dividerImage);
  QCOMPARE(dividerImage.pixelColor(menuDivider->width() / 2,
                                   menuDivider->height() / 2),
           menuDivider->palette()
               .color(QPalette::Active, QPalette::Text)
               .darker(200));
  QCOMPARE(dividerImage.pixelColor(menuDivider->width() / 2,
                                   menuDivider->height() - 1),
           menuDivider->palette().color(QPalette::Window));
  const int fiveRowHeight = localView->height();

  QVERIFY(localRepo.createBranch("branch-5", initial).isValid());
  navigator.setRepository(localRepo);
  localRoot = localView->rootIndex();
  QCOMPARE(navigator.model()->rowCount(localRoot), 6);
  QTRY_COMPARE(localView->height(), fiveRowHeight);
  QTRY_VERIFY(localView->verticalScrollBar()->maximum() > 0);
  for (const QString &section : QStringList(
           {"Remote", "Worktrees", "Stashes", "CloudPatches", "PullRequests",
            "GitHubIssues", "Teams", "Tags", "Submodules"})) {
    QWidget *panel = navigator.findChild<QWidget *>("RepositoryNavigation" +
                                                    section + "Panel");
    QWidget *header = navigator.findChild<QWidget *>("RepositoryNavigation" +
                                                     section + "Header");
    QVERIFY(panel);
    QVERIFY(header);
    QCOMPARE(panel->height(), header->height());
  }
  QTRY_COMPARE(lastPanel->geometry().bottom(), splitter->rect().bottom());
  localToggle->click();
  QTRY_VERIFY(splitter->maximumHeight() <
              navigator.height() - actionBar->height());

  expandCollapseAll->click();
  QCoreApplication::processEvents();
  QCOMPARE(expandCollapseAll->text(), QString("Collapse all"));
  QTRY_COMPARE(splitter->height(), splitter->maximumHeight());
  QTRY_VERIFY(splitter->maximumHeight() <
              navigator.height() - actionBar->height());
  QTRY_VERIFY(splitter->geometry().bottom() < navigator.rect().bottom());
  for (const QString &section : availableSections) {
    QToolButton *toggle = navigator.findChild<QToolButton *>(
        "RepositoryNavigation" + section + "Toggle");
    QVERIFY(toggle->isChecked());
  }
  QVERIFY(!issuesToggle->isChecked());

  QTreeView *worktreesView =
      navigator.sectionView(RepositoryNavigatorModel::Section::Worktrees);
  QVERIFY(worktreesView);
  const int desiredHeight = splitter->maximumHeight();
  host.resize(320, actionBar->sizeHint().height() + desiredHeight - 36);
  QToolButton *worktreesToggle =
      navigator.findChild<QToolButton *>("RepositoryNavigationWorktreesToggle");
  QVERIFY(worktreesToggle);
  worktreesToggle->setChecked(false);
  worktreesToggle->setChecked(true);
  QTRY_VERIFY(worktreesView->height() >= worktreesView->sizeHintForRow(0));
  QTRY_VERIFY(localView->height() < fiveRowHeight);
  QTRY_VERIFY(localView->verticalScrollBar()->maximum() > 0);
  host.resize(320, 700);

  localToggle->setChecked(false);
  QCoreApplication::processEvents();
  QCOMPARE(expandCollapseAll->text(), QString("Expand all"));
  QCOMPARE(
      QSettings().value("sidebar/repositoryNavigator/expanded/Local").toBool(),
      false);

  QVERIFY(localHeader);
  QPoint titleCenter =
      localTitle->mapTo(localHeader, localTitle->rect().center());
  QTest::mouseClick(localHeader, Qt::LeftButton, Qt::NoModifier, titleCenter);
  QVERIFY(localToggle->isChecked());
  QTest::mouseClick(localHeader, Qt::LeftButton, Qt::NoModifier, titleCenter);
  QVERIFY(!localToggle->isChecked());

  Test::ScratchRepository stashedRepo;
  QVERIFY(writeFile(stashedRepo, "tracked.txt", "base\n"));
  QCOMPARE(runGit(stashedRepo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(stashedRepo, {"commit", "-m", "base"}), 0);
  QVERIFY(writeFile(stashedRepo, "tracked.txt", "stashed\n"));
  QVERIFY(stashedRepo->stash("navigator stash").isValid());
  navigator.setRepository(stashedRepo);
  QVERIFY(stashesToggle->isEnabled());
  QVERIFY(stashedRepo->dropStash(0));
  QTRY_VERIFY(!stashesToggle->isEnabled());
  QVERIFY(!stashesToggle->isChecked());
  QCOMPARE(stashesToggle->arrowType(), Qt::RightArrow);
  QCOMPARE(stashesPanel->height(), stashesHeader->height());
  QModelIndex stashes =
      model->sectionIndex(RepositoryNavigatorModel::Section::Stashes);
  QVERIFY(!stashes.data(RepositoryNavigatorModel::AvailableRole).toBool());

  navigator.setRepository(mRepo);
  QVERIFY(!localToggle->isChecked());
  QVERIFY(worktreeAdd->isEnabled());

  QSignalSpy openSpy(&navigator, &RepositoryNavigator::openRepositoryRequested);
  QSignalSpy selectSpy(&navigator,
                       &RepositoryNavigator::selectRepositoryRequested);
  QTreeView *worktreeView =
      navigator.sectionView(RepositoryNavigatorModel::Section::Worktrees);
  QVERIFY(worktreeView);
  QCOMPARE(worktreeView->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
  QModelIndex home = model->index(0, 0, worktreeView->rootIndex());
  QVERIFY(QMetaObject::invokeMethod(
      worktreeView, "clicked", Qt::DirectConnection, Q_ARG(QModelIndex, home)));
  QCOMPARE(selectSpy.count(), 1);
  QCOMPARE(selectSpy.takeFirst().at(0).toString(), mRepo->workdir().path());
  QCOMPARE(openSpy.count(), 0);
  QVERIFY(QMetaObject::invokeMethod(worktreeView, "doubleClicked",
                                    Qt::DirectConnection,
                                    Q_ARG(QModelIndex, home)));
  QCOMPARE(openSpy.count(), 1);
  QList<QVariant> openArguments = openSpy.takeFirst();
  QCOMPARE(openArguments.at(0).toString(), mRepo->workdir().path());
  QCOMPARE(openArguments.at(1).toBool(), false);
}

void TestRepositorySideBar::submoduleExpansionSizing() {
  Test::ScratchRepository child;
  QVERIFY(writeFile(child, "child.txt", "child\n"));
  QCOMPARE(runGit(child, {"add", "child.txt"}), 0);
  QCOMPARE(runGit(child, {"commit", "-m", "child"}), 0);

  Test::ScratchRepository parent;
  for (int index = 0; index < 6; ++index) {
    QCOMPARE(runGit(parent,
                    {"-c", "protocol.file.allow=always", "submodule", "add",
                     child->workdir().path(), QString("child-%1").arg(index)}),
             0);
  }
  QCOMPARE(runGit(parent, {"commit", "-am", "add submodules"}), 0);

  QWidget host;
  RepositoryNavigator navigator(&host);
  QVBoxLayout *layout = new QVBoxLayout(&host);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(&navigator);
  host.resize(320, 900);
  host.show();
  QVERIFY(qWaitForWindowExposed(&host));
  navigator.setRepository(parent);

  for (const QString &section : QStringList({"Local", "Worktrees"})) {
    QToolButton *toggle = navigator.findChild<QToolButton *>(
        "RepositoryNavigation" + section + "Toggle");
    QVERIFY(toggle);
    toggle->setChecked(false);
  }

  QToolButton *submodulesToggle = navigator.findChild<QToolButton *>(
      "RepositoryNavigationSubmodulesToggle");
  QTreeView *submodulesView =
      navigator.sectionView(RepositoryNavigatorModel::Section::Submodules);
  QSplitter *splitter =
      navigator.findChild<QSplitter *>("RepositorySectionSplitter");
  QWidget *actionBar = navigator.findChild<QWidget *>(
      "RepositoryNavigationActionBar", Qt::FindDirectChildrenOnly);
  QVERIFY(submodulesToggle);
  QVERIFY(submodulesView);
  QVERIFY(splitter);
  QVERIFY(actionBar);
  submodulesToggle->setChecked(false);
  submodulesToggle->setChecked(true);

  QModelIndex root = submodulesView->rootIndex();
  QCOMPARE(navigator.model()->rowCount(root), 6);
  QModelIndex last = navigator.model()->index(5, 0, root);
  QTRY_COMPARE(submodulesView->verticalScrollBar()->maximum(), 0);
  QTRY_VERIFY(submodulesView->visualRect(last).bottom() <=
              submodulesView->viewport()->rect().bottom());
  const int fullHeight = submodulesView->height();
  const int rowHeight = submodulesView->sizeHintForRow(0);
  QVERIFY(fullHeight >= 6 * rowHeight);

  host.resize(320, actionBar->sizeHint().height() + splitter->maximumHeight() -
                       2 * rowHeight);
  QTRY_VERIFY(submodulesView->height() < fullHeight);
  QTRY_VERIFY(submodulesView->verticalScrollBar()->maximum() > 0);

  host.resize(320, 900);
  QTRY_COMPARE(submodulesView->height(), fullHeight);
  QTRY_COMPARE(submodulesView->verticalScrollBar()->maximum(), 0);
}

void TestRepositorySideBar::githubIssuesModel() {
  RepositoryNavigatorModel model;
  QModelIndex section =
      model.sectionIndex(RepositoryNavigatorModel::Section::GitHubIssues);
  QVERIFY(!section.data(RepositoryNavigatorModel::AvailableRole).toBool());
  QCOMPARE(section.data(RepositoryNavigatorModel::LoadStateRole)
               .value<RepositoryNavigatorModel::LoadState>(),
           RepositoryNavigatorModel::LoadState::Unavailable);

  model.setGitHubIssuesAvailable(true);
  model.beginGitHubIssuesLoad(false);
  section = model.sectionIndex(RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 0);
  QCOMPARE(model.rowCount(section), 2);
  QCOMPARE(static_cast<RepositoryNavigatorModel::ItemKind>(
               model.index(1, 0, section)
                   .data(RepositoryNavigatorModel::ItemKindRole)
                   .toInt()),
           RepositoryNavigatorModel::ItemKind::Status);
  QCOMPARE(section.data(RepositoryNavigatorModel::LoadStateRole)
               .value<RepositoryNavigatorModel::LoadState>(),
           RepositoryNavigatorModel::LoadState::Loading);

  GitHub::Issues issues{{17, "First issue", "alice",
                         QUrl("https://github.com/acme/widget/issues/17")},
                        {42, "Second issue", "bob",
                         QUrl("https://github.com/acme/widget/issues/42")}};
  model.setGitHubIssues(issues);
  section = model.sectionIndex(RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 2);
  QCOMPARE(model.rowCount(section), 3);
  QModelIndex first = model.index(1, 0, section);
  QCOMPARE(first.data().toString(), QString("#17 First issue"));
  QCOMPARE(first.data(RepositoryNavigatorModel::IssueNumberRole).toInt(), 17);
  QCOMPARE(first.data(RepositoryNavigatorModel::IssueAuthorRole).toString(),
           QString("alice"));
  QCOMPARE(first.data(RepositoryNavigatorModel::UrlRole).toString(),
           QString("https://github.com/acme/widget/issues/17"));
  QVERIFY(first.data(Qt::ToolTipRole).toString().contains("alice"));

  model.beginGitHubIssuesLoad(true);
  section = model.sectionIndex(RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 2);
  QCOMPARE(model.rowCount(section), 4);
  QCOMPARE(static_cast<RepositoryNavigatorModel::ItemKind>(
               model.index(3, 0, section)
                   .data(RepositoryNavigatorModel::ItemKindRole)
                   .toInt()),
           RepositoryNavigatorModel::ItemKind::Status);
  model.failGitHubIssues("offline", true);
  section = model.sectionIndex(RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::LoadStateRole)
               .value<RepositoryNavigatorModel::LoadState>(),
           RepositoryNavigatorModel::LoadState::Stale);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 2);
  QCOMPARE(model.rowCount(section), 4);

  model.refresh();
  section = model.sectionIndex(RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 2);
  QCOMPARE(model.index(1, 0, section).data().toString(),
           QString("#17 First issue"));
}

void TestRepositorySideBar::githubIssuesRemoteFilter() {
  struct Request {
    QString owner;
    QString repository;
    GitHub::IssuesCallback callback;
  };
  QList<Request> requests;
  qint64 now = 1000;
  RepositoryNavigator navigator(
      nullptr,
      [&requests](GitHub *, const QString &owner, const QString &repository,
                  const GitHub::IssuesCallback &callback) {
        requests.append({owner, repository, callback});
      },
      [&now] { return now; });
  QComboBox *filter = navigator.issuesRemoteFilter();
  QTreeView *issuesView =
      navigator.sectionView(RepositoryNavigatorModel::Section::GitHubIssues);
  QVERIFY(filter);
  QVERIFY(issuesView);
  QVERIFY(!navigator.findChild<QToolButton *>("GitHubIssuesRefresh"));
  QCOMPARE(filter->objectName(), QString("GitHubIssuesRemoteFilter"));
  QCOMPARE(issuesView->objectName(), QString("GitHubIssuesView"));
  QVERIFY(!filter->accessibleName().isEmpty());

  Test::ScratchRepository repo;
  QVERIFY(
      repo->addRemote("origin", "git@github.com:acme/widget.git").isValid());
  navigator.setRepository(repo);
  QCOMPARE(filter->count(), 1);
  QToolButton *issuesToggle = navigator.findChild<QToolButton *>(
      "RepositoryNavigationGitHubIssuesToggle");
  QVERIFY(issuesToggle);
  QVERIFY(issuesToggle->isEnabled());
  QCOMPARE(filter->itemText(0), QString("origin - acme/widget"));
  QVERIFY(!filter->isHidden());
  QModelIndex issuesSection = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(issuesView->rootIndex(), issuesSection);
  QVERIFY(issuesView->isRowHidden(0, issuesSection));
  QModelIndex filterRow = navigator.model()->index(0, 0, issuesSection);
  QCOMPARE(static_cast<RepositoryNavigatorModel::ItemKind>(
               filterRow.data(RepositoryNavigatorModel::ItemKindRole).toInt()),
           RepositoryNavigatorModel::ItemKind::GitHubIssuesFilter);
  QCOMPARE(filterRow.data().toString(),
           QString("Issues repository: origin - acme/widget"));
  QCOMPARE(requests.size(), 1);
  QCOMPARE(requests.constFirst().owner, QString("acme"));
  QCOMPARE(requests.constFirst().repository, QString("widget"));

  QVERIFY(repo->addRemote("upstream", "https://github.com/core/widget.git")
              .isValid());
  QCOMPARE(filter->count(), 2);
  QCOMPARE(filter->itemText(0), QString("origin - acme/widget"));
  QCOMPARE(filter->itemText(1), QString("upstream - core/widget"));
  QVERIFY(!filter->isHidden());
  QCOMPARE(requests.size(), 1);

  GitHub::IssuesCallback stale = requests.constFirst().callback;
  filter->setCurrentIndex(1);
  QCOMPARE(requests.constLast().owner, QString("core"));
  QCOMPARE(requests.constLast().repository, QString("widget"));
  issuesSection = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(navigator.model()->index(0, 0, issuesSection).data().toString(),
           QString("Issues repository: upstream - core/widget"));
  GitHub::IssuesCallback selected = requests.constLast().callback;

  stale(true,
        {{1, "Stale issue", "old",
          QUrl("https://github.com/acme/widget/issues/1")}},
        1, QString());
  QModelIndex section = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 0);

  selected(true,
           {{9, "Selected issue", "new",
             QUrl("https://github.com/core/widget/issues/9")}},
           1, QString());
  section = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 1);
  QCOMPARE(navigator.model()->index(1, 0, section).data().toString(),
           QString("#9 Selected issue"));

  const int freshRequestCount = requests.size();
  navigator.setRepository(git::Repository());
  navigator.setRepository(repo);
  QCOMPARE(requests.size(), freshRequestCount);
  section = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 1);
  QCOMPARE(navigator.model()->index(1, 0, section).data().toString(),
           QString("#9 Selected issue"));

  now += 5 * 60 * 1000 + 1;
  navigator.setRepository(git::Repository());
  navigator.setRepository(repo);
  QCOMPARE(requests.size(), freshRequestCount + 1);
  section = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::LoadStateRole)
               .value<RepositoryNavigatorModel::LoadState>(),
           RepositoryNavigatorModel::LoadState::Refreshing);
  navigator.setRepository(repo);
  QCOMPARE(requests.size(), freshRequestCount + 1);
  requests.constLast().callback(
      true,
      {{10, "Refreshed issue", "new",
        QUrl("https://github.com/core/widget/issues/10")}},
      1, QString());

  now += 10 * 1000 + 1;
  navigator.model()->refresh();
  navigator.refresh();
  QCOMPARE(requests.size(), freshRequestCount + 2);
  navigator.refresh();
  QCOMPARE(requests.size(), freshRequestCount + 2);
  requests.constLast().callback(false, {}, 0, "offline");
  section = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QCOMPARE(section.data(RepositoryNavigatorModel::LoadStateRole)
               .value<RepositoryNavigatorModel::LoadState>(),
           RepositoryNavigatorModel::LoadState::Stale);
  QCOMPARE(section.data(RepositoryNavigatorModel::CountRole).toInt(), 1);

  GitHub::Issues manyIssues;
  for (int i = 1; i <= 50; ++i) {
    manyIssues.append(
        {i, QString("Issue %1").arg(i), "author",
         QUrl(QString("https://github.com/core/widget/issues/%1").arg(i))});
  }
  navigator.model()->setGitHubIssues(manyIssues);
  navigator.resize(320, 700);
  navigator.show();
  QVERIFY(qWaitForWindowExposed(&navigator));
  issuesToggle->setChecked(true);
  QTRY_VERIFY(issuesView->isVisible());
  QTRY_COMPARE(issuesView->height(), 5 * issuesView->sizeHintForRow(1) +
                                         2 * issuesView->frameWidth());
  QTRY_VERIFY(issuesView->verticalScrollBar()->maximum() > 0);
  section = navigator.model()->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  QModelIndex lastIssue = navigator.model()->index(50, 0, section);
  QCOMPARE(menuTexts(contextMenuItems(issuesView, lastIssue)),
           QStringList({"Open in Browser"}));
  issuesView->scrollTo(lastIssue, QAbstractItemView::PositionAtBottom);
  QCoreApplication::processEvents();
  QVERIFY(issuesView->visualRect(lastIssue).intersects(
      issuesView->viewport()->rect()));
  issuesView->verticalScrollBar()->setValue(
      issuesView->verticalScrollBar()->maximum() / 2);
  const int scrollPosition = issuesView->verticalScrollBar()->value();
  navigator.model()->beginGitHubIssuesLoad(true);
  QTRY_COMPARE(issuesView->verticalScrollBar()->value(), scrollPosition);

  int remoteUpdateCount = 0;
  connect(repo->notifier(), &git::RepositoryNotifier::remoteUpdated,
          [&remoteUpdateCount](const git::Remote &) { ++remoteUpdateCount; });
  git::Remote upstream = repo->lookupRemote("upstream");
  upstream.setUrl("https://github.com/core-next/widget.git");
  QCOMPARE(remoteUpdateCount, 1);
  QCOMPARE(filter->itemText(filter->currentIndex()),
           QString("upstream - core-next/widget"));
}

void TestRepositorySideBar::activeRepositoryBinding() {
  MainWindow window(mRepo);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  QVERIFY(window.isSideBarVisible());
  SideBar *sideBar = window.findChild<SideBar *>();
  RepositoryNavigator *navigator =
      sideBar->findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);
  QVERIFY(!sideBar->findChild<QTreeView *>("RepositoryTree"));
  QVERIFY(!sideBar->findChild<QSplitter *>("RepositorySidebarContent"));
  QCOMPARE(navigator->model()->repository().dir(false).path(),
           mRepo->dir(false).path());

  QSignalSpy generalRefresh(window.currentView(),
                            &RepoView::manualRefreshRequested);
  QAction *refreshAction = nullptr;
  for (QAction *action : window.findChildren<QAction *>()) {
    if (action->text() == "Refresh") {
      refreshAction = action;
      break;
    }
  }
  QVERIFY(refreshAction);
  refreshAction->trigger();
  QCOMPARE(generalRefresh.count(), 1);

  RepositoryNavigatorModel *model = navigator->model();
  QTreeView *localView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Local);
  QTreeView *remoteView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Remote);
  QVERIFY(localView);
  QVERIFY(remoteView);
  QModelIndex local =
      model->sectionIndex(RepositoryNavigatorModel::Section::Local);
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
      contextMenuItems(localView, current);
  QString currentName = current.data().toString();
  QCOMPARE(menuTexts(currentItems),
           QStringList({"Pull", "Push", "Force Push...", "Checkout",
                        "Rename " + currentName, "Delete", "Merge...",
                        "Rebase...", "Squash..."}));
  QVERIFY(currentItems.at(0).second);
  QVERIFY(currentItems.at(1).second);
  QVERIFY(currentItems.at(2).second);
  QCOMPARE(currentItems.at(3).second, false);
  QVERIFY(currentItems.at(4).second);
  QCOMPARE(currentItems.at(5).second, false);

  QList<QPair<QString, bool>> otherItems = contextMenuItems(localView, other);
  QString otherName = other.data().toString();
  QCOMPARE(menuTexts(otherItems),
           QStringList({"Checkout", "Rename " + otherName, "Delete", "Merge...",
                        "Rebase...", "Squash..."}));
  QVERIFY(otherItems.at(0).second);
  QVERIFY(otherItems.at(1).second);
  QVERIFY(otherItems.at(2).second);

  QModelIndex remotes =
      model->sectionIndex(RepositoryNavigatorModel::Section::Remote);
  QModelIndex remote = model->index(0, 0, remotes);
  QString remoteName = remote.data().toString();
  QList<QPair<QString, bool>> remoteItems =
      contextMenuItems(remoteView, remote);
  QCOMPARE(
      menuTexts(remoteItems),
      QStringList({"Checkout", "Rename " + remoteName, "Delete " + remoteName,
                   "New Local Branch", "Merge...", "Rebase...", "Squash..."}));
  for (const auto &item : remoteItems)
    QVERIFY(item.second);

  CommitList *commits = window.currentView()->findChild<CommitList *>();
  QVERIFY(commits);
  QModelIndex commitIndex;
  for (int row = 0; row < commits->model()->rowCount(); ++row) {
    QModelIndex candidate = commits->model()->index(row, 0);
    if (candidate.data(CommitList::CommitRole).value<git::Commit>() ==
        mRepo->head().target()) {
      commitIndex = candidate;
      break;
    }
  }
  QVERIFY(commitIndex.isValid());
  QStringList renameItems = contextSubmenuItems(commits, commitIndex, "Rename");
  QVERIFY(renameItems.contains(currentName));
  QVERIFY(renameItems.contains(otherName));
  QVERIFY(renameItems.contains("origin/main"));
  QVERIFY(renameItems.contains("origin/NOTHEAD"));
  QStringList deleteItems = contextSubmenuItems(commits, commitIndex, "Delete");
  QVERIFY(deleteItems.contains("origin/main"));
  QVERIFY(deleteItems.contains("origin/NOTHEAD"));

  git::Reference selectedReference =
      current.data(RepositoryNavigatorModel::ReferenceRole)
          .value<git::Reference>();
  remoteView->setCurrentIndex(remote);
  localView->setCurrentIndex(current);
  QVERIFY(!remoteView->currentIndex().isValid());
  model->setGitHubIssuesAvailable(true);
  model->beginGitHubIssuesLoad(false);
  model->setGitHubIssues({{1, "Selection test", "tester",
                           QUrl("https://github.com/acme/widget/issues/1")}});
  QCOMPARE(localView->currentIndex()
               .data(RepositoryNavigatorModel::ReferenceRole)
               .value<git::Reference>()
               .qualifiedName(),
           selectedReference.qualifiedName());
  model->setGitHubIssuesAvailable(false);

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
  QVERIFY(QMetaObject::invokeMethod(localView, "clicked",
                                    Q_ARG(QModelIndex, feature)));
  QTRY_COMPARE(referenceSelected.count(), 1);
  QCOMPARE(referenceSelected.first().first().value<git::Reference>().name(),
           QString("feature"));

  QVERIFY(QMetaObject::invokeMethod(localView, "doubleClicked",
                                    Q_ARG(QModelIndex, feature)));
  QTRY_COMPARE(mRepo->head().name(), QString("feature"));

  QVERIFY(window.tabWidget()->closeTab(window.currentView()));
  QTRY_COMPARE(window.count(), 0);
  QVERIFY(!navigator->model()->repository().isValid());
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void TestRepositorySideBar::checkoutConflictStash() {
  Test::ScratchRepository repo;
  QVERIFY(writeFile(repo, "tracked.txt", "base\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "base"}), 0);
  const QString mainBranch = repo->head().name();

  QCOMPARE(runGit(repo, {"checkout", "-b", "checkout-target"}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "target\n"));
  QVERIFY(writeFile(repo, "collision.txt", "target collision\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt", "collision.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "target changes"}), 0);

  QCOMPARE(runGit(repo, {"checkout", mainBranch}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "main\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "main changes"}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "uncommitted\n"));
  QVERIFY(writeFile(repo, "collision.txt", "untracked collision\n"));

  MainWindow window(repo);
  RepoView *view = window.currentView();
  git::Reference target = repo->lookupRef("refs/heads/checkout-target");
  QVERIFY(target.isValid());
  view->checkoutFromNavigator(target);

  QPointer<QMessageBox> dialog;
  QTRY_VERIFY(dialog =
                  view->findChild<QMessageBox *>("CheckoutConflictsDialog"));
  QCOMPARE(repo->head().name(), mainBranch);
  QVERIFY(dialog->text().contains("would overwrite uncommitted changes"));
  QPushButton *stash = messageButton(dialog, "Stash and Checkout");
  QVERIFY(stash);
  stash->click();

  QTRY_COMPARE(repo->head().name(), QString("checkout-target"));
  QCOMPARE(repo->stashes().size(), 1);
  QFile collision(repo->workdir().filePath("collision.txt"));
  QVERIFY(collision.open(QIODevice::ReadOnly));
  QCOMPARE(collision.readAll(), QByteArray("target collision\n"));
}

void TestRepositorySideBar::checkoutKeepsNonConflictingChanges() {
  Test::ScratchRepository repo;
  QVERIFY(writeFile(repo, "tracked.txt", "base\n"));
  QVERIFY(writeFile(repo, "kept.txt", "base kept\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt", "kept.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "base"}), 0);
  const QString mainBranch = repo->head().name();

  QCOMPARE(runGit(repo, {"checkout", "-b", "non-conflicting-target"}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "target\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "target changes"}), 0);
  QCOMPARE(runGit(repo, {"checkout", mainBranch}), 0);
  QVERIFY(writeFile(repo, "kept.txt", "uncommitted but safe\n"));

  MainWindow window(repo);
  RepoView *view = window.currentView();
  git::Reference target = repo->lookupRef("refs/heads/non-conflicting-target");
  QVERIFY(target.isValid());
  view->checkoutFromNavigator(target);

  QTRY_COMPARE(repo->head().name(), QString("non-conflicting-target"));
  QVERIFY(!view->findChild<QMessageBox *>("CheckoutConflictsDialog"));
  QFile kept(repo->workdir().filePath("kept.txt"));
  QVERIFY(kept.open(QIODevice::ReadOnly));
  QCOMPARE(kept.readAll(), QByteArray("uncommitted but safe\n"));
}

void TestRepositorySideBar::checkoutConflictCommit() {
  Test::ScratchRepository repo;
  QVERIFY(writeFile(repo, "tracked.txt", "base\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "base"}), 0);
  const QString mainBranch = repo->head().name();

  QCOMPARE(runGit(repo, {"checkout", "-b", "commit-target"}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "target\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "target changes"}), 0);
  QCOMPARE(runGit(repo, {"checkout", mainBranch}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "main\n"));
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  QCOMPARE(runGit(repo, {"commit", "-m", "main changes"}), 0);
  QVERIFY(writeFile(repo, "tracked.txt", "commit before checkout\n"));

  MainWindow window(repo);
  RepoView *view = window.currentView();
  git::Reference target = repo->lookupRef("refs/heads/commit-target");
  QVERIFY(target.isValid());
  view->checkoutFromNavigator(target);

  QPointer<QMessageBox> dialog;
  QTRY_VERIFY(dialog =
                  view->findChild<QMessageBox *>("CheckoutConflictsDialog"));
  QPushButton *commit = messageButton(dialog, "Commit Changes");
  QVERIFY(commit);
  commit->click();
  QTRY_VERIFY(!dialog);
  CommitList *commits = view->findChild<CommitList *>();
  QVERIFY(commits);
  QTRY_COMPARE(commits->selectedRange(), QString("status"));

  view->selectCommit(repo->head().target(), QString());
  QTRY_VERIFY(commits->selectedRange() != "status");
  QCoreApplication::processEvents();
  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  repo->index().read();
  QVERIFY(view->commit(QStringLiteral("abandon pending checkout")));
  QCoreApplication::processEvents();
  QCOMPARE(repo->head().name(), mainBranch);

  QVERIFY(writeFile(repo, "tracked.txt", "commit and checkout\n"));
  window.toolBar()->searchField()->setText("filtered");
  target = repo->lookupRef("refs/heads/commit-target");
  view->checkoutFromNavigator(target);
  QTRY_VERIFY(dialog =
                  view->findChild<QMessageBox *>("CheckoutConflictsDialog"));
  commit = messageButton(dialog, "Commit Changes");
  QVERIFY(commit);
  commit->click();
  QTRY_VERIFY(!dialog);
  QCOMPARE(window.toolBar()->searchField()->text(), QString());
  QTRY_COMPARE(commits->selectedRange(), QString("status"));

  QCOMPARE(runGit(repo, {"add", "tracked.txt"}), 0);
  repo->index().read();
  QVERIFY(view->commit(QStringLiteral("save before checkout")));
  QTRY_COMPARE(repo->head().name(), QString("commit-target"));
}

void TestRepositorySideBar::branchGraphColors() {
  Test::ScratchRepository repo;
  QProcess git;
  git.setWorkingDirectory(repo->workdir().path());
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "color base"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git::Id baseCommit = repo->head().target().id();
  QString mainBranch = repo->head().name();
  QVERIFY(!mainBranch.isEmpty());
  git.start(GIT_EXECUTABLE, {"checkout", "-b", "color-side"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "color side"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"checkout", mainBranch});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "color main"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"branch", "sender-alive"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"commit", "--allow-empty", "-m", "color main latest"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  for (int branch = 0; branch < 10; ++branch) {
    const QString name = QString("badge-extra-%1").arg(branch);
    git.start(GIT_EXECUTABLE, {"checkout", "-b", name, baseCommit.toString()});
    QVERIFY(git.waitForFinished());
    QCOMPARE(git.exitCode(), 0);
    git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", name});
    QVERIFY(git.waitForFinished());
    QCOMPARE(git.exitCode(), 0);
  }
  git.start(GIT_EXECUTABLE, {"checkout", mainBranch});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE,
            {"update-ref", "refs/remotes/origin/color-side", "color-side"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git::Config config = repo->appConfig();
  config.setValue(ConfigKeys::kRefsKey,
                  static_cast<int>(CommitList::RefsFilter::AllRefs));
  config.setValue(ConfigKeys::kStatusKey, false);
  config.setValue(ConfigKeys::kGraphKey, true);

  MainWindow window(repo);
  window.resize(1200, 700);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  RepoView *view = window.currentView();
  CommitList *commitList = view->findChild<CommitList *>();
  QVERIFY(commitList);
  QHeaderView *header = commitList->findChild<QHeaderView *>("CommitHeader");
  QVERIFY(header);
  bool referencesHidden = header->isSectionHidden(CommitList::ReferencesColumn);
  auto restoreReferences = qScopeGuard([header, referencesHidden] {
    header->setSectionHidden(CommitList::ReferencesColumn, referencesHidden);
  });
  header->setSectionHidden(CommitList::ReferencesColumn, false);
  QAbstractItemModel *model = commitList->model();
  while (model->canFetchMore(QModelIndex()))
    model->fetchMore(QModelIndex());

  git::Reference main = repo->lookupRef("refs/heads/" + mainBranch);
  git::Reference side = repo->lookupRef("refs/heads/color-side");
  git::Reference sender = repo->lookupRef("refs/heads/sender-alive");
  git::Reference remote = repo->lookupRef("refs/remotes/origin/color-side");
  QVERIFY(main.isValid());
  QVERIFY(side.isValid());
  QVERIFY(sender.isValid());
  QVERIFY(remote.isValid());
  QCOMPARE(remote.target().id(), side.target().id());
  QModelIndex mainIndex = commitIndex(model, main.target().id());
  QModelIndex sideIndex = commitIndex(model, side.target().id());
  QModelIndex senderIndex = commitIndex(model, sender.target().id());
  QVERIFY(mainIndex.isValid());
  QVERIFY(sideIndex.isValid());
  QVERIFY(senderIndex.isValid());

  const QColor headColor = Application::theme()->badge(
      Theme::BadgeRole::Background, Theme::BadgeState::Head);
  const QColor remoteBadge("#a5a7aa");
  QColor mainBase = graphNodeColor(mainIndex, CommitList::GraphBaseColorRole);
  QColor sideBase = graphNodeColor(sideIndex, CommitList::GraphBaseColorRole);
  QVERIFY(mainBase.isValid());
  QVERIFY(sideBase.isValid());
  QVERIFY(mainBase != headColor);
  QVERIFY(sideBase != headColor);
  QVERIFY(!isGreenBranchColor(mainBase));
  QVERIFY(!isGreenBranchColor(sideBase));
  QVERIFY(sideBase != remoteBadge);
  QCOMPARE(graphNodeColor(mainIndex, CommitList::GraphColorRole), headColor);
  QCOMPARE(graphNodeColor(sideIndex, CommitList::GraphColorRole), sideBase);
  QCOMPARE(graphNodeColor(senderIndex, CommitList::GraphBaseColorRole),
           mainBase);
  QCOMPARE(branchBadgeColor(commitList, header, mainIndex), headColor);
  const QColor sideBadge = branchBadgeColor(commitList, header, sideIndex);
  const QColor senderBadge = branchBadgeColor(commitList, header, senderIndex);
  QVERIFY(sideBadge.isValid());
  QVERIFY(senderBadge.isValid());
  QVERIFY(sideBadge != headColor);
  QVERIFY(senderBadge != headColor);
  QVERIFY(sideBadge != senderBadge);
  QVERIFY(
      referenceBadgesContainColor(commitList, header, sideIndex, sideBadge));
  QVERIFY(
      referenceBadgesContainColor(commitList, header, sideIndex, remoteBadge));

  QSet<QRgb> branchBadgeColors;
  for (int branch = 0; branch < 10; ++branch) {
    git::Reference extra =
        repo->lookupRef(QString("refs/heads/badge-extra-%1").arg(branch));
    QVERIFY(extra.isValid());
    QModelIndex extraIndex = commitIndex(model, extra.target().id());
    QVERIFY(extraIndex.isValid());
    QColor color = branchBadgeColor(commitList, header, extraIndex);
    QVERIFY(color.isValid());
    QVERIFY(color != headColor);
    QVERIFY(color != remoteBadge);
    branchBadgeColors.insert(color.rgba());
  }
  QCOMPARE(branchBadgeColors.size(), 10);

  for (int row = 0; row < model->rowCount(); ++row) {
    for (const QVariant &column :
         model->index(row, 0).data(CommitList::GraphBaseColorRole).toList()) {
      for (const QVariant &value : column.toList()) {
        QColor color = value.value<QColor>();
        if (color.isValid() && color != QColor(Qt::gray))
          QVERIFY(!isGreenBranchColor(color));
      }
    }
  }

  config.setValue(ConfigKeys::kGraphKey, false);
  commitList->resetSettings();
  mainIndex = commitIndex(model, main.target().id());
  senderIndex = commitIndex(model, sender.target().id());
  QVERIFY(mainIndex.isValid());
  QVERIFY(senderIndex.isValid());
  QCOMPARE(branchBadgeColor(commitList, header, mainIndex), headColor);
  QCOMPARE(branchBadgeColor(commitList, header, senderIndex), senderBadge);
  config.setValue(ConfigKeys::kGraphKey, true);
  commitList->resetSettings();
  while (model->canFetchMore(QModelIndex()))
    model->fetchMore(QModelIndex());

  QSignalSpy colorChanged(model, &QAbstractItemModel::dataChanged);
  view->checkout(side);
  QTRY_COMPARE(repo->head().name(), QString("color-side"));
  QTRY_VERIFY(colorChanged.count() > 0);
  bool graphColorChanged = false;
  for (const QList<QVariant> &arguments : colorChanged) {
    if (arguments.at(2).value<QList<int>>().contains(
            CommitList::GraphColorRole)) {
      graphColorChanged = true;
      break;
    }
  }
  QVERIFY(graphColorChanged);
  QTRY_VERIFY(commitIndex(model, side.target().id()).isValid());
  while (model->canFetchMore(QModelIndex()))
    model->fetchMore(QModelIndex());
  mainIndex = commitIndex(model, main.target().id());
  sideIndex = commitIndex(model, side.target().id());
  senderIndex = commitIndex(model, sender.target().id());
  QTRY_COMPARE(graphNodeColor(sideIndex, CommitList::GraphColorRole),
               headColor);
  QCOMPARE(graphNodeColor(mainIndex, CommitList::GraphColorRole), mainBase);
  QVERIFY(
      referenceBadgesContainColor(commitList, header, sideIndex, headColor));
  QVERIFY(
      referenceBadgesContainColor(commitList, header, sideIndex, remoteBadge));
  QColor mainBadge = branchBadgeColor(commitList, header, mainIndex);
  QVERIFY(mainBadge.isValid());
  QVERIFY(mainBadge != headColor);
  QVERIFY(mainBadge != remoteBadge);
  QVERIFY(mainBadge != senderBadge);
  QCOMPARE(branchBadgeColor(commitList, header, senderIndex), senderBadge);
}

void TestRepositorySideBar::stashInteraction() {
  const QString headerStateKey = "commit/columns/headerStateV12";
  const QString previousHeaderStateKey = "commit/columns/headerStateV11";
  const QString legacyHeaderStateKey = "commit/columns/headerStateV10";
  QSettings columnSettings;
  bool hadHeaderState = columnSettings.contains(headerStateKey);
  QByteArray headerState = columnSettings.value(headerStateKey).toByteArray();
  bool hadPreviousHeaderState = columnSettings.contains(previousHeaderStateKey);
  QByteArray previousHeaderState =
      columnSettings.value(previousHeaderStateKey).toByteArray();
  bool hadLegacyHeaderState = columnSettings.contains(legacyHeaderStateKey);
  QByteArray legacyHeaderState =
      columnSettings.value(legacyHeaderStateKey).toByteArray();
  auto restoreHeaderState = qScopeGuard(
      [hadHeaderState, headerStateKey, headerState, hadPreviousHeaderState,
       previousHeaderStateKey, previousHeaderState, hadLegacyHeaderState,
       legacyHeaderStateKey, legacyHeaderState] {
        QSettings settings;
        if (hadHeaderState)
          settings.setValue(headerStateKey, headerState);
        else
          settings.remove(headerStateKey);
        if (hadPreviousHeaderState)
          settings.setValue(previousHeaderStateKey, previousHeaderState);
        else
          settings.remove(previousHeaderStateKey);
        if (hadLegacyHeaderState)
          settings.setValue(legacyHeaderStateKey, legacyHeaderState);
        else
          settings.remove(legacyHeaderStateKey);
      });
  columnSettings.remove(headerStateKey);
  columnSettings.remove(previousHeaderStateKey);
  columnSettings.remove(legacyHeaderStateKey);

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
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "stash tip"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  QString mainBranch = repo->head().name();
  git.start(GIT_EXECUTABLE, {"checkout", "-b", "graph-side", "HEAD~1"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "graph side"});
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

  QStandardItemModel legacyHeaderModel(0, 6);
  QHeaderView legacyHeader(Qt::Horizontal);
  legacyHeader.setMinimumSectionSize(24);
  legacyHeader.setModel(&legacyHeaderModel);
  const int legacySizes[] = {55, 74, 180, 82, 116, 71};
  for (int column = 0; column < 6; ++column)
    legacyHeader.resizeSection(column, legacySizes[column]);
  columnSettings.setValue(previousHeaderStateKey, legacyHeader.saveState());

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

  QCOMPARE(commitList->sizeHintForRow(0), 28);

  QHeaderView *header = commitList->findChild<QHeaderView *>("CommitHeader");
  QVERIFY(header);
  QVERIFY(header->isVisible());
  QVERIFY(header->sectionsMovable());
  QCOMPARE(header->count(), 6);
  QCOMPARE(header->length(), header->width());
  QTRY_COMPARE(header->sectionSize(0),
               2 * qBound(55, header->width() * 19 / 100, 360));
  QCOMPARE(header->sectionSize(3), legacySizes[3]);
  QVERIFY(columnSettings.contains(headerStateKey));

  int preferredReferencesWidth = header->sectionSize(0);
  commitList->setMinimumWidth(0);
  commitList->setFixedWidth(360);
  QTRY_COMPARE(header->sectionSize(0), preferredReferencesWidth);
  QTRY_VERIFY(commitList->horizontalScrollBar()->maximum() > 0);
  header->moveSection(0, 1);
  header->moveSection(1, 0);

  QHeaderView savedHeader(Qt::Horizontal);
  QStandardItemModel savedHeaderModel(0, 6);
  savedHeader.setModel(&savedHeaderModel);
  QVERIFY(savedHeader.restoreState(
      columnSettings.value(headerStateKey).toByteArray()));
  QCOMPARE(savedHeader.sectionSize(0), preferredReferencesWidth);

  commitList->setMinimumWidth(0);
  commitList->setMaximumWidth(QWIDGETSIZE_MAX);
  commitList->setMinimumWidth(900);
  window.resize(1600, 900);
  QTRY_COMPARE(header->sectionSize(0), preferredReferencesWidth);
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
      int pairWidth = compactMetrics.horizontalAdvance(QString(first) + second);
      int secondWidth = compactMetrics.horizontalAdvance(second);
      maxPairAdjustment =
          qMax(maxPairAdjustment, pairWidth - firstWidth - secondWidth);
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
  for (QToolButton *button : commitList->findChildren<QToolButton *>(
           QString(), Qt::FindDirectChildrenOnly)) {
    if (button->menu()) {
      columnOptions = button;
      break;
    }
  }
  QVERIFY(columnOptions);
  QAction *referencesAction = columnOptions->menu()->actions().constFirst();
  QAction *graphAction =
      columnOptions->menu()->actions().at(CommitList::GraphColumn);
  QVERIFY(graphAction->isCheckable());
  referencesAction->setChecked(true);
  referencesAction->trigger();
  QVERIFY(header->isSectionHidden(0));
  referencesAction->trigger();
  QVERIFY(!header->isSectionHidden(0));
  QAction *resetColumns = columnOptions->menu()->actions().constLast();
  QVERIFY(!resetColumns->isCheckable());
  resetColumns->trigger();
  QCoreApplication::processEvents();
  QAction *status = nullptr;
  for (QAction *action : columnOptions->menu()->actions()) {
    if (action->text() == "Show Clean Status") {
      status = action;
      break;
    }
  }
  QVERIFY(status);
  QVERIFY(status->isCheckable());
  int laneWidth = qMax(compactMetrics.ascent(), 20);
  QCOMPARE(header->sectionSize(CommitList::GraphColumn), 8 * laneWidth);

  while (graphModel->canFetchMore(QModelIndex()))
    graphModel->fetchMore(QModelIndex());
  constexpr int dotSegment = 0;
  constexpr int topSegment = 1;
  constexpr int bottomSegment = 3;
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
  QList<QColor> stashNodeColors;
  QList<QColor> stashForkColors;
  struct StashLane {
    int row;
    int column;
    QColor color;
  };
  QList<StashLane> stashLanes;
  bool stashForkStartsAtParentRight = false;
  bool stashForkReachesLane = false;
  QColor divergenceColor;
  QList<QColor> forkColors;
  bool divergenceContinuesStraight = false;
  bool forkStartsAtParent = false;
  bool forkReachesDeferredLane = false;
  int mergeRow = -1;
  int mergeTargetColumn = -1;
  for (int row = 0; row < graphModel->rowCount(); ++row) {
    QModelIndex index = graphModel->index(row, 0);
    git::Commit commit =
        index.data(CommitList::CommitRole).value<git::Commit>();
    bool targetMerge = commit.isValid() && commit.summary() == "graph merge";
    bool targetDivergence =
        commit.isValid() && commit.summary() == "graph side";
    bool targetParent = commit.isValid() && commit.summary() == "stash base";
    bool stash = index.data(CommitList::StashIndexRole).isValid();
    QColor stashNodeColor;
    QList<QColor> stashEdgeColors;
    QVariantList columns = index.data(CommitList::GraphRole).toList();
    QVariantList colorColumns = index.data(CommitList::GraphColorRole).toList();
    QVariantList styleColumns = index.data(CommitList::GraphStyleRole).toList();
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
        if (targetDivergence && hasDot && type == bottomSegment)
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
        if (targetMerge && type >= firstMergeSegment &&
            type <= mergeRightOutSegment)
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
        if (stash && type == dotSegment) {
          stashNodeColor = colors.at(segment).value<QColor>();
          stashNodeColors.append(stashNodeColor);
          stashLanes.append({row, column, stashNodeColor});
        }
        if (styles.at(segment).toInt() == Qt::DotLine &&
            type >= firstForkSegment) {
          stashForkColors.append(colors.at(segment).value<QColor>());
          if (type == forkRightOutSegment) {
            QCOMPARE(column, dotColumn);
            stashForkStartsAtParentRight = true;
          }
          if (type == forkLeftInSegment) {
            QVERIFY(column != dotColumn);
            stashForkReachesLane = true;
          }
          QVERIFY(type != forkLeftOutSegment);
          QVERIFY(type != forkRightInSegment);
        }
        if (stash && styles.at(segment).toInt() == Qt::DotLine && type >= 4 &&
            type < firstMergeSegment) {
          stashEdgeColors.append(colors.at(segment).value<QColor>());
        }
      }
      if (hasDot && stash) {
        QColor stashEdgeColor;
        for (int segment = 0; segment < segments.size(); ++segment) {
          int type = segments.at(segment).toInt();
          if (type == bottomSegment &&
              styles.at(segment).toInt() == Qt::DotLine)
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
  QVERIFY(stashForkStartsAtParentRight);
  QVERIFY(stashForkReachesLane);
  QVERIFY(!stashForkColors.isEmpty());
  for (const QColor &color : stashForkColors)
    QVERIFY(stashNodeColors.contains(color));
  for (const StashLane &stashLane : stashLanes) {
    QVERIFY(stashLane.row + 1 < graphModel->rowCount());
    QModelIndex next = graphModel->index(stashLane.row + 1, 0);
    QVariantList nextColumns = next.data(CommitList::GraphRole).toList();
    QVariantList nextColorColumns =
        next.data(CommitList::GraphColorRole).toList();
    QVariantList nextStyleColumns =
        next.data(CommitList::GraphStyleRole).toList();
    QVERIFY(stashLane.column < nextColumns.size());
    QVERIFY(stashLane.column < nextColorColumns.size());
    QVERIFY(stashLane.column < nextStyleColumns.size());
    QVariantList nextSegments = nextColumns.at(stashLane.column).toList();
    QVariantList nextColors = nextColorColumns.at(stashLane.column).toList();
    QVariantList nextStyles = nextStyleColumns.at(stashLane.column).toList();
    bool continues = false;
    for (int segment = 0; segment < nextSegments.size(); ++segment) {
      int type = nextSegments.at(segment).toInt();
      if ((type == topSegment || type == forkLeftInSegment) &&
          nextStyles.at(segment).toInt() == Qt::DotLine &&
          nextColors.at(segment).value<QColor>() == stashLane.color) {
        continues = true;
        break;
      }
    }
    QVERIFY(continues);
  }
  QVERIFY(divergenceColor.isValid());
  QVERIFY(divergenceContinuesStraight);
  QVERIFY(forkStartsAtParent);
  QVERIFY(forkReachesDeferredLane);
  QVERIFY(!forkColors.isEmpty());
  QVERIFY(forkColors.count(divergenceColor) >= 2);

  QVERIFY(!commitList->findChild<QScrollBar *>("CommitGraphScrollBar"));

  QStandardItemModel wideGraph(1, 1);
  QVariantList wideColumns;
  for (int lane = 0; lane < 60; ++lane)
    wideColumns.append(QVariant(QVariantList()));
  int referencesWidthBeforeGraphGrowth = header->sectionSize(0);
  int authorWidthBeforeGraphGrowth = header->sectionSize(3);
  int dateWidthBeforeGraphGrowth = header->sectionSize(4);
  int idWidthBeforeGraphGrowth = header->sectionSize(5);
  int graphWidthBeforeGraphGrowth = header->sectionSize(1);
  int viewScrollMaximumBeforeGraphGrowth =
      commitList->horizontalScrollBar()->maximum();
  QModelIndex wideIndex = wideGraph.index(0, 0);
  wideGraph.setData(wideIndex, wideColumns, CommitList::GraphRole);
  wideGraph.setData(wideIndex, wideColumns, CommitList::GraphColorRole);
  wideGraph.setData(wideIndex, wideColumns, CommitList::GraphStyleRole);
  commitList->setModel(&wideGraph);
  QCoreApplication::processEvents();
  QCOMPARE(header->sectionSize(0), referencesWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(1), graphWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(3), authorWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(4), dateWidthBeforeGraphGrowth);
  QCOMPARE(header->sectionSize(5), idWidthBeforeGraphGrowth);
  QCOMPARE(commitList->horizontalScrollBar()->maximum(),
           viewScrollMaximumBeforeGraphGrowth);
  QVERIFY(!commitList->findChild<QScrollBar *>("CommitGraphScrollBar"));

  commitList->setModel(graphModel);
  QCoreApplication::processEvents();
  QCOMPARE(header->sectionSize(1), graphWidthBeforeGraphGrowth);

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
    git::Commit commit =
        index.data(CommitList::CommitRole).value<git::Commit>();
    QCOMPARE(commit.id(), expectedStashes.at(stashIndex).id());
    QCOMPARE(
        index.data(CommitList::GraphNodeRole).value<CommitList::GraphNode>(),
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
    git::Commit commit =
        index.data(CommitList::CommitRole).value<git::Commit>();
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
  QModelIndex headIndex;
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
    if (commit.isValid() && commit.id() == head) {
      passedHead = true;
      headIndex = index;
    }
  }
  QVERIFY(passedHead);
  QVERIFY(headIndex.isValid());

  QStringList headMenu = contextMenuItems(commitList, headIndex);
  QVERIFY(headMenu.contains("Pull"));
  QVERIFY(headMenu.contains("Push"));
  QVERIFY(headMenu.contains("Force Push..."));

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

  QTreeView *stashesView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Stashes);
  QVERIFY(stashesView);
  QVERIFY(QMetaObject::invokeMethod(stashesView, "clicked",
                                    Q_ARG(QModelIndex, stash)));
  QTRY_VERIFY(!window.currentView()->commits().isEmpty());
  QCOMPARE(window.currentView()->commits().first().id(), expected.id());

  window.currentView()->dropStash(0);
  QTRY_COMPARE(navigator->model()->rowCount(stashes), 1);
  QTRY_COMPARE(graphStashes().size(), 1);
}

void TestRepositorySideBar::historyPrefetch() {
  constexpr int commitCount = 600;
  Test::ScratchRepository repo;
  for (int i = 0; i < commitCount; ++i) {
    QVERIFY(repo->commit(QString("prefetch %1").arg(i)).isValid());
  }
  repo->appConfig().setValue("index.enable", false);
  repo->appConfig().setValue(ConfigKeys::kStatusKey, false);
  repo->appConfig().setValue(
      ConfigKeys::kRefsKey,
      static_cast<int>(CommitList::RefsFilter::SelectedRef));

  Settings *settings = Settings::instance();
  QVariant automaticChecks =
      settings->value(Setting::Id::CheckSubmodulesForUpdatesAutomatically);
  auto restoreSettings = qScopeGuard([settings, automaticChecks] {
    settings->setValue(Setting::Id::CheckSubmodulesForUpdatesAutomatically,
                       automaticChecks);
  });
  settings->setValue(Setting::Id::CheckSubmodulesForUpdatesAutomatically,
                     false);

  MainWindow window(repo);
  window.resize(900, 400);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  CommitList *commitList = window.currentView()->findChild<CommitList *>();
  QVERIFY(commitList);
  QAbstractItemModel *model = commitList->model();
  QTimer *prefetchTimer =
      commitList->findChild<QTimer *>("HistoryPrefetchTimer");
  QVERIFY(prefetchTimer);

  QTRY_VERIFY(model->rowCount() > 0);
  QTest::qWait(50);
  int initialRows = model->rowCount();
  QVERIFY(initialRows < commitCount);
  QVERIFY(model->canFetchMore(QModelIndex()));

  QScrollBar *scrollBar = commitList->verticalScrollBar();
  scrollBar->setSliderDown(true);
  int pausedRows = model->rowCount();
  scrollBar->setValue(scrollBar->maximum());
  QTest::qWait(30);
  QCOMPARE(model->rowCount(), pausedRows);

  scrollBar->setSliderDown(false);
  QTRY_VERIFY(model->rowCount() > pausedRows);
  QVERIFY(model->rowCount() < commitCount);

  QAbstractItemModelTester tester(
      model, QAbstractItemModelTester::FailureReportingMode::QtTest);
  commitList->resetSettings();
  QCoreApplication::processEvents();
}

void TestRepositorySideBar::tagPushToOrigin() {
  Test::ScratchRepository repo;
  QVERIFY(repo->commit("initial").isValid());

  QTemporaryDir origin;
  QVERIFY(origin.isValid());
  QProcess git;
  git.start(GIT_EXECUTABLE, {"init", "--bare", origin.path()});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  QVERIFY(repo->addRemote("origin", origin.path()).isValid());
  QCOMPARE(runGit(repo, {"push", "origin", "HEAD:refs/heads/main"}), 0);

  const git::TagRef reachable = repo->createTag(repo->head().target(), "v1");
  QVERIFY(reachable.isValid());
  const git::TagRef missing = repo->createTag(repo->head().target(), "v2");
  QVERIFY(missing.isValid());
  QCOMPARE(runGit(repo, {"push", "origin", "refs/tags/v1"}), 0);
  repo->appConfig().setValue("autofetch.enable", false);
  MainWindow window(repo);
  RepoView *view = window.currentView();
  QVERIFY(view);
  auto actionFor = [](QMenu &menu) {
    for (QAction *action : menu.actions()) {
      if (action->text().startsWith("Push Tag "))
        return action;
    }
    return static_cast<QAction *>(nullptr);
  };

  QMenu reachableMenu;
  view->populateReferenceContextMenu(&reachableMenu, missing);
  QAction *pushReachable = actionFor(reachableMenu);
  QVERIFY(pushReachable);
  QCOMPARE(pushReachable->text(), "Push Tag v2 to origin");
  QTRY_VERIFY(pushReachable->isEnabled());

  auto originHasTag = [&origin](const QString &name) {
    QProcess check;
    check.start(
        GIT_EXECUTABLE,
        {"--git-dir", origin.path(), "show-ref", "--verify",
         "refs/tags/" + name});
    return check.waitForFinished() && check.exitCode() == 0;
  };
  QVERIFY(originHasTag("v1"));
  QMenu presentMenu;
  view->populateReferenceContextMenu(&presentMenu, reachable);
  QAction *pushPresent = actionFor(presentMenu);
  QVERIFY(pushPresent);
  QCOMPARE(pushPresent->text(), "Push Tag v1 to origin");
  QTRY_VERIFY(!pushPresent->toolTip().contains("Checking"));
  QVERIFY(!pushPresent->isEnabled());
  QVERIFY(pushPresent->toolTip().contains("already present on origin"));

  QVERIFY(repo->commit("local only").isValid());
  const git::TagRef localOnly =
      repo->createTag(repo->head().target(), "local-only");
  QVERIFY(localOnly.isValid());
  QMenu localOnlyMenu;
  view->populateReferenceContextMenu(&localOnlyMenu, localOnly);
  QAction *pushLocalOnly = actionFor(localOnlyMenu);
  QVERIFY(pushLocalOnly);
  QTRY_VERIFY(!pushLocalOnly->toolTip().contains("Checking"));
  QVERIFY(!pushLocalOnly->isEnabled());
  QVERIFY(pushLocalOnly->toolTip().contains("not reachable from origin"));

  // The public push entry point must not bypass the contextual validation.
  view->push(repo->lookupRemote("origin"), localOnly);
  QTest::qWait(100);
  QVERIFY(!originHasTag("local-only"));
}

void TestRepositorySideBar::submoduleInteraction() {
  Settings *settings = Settings::instance();
  QVariant openInTabs = settings->value(Setting::Id::OpenSubmodulesInTabs);
  QVariant automaticChecks =
      settings->value(Setting::Id::CheckSubmodulesForUpdatesAutomatically);
  auto restoreSettings = qScopeGuard([settings, openInTabs, automaticChecks] {
    settings->setValue(Setting::Id::OpenSubmodulesInTabs, openInTabs);
    settings->setValue(Setting::Id::CheckSubmodulesForUpdatesAutomatically,
                       automaticChecks);
  });
  settings->setValue(Setting::Id::OpenSubmodulesInTabs, true);
  settings->setValue(Setting::Id::CheckSubmodulesForUpdatesAutomatically,
                     false);

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
  git.start(GIT_EXECUTABLE, {"-c", "protocol.file.allow=always", "submodule",
                             "add", child->workdir().path(), "child"});
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
  git::Id originCommit = child->head().target().id();

  MainWindow window(parent);
  window.show();
  QVERIFY(qWaitForWindowActive(&window));
  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);

  QModelIndex submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  QTreeView *submodulesView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Submodules);
  QVERIFY(submodulesView);
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
  QVERIFY(
      submodule.data(Qt::ToolTipRole)
          .toString()
          .contains("matches the commit recorded by the parent repository"));
  QVERIFY(submodule.data(Qt::ToolTipRole)
              .toString()
              .contains("Waiting for a submodule update check"));
  QString tooltip = submodule.data(Qt::ToolTipRole).toString();
  QVERIFY(tooltip.startsWith("<qt>"));
  QVERIFY(tooltip.contains("color:#36c96b"));
  QVERIFY(tooltip.contains("Left icon: Pin summary."));
  QVERIFY(tooltip.contains("Pin delta"));
  QVERIFY(tooltip.contains("Origin delta"));
  QList<QPair<QString, bool>> cleanMenu =
      contextMenuItems(submodulesView, submodule);
  QCOMPARE(menuTexts(cleanMenu),
           QStringList({"Open", "Commit Changes", "Check for Updates", "Update",
                        "Modify...", "Delete Submodule..."}));
  QVERIFY(!cleanMenu.at(1).second);
  QVERIFY(cleanMenu.at(2).second);

  git::Submodule selected =
      submodule.data(RepositoryNavigatorModel::SubmoduleRole)
          .value<git::Submodule>();
  git::Submodule::UpdateStatus synchronized;
  synchronized.name = "child";
  synchronized.path = selected.path();
  synchronized.url = selected.url();
  synchronized.branch = childBranch;
  synchronized.pinnedId = selected.indexId();
  synchronized.targetId = selected.workdirId();
  navigator->model()->setSubmoduleUpdateStatuses({synchronized});
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  navigator->model()->setBusySubmodulePaths({selected.path()});
  QVERIFY(submodule.data(RepositoryNavigatorModel::SubmoduleBusyRole).toBool());
  navigator->model()->setBusySubmodulePaths({});
  QVERIFY(!submodule.data(RepositoryNavigatorModel::SubmoduleBusyRole).toBool());
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
  parentView->checkSubmoduleUpdates(QList<git::Submodule>{selected});
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
  failed.path = selected.path();
  failed.url = selected.url();
  failed.branch = childBranch;
  failed.pinnedId = selected.indexId();
  failed.state = git::Submodule::UpdateStatus::Error;
  failed.message = "test failure";
  navigator->model()->setSubmoduleUpdateStatuses({failed});
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginStateRole)
               .value<RepositoryNavigatorModel::OriginState>(),
           RepositoryNavigatorModel::OriginState::Failed);
  QVERIFY(submodule.data(Qt::ToolTipRole)
              .toString()
              .contains("comparison failed - test failure"));
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
  QVERIFY(submodule.data(Qt::ToolTipRole)
              .toString()
              .contains("have diverged (1 commit ahead, 1 commit behind)"));

  selected = parentView->repo().lookupSubmodule("child");
  git::Id localCommit = selected.workdirId();

  QSignalSpy inserted(window.tabWidget(), &TabWidget::tabInserted);
  QVERIFY(QMetaObject::invokeMethod(submodulesView, "doubleClicked",
                                    Q_ARG(QModelIndex, submodule)));
  QTRY_COMPARE(window.count(), 2);
  QCOMPARE(inserted.count(), 1);
  RepoView *freshChildView = window.currentView();
  QVERIFY(freshChildView && freshChildView != parentView);
  QVERIFY(freshChildView->isSubmoduleTab());
  QCOMPARE(window.tabWidget()->tabText(window.tabWidget()->currentIndex()),
           QString("%1 / child").arg(parent->workdir().dirName()));

  QSignalSpy removed(window.tabWidget(), &TabWidget::tabRemoved);
  QVERIFY(window.tabWidget()->closeTab(freshChildView));
  QTRY_COMPARE(removed.count(), 1);
  QTRY_COMPARE(window.count(), 1);
  QCOMPARE(window.currentView(), parentView);

  RepoView *ordinaryChildView = window.addTab(selected.open());
  QVERIFY(ordinaryChildView);
  QCOMPARE(window.count(), 2);
  QVERIFY(!ordinaryChildView->isSubmoduleTab());
  QCOMPARE(window.tabWidget()
               ->tabIcon(window.tabWidget()->currentIndex())
               .pixmap(16, 16)
               .toImage(),
           QIcon(":/branches.png").pixmap(16, 16).toImage());
  window.tabWidget()->setCurrentIndex(0);

  QVERIFY(parentView->openSubmodule(selected));
  RepoView *childView = window.currentView();
  QVERIFY(childView && childView != parentView);
  QCOMPARE(childView, ordinaryChildView);
  QVERIFY(childView->isSubmoduleTab());
  QCOMPARE(window.tabWidget()
               ->tabIcon(window.tabWidget()->currentIndex())
               .pixmap(16, 16)
               .toImage(),
           QIcon(":/submodules.png").pixmap(16, 16).toImage());
  QSignalSpy pushedStatusChanged(parentView,
                                 &RepoView::submoduleUpdateStatusesChanged);

  git.setWorkingDirectory(child->workdir().path());
  git.start(GIT_EXECUTABLE,
            {"fetch", parent->workdir().filePath("child"), childBranch});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"reset", "--hard", localCommit.toString()});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  childView->pushSucceeded(childView->repo().workdir().canonicalPath());
  QTRY_VERIFY(!pushedStatusChanged.isEmpty());

  window.tabWidget()->setCurrentWidget(parentView);
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginAheadRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginBehindRole).toInt(),
           0);

  git.start(GIT_EXECUTABLE, {"reset", "--hard", originCommit.toString()});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  pushedStatusChanged.clear();
  parentView->checkSubmoduleUpdates(QList<git::Submodule>{selected}, true);
  QTRY_VERIFY(!pushedStatusChanged.isEmpty());

  parentView->commitSubmoduleChanges(selected);
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PinnedAheadRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::PinnedBehindRole).toInt(),
           0);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginAheadRole).toInt(),
           1);
  QCOMPARE(submodule.data(RepositoryNavigatorModel::OriginBehindRole).toInt(),
           1);

  QList<QPair<QString, bool>> behindMenu =
      contextMenuItems(submodulesView, submodule);
  QVERIFY(menuTexts(behindMenu)
              .contains(QString("Checkout origin/%1").arg(childBranch)));
  git::Id originTarget =
      submodule.data(RepositoryNavigatorModel::OriginTargetRole)
          .value<git::Id>();
  QVERIFY(originTarget.isValid());
  selected = parentView->repo().lookupSubmodule("child");
  git::Id parentPin = selected.indexId();
  QCOMPARE(parentPin, localCommit);
  QVERIFY(
      parentView->checkoutSubmoduleOrigin("child", childBranch, originTarget));
  selected = parentView->repo().lookupSubmodule("child");
  QCOMPARE(selected.workdirId(), originTarget);
  QCOMPARE(selected.indexId(), parentPin);
  QVERIFY(selected.open().isHeadDetached());

  git.setWorkingDirectory(parent->workdir().filePath("child"));
  git.start(GIT_EXECUTABLE, {"checkout", "--detach", localCommit.toString()});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  QFile secondLocalFile(parent->workdir().filePath("child/second.txt"));
  QVERIFY(secondLocalFile.open(QIODevice::WriteOnly));
  secondLocalFile.write("second local change\n");
  secondLocalFile.close();
  git.start(GIT_EXECUTABLE, {"add", "second.txt"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "-m", "second local change"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  parent->invalidateSubmoduleCache();
  parentView->repo().invalidateSubmoduleCache();
  git::Id secondLocalCommit =
      parentView->repo().lookupSubmodule("child").workdirId();

  git.setWorkingDirectory(parent->workdir().path());
  git.start(GIT_EXECUTABLE, {"submodule", "set-branch", "--default", "child"});
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
  QVERIFY(submodule.data(Qt::ToolTipRole)
              .toString()
              .contains("Not shown because no remote branch is configured"));
  selected = parentView->repo().lookupSubmodule("child");
  QCOMPARE(selected.open().head().target().id(), secondLocalCommit);
  QCOMPARE(selected.indexId(), parentPin);
  QVERIFY(parentView->repo().head().isLocalBranch());
  QCOMPARE(parentView->repo().state(), GIT_REPOSITORY_STATE_NONE);
  QCOMPARE(parentView->repo().head().target().tree().id("child"), parentPin);
  QVERIFY(parentPin != secondLocalCommit);
  QVERIFY(parentView->canCommitSubmoduleChanges(selected));

  QList<QPair<QString, bool>> menu =
      contextMenuItems(submodulesView, submodule);
  QCOMPARE(menuTexts(menu),
           QStringList({"Open", "Commit Changes", "Check for Updates", "Update",
                        "Modify...", "Delete Submodule..."}));
  for (int i = 0; i < menu.size(); ++i)
    QCOMPARE(menu.at(i).second, i != 2);

  selected = submodule.data(RepositoryNavigatorModel::SubmoduleRole)
                 .value<git::Submodule>();
  git::Id oldPin = parentView->repo().head().target().tree().id("child");
  git::Commit childHead = selected.open().head().target();
  git::Id newPin = childHead.id();
  parentView->commitSubmoduleChanges(selected);
  git::Commit submoduleCommit = parentView->repo().head().target();
  QString expectedMessage =
      QString("Update child from %1 to %2:\n- second local change")
          .arg(oldPin.toString().left(7), newPin.toString().left(7));
  QCOMPARE(submoduleCommit.message().trimmed(), expectedMessage);
  QCOMPARE(submoduleCommit.tree().id("child"), newPin);
  QVERIFY(!parentView->canCommitSubmoduleChanges(selected));

  QSignalSpy configurationCheck(parentView,
                                &RepoView::submoduleUpdateStatusesChanged);
  selected = parentView->repo().lookupSubmodule("child");
  QVERIFY(parentView->modifySubmodule(selected.name(), selected.name(),
                                      selected.path(), selected.url(),
                                      childBranch));
  QTRY_VERIFY(!configurationCheck.isEmpty());

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
  confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
  QVERIFY(confirmation);
  QVERIFY(confirmation->informativeText().contains("uncommitted changes"));
  confirmation->button(QMessageBox::Cancel)->click();

  window.tabWidget()->setCurrentIndex(0);

  QVERIFY(QMetaObject::invokeMethod(submodulesView, "doubleClicked",
                                    Q_ARG(QModelIndex, submodule)));
  QTRY_COMPARE(window.count(), 2);
  QVERIFY(window.currentView()->isSubmoduleTab());
  QCOMPARE(window.tabWidget()
               ->tabIcon(window.tabWidget()->currentIndex())
               .pixmap(16, 16)
               .toImage(),
           QIcon(":/submodules.png").pixmap(16, 16).toImage());
  QCOMPARE(window.currentView()->repo().workdir().path(),
           parent->workdir().filePath("child"));
  QCOMPARE(window.tabWidget()->tabText(window.tabWidget()->currentIndex()),
           QString("%1 / child").arg(parent->workdir().dirName()));

  window.tabWidget()->setCurrentIndex(0);
  selected = parentView->repo().lookupSubmodule("child");
  selected.deinitialize();
  parentView->repo().invalidateSubmoduleCache();
  navigator->model()->refresh();
  submodules = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Submodules);
  submodule = navigator->model()->index(0, 0, submodules);
  QVERIFY(!submodule.data(RepositoryNavigatorModel::InitializedRole).toBool());
  menu = contextMenuItems(submodulesView, submodule);
  QCOMPARE(menuTexts(menu),
           QStringList({"Initialize", "Initialize All Uninitialized", "Open",
                        "Commit Changes", "Check for Updates", "Modify...",
                        "Delete Submodule..."}));
}

void TestRepositorySideBar::submoduleInitialization() {
  Test::ScratchRepository child;
  QVERIFY(writeFile(child, "child.txt", "child\n"));
  QCOMPARE(runGit(child, {"add", "child.txt"}), 0);
  QCOMPARE(runGit(child, {"commit", "-m", "child"}), 0);

  Test::ScratchRepository parent;
  QCOMPARE(runGit(parent, {"-c", "protocol.file.allow=always", "submodule",
                           "add", child->workdir().path(), "child-one"}),
           0);
  QCOMPARE(runGit(parent, {"-c", "protocol.file.allow=always", "submodule",
                           "add", child->workdir().path(), "child-two"}),
           0);
  QCOMPARE(runGit(parent, {"commit", "-am", "add submodules"}), 0);
  git::Branch linkedBranch =
      parent->createBranch("linked", parent->head().target());
  QVERIFY(linkedBranch.isValid());
  const QString linkedPath = parent->workdir().path() + ".worktrees/linked";
  QVERIFY(QDir().mkpath(QFileInfo(linkedPath).dir().path()));
  git::Result result;
  git::Repository linked = parent->createWorktree(
      "linked", linkedPath, linkedBranch, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(linked.isValid());

  MainWindow window(linked);
  RepoView *parentView = window.currentView();
  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(parentView);
  QVERIFY(navigator);
  git::Submodule invalidSubmodule;
  QVERIFY(!invalidSubmodule.isInitialized());
  QVERIFY(!parentView->canCommitSubmoduleChanges(invalidSubmodule));
  QTreeView *submodulesView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Submodules);
  QVERIFY(submodulesView);

  auto submoduleIndex = [navigator](const QString &path) {
    QModelIndex section = navigator->model()->sectionIndex(
        RepositoryNavigatorModel::Section::Submodules);
    for (int row = 0; row < navigator->model()->rowCount(section); ++row) {
      QModelIndex index = navigator->model()->index(row, 0, section);
      if (index.data(RepositoryNavigatorModel::PathRole).toString() == path)
        return index;
    }
    return QModelIndex();
  };

  QModelIndex uninitialized = submoduleIndex("child-one");
  QModelIndex secondUninitialized = submoduleIndex("child-two");
  QVERIFY(uninitialized.isValid());
  QVERIFY(secondUninitialized.isValid());
  QStringList uninitializedMenu =
      menuTexts(contextMenuItems(submodulesView, uninitialized));
  QVERIFY(uninitializedMenu.contains("Initialize"));
  QVERIFY(uninitializedMenu.contains("Initialize All Uninitialized"));

  QSignalSpy initializationFinished(parentView, &RepoView::submodulesChanged);
  QVERIFY(triggerContextMenuItem(submodulesView, uninitialized, "Initialize"));
  QTRY_VERIFY(!initializationFinished.isEmpty());
  QTRY_VERIFY(QFileInfo::exists(linked.workdir().filePath("child-one/.git")));
  QTRY_VERIFY(parentView->repo().lookupSubmodule("child-one").isInitialized());

  QModelIndex initialized = submoduleIndex("child-one");
  QStringList initializedMenu =
      menuTexts(contextMenuItems(submodulesView, initialized));
  QVERIFY(!initializedMenu.contains("Initialize"));
  QVERIFY(initializedMenu.contains("Initialize All Uninitialized"));

  initializationFinished.clear();
  QVERIFY(triggerContextMenuItem(submodulesView, initialized,
                                 "Initialize All Uninitialized"));
  QTRY_VERIFY(!initializationFinished.isEmpty());
  QTRY_VERIFY(QFileInfo::exists(linked.workdir().filePath("child-two/.git")));
  QTRY_VERIFY(parentView->repo().lookupSubmodule("child-two").isInitialized());

  initialized = submoduleIndex("child-one");
  initializedMenu = menuTexts(contextMenuItems(submodulesView, initialized));
  QVERIFY(!initializedMenu.contains("Initialize"));
  QVERIFY(!initializedMenu.contains("Initialize All Uninitialized"));
}

void TestRepositorySideBar::worktreeSubmoduleInitialization() {
  Test::ScratchRepository grandchild;
  QVERIFY(writeFile(grandchild, "grandchild.txt", "grandchild\n"));
  QCOMPARE(runGit(grandchild, {"add", "grandchild.txt"}), 0);
  QCOMPARE(runGit(grandchild, {"commit", "-m", "grandchild"}), 0);

  Test::ScratchRepository child;
  QCOMPARE(runGit(child, {"-c", "protocol.file.allow=always", "submodule",
                          "add", grandchild->workdir().path(), "grandchild"}),
           0);
  QCOMPARE(runGit(child, {"commit", "-am", "add grandchild"}), 0);

  Test::ScratchRepository parent;
  QCOMPARE(runGit(parent, {"-c", "protocol.file.allow=always", "submodule",
                           "add", child->workdir().path(), "child"}),
           0);
  QCOMPARE(runGit(parent, {"commit", "-am", "add child"}), 0);
  git::Branch feature =
      parent->createBranch("feature", parent->head().target());
  QVERIFY(feature.isValid());
  git::Branch uninitializedBranch =
      parent->createBranch("uninitialized", parent->head().target());
  QVERIFY(uninitializedBranch.isValid());

  const QString uninitializedPath =
      parent->workdir().path() + ".worktrees/uninitialized";
  QVERIFY(QDir().mkpath(QFileInfo(uninitializedPath).dir().path()));
  git::Result result;
  git::Repository uninitializedWorktree =
      parent->createWorktree("uninitialized", uninitializedPath,
                             uninitializedBranch, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(uninitializedWorktree.isValid());
  QVERIFY(!uninitializedWorktree.lookupSubmodule("child").isInitialized());

  {
    MainWindow uninitializedWindow(uninitializedWorktree);
    RepositoryNavigator *uninitializedNavigator =
        uninitializedWindow.findChild<RepositoryNavigator *>(
            "RepositoryNavigator");
    QVERIFY(uninitializedNavigator);
    QModelIndex submodules = uninitializedNavigator->model()->sectionIndex(
        RepositoryNavigatorModel::Section::Submodules);
    QModelIndex submodule =
        uninitializedNavigator->model()->index(0, 0, submodules);
    QTreeView *submodulesView = uninitializedNavigator->sectionView(
        RepositoryNavigatorModel::Section::Submodules);
    QVERIFY(submodule.isValid());
    QVERIFY(submodulesView);
    QStringList menu = menuTexts(contextMenuItems(submodulesView, submodule));
    QVERIFY(menu.contains("Initialize"));
    QVERIFY(menu.contains("Initialize All Uninitialized"));
    QVERIFY(!menu.contains("Update"));
  }

  MainWindow window(parent);
  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);
  QToolButton *add =
      navigator->findChild<QToolButton *>("RepositoryNavigationWorktreesAdd");
  QVERIFY(add);
  QSignalSpy openSpy(navigator, &RepositoryNavigator::openRepositoryRequested);

  add->click();
  WorktreeDialog *dialog = navigator->findChild<WorktreeDialog *>();
  QTRY_VERIFY(dialog);
  ReferenceList *branches =
      dialog->findChild<ReferenceList *>("WorktreeBranch");
  QCheckBox *initializeSubmodules =
      dialog->findChild<QCheckBox *>("WorktreeInitializeSubmodules");
  QPushButton *create = dialog->findChild<QPushButton *>("WorktreeCreate");
  QVERIFY(branches);
  QVERIFY(initializeSubmodules);
  QVERIFY(create);
  QVERIFY(initializeSubmodules->isChecked());
  branches->select(feature);
  QVERIFY(create->isEnabled());
  create->click();

  QTRY_COMPARE(openSpy.count(), 1);
  QCOMPARE(openSpy.first().at(1).toBool(), true);
  QTRY_COMPARE(window.count(), 2);
  const QString worktreePath = openSpy.first().at(0).toString();
  QTRY_VERIFY(QFileInfo::exists(QDir(worktreePath).filePath("child/.git")));
  QTRY_VERIFY(
      QFileInfo::exists(QDir(worktreePath).filePath("child/grandchild/.git")));
}

void TestRepositorySideBar::worktreeDeletion() {
  Test::ScratchRepository parent;
  QVERIFY(parent->commit("initial").isValid());
  git::Branch feature =
      parent->createBranch("feature", parent->head().target());
  QVERIFY(feature.isValid());

  const QString root = parent->workdir().path() + ".worktrees";
  QVERIFY(QDir().mkpath(root));
  const QString linkedPath = QDir(root).filePath("feature");
  git::Result result;
  git::Repository linked = parent->createWorktree("feature", linkedPath,
                                                  feature, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(linked.isValid());

  MainWindow window(parent);
  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);
  QTreeView *worktreesView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Worktrees);
  QVERIFY(worktreesView);

  auto worktreeIndex = [navigator](const QString &name) {
    QModelIndex section = navigator->model()->sectionIndex(
        RepositoryNavigatorModel::Section::Worktrees);
    for (int row = 0; row < navigator->model()->rowCount(section); ++row) {
      QModelIndex index = navigator->model()->index(row, 0, section);
      git::Worktree worktree =
          index.data(RepositoryNavigatorModel::WorktreeRole)
              .value<git::Worktree>();
      if (worktree.name() == name)
        return index;
    }
    return QModelIndex();
  };

  QModelIndex home = worktreeIndex("Home");
  QModelIndex featureIndex = worktreeIndex("feature");
  QVERIFY(home.isValid());
  QVERIFY(featureIndex.isValid());
  const QList<QPair<QString, bool>> homeMenu{{"Delete Worktree...", false}};
  const QList<QPair<QString, bool>> linkedMenu{{"Delete Worktree...", true}};
  QCOMPARE(contextMenuItems(worktreesView, home), homeMenu);
  QCOMPARE(contextMenuItems(worktreesView, featureIndex), linkedMenu);

  QVERIFY(triggerContextMenuItem(worktreesView, featureIndex,
                                 "Delete Worktree..."));
  QTRY_VERIFY(QApplication::activeModalWidget());
  QMessageBox *confirmation =
      qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
  QVERIFY(confirmation);
  QCOMPARE(confirmation->windowTitle(), QString("Delete Worktree?"));
  QVERIFY(confirmation->defaultButton() ==
          confirmation->button(QMessageBox::Cancel));
  QVERIFY(!confirmation->checkBox());
  confirmation->button(QMessageBox::Cancel)->click();
  QVERIFY(QFileInfo::exists(linkedPath));

  QFile dirty(QDir(linkedPath).filePath("untracked.txt"));
  QVERIFY(dirty.open(QIODevice::WriteOnly));
  QVERIFY(dirty.write("uncommitted\n") > 0);
  dirty.close();
  QVERIFY(window.addTab(linked));
  QCOMPARE(window.count(), 2);

  featureIndex = worktreeIndex("feature");
  QVERIFY(featureIndex.isValid());
  QVERIFY(triggerContextMenuItem(worktreesView, featureIndex,
                                 "Delete Worktree..."));
  QTRY_VERIFY(QApplication::activeModalWidget());
  confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
  QVERIFY(confirmation);
  QCheckBox *acknowledge =
      confirmation->findChild<QCheckBox *>("WorktreeDataLossAcknowledgment");
  QVERIFY(acknowledge);
  QVERIFY(confirmation->informativeText().contains("uncommitted or untracked"));

  QPushButton *remove = nullptr;
  for (QAbstractButton *button : confirmation->buttons()) {
    if (button->text() == QString("Delete Worktree"))
      remove = qobject_cast<QPushButton *>(button);
  }
  QVERIFY(remove);
  QVERIFY(!remove->isEnabled());
  acknowledge->setChecked(true);
  QVERIFY(remove->isEnabled());
  remove->click();

  QTRY_COMPARE(window.count(), 1);
  QTRY_VERIFY(!QFileInfo::exists(linkedPath));
  QTRY_VERIFY(!QFileInfo::exists(root));
  QTRY_COMPARE(parent->worktrees().size(), 1);
  QVERIFY(parent->lookupBranch("feature", GIT_BRANCH_LOCAL).isValid());
  QModelIndex worktrees = navigator->model()->sectionIndex(
      RepositoryNavigatorModel::Section::Worktrees);
  QTRY_COMPARE(navigator->model()->rowCount(worktrees), 1);
}

void TestRepositorySideBar::worktreeTabs() {
  QTemporaryDir sandbox;
  QVERIFY(sandbox.isValid());

  git::Repository repo =
      git::Repository::init(QDir(sandbox.path()).filePath("repo"));
  QVERIFY(repo.isValid());
  Test::initRepo(repo);
  git::Commit commit = repo.commit("initial");
  QVERIFY(commit.isValid());
  git::Branch feature = repo.createBranch("feature", commit);
  QVERIFY(feature.isValid());
  git::Branch reserved = repo.createBranch("CON", commit);
  QVERIFY(reserved.isValid());

  WorktreeDialog dialog(repo);
  QCheckBox *initializeSubmodules =
      dialog.findChild<QCheckBox *>("WorktreeInitializeSubmodules");
  QVERIFY(initializeSubmodules);
  QVERIFY(initializeSubmodules->isChecked());
  QVERIFY(dialog.initializeSubmodules());
  initializeSubmodules->setChecked(false);
  QVERIFY(!dialog.initializeSubmodules());
  initializeSubmodules->setChecked(true);
  ReferenceList *branches = dialog.findChild<ReferenceList *>("WorktreeBranch");
  QVERIFY(branches);
  branches->select(reserved);
  QCOMPARE(dialog.worktreeName(), QString("CON-"));

  const QString path = QDir(sandbox.path()).filePath("repo.worktrees/feature");
  QVERIFY(QDir().mkpath(QFileInfo(path).dir().path()));
  git::Result result;
  git::Repository linked =
      repo.createWorktree("feature", path, feature, QString(), &result);
  QVERIFY2(result, qPrintable(result.errorString()));
  QVERIFY(linked.isValid());

  RepositoryNavigatorModel model;
  model.setRepository(repo);
  QModelIndex worktrees =
      model.sectionIndex(RepositoryNavigatorModel::Section::Worktrees);
  QCOMPARE(model.rowCount(worktrees), 2);
  QModelIndex home = model.index(0, 0, worktrees);
  QModelIndex tree = model.index(1, 0, worktrees);
  QCOMPARE(home.data().toString(), repo.head().name());
  QVERIFY(home.data(RepositoryNavigatorModel::MainWorktreeRole).toBool());
  QCOMPARE(tree.data().toString(), QString("feature"));
  QVERIFY(!tree.data(RepositoryNavigatorModel::MainWorktreeRole).toBool());

  MainWindow window(repo);
  window.resize(1200, 700);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));
  QCOMPARE(window.count(), 1);
  QIcon localIcon(":/branches.png");
  QCOMPARE(window.tabWidget()->tabIcon(0).pixmap(16, 16).toImage(),
           localIcon.pixmap(16, 16).toImage());

  RepositoryNavigator *navigator =
      window.findChild<RepositoryNavigator *>("RepositoryNavigator");
  QVERIFY(navigator);
  RepositoryNavigatorModel *navigatorModel = navigator->model();
  QModelIndex navigatorWorktrees = navigatorModel->sectionIndex(
      RepositoryNavigatorModel::Section::Worktrees);
  QTreeView *worktreeView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Worktrees);
  QTreeView *localView =
      navigator->sectionView(RepositoryNavigatorModel::Section::Local);
  QVERIFY(worktreeView);
  QVERIFY(localView);
  QToolButton *localToggle =
      navigator->findChild<QToolButton *>("RepositoryNavigationLocalToggle");
  QVERIFY(localToggle);
  QVERIFY(localToggle->isChecked());
  QVERIFY(localView->isVisible());
  QCOMPARE(localView->viewport()->backgroundRole(), QPalette::Window);
  QModelIndex checkedOutBranch = localView->currentIndex();
  QVERIFY(checkedOutBranch.isValid());
  QVERIFY(localView->selectionModel()->isSelected(checkedOutBranch));
  QCOMPARE(checkedOutBranch.data(RepositoryNavigatorModel::ReferenceRole)
               .value<git::Reference>()
               .qualifiedName(),
           repo.head().qualifiedName());
  QVERIFY(!worktreeView->currentIndex().isValid());
  QToolButton *worktreesToggle = navigator->findChild<QToolButton *>(
      "RepositoryNavigationWorktreesToggle");
  QVERIFY(worktreesToggle);
  for (QToolButton *toggle : navigator->findChildren<QToolButton *>()) {
    if (toggle != worktreesToggle && toggle->objectName().endsWith("Toggle"))
      toggle->setChecked(false);
  }
  worktreesToggle->setChecked(true);
  QTRY_VERIFY(worktreeView->isVisible());
  QTRY_VERIFY(worktreeView->height() > worktreeView->sizeHintForRow(0));
  QModelIndex navigatorHome = navigatorModel->index(0, 0, navigatorWorktrees);
  QModelIndex navigatorTree = navigatorModel->index(1, 0, navigatorWorktrees);
  QVERIFY(navigatorHome.isValid());
  QVERIFY(navigatorTree.isValid());

  worktreeView->scrollTo(navigatorTree);
  QTest::mouseClick(worktreeView->viewport(), Qt::LeftButton, Qt::NoModifier,
                    worktreeView->visualRect(navigatorTree).center());
  QCOMPARE(window.count(), 1);
  QCOMPARE(window.currentView()->repo().workdir().path(),
           repo.workdir().path());
  QCOMPARE(worktreeView->currentIndex()
               .data(RepositoryNavigatorModel::PathRole)
               .toString(),
           linked.workdir().path());
  QVERIFY(!localView->currentIndex().isValid());
  QCOMPARE(QApplication::focusWidget(), worktreeView);

  QTest::mouseDClick(worktreeView->viewport(), Qt::LeftButton, Qt::NoModifier,
                     worktreeView->visualRect(navigatorTree).center());
  QTRY_COMPARE(window.count(), 2);
  RepoView *linkedView = window.currentView();
  QCOMPARE(linkedView->repo().workdir().path(), linked.workdir().path());
  QCOMPARE(window.count(), 2);
  int linkedIndex = window.tabWidget()->indexOf(linkedView);
  QVERIFY(linkedIndex >= 0);
  QVERIFY(!window.tabWidget()->tabIcon(linkedIndex).isNull());
  QCOMPARE(window.tabWidget()->tabIcon(linkedIndex).cacheKey(),
           WorktreeIcon::icon().cacheKey());

  navigatorModel = navigator->model();
  navigatorWorktrees = navigatorModel->sectionIndex(
      RepositoryNavigatorModel::Section::Worktrees);
  navigatorHome = navigatorModel->index(0, 0, navigatorWorktrees);
  navigatorTree = navigatorModel->index(1, 0, navigatorWorktrees);
  worktreeView->scrollTo(navigatorHome, QAbstractItemView::PositionAtTop);
  QTRY_VERIFY(worktreeView->viewport()->rect().contains(
      worktreeView->visualRect(navigatorHome).center()));
  QTest::mouseClick(worktreeView->viewport(), Qt::LeftButton, Qt::NoModifier,
                    worktreeView->visualRect(navigatorHome).center());
  QTRY_COMPARE(window.tabWidget()->currentIndex(), 0);
  QCOMPARE(worktreeView->currentIndex()
               .data(RepositoryNavigatorModel::PathRole)
               .toString(),
           repo.workdir().path());
  QVERIFY(!localView->currentIndex().isValid());
  QCOMPARE(QApplication::focusWidget(), worktreeView);
  navigatorModel = navigator->model();
  navigatorWorktrees = navigatorModel->sectionIndex(
      RepositoryNavigatorModel::Section::Worktrees);
  navigatorTree = navigatorModel->index(1, 0, navigatorWorktrees);
  worktreeView->scrollTo(navigatorTree, QAbstractItemView::PositionAtBottom);
  QTRY_VERIFY(worktreeView->viewport()->rect().contains(
      worktreeView->visualRect(navigatorTree).center()));
  QTest::mouseClick(worktreeView->viewport(), Qt::LeftButton, Qt::NoModifier,
                    worktreeView->visualRect(navigatorTree).center());
  QTRY_COMPARE(window.tabWidget()->currentIndex(), linkedIndex);
  QCOMPARE(worktreeView->currentIndex()
               .data(RepositoryNavigatorModel::PathRole)
               .toString(),
           linked.workdir().path());
  QVERIFY(!localView->currentIndex().isValid());
  QCOMPARE(QApplication::focusWidget(), worktreeView);

  navigator->model()->refresh();
  QTRY_COMPARE(worktreeView->currentIndex()
                   .data(RepositoryNavigatorModel::PathRole)
                   .toString(),
               linked.workdir().path());
  QVERIFY(!localView->currentIndex().isValid());

  QCOMPARE(window.addTab(QDir(path).filePath(".")), linkedView);
  QCOMPARE(window.count(), 2);
}

void TestRepositorySideBar::cleanupTestCase() { mWindow->close(); }

int main(int argc, char *argv[]) {
  QTemporaryDir settings;
  if (!settings.isValid())
    return 1;

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  qunsetenv("GITNORTEK_OAUTH");
  qunsetenv("GITTYUP_OAUTH");

  Application::setInTest();
  int testArgc = argc;
  auto app = Test::createApp(argc, argv);
  TestRepositorySideBar test;
  return QTest::qExec(&test, testArgc, argv);
}

#include "repository_sidebar.moc"

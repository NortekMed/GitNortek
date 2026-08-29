//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Test.h"
#include "conf/RecentRepositories.h"
#include "conf/RecentRepository.h"
#include "conf/Settings.h"
#include "dialogs/FastIssueDialog.h"
#include "dialogs/RenameBranchDialog.h"
#include "editor/TextEditor.h"
#include "git/Config.h"
#include "host/GitHub.h"
#include "ui/CommitList.h"
#include "ui/DetailView.h"
#include "ui/FontUtils.h"
#include "ui/MainWindow.h"
#include "ui/MenuBar.h"
#include "ui/RepoView.h"
#include "ui/RepositoryNavigatorModel.h"
#include "ui/TabWidget.h"
#include "ui/ToolBar.h"
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTimer>
#include <QToolButton>

using namespace Test;
using namespace QTest;

class TestMainWindow : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void show();
  void fastIssueAccess();
  void initialRefreshOnce();
  void navigatorRefreshCoalesced();
  void diffPresentationControls();
  void consistentBodyFontSize();
  void commitReferencesOnSecondLine();
  void toggleLogPanel();
  void preserveSelectionAfterRemoteUpdate();
  void cancelRemoteBranchCreation();
  void createAndTrackRemoteBranch();
  void trackExistingRemoteBranch();
  void pushTrackedBranchWithoutPrompt();
  void renameRemoteBranch();
  void deleteRemoteBranch();
  void forcePushResetBranch();
  void closeTab();
  void recentRepositoryLimit();
  void invalidRecentRepository();
  void restoreActiveRepositoryOnly();
  void cleanupTestCase();

private:
  QTemporaryDir mRemoteDir;
  QTemporaryDir mInvalidRepoDir;
  ScratchRepository mRepo;
  ScratchRepository mSecondRepo;
  QStringList mRecentRepositories;
  MainWindow *mWindow = nullptr;
};

void TestMainWindow::initTestCase() {
  mRecentRepositories = QSettings().value("recent").toStringList();

  QProcess git;
  git.start(GIT_EXECUTABLE, {"init", "--bare", mRemoteDir.path()});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git.setWorkingDirectory(mRepo->workdir().path());
  for (int i = 1; i <= 3; ++i) {
    git.start(GIT_EXECUTABLE,
              {"commit", "--allow-empty", "-m", QString("commit %1").arg(i)});
    QVERIFY(git.waitForFinished());
    QCOMPARE(git.exitCode(), 0);
  }

  QVERIFY(mRepo->addRemote("origin", mRemoteDir.path()).isValid());
  mRepo->appConfig().setValue("autofetch.enable", false);

  mWindow = new MainWindow(mRepo);
}

void TestMainWindow::show() {
  mWindow->show();
  QVERIFY(qWaitForWindowActive(mWindow));
}

void TestMainWindow::fastIssueAccess() {
  ToolBar *toolbar = mWindow->toolBar();
  QToolButton *button = toolbar->findChild<QToolButton *>("FastIssueButton");
  QVERIFY(button);

  toolbar->setFastIssueAccount(nullptr);
  QTRY_VERIFY(!button->isVisible());

  GitHub account("member");
  toolbar->setFastIssueAccount(&account);
  QTRY_VERIFY(button->isVisible());

  QTest::mouseClick(button, Qt::LeftButton);
  QPointer<FastIssueDialog> dialog =
      mWindow->findChild<FastIssueDialog *>();
  QVERIFY(dialog);
  QVERIFY(dialog->isVisible());
  dialog->close();
  QTRY_VERIFY(!dialog);

  button->click();
  dialog = mWindow->findChild<FastIssueDialog *>();
  QVERIFY(dialog);
  QVERIFY(dialog->isVisible());
  dialog->close();
  QTRY_VERIFY(!dialog);

  toolbar->setFastIssueAccount(nullptr);
  QTRY_VERIFY(!button->isVisible());

  GitHub *staleAccount = new GitHub("stale-member");
  toolbar->setFastIssueAccount(staleAccount);
  delete staleAccount;
  QTRY_VERIFY(button->isVisible());
  QTest::mouseClick(button, Qt::LeftButton);
  QTRY_VERIFY(!button->isVisible());
}

void TestMainWindow::initialRefreshOnce() {
  ScratchRepository repo;
  QSignalSpy refreshed(repo->notifier(),
                       &git::RepositoryNotifier::referenceUpdated);
  MainWindow window(repo, nullptr, Qt::WindowFlags(), false);
  QCOMPARE(refreshed.count(), 1);
}

void TestMainWindow::navigatorRefreshCoalesced() {
  RepositoryNavigatorModel model;
  model.setRepository(mRepo);
  QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

  emit mRepo->notifier()->referenceUpdated(mRepo->head());
  emit mRepo->notifier()->referenceUpdated(mRepo->head());
  emit mRepo->notifier()->referenceUpdated(mRepo->head());

  QTRY_COMPARE(reset.count(), 1);
}

void TestMainWindow::diffPresentationControls() {
  Settings *settings = Settings::instance();
  QToolButton *inlineMode = mWindow->findChild<QToolButton *>("InlineDiffMode");
  QToolButton *hunkMode = mWindow->findChild<QToolButton *>("HunkDiffMode");
  QToolButton *splitMode = mWindow->findChild<QToolButton *>("SplitDiffMode");
  QToolButton *ignoreWhitespace =
      mWindow->findChild<QToolButton *>("IgnoreEdgeWhitespace");
  QToolButton *wordWrap = mWindow->findChild<QToolButton *>("DiffWordWrap");
  QVERIFY(inlineMode);
  QVERIFY(hunkMode);
  QVERIFY(splitMode);
  QVERIFY(ignoreWhitespace);
  QVERIFY(wordWrap);

  mouseClick(hunkMode, Qt::LeftButton);
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Hunk);
  QVERIFY(hunkMode->isChecked());
  QVERIFY(!inlineMode->isChecked());
  mouseClick(splitMode, Qt::LeftButton);
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Split);
  QVERIFY(splitMode->isChecked());

  const bool ignored = settings->isEdgeWhitespaceIgnored();
  mouseClick(ignoreWhitespace, Qt::LeftButton);
  QCOMPARE(settings->isEdgeWhitespaceIgnored(), !ignored);
  const bool wrapped = settings->isTextEditorWrapLines();
  mouseClick(wordWrap, Qt::LeftButton);
  QCOMPARE(settings->isTextEditorWrapLines(), !wrapped);

  settings->setEdgeWhitespaceIgnored(ignored);
  settings->setTextEditorWrapLines(wrapped);
  mouseClick(inlineMode, Qt::LeftButton);
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Inline);
}

void TestMainWindow::consistentBodyFontSize() {
  RepoView *view = mWindow->currentView();
  CommitList *commits = view->findChild<CommitList *>();
  DetailView *details = view->findChild<DetailView *>();
  MenuBar *menuBar = MenuBar::instance(mWindow);
  QVERIFY(commits);
  QVERIFY(details);
  QVERIFY(menuBar);

  const int pointSize = 8;
  QCOMPARE(FontUtils::pointSize(details->font()), pointSize);
  for (QWidget *widget : details->findChildren<QWidget *>()) {
    if (widget->inherits("QScrollBar"))
      continue;
    const QString message =
        QString("%1 (%2)")
            .arg(widget->metaObject()->className(), widget->objectName());
    QVERIFY2(FontUtils::pointSize(widget->font()) == pointSize,
             qPrintable(message));
  }
  QCOMPARE(FontUtils::pointSize(menuBar->font()), pointSize);
  for (QMenu *menu : menuBar->findChildren<QMenu *>())
    QCOMPARE(FontUtils::pointSize(menu->font()), pointSize);

  const QList<TextEditor *> editors = view->findChildren<TextEditor *>();
  QVERIFY(!editors.isEmpty());
  for (TextEditor *editor : editors)
    QCOMPARE(editor->styleFont(STYLE_DEFAULT).pointSize(), pointSize);
}

void TestMainWindow::commitReferencesOnSecondLine() {
  mWindow->currentView()->selectHead();
  QWidget *id = mWindow->findChild<QWidget *>("CommitId");
  QWidget *references = mWindow->findChild<QWidget *>("CommitReferences");
  QVERIFY(id);
  QVERIFY(references);
  QTRY_VERIFY(references->isVisible());
  QTRY_VERIFY(references->geometry().top() > id->geometry().bottom());
}

void TestMainWindow::toggleLogPanel() {
  RepoView *view = mWindow->currentView();
  QWidget *panel = view->findChild<QWidget *>("RepositoryLogPanel");
  QWidget *header = view->findChild<QWidget *>("RepositoryLogHeader");
  QToolButton *toggle =
      view->findChild<QToolButton *>("RepositoryLogToggle");
  QVERIFY(panel);
  QVERIFY(header);
  QVERIFY(toggle);
  QVERIFY(!view->isLogVisible());
  QTRY_COMPARE(panel->height(), header->height());
  QVERIFY(header->isVisible());
  QCOMPARE(toggle->arrowType(), Qt::UpArrow);

  toggle->click();

  QTRY_VERIFY(view->isLogVisible());
  QTRY_VERIFY(panel->height() > header->height());
  QCOMPARE(toggle->arrowType(), Qt::DownArrow);

  toggle->click();

  QTRY_VERIFY(!view->isLogVisible());
  QTRY_COMPARE(panel->height(), header->height());

  view->addLogEntry("output", "operation");

  QTRY_VERIFY(view->isLogVisible());
  QTRY_VERIFY(panel->height() > header->height());
  QCOMPARE(toggle->arrowType(), Qt::DownArrow);
}

void TestMainWindow::preserveSelectionAfterRemoteUpdate() {
  RepoView *view = mWindow->currentView();
  CommitList *commits = view->findChild<CommitList *>();
  QVERIFY(commits);

  git::Commit selected = mRepo->head().target().parents().first();
  QVERIFY(selected.isValid());
  QVERIFY(commits->selectRange(selected.id().toString()));
  QCOMPARE(commits->selectedRange(), selected.id().toString());

  git::Reference head = mRepo->head();
  const QString remoteName = QString("refs/remotes/origin/%1").arg(head.name());
  bool notified = false;
  connect(mRepo->notifier(), &git::RepositoryNotifier::referenceUpdated,
          [&notified, remoteName](const git::Reference &ref,
                                  bool restoreSelection) {
            if (ref.qualifiedName() == remoteName) {
              QVERIFY(restoreSelection);
              notified = true;
            }
          });

  view->push(mRepo->lookupRemote("origin"), head);

  QTRY_VERIFY_WITH_TIMEOUT(notified, 10000);
  QCOMPARE(mRepo->lookupRef(remoteName).target().id(), head.target().id());

  QCOMPARE(commits->selectedRange(), selected.id().toString());
}

void TestMainWindow::cancelRemoteBranchCreation() {
  git::Branch branch = mRepo->createBranch("create-on-origin");
  QVERIFY(branch.isValid());
  QVERIFY(mRepo->setHead(branch));
  QVERIFY(mRepo->defaultRemote().isValid());
  QVERIFY(!git::Branch(mRepo->head()).upstream().isValid());
  QVERIFY(!mRepo->lookupBranch("origin/create-on-origin", GIT_BRANCH_REMOTE));

  RepoView *view = mWindow->currentView();
  view->push();
  QTRY_VERIFY(view->findChild<QMessageBox *>());
  QMessageBox *dialog = view->findChild<QMessageBox *>();
  QVERIFY(dialog);
  QCOMPARE(dialog->windowTitle(), QString("Create Remote Branch?"));
  QVERIFY(dialog->text().contains("does not exist on 'origin'"));
  dialog->button(QMessageBox::Cancel)->click();
  QTRY_VERIFY(!view->findChild<QMessageBox *>());

  QVERIFY(!mRepo->lookupBranch("origin/create-on-origin", GIT_BRANCH_REMOTE));
  QVERIFY(!git::Branch(mRepo->head()).upstream().isValid());
}

void TestMainWindow::createAndTrackRemoteBranch() {
  RepoView *view = mWindow->currentView();
  view->push();
  QTRY_VERIFY(view->findChild<QMessageBox *>());
  QMessageBox *dialog = view->findChild<QMessageBox *>();
  QVERIFY(dialog);

  QPushButton *create = nullptr;
  foreach (QPushButton *button, dialog->findChildren<QPushButton *>()) {
    if (button->text() == "Create Branch") {
      create = button;
      break;
    }
  }
  QVERIFY(create);
  create->click();
  QTRY_VERIFY(!view->findChild<QMessageBox *>());

  QTRY_VERIFY_WITH_TIMEOUT(
      mRepo->lookupBranch("origin/create-on-origin", GIT_BRANCH_REMOTE)
          .isValid(),
      10000);
  QTRY_VERIFY_WITH_TIMEOUT(git::Branch(mRepo->head()).upstream().isValid(),
                           10000);
  QCOMPARE(git::Branch(mRepo->head()).upstream().name(),
           QString("origin/create-on-origin"));
}

void TestMainWindow::trackExistingRemoteBranch() {
  RepoView *view = mWindow->currentView();
  git::Branch branch = mRepo->createBranch("existing-on-origin");
  QVERIFY(branch.isValid());
  QVERIFY(mRepo->setHead(branch));

  view->push(mRepo->lookupRemote("origin"), branch);
  QTRY_VERIFY_WITH_TIMEOUT(
      mRepo->lookupBranch("origin/existing-on-origin", GIT_BRANCH_REMOTE)
          .isValid(),
      10000);
  QVERIFY(!git::Branch(mRepo->head()).upstream().isValid());

  view->push();
  QTRY_VERIFY(view->findChild<QMessageBox *>());
  QMessageBox *dialog = view->findChild<QMessageBox *>();
  QVERIFY(dialog);
  QCOMPARE(dialog->windowTitle(), QString("Track Remote Branch?"));
  dialog->button(QMessageBox::Cancel)->click();
  QTRY_VERIFY(!view->findChild<QMessageBox *>());
  QVERIFY(!git::Branch(mRepo->head()).upstream().isValid());

  view->push();
  QTRY_VERIFY(view->findChild<QMessageBox *>());
  dialog = view->findChild<QMessageBox *>();
  QVERIFY(dialog);

  QPushButton *track = nullptr;
  foreach (QPushButton *button, dialog->findChildren<QPushButton *>()) {
    if (button->text() == "Track and Push") {
      track = button;
      break;
    }
  }
  QVERIFY(track);
  track->click();
  QTRY_VERIFY(!view->findChild<QMessageBox *>());

  QTRY_VERIFY_WITH_TIMEOUT(git::Branch(mRepo->head()).upstream().isValid(),
                           10000);
  QCOMPARE(git::Branch(mRepo->head()).upstream().name(),
           QString("origin/existing-on-origin"));
}

void TestMainWindow::pushTrackedBranchWithoutPrompt() {
  QVERIFY(git::Branch(mRepo->head()).upstream().isValid());
  RepoView *view = mWindow->currentView();
  view->push();
  QTest::qWait(100);
  QVERIFY(!view->findChild<QMessageBox *>());
}

void TestMainWindow::renameRemoteBranch() {
  RepoView *view = mWindow->currentView();
  git::Branch branch = mRepo->createBranch("rename-on-origin");
  QVERIFY(branch.isValid());
  view->push(mRepo->lookupRemote("origin"), branch);

  QTRY_VERIFY_WITH_TIMEOUT(
      mRepo->lookupBranch("origin/rename-on-origin", GIT_BRANCH_REMOTE)
          .isValid(),
      10000);
  git::Branch remote =
      mRepo->lookupBranch("origin/rename-on-origin", GIT_BRANCH_REMOTE);
  branch.setUpstream(remote);
  QCOMPARE(branch.upstream().name(), QString("origin/rename-on-origin"));

  git::Repository published = git::Repository::open(mRemoteDir.path());
  QVERIFY(published.isValid());
  const QString oldQualifiedName = "refs/heads/rename-on-origin";
  const QString newQualifiedName = "refs/heads/renamed-on-origin";
  QVERIFY(published.lookupRef(oldQualifiedName).isValid());

  view->promptToRenameBranch(remote);
  QTRY_VERIFY(view->findChild<RenameBranchDialog *>());
  RenameBranchDialog *dialog = view->findChild<RenameBranchDialog *>();
  QVERIFY(dialog);
  QLineEdit *name = dialog->findChild<QLineEdit *>();
  QVERIFY(name);
  QCOMPARE(name->text(), QString("rename-on-origin"));
  name->setText("canceled-rename");
  dialog->reject();
  QTRY_VERIFY(!view->findChild<RenameBranchDialog *>());
  QVERIFY(published.lookupRef(oldQualifiedName).isValid());
  QVERIFY(!published.lookupRef("refs/heads/canceled-rename").isValid());

  view->promptToRenameBranch(remote);
  QTRY_VERIFY(view->findChild<RenameBranchDialog *>());
  dialog = view->findChild<RenameBranchDialog *>();
  name = dialog->findChild<QLineEdit *>();
  QPushButton *rename = nullptr;
  for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
    if (button->text() == "Rename Branch") {
      rename = button;
      break;
    }
  }
  QVERIFY(rename);
  name->setText("existing-on-origin");
  QVERIFY(!rename->isEnabled());
  name->setText("renamed-on-origin");
  QVERIFY(rename->isEnabled());
  rename->click();

  QTRY_VERIFY_WITH_TIMEOUT(published.lookupRef(newQualifiedName).isValid(),
                           10000);
  QTRY_VERIFY_WITH_TIMEOUT(!published.lookupRef(oldQualifiedName).isValid(),
                           10000);
  QTRY_VERIFY_WITH_TIMEOUT(
      mRepo->lookupBranch("origin/renamed-on-origin", GIT_BRANCH_REMOTE)
          .isValid(),
      10000);
  QTRY_VERIFY_WITH_TIMEOUT(
      !mRepo->lookupBranch("origin/rename-on-origin", GIT_BRANCH_REMOTE)
           .isValid(),
      10000);
  QVERIFY(mRepo->lookupBranch("rename-on-origin", GIT_BRANCH_LOCAL).isValid());
  QCOMPARE(mRepo->gitConfig().value<QString>("branch.rename-on-origin.merge"),
           QString("refs/heads/rename-on-origin"));
  QVERIFY(!branch.upstream().isValid());
}

void TestMainWindow::deleteRemoteBranch() {
  RepoView *view = mWindow->currentView();
  git::Branch branch = mRepo->createBranch("delete-on-origin");
  QVERIFY(branch.isValid());
  view->push(mRepo->lookupRemote("origin"), branch);

  QTRY_VERIFY_WITH_TIMEOUT(
      mRepo->lookupBranch("origin/delete-on-origin", GIT_BRANCH_REMOTE)
          .isValid(),
      10000);
  git::Repository published = git::Repository::open(mRemoteDir.path());
  QVERIFY(published.isValid());
  const QString qualifiedName = "refs/heads/delete-on-origin";
  QVERIFY(published.lookupRef(qualifiedName).isValid());

  git::Branch remote =
      mRepo->lookupBranch("origin/delete-on-origin", GIT_BRANCH_REMOTE);
  view->promptToDeleteBranch(remote);
  QTRY_VERIFY(view->findChild<QMessageBox *>());
  QMessageBox *dialog = view->findChild<QMessageBox *>();
  QVERIFY(dialog);
  QCOMPARE(dialog->windowTitle(), QString("Delete Remote Branch?"));
  QCOMPARE(dialog->text(),
           QString("Are you sure you want to delete remote branch "
                   "'origin/delete-on-origin'?"));
  dialog->button(QMessageBox::Cancel)->click();
  QTRY_VERIFY(!view->findChild<QMessageBox *>());
  QVERIFY(published.lookupRef(qualifiedName).isValid());

  view->promptToDeleteBranch(remote);
  QTRY_VERIFY(view->findChild<QMessageBox *>());
  dialog = view->findChild<QMessageBox *>();
  QPushButton *remove = nullptr;
  for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
    if (button->text() == "Delete") {
      remove = button;
      break;
    }
  }
  QVERIFY(remove);
  remove->click();

  QTRY_VERIFY_WITH_TIMEOUT(!published.lookupRef(qualifiedName).isValid(), 10000);
  QTRY_VERIFY_WITH_TIMEOUT(
      !mRepo->lookupBranch("origin/delete-on-origin", GIT_BRANCH_REMOTE)
           .isValid(),
      10000);
  QVERIFY(mRepo->lookupBranch("delete-on-origin", GIT_BRANCH_LOCAL).isValid());
}

void TestMainWindow::forcePushResetBranch() {
  git::Reference branch = mRepo->head();
  git::Branch upstream = branch;
  QVERIFY(upstream.upstream().isValid());

  git::Repository published = git::Repository::open(mRemoteDir.path());
  QVERIFY(published.isValid());
  const QString remoteRef = "refs/heads/" + branch.name();
  const git::Id publishedId = published.lookupRef(remoteRef).target().id();
  QCOMPARE(publishedId, branch.target().id());

  QProcess git;
  git.setWorkingDirectory(mRepo->workdir().path());
  git.start(GIT_EXECUTABLE, {"reset", "--hard", "HEAD~"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  const git::Id resetId = mRepo->head().target().id();
  QVERIFY(resetId != publishedId);

  QMenu *remoteMenu = nullptr;
  foreach (QAction *action, MenuBar::instance(mWindow)->actions()) {
    if (action->text() == "Remote") {
      remoteMenu = action->menu();
      break;
    }
  }
  QVERIFY(remoteMenu);

  QAction *forcePush = nullptr;
  foreach (QAction *action, remoteMenu->actions()) {
    if (action->text() == "Force Push...") {
      forcePush = action;
      break;
    }
  }
  QVERIFY(forcePush);
  QVERIFY(forcePush->isEnabled());

  bool toolbarActionFound = false;
  foreach (QToolButton *button, mWindow->findChildren<QToolButton *>()) {
    if (!button->menu())
      continue;
    foreach (QAction *action, button->menu()->actions())
      toolbarActionFound |= action->text() == "Force Push...";
  }
  QVERIFY(toolbarActionFound);

  forcePush->trigger();
  QTRY_VERIFY(mWindow->findChild<QMessageBox *>());
  QMessageBox *dialog = mWindow->findChild<QMessageBox *>();
  QCOMPARE(dialog->windowTitle(), QString("Force Push to origin?"));

  QPushButton *accept = nullptr;
  foreach (QPushButton *button, dialog->findChildren<QPushButton *>()) {
    if (button->text() == "Force Push") {
      accept = button;
      break;
    }
  }
  QVERIFY(accept);
  accept->click();

  QTRY_COMPARE_WITH_TIMEOUT(published.lookupRef(remoteRef).target().id(),
                            resetId, 10000);
}

void TestMainWindow::closeTab() {
  RepoView *closingView = mWindow->addTab(mSecondRepo);
  QVERIFY(closingView);
  QCOMPARE(mWindow->count(), 2);

  TabWidget *tabs = mWindow->tabWidget();
  tabs->setCurrentIndex(0);
  QCOMPARE(tabs->currentIndex(), 0);
  tabs->setCurrentIndex(1);
  QCOMPARE(tabs->currentIndex(), 1);

  QSignalSpy aboutToRemove(tabs, &TabWidget::tabAboutToBeRemoved);
  QSignalSpy removed(tabs, QOverload<>::of(&TabWidget::tabRemoved));
  QPointer<RepoView> guard(closingView);

  tabs->closeTab(closingView);

  QTRY_COMPARE(aboutToRemove.count(), 1);
  QTRY_COMPARE(removed.count(), 1);
  QTRY_VERIFY(guard.isNull());
  QCOMPARE(mWindow->count(), 1);
  QCOMPARE(mWindow->currentView(), mWindow->view(0));

  RecentRepositories *recent = RecentRepositories::instance();
  bool found = false;
  for (int i = 0; i < recent->count(); ++i)
    found |= recent->repository(i)->gitpath() == mSecondRepo->dir(false).path();
  QVERIFY(found);
}

void TestMainWindow::recentRepositoryLimit() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  RecentRepositories *repos = RecentRepositories::instance();
  repos->clear();

  QStringList paths;
  for (int i = 0; i < 21; ++i) {
    const QString path = dir.filePath(QString::number(i));
    QVERIFY(QDir().mkpath(path));
    paths.append(path);
    repos->add(path);
  }

  QCOMPARE(repos->count(), 20);
  for (int i = 0; i < 20; ++i)
    QCOMPARE(repos->repository(i)->gitpath(), paths.at(20 - i));

  QStringList expected = paths.mid(1);
  std::reverse(expected.begin(), expected.end());
  QCOMPARE(QSettings().value("recent").toStringList(), expected);
}

void TestMainWindow::invalidRecentRepository() {
  QString path = mInvalidRepoDir.path();
  QVERIFY(QDir(path).mkdir(".git"));
  RecentRepositories::instance()->add(path);

  auto contains = [path] {
    RecentRepositories *repos = RecentRepositories::instance();
    for (int i = 0; i < repos->count(); ++i) {
      if (repos->repository(i)->gitpath() == path)
        return true;
    }
    return false;
  };
  QVERIFY(contains());

  auto clickButton = [](const QString &text) {
    QTimer::singleShot(0, [text] {
      QMessageBox *dialog =
          qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      QVERIFY(dialog);
      foreach (QPushButton *button, dialog->findChildren<QPushButton *>()) {
        if (button->text() == text) {
          button->click();
          return;
        }
      }
      QFAIL("Expected recent repository action was not found");
    });
  };

  clickButton("Keep");
  QVERIFY(!MainWindow::open(path, true,
                            MainWindow::OpenSource::RecentRepository));
  QVERIFY(contains());

  clickButton("OK");
  QVERIFY(!MainWindow::open(path));
  QVERIFY(contains());

  clickButton("Remove From Recent");
  QVERIFY(!MainWindow::open(path, true,
                            MainWindow::OpenSource::RecentRepository));
  QVERIFY(!contains());
}

void TestMainWindow::restoreActiveRepositoryOnly() {
  mWindow->close();
  delete mWindow;
  mWindow = nullptr;
  QCOMPARE(MainWindow::windows().size(), 0);

  MainWindow::setSaveWindowSettings(false);
  QSettings settings;
  settings.remove("windows");
  settings.beginGroup("windows/inactive");
  settings.setValue("path", QStringList{mRepo->workdir().path()});
  settings.setValue("index", 0);
  settings.setValue("active", false);
  settings.endGroup();
  settings.beginGroup("windows/active");
  settings.setValue("path", QStringList{mRepo->workdir().path(),
                                        mSecondRepo->workdir().path()});
  settings.setValue("tabContext", QStringList{"first", "selected"});
  settings.setValue("index", 1);
  settings.setValue("active", true);
  settings.endGroup();

  QVERIFY(MainWindow::restoreWindows());
  const QList<MainWindow *> windows = MainWindow::windows();
  QCOMPARE(windows.size(), 1);
  mWindow = windows.first();
  QCOMPARE(mWindow->count(), 1);
  QCOMPARE(mWindow->currentView()->repo().workdir().path(),
           mSecondRepo->workdir().path());
  QCOMPARE(mWindow->currentView()->tabContext(), QString("selected"));
  QVERIFY(!settings.contains("windows/active/path"));
  QVERIFY(!settings.contains("windows/inactive/path"));
}

void TestMainWindow::cleanupTestCase() {
  if (mWindow)
    mWindow->close();
  QSettings().remove("windows");
  QSettings().setValue("recent", mRecentRepositories);
}

TEST_MAIN(TestMainWindow)

#include "main_window.moc"

//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Test.h"
#include "dialogs/AmendDialog.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/StartDialog.h"
#include "qnamespace.h"
#include "ui/CommitList.h"
#include "ui/DetailView.h"
#include "ui/DiffView/DiffView.h"
#include "ui/DoubleTreeWidget.h"
#include "ui/Footer.h"
#include "ui/MainWindow.h"
#include "ui/MenuBar.h"
#include "ui/RepoView.h"
#include "ui/TreeView.h"
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QToolButton>
#include <qtestcase.h>

using namespace Test;
using namespace QTest;

class TestInitRepo : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void addFile();
  void commitFile();
  void amendCommit();
  void editFile();
  void cleanupTestCase();

private:
  MainWindow *mWindow = nullptr;
};

void TestInitRepo::initTestCase() {
  QDir dir = QDir::temp();
  if (dir.cd("test_init_repo"))
    QVERIFY(dir.removeRecursively());

  StartDialog *dialog = StartDialog::openSharedInstance();
  QVERIFY(qWaitForWindowActive(dialog));

  // Find the first button in the first footer.
  Footer *footer = dialog->findChild<Footer *>();
  QToolButton *plus = footer->findChild<QToolButton *>();

  // Set up timer to dismiss the popup.
  QTimer::singleShot(500, [] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(menu);

    keyClick(menu, Qt::Key_Down);
    keyClick(menu, Qt::Key_Down);
    keyClick(menu, Qt::Key_Down);
    keyClick(menu, Qt::Key_Return);
  });

  {
    auto timeout = Timeout(1000, "Start dialog didn't close in time");

    // Show popup menu.
    mouseClick(plus, Qt::LeftButton);
  }

  CloneDialog *cloneDialog =
      qobject_cast<CloneDialog *>(QApplication::activeModalWidget());
  QVERIFY(cloneDialog);

  // Set fields.
  cloneDialog->setField("name", "test_init_repo");
  cloneDialog->setField("path", QDir::tempPath());

  // Click return.
  keyClick(cloneDialog, Qt::Key_Return);

  // Wait on the new window.
  mWindow = MainWindow::activeWindow();
  QVERIFY(mWindow && qWaitForWindowExposed(mWindow));

  RepoView *view = mWindow->currentView();
  QVERIFY(view);
  git::Repository repo = view->repo();
  QVERIFY(repo.isValid());
  initRepo(repo);
}

void TestInitRepo::addFile() {
  // Create a file.
  QDir dir = QDir::temp();
  QVERIFY(dir.cd("test_init_repo"));

  QFile file(dir.filePath("test"));
  QVERIFY(file.open(QFile::WriteOnly));
  QTextStream(&file) << "This is a test.";
  file.close();
  QFile secondFile(dir.filePath("test2"));
  QVERIFY(secondFile.open(QFile::WriteOnly));
  QTextStream(&secondFile) << "This is another test.";
  secondFile.close();

  // Check for a single file called "test".
  RepoView *view = mWindow->currentView();
  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  QAbstractItemModel *model = files->model();

  {
    // Wait for refresh
    auto timeout = Timeout(10000, "Repository didn't refresh in time");
    while (model->rowCount() < 2)
      qWait(300);
  }

  QCOMPARE(model->rowCount(), 2);
  QCOMPARE(model->data(model->index(0, 0)).toString(), QString("test"));

  QStackedWidget *primaryView =
      view->findChild<QStackedWidget *>("RepositoryPrimaryView");
  QWidget *fileInspection =
      view->findChild<QWidget *>("FileInspectionView");
  QToolButton *close =
      view->findChild<QToolButton *>("CloseFileInspection");
  QVERIFY(primaryView);
  QVERIFY(fileInspection);
  QVERIFY(close);
  QVERIFY(primaryView->currentWidget() != fileInspection);

  MenuBar *menuBar = MenuBar::instance(mWindow);
  QVERIFY(menuBar);
  files->setFocus();
  QTRY_VERIFY(files->hasFocus());
  menuBar->setMaximized(true);
  QVERIFY(view->detailsMaximized());
  QVERIFY(menuBar->isMaximized());
  files->selectionModel()->clearSelection();
  files->selectionModel()->select(model->index(0, 0),
                                  QItemSelectionModel::Select);
  QVERIFY(primaryView->currentWidget() != fileInspection);
  mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier,
             files->visualRect(model->index(0, 0)).center());
  QCOMPARE(primaryView->currentWidget(), fileInspection);
  QVERIFY(!view->detailsMaximized());
  QVERIFY(!menuBar->isMaximized());

  files->selectionModel()->select(model->index(1, 0),
                                  QItemSelectionModel::Select);
  QCOMPARE(files->selectionModel()->selectedRows().size(), 2);
  QCOMPARE(primaryView->currentWidget(), fileInspection);

  close->setFocus();
  QTRY_VERIFY(close->hasFocus());
  menuBar->setMaximized(true);
  QVERIFY(view->detailsMaximized());
  QVERIFY(menuBar->isMaximized());
  view->setViewMode(RepoView::Tree);
  QVERIFY(!view->detailsMaximized());
  QVERIFY(!menuBar->isMaximized());
  view->setViewMode(RepoView::DoubleTree);
  QVERIFY(primaryView->currentWidget() != fileInspection);
  QVERIFY(QMetaObject::invokeMethod(files, "fileSelectionRequested"));
  QCOMPARE(primaryView->currentWidget(), fileInspection);

  close->click();
  QVERIFY(primaryView->currentWidget() != fileInspection);
  QVERIFY(files->selectionModel()->selectedIndexes().isEmpty());
  view->refresh();
  QCoreApplication::processEvents();
  QVERIFY(primaryView->currentWidget() != fileInspection);
  QVERIFY(files->selectionModel()->selectedIndexes().isEmpty());

  files->selectionModel()->select(model->index(0, 0),
                                  QItemSelectionModel::Select);
  QVERIFY(primaryView->currentWidget() != fileInspection);
  keyClick(files, Qt::Key_Return);
  QCOMPARE(primaryView->currentWidget(), fileInspection);
  close->click();

  QModelIndex firstUnstaged = model->index(0, 0);
  mouseClick(files->viewport(), Qt::LeftButton, Qt::NoModifier,
             files->checkRect(firstUnstaged).center());
  QVERIFY(primaryView->currentWidget() != fileInspection);
  TreeView *stagedFiles = doubleTree->findChild<TreeView *>("Staged");
  QVERIFY(stagedFiles);
  QTRY_VERIFY(stagedFiles->model()->rowCount() > 0);
  QModelIndex stagedFile = stagedFiles->model()->index(0, 0);
  stagedFiles->selectionModel()->select(stagedFile,
                                        QItemSelectionModel::Select);
  QVERIFY(
      QMetaObject::invokeMethod(stagedFiles, "fileSelectionRequested"));
  QCOMPARE(primaryView->currentWidget(), fileInspection);
  close->click();
}

void TestInitRepo::commitFile() {
  RepoView *view = mWindow->currentView();
  DetailView *detailView = view->findChild<DetailView *>();
  QVERIFY(detailView);

  QPushButton *stageAll = detailView->findChild<QPushButton *>("StageAll");
  QVERIFY(stageAll);

  mouseClick(stageAll, Qt::LeftButton);
  view->commit();
}

void TestInitRepo::amendCommit() {
  RepoView *view = mWindow->currentView();
  QVERIFY(view);

  bool finished = false;
  connect(view, &RepoView::statusChanged, [&finished]() { finished = true; });

  CommitList *commitList = view->findChild<CommitList *>();
  QVERIFY(commitList);
  QTRY_VERIFY(commitList->model()
                  ->index(0, 0)
                  .data(CommitList::Role::CommitRole)
                  .value<git::Commit>()
                  .isValid());
  QModelIndex index = commitList->model()->index(0, 0);
  QVERIFY(index.isValid());
  commitList->selectionModel()->select(index,
                                       QItemSelectionModel::ClearAndSelect);

  auto message = view->findChild<QTextEdit *>("MessageLabel");
  QVERIFY(message);
  QTRY_COMPARE(message->viewport()->cursor().shape(), Qt::PointingHandCursor);
  mouseClick(message->viewport(), Qt::LeftButton);

  auto dialog = view->findChild<AmendDialog *>();
  QVERIFY(dialog);
  dialog->findChild<QTextEdit *>()->setText("Some other commit message");
  dialog->accept();

  qWait(300);

  {
    auto timeout =
        Timeout(10000, "Repository didn't detect status change in time");
    while (!finished)
      qWait(300);
  }

  // Verify commit amended
  QAbstractItemModel *commitModel = commitList->model();
  index = commitModel->index(0, 0);
  QVERIFY(index.isValid());
  auto commit = commitModel->data(index, CommitList::Role::CommitRole)
                    .value<git::Commit>();
  QCOMPARE(commit.message(), QString("Some other commit message"));
}

void TestInitRepo::editFile() {
  RepoView *view = mWindow->currentView();

  {
    QFile file(view->repo().workdir().filePath("test"));
    QVERIFY(file.open(QFile::Append));
    QTextStream(&file) << "Changed for editing.";
  }
  view->refresh();

  auto doubleTree = view->findChild<DoubleTreeWidget *>();
  QVERIFY(doubleTree);

  auto files = doubleTree->findChild<TreeView *>("Unstaged");
  QVERIFY(files);

  QTRY_VERIFY(files->model()->rowCount() > 0);
  QModelIndex selectedFile = files->model()->index(0, 0);
  QVERIFY(selectedFile.isValid());
  files->selectionModel()->select(selectedFile, QItemSelectionModel::Select);
  QVERIFY(QMetaObject::invokeMethod(files, "fileSelectionRequested"));

  DiffView *diff = view->findChild<DiffView *>();
  QVERIFY(diff);

  QToolButton *edit = diff->findChild<QToolButton *>("EditButton");
  QVERIFY(edit);

  // Set up timer to dismiss the popup.
  QTimer::singleShot(500, [] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(menu);

    keyClick(menu, Qt::Key_Down);
    keyClick(menu, Qt::Key_Return);
  });

  {
    auto timeout = Timeout(1000, "Popup didn't close in time");
    // mouseClick(edit, Qt::LeftButton);
    edit->click();
  }
}

void TestInitRepo::cleanupTestCase() {
  if (mWindow) {
    mWindow->close();
  }
  QDir dir = QDir::temp();
  QVERIFY(dir.cd("test_init_repo"));
  QVERIFY(dir.removeRecursively());
}

TEST_MAIN(TestInitRepo)

#include "init_repo.moc"

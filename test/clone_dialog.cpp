#include "Test.h"

#include "conf/Setting.h"
#include "conf/Settings.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/StartDialog.h"
#include "git/Index.h"
#include "host/Account.h"
#include "host/Accounts.h"
#include "host/Repository.h"
#include "log/LogEntry.h"
#include "log/LogModel.h"
#include "log/LogView.h"
#include "ui/Footer.h"
#include "ui/MainWindow.h"
#include "ui/MenuBar.h"
#include "ui/RepoView.h"
#include "ui/SideBar.h"
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QItemSelectionModel>
#include <QListView>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

namespace {

void clearAccounts() {
  Accounts *accounts = Accounts::instance();
  while (accounts->count() > 0)
    accounts->removeAccount(0);

  QSettings settings;
  settings.remove("accounts");
  settings.remove("remote");
  settings.remove("start/remote/fullname");
  settings.sync();
}

CloneDialog *activeCloneDialog() {
  return qobject_cast<CloneDialog *>(QApplication::activeModalWidget());
}

MainWindow *windowForRepository(const QString &path) {
  const QString expected = QDir::cleanPath(path);
  for (MainWindow *window : MainWindow::windows()) {
    RepoView *view = window->currentView();
    if (view && QDir::cleanPath(view->repo().dir(false).path()) == expected)
      return window;
  }

  return nullptr;
}

bool hasCloneLog(RepoView *view, const QString &path) {
  LogView *logView = view->findChild<LogView *>();
  if (!logView)
    return false;

  auto *model = static_cast<LogModel *>(logView->model());
  for (int row = 0; row < model->rowCount(); ++row) {
    LogEntry *entry = model->entry(model->index(row, 0));
    if (entry && entry->title() == QStringLiteral("Clone") &&
        entry->text().contains(path))
      return true;
  }

  return false;
}

void closeTestWindows() {
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    if (qobject_cast<MainWindow *>(widget) ||
        qobject_cast<StartDialog *>(widget) ||
        qobject_cast<CloneDialog *>(widget))
      widget->close();
  }

  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

} // namespace

class TestCloneDialog : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void init();
  void cleanup();
  void cloneOtherRepositoryFromChooser();
  void cloneFailureLeavesChooserOpen();
  void cloneSelectedAccountRepository();
  void cloneFromEmptyTab();
  void cloneFromFileMenu();
  void cloneFromRepositorySidebar();

private:
  void completeClone(CloneDialog *dialog, const QString &name,
                     QPointer<StartDialog> chooser = {},
                     bool expectChooserToClose = false,
                     bool expectCloneLog = true);

  Test::ScratchRepository mSource;
  QTemporaryDir mDestination;
};

void TestCloneDialog::initTestCase() {
  QVERIFY(mDestination.isValid());

  QFile sourceFile(mSource->workdir().filePath("README.md"));
  QVERIFY(sourceFile.open(QIODevice::WriteOnly));
  QCOMPARE(sourceFile.write("clone source\n"), qint64(13));
  sourceFile.close();

  mSource->index().add("README.md", "clone source\n");
  QVERIFY(mSource->commit("Initial clone source").isValid());
}

void TestCloneDialog::init() {
  closeTestWindows();
  clearAccounts();
  Settings::instance()->setValue(Setting::Id::OpenAllReposInTabs, false);
}

void TestCloneDialog::cleanup() {
  closeTestWindows();
  clearAccounts();
}

void TestCloneDialog::completeClone(CloneDialog *dialog, const QString &name,
                                    QPointer<StartDialog> chooser,
                                    bool expectChooserToClose,
                                    bool expectCloneLog) {
  QVERIFY(dialog);
  if (expectChooserToClose)
    QVERIFY(chooser);

  const QString clonePath = QDir(mDestination.path()).filePath(name);
  QSignalSpy accepted(dialog, &QDialog::accepted);
  QVERIFY(accepted.isValid());
  QPointer<CloneDialog> dialogGuard(dialog);

  dialog->setField("url", mSource->workdir().path());
  dialog->setField("name", name);
  dialog->setField("path", mDestination.path());
  dialog->setField("bare", false);
  dialog->setField("updateSubmodules", false);
  dialog->page(2)->initializePage();

  QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 15000);
  QTRY_VERIFY_WITH_TIMEOUT(windowForRepository(clonePath), 15000);
  MainWindow *window = windowForRepository(clonePath);
  QVERIFY(window);
  RepoView *view = window->currentView();
  QVERIFY(view);
  if (expectCloneLog)
    QTRY_VERIFY_WITH_TIMEOUT(hasCloneLog(view, clonePath), 5000);

  if (expectChooserToClose)
    QTRY_VERIFY(chooser.isNull());
  QTRY_VERIFY(dialogGuard.isNull());
}

void TestCloneDialog::cloneOtherRepositoryFromChooser() {
  auto *chooser = new StartDialog;
  QPointer<StartDialog> chooserGuard(chooser);
  chooser->show();
  QVERIFY(QTest::qWaitForWindowExposed(chooser));

  QPushButton *cloneOther =
      chooser->findChild<QPushButton *>("CloneOtherRepository");
  QVERIFY(cloneOther);
  cloneOther->click();
  QTRY_VERIFY(activeCloneDialog());

  completeClone(activeCloneDialog(), "chooser-other", chooserGuard, true);
}

void TestCloneDialog::cloneFailureLeavesChooserOpen() {
  auto *chooser = new StartDialog;
  QPointer<StartDialog> chooserGuard(chooser);
  chooser->show();
  QVERIFY(QTest::qWaitForWindowExposed(chooser));

  QPushButton *cloneOther =
      chooser->findChild<QPushButton *>("CloneOtherRepository");
  QVERIFY(cloneOther);
  cloneOther->click();
  QTRY_VERIFY(activeCloneDialog());

  CloneDialog *dialog = activeCloneDialog();
  QPointer<CloneDialog> dialogGuard(dialog);
  QSignalSpy accepted(dialog, &QDialog::accepted);
  QVERIFY(accepted.isValid());

  const QString name = "failed-clone";
  const QString clonePath = QDir(mDestination.path()).filePath(name);
  dialog->setField("url", QDir(mDestination.path()).filePath("missing-source"));
  dialog->setField("name", name);
  dialog->setField("path", mDestination.path());
  dialog->setField("bare", false);
  dialog->page(2)->initializePage();

  LogView *logView = dialog->findChild<LogView *>();
  QVERIFY(logView);
  auto *model = static_cast<LogModel *>(logView->model());
  QTRY_VERIFY_WITH_TIMEOUT(model->rowCount(model->index(0, 0)) > 0, 15000);
  QCOMPARE(accepted.count(), 0);
  QVERIFY(chooserGuard);
  QVERIFY(!windowForRepository(clonePath));

  dialog->reject();
  QTRY_VERIFY(dialogGuard.isNull());
  QVERIFY(chooserGuard);
  QVERIFY(chooserGuard->isVisible());
}

void TestCloneDialog::cloneSelectedAccountRepository() {
  Account *account =
      Accounts::instance()->createAccount(Account::GitHub, "clone-test");
  Repository *repository =
      account->addRepository("project", "clone-test/project");
  repository->setUrl(Repository::Ssh, mSource->workdir().path());

  auto *chooser = new StartDialog;
  QPointer<StartDialog> chooserGuard(chooser);
  auto *repositoryList = chooser->findChild<QListView *>("RemoteRepositories");
  auto *buttonBox = chooser->findChild<QDialogButtonBox *>();
  QVERIFY(repositoryList);
  QVERIFY(buttonBox);

  const QModelIndex repositoryIndex = repositoryList->model()->index(0, 0);
  repositoryList->selectionModel()->setCurrentIndex(
      repositoryIndex, QItemSelectionModel::ClearAndSelect);
  QPushButton *clone = buttonBox->button(QDialogButtonBox::Open);
  QVERIFY(clone);
  QCOMPARE(clone->text(), QString("Clone"));

  chooser->show();
  QVERIFY(QTest::qWaitForWindowExposed(chooser));
  clone->click();
  QTRY_VERIFY(activeCloneDialog());

  completeClone(activeCloneDialog(), "account-project", chooserGuard, true,
                false);
  QCOMPARE(
      QDir::cleanPath(account->repositoryPath(0)),
      QDir::cleanPath(QDir(mDestination.path()).filePath("account-project")));
}

void TestCloneDialog::cloneFromEmptyTab() {
  auto *host = new MainWindow(git::Repository());
  host->show();
  QVERIFY(QTest::qWaitForWindowExposed(host));

  QPushButton *clone = nullptr;
  for (QPushButton *button : host->findChildren<QPushButton *>()) {
    if (button->text() == QStringLiteral("Clone repository")) {
      clone = button;
      break;
    }
  }
  QVERIFY(clone);
  clone->click();
  QTRY_VERIFY(activeCloneDialog());

  completeClone(activeCloneDialog(), "empty-tab");
}

void TestCloneDialog::cloneFromFileMenu() {
  auto *host = new MainWindow(git::Repository());
  host->show();
  QVERIFY(QTest::qWaitForWindowExposed(host));

  QAction *clone = nullptr;
  for (QAction *menuAction : host->menuBar()->actions()) {
    if (menuAction->text() != QStringLiteral("File") || !menuAction->menu())
      continue;
    for (QAction *action : menuAction->menu()->actions()) {
      if (action->text() == QStringLiteral("Clone Repository...")) {
        clone = action;
        break;
      }
    }
  }
  QVERIFY(clone);
  clone->trigger();
  QTRY_VERIFY(activeCloneDialog());

  completeClone(activeCloneDialog(), "file-menu");
}

void TestCloneDialog::cloneFromRepositorySidebar() {
  auto *host = new MainWindow(git::Repository());
  host->show();
  QVERIFY(QTest::qWaitForWindowExposed(host));

  SideBar *sideBar = host->findChild<SideBar *>();
  QVERIFY(sideBar);
  Footer *footer = sideBar->findChild<Footer *>("RepositoryFooter");
  QVERIFY(footer);
  QToolButton *add = footer->findChild<QToolButton *>("Add");
  QVERIFY(add);
  QVERIFY(add->menu());

  QAction *clone = nullptr;
  for (QAction *action : add->menu()->actions()) {
    if (action->text() == QStringLiteral("Clone Repository")) {
      clone = action;
      break;
    }
  }
  QVERIFY(clone);
  clone->trigger();
  QTRY_VERIFY(activeCloneDialog());

  completeClone(activeCloneDialog(), "sidebar");
}

int main(int argc, char *argv[]) {
  QTemporaryDir settingsDirectory;
  if (!settingsDirectory.isValid())
    return 1;

  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     settingsDirectory.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settingsDirectory.path());

  QTEST_SET_MAIN_SOURCE_PATH
  return Test::runTest<TestCloneDialog>(argc, argv);
}

#include "clone_dialog.moc"

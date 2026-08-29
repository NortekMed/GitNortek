//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//


#include "Test.h"
#include "dialogs/FastIssueDialog.h"
#include "host/GitHub.h"
#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>

namespace {

class FakeGitHub : public GitHub {
public:
  FakeGitHub() : GitHub("member") {}

  void createIssue(const QString &owner, const QString &repository,
                   const QString &title, const QString &body,
                   const CreateIssueCallback &callback) override {
    ++calls;
    this->owner = owner;
    this->repository = repository;
    this->title = title;
    this->body = body;
    if (!defer)
      callback(success, success ? 42 : 0,
               success ? QUrl("https://github.com/NortekMed/GitNortek/issues/42")
                       : QUrl(),
               success ? QString() : QString("Permission denied"));
  }

  bool success = true;
  bool defer = false;
  int calls = 0;
  QString owner;
  QString repository;
  QString title;
  QString body;
};

void dismissMessageBox() {
  QTimer::singleShot(0, [] {
    QMessageBox *message =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (message)
      message->accept();
  });
}

} // namespace

class TestFastIssueDialog : public QObject {
  Q_OBJECT

private slots:
  void createWithDiagnostics();
  void recoverFromFailure();
  void preventDuplicateSubmission();
};

void TestFastIssueDialog::createWithDiagnostics() {
  FakeGitHub github;
  QPointer<FastIssueDialog> dialog = new FastIssueDialog(&github);
  QSignalSpy accepted(dialog, &QDialog::accepted);
  QLineEdit *title = dialog->findChild<QLineEdit *>("FastIssueTitle");
  QTextEdit *details = dialog->findChild<QTextEdit *>("FastIssueDetails");
  QCheckBox *diagnostics =
      dialog->findChild<QCheckBox *>("FastIssueDiagnostics");
  QPushButton *create = dialog->findChild<QPushButton *>("FastIssueCreate");
  QVERIFY(title);
  QVERIFY(details);
  QVERIFY(diagnostics);
  QVERIFY(create);
  QVERIFY(!create->isEnabled());
  QVERIFY(!diagnostics->isChecked());

  title->setText("  Improve startup  ");
  details->setPlainText("Open repositories faster.");
  diagnostics->setChecked(true);
  QVERIFY(create->isEnabled());

  dismissMessageBox();
  create->click();

  QCOMPARE(github.calls, 1);
  QCOMPARE(github.owner, "NortekMed");
  QCOMPARE(github.repository, "GitNortek");
  QCOMPARE(github.title, "Improve startup");
  QVERIFY(github.body.startsWith("Open repositories faster.\n\n---\n"));
  QVERIFY(github.body.contains("GitNortek diagnostics"));
  QVERIFY(github.body.contains("- Qt:"));
  QVERIFY(github.body.contains("- Operating system:"));
  QVERIFY(github.body.contains("- Architecture:"));
  QCOMPARE(accepted.count(), 1);
  QTRY_VERIFY(!dialog);
}

void TestFastIssueDialog::recoverFromFailure() {
  FakeGitHub github;
  github.success = false;
  FastIssueDialog dialog(&github);
  QLineEdit *title = dialog.findChild<QLineEdit *>("FastIssueTitle");
  QTextEdit *details = dialog.findChild<QTextEdit *>("FastIssueDetails");
  QCheckBox *diagnostics =
      dialog.findChild<QCheckBox *>("FastIssueDiagnostics");
  QPushButton *create = dialog.findChild<QPushButton *>("FastIssueCreate");
  title->setText("Failure");

  dismissMessageBox();
  create->click();

  QCOMPARE(github.calls, 1);
  QVERIFY(title->isEnabled());
  QVERIFY(details->isEnabled());
  QVERIFY(diagnostics->isEnabled());
  QVERIFY(create->isEnabled());
  QCOMPARE(dialog.result(), 0);
}

void TestFastIssueDialog::preventDuplicateSubmission() {
  FakeGitHub github;
  github.defer = true;
  FastIssueDialog dialog(&github);
  QLineEdit *title = dialog.findChild<QLineEdit *>("FastIssueTitle");
  QPushButton *create = dialog.findChild<QPushButton *>("FastIssueCreate");
  title->setText("One request");

  create->click();
  create->click();

  QCOMPARE(github.calls, 1);
  QVERIFY(!create->isEnabled());
  QVERIFY(!title->isEnabled());
}

TEST_MAIN(TestFastIssueDialog)

#include "FastIssueDialog.moc"

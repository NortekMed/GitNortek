//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//


#include "FastIssueDialog.h"
#include "host/GitHub.h"
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSysInfo>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {

const QString kIssueOwner = QStringLiteral("NortekMed");
const QString kIssueRepository = QStringLiteral("GitNortek");

} // namespace

FastIssueDialog::FastIssueDialog(GitHub *github, QWidget *parent)
    : QDialog(parent), mGitHub(github) {
  Q_ASSERT(github);

  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Fast Issue"));
  setMinimumWidth(440);
  connect(github, &QObject::destroyed, this, &QDialog::reject);

  QLabel *destination =
      new QLabel(tr("Create an issue in NortekMed/GitNortek"), this);

  mTitle = new QLineEdit(this);
  mTitle->setObjectName("FastIssueTitle");
  mTitle->setPlaceholderText(tr("Issue title"));

  mDetails = new QTextEdit(this);
  mDetails->setObjectName("FastIssueDetails");
  mDetails->setPlaceholderText(tr("What happened or what should improve?"));
  mDetails->setMinimumHeight(140);

  mDiagnostics = new QCheckBox(tr("Include application diagnostics"), this);
  mDiagnostics->setObjectName("FastIssueDiagnostics");
  mDiagnostics->setToolTip(
      tr("Include application, Qt, operating system, and architecture versions. "
         "Repository paths and account information are never included."));

  QDialogButtonBox *buttons = new QDialogButtonBox(this);
  buttons->addButton(QDialogButtonBox::Cancel);
  mCreateButton =
      buttons->addButton(tr("Create Issue"), QDialogButtonBox::AcceptRole);
  mCreateButton->setObjectName("FastIssueCreate");
  mCreateButton->setDefault(true);
  mCreateButton->setEnabled(false);

  connect(mTitle, &QLineEdit::textChanged, this, [this](const QString &title) {
    mCreateButton->setEnabled(!title.trimmed().isEmpty());
  });
  connect(mCreateButton, &QPushButton::clicked, this,
          &FastIssueDialog::createIssue);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setSpacing(10);
  layout->addWidget(destination);
  layout->addWidget(mTitle);
  layout->addWidget(mDetails);
  layout->addWidget(mDiagnostics);
  layout->addWidget(buttons);
}

QString FastIssueDialog::body() const {
  QString result = mDetails->toPlainText().trimmed();
  if (!mDiagnostics->isChecked())
    return result;

  if (!result.isEmpty())
    result.append("\n\n");
  result.append(QString("---\n"
                        "GitNortek diagnostics\n\n"
                        "- Version: %1\n"
                        "- Revision: %2\n"
                        "- Qt: %3\n"
                        "- Operating system: %4\n"
                        "- Architecture: %5")
                    .arg(QCoreApplication::applicationVersion(),
                         QStringLiteral(GITNORTEK_BUILD_REVISION),
                         QString::fromLatin1(qVersion()),
                         QSysInfo::prettyProductName(),
                         QSysInfo::currentCpuArchitecture()));
  return result;
}

void FastIssueDialog::createIssue() {
  QString title = mTitle->text().trimmed();
  if (title.isEmpty() || !mGitHub)
    return;

  mCreateButton->setEnabled(false);
  mTitle->setEnabled(false);
  mDetails->setEnabled(false);
  mDiagnostics->setEnabled(false);

  QString failureTitle = tr("Fast Issue Failed");
  QString successMessage = tr("GitHub issue #%1 was created.");
  QPointer<FastIssueDialog> guard(this);
  mGitHub->createIssue(
      kIssueOwner, kIssueRepository, title, body(),
      [guard, failureTitle,
       successMessage](bool success, int number, const QUrl &,
                       const QString &error) {
        if (!guard)
          return;

        if (!success) {
          guard->mCreateButton->setEnabled(true);
          guard->mTitle->setEnabled(true);
          guard->mDetails->setEnabled(true);
          guard->mDiagnostics->setEnabled(true);
          QMessageBox::warning(guard, failureTitle, error);
          return;
        }

        QWidget *parent = guard->parentWidget();
        guard->accept();
        QMessageBox::information(
            parent, QCoreApplication::applicationName(),
            successMessage.arg(number));
      });
}

//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "PublishRepositoryDialog.h"
#include "AccountDialog.h"
#include "git/Remote.h"
#include "host/Accounts.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <git2/remote.h>

PublishRepositoryDialog::PublishRepositoryDialog(
    const git::Repository &repo, QWidget *parent, Mode mode,
    Account::Kind preferredKind, Repository::Protocol preferredProtocol)
    : QDialog(parent), mRepo(repo), mMode(mode), mPreferredKind(preferredKind) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(mMode == CreateOnly ? tr("Create Repository")
                                     : tr("Publish Repository"));

  mAccounts = new QComboBox(this);
  mAccounts->setObjectName("PublishAccount");
  mNamespaces = new QComboBox(this);
  mNamespaces->setObjectName("PublishNamespace");
  mNamespaces->setEnabled(false);

  mRepositoryName = new QLineEdit(this);
  mRepositoryName->setObjectName("PublishRepositoryName");
  mRepositoryName->setText(QFileInfo(repo.workdir().path()).fileName());
  mRemoteName = new QLineEdit(QStringLiteral("origin"), this);
  mRemoteName->setObjectName("PublishRemoteName");

  mDescription = new QPlainTextEdit(this);
  mDescription->setObjectName("PublishDescription");
  mDescription->setMaximumHeight(mDescription->fontMetrics().height() * 4);

  mVisibility = new QComboBox(this);
  mVisibility->setObjectName("PublishVisibility");
  mProtocol = new QComboBox(this);
  mProtocol->setObjectName("PublishProtocol");
  mProtocol->addItem(tr("HTTPS"), Repository::Https);
  mProtocol->addItem(tr("SSH"), Repository::Ssh);
  mProtocol->setCurrentIndex(mProtocol->findData(preferredProtocol));

  mNamespaceStatus = new QLabel(this);
  mNamespaceStatus->setWordWrap(true);
  mVisibilityStatus = new QLabel(this);
  mVisibilityStatus->setWordWrap(true);
  mError = new QLabel(this);
  mError->setWordWrap(true);
  mError->setStyleSheet(QStringLiteral("color: palette(dark);"));
  mError->hide();

  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(tr("Account:"), mAccounts);
  form->addRow(tr("Destination:"), mNamespaces);
  form->addRow(QString(), mNamespaceStatus);
  form->addRow(tr("Repository name:"), mRepositoryName);
  if (mMode == PublishAndPush)
    form->addRow(tr("Remote name:"), mRemoteName);
  else
    mRemoteName->hide();
  form->addRow(tr("Description:"), mDescription);
  form->addRow(tr("Access:"), mVisibility);
  form->addRow(QString(), mVisibilityStatus);
  form->addRow(mMode == CreateOnly ? tr("Clone protocol:")
                                   : tr("Push protocol:"),
               mProtocol);

  mAddAccount = new QPushButton(tr("Add Account…"), this);
  mAddAccount->setObjectName("AddPublishAccount");
  mAddRemote = new QPushButton(tr("Add Existing Remote by URL…"), this);
  mAddRemote->setObjectName("AddExistingRemote");
  mAddRemote->setVisible(mMode == PublishAndPush);
  auto *alternatives = new QHBoxLayout;
  alternatives->addWidget(mAddAccount);
  alternatives->addWidget(mAddRemote);
  alternatives->addStretch();

  mButtons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  connect(mButtons, &QDialogButtonBox::rejected, this,
          &PublishRepositoryDialog::reject);
  const QString submitText = mMode == CreateOnly
                                 ? tr("Create Repository")
                                 : tr("Create Repository and Push");
  mPublish = mButtons->addButton(submitText, QDialogButtonBox::AcceptRole);
  mPublish->setObjectName(mMode == CreateOnly ? "CreateRepository"
                                              : "CreateRepositoryAndPush");

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(mError);
  layout->addLayout(alternatives);
  layout->addWidget(mButtons);

  connect(mAccounts, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { refreshNamespaces(); });
  connect(mRepositoryName, &QLineEdit::textChanged, this,
          [this] { updateButtons(); });
  connect(mRemoteName, &QLineEdit::textChanged, this,
          [this] { updateButtons(); });
  connect(mNamespaces, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { updateButtons(); });
  connect(mVisibility, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { updateButtons(); });
  connect(mProtocol, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this] { updateButtons(); });
  connect(mPublish, &QPushButton::clicked, this, [this] {
    QString error;
    if (!validate(&error)) {
      showError(error);
      return;
    }
    mError->hide();
    setBusy(true);
    if (mMode == CreateOnly)
      emit createRepositoryRequested();
    else
      emit publishRequested();
  });
  connect(mAddRemote, &QPushButton::clicked, this,
          &PublishRepositoryDialog::addRemoteRequested);
  connect(mAddAccount, &QPushButton::clicked, this, [this] {
    auto *dialog = new AccountDialog(nullptr, this);
    if (mMode == CreateOnly)
      dialog->setKind(mPreferredKind);
    connect(dialog, &QDialog::accepted, this, [this] {
      populateAccounts();
      refreshNamespaces();
    });
    dialog->open();
  });

  populateAccounts();
  refreshNamespaces();
  updateButtons();
}

Account *PublishRepositoryDialog::account() const {
  return qobject_cast<Account *>(mAccounts->currentData().value<QObject *>());
}

QString PublishRepositoryDialog::namespaceId() const {
  return mNamespaces->currentData().toString();
}

QString PublishRepositoryDialog::repositoryName() const {
  return mRepositoryName->text().trimmed();
}

QString PublishRepositoryDialog::remoteName() const {
  return mRemoteName->text().trimmed();
}

QString PublishRepositoryDialog::description() const {
  return mDescription->toPlainText().trimmed();
}

bool PublishRepositoryDialog::isPublic() const {
  return mVisibility->currentData().toBool();
}

Repository::Protocol PublishRepositoryDialog::protocol() const {
  return static_cast<Repository::Protocol>(mProtocol->currentData().toInt());
}

void PublishRepositoryDialog::populateAccounts() {
  Account *selected = account();
  const QSignalBlocker blocker(mAccounts);
  mAccounts->clear();

  Accounts *accounts = Accounts::instance();
  for (int i = 0; i < accounts->count(); ++i) {
    Account *current = accounts->account(i);
    if (mMode == CreateOnly && current->kind() != mPreferredKind)
      continue;
    const QString label = QString("%1 (%2)").arg(Account::name(current->kind()),
                                                 current->username());
    mAccounts->addItem(label,
                       QVariant::fromValue(static_cast<QObject *>(current)));
    if (current == selected)
      mAccounts->setCurrentIndex(mAccounts->count() - 1);
  }

  if (mAccounts->count() == 0)
    mAccounts->addItem(tr("No connected accounts"), QVariant());
  updateButtons();
}

void PublishRepositoryDialog::refreshNamespaces() {
  const quint64 request = ++mNamespaceRequest;
  Account *selected = account();
  mNamespaces->clear();
  mNamespaces->setEnabled(false);
  mNamespaceStatus->clear();
  mVisibility->clear();
  mVisibility->addItem(tr("Private"), false);
  mVisibilityStatus->clear();

  if (!selected) {
    mNamespaceStatus->setText(
        mMode == CreateOnly
            ? tr("Connect a %1 account to create this repository.")
                  .arg(Account::name(mPreferredKind))
            : tr("Connect a hosting account or add an existing remote by "
                 "URL."));
    updateButtons();
    return;
  }

  if (selected->supportsPublicRepositories()) {
    mVisibility->addItem(tr("Public"), true);
  } else {
    mVisibilityStatus->setText(
        tr("This hosting service only supports private repositories."));
  }

  mLoadingNamespaces = true;
  mNamespaceStatus->setText(tr("Loading available destinations…"));
  mPublish->setEnabled(false);
  QPointer<PublishRepositoryDialog> dialog(this);
  QPointer<Account> accountGuard(selected);
  selected->requestRepositoryNamespaces(
      [dialog, accountGuard,
       request](bool success, const Account::RepositoryNamespaces &namespaces,
                const QString &error) {
        if (!dialog || !accountGuard || request != dialog->mNamespaceRequest ||
            dialog->account() != accountGuard)
          return;

        dialog->mNamespaces->clear();
        int personalIndex = -1;
        for (const Account::RepositoryNamespace &space : namespaces) {
          QString label =
              space.fullName.isEmpty() ? space.name : space.fullName;
          if (space.personal)
            label = tr("Personal (%1)").arg(label);
          dialog->mNamespaces->addItem(label, space.id);
          if (space.personal)
            personalIndex = dialog->mNamespaces->count() - 1;
        }
        if (personalIndex >= 0)
          dialog->mNamespaces->setCurrentIndex(personalIndex);

        dialog->mLoadingNamespaces = false;
        dialog->mNamespaces->setEnabled(!namespaces.isEmpty());
        if (!success && !error.isEmpty())
          dialog->mNamespaceStatus->setText(error);
        else
          dialog->mNamespaceStatus->clear();
        dialog->updateButtons();
      });
}

void PublishRepositoryDialog::updateButtons() {
  if (!mPublish)
    return;
  QString error;
  mPublish->setEnabled(!mBusy && !mLoadingNamespaces && validate(&error));
}

bool PublishRepositoryDialog::validate(QString *error) const {
  auto fail = [error](const QString &text) {
    if (error)
      *error = text;
    return false;
  };

  if (!account())
    return fail(tr("Select or add a hosting account."));
  if (mNamespaces->currentIndex() < 0)
    return fail(tr("Select a repository destination."));
  if (repositoryName().isEmpty())
    return fail(tr("Enter a repository name."));

  if (mMode == PublishAndPush) {
    const QByteArray remoteNameBytes = remoteName().toUtf8();
    int validRemoteName = 0;
    if (remoteNameBytes.isEmpty() ||
        git_remote_name_is_valid(&validRemoteName,
                                 remoteNameBytes.constData()) < 0 ||
        !validRemoteName)
      return fail(tr("Enter a valid Git remote name."));
    if (mRepo.lookupRemote(remoteName()).isValid())
      return fail(tr("A remote named '%1' already exists.").arg(remoteName()));
  }

  return true;
}

void PublishRepositoryDialog::setBusy(bool busy) {
  mBusy = busy;
  mAccounts->setEnabled(!busy);
  mNamespaces->setEnabled(!busy && mNamespaces->count() > 0);
  mRepositoryName->setEnabled(!busy);
  mRemoteName->setEnabled(!busy && mMode == PublishAndPush);
  mDescription->setEnabled(!busy);
  mVisibility->setEnabled(!busy && mVisibility->count() > 1);
  mProtocol->setEnabled(!busy);
  mAddAccount->setEnabled(!busy);
  mAddRemote->setEnabled(!busy && mMode == PublishAndPush);
  mButtons->button(QDialogButtonBox::Cancel)->setEnabled(!busy);
  mPublish->setText(busy                  ? tr("Creating Repository…")
                    : mMode == CreateOnly ? tr("Create Repository")
                                          : tr("Create Repository and Push"));
  updateButtons();
}

void PublishRepositoryDialog::showError(const QString &error) {
  setBusy(false);
  mError->setText(error);
  mError->show();
}

void PublishRepositoryDialog::complete() {
  mBusy = false;
  QDialog::accept();
}

void PublishRepositoryDialog::reject() {
  if (mBusy)
    return;
  QDialog::reject();
}

void PublishRepositoryDialog::closeEvent(QCloseEvent *event) {
  if (mBusy) {
    event->ignore();
    return;
  }
  QDialog::closeEvent(event);
}

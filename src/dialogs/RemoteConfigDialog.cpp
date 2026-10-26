//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RemoteConfigDialog.h"
#include "git/Result.h"
#include "host/Accounts.h"
#include "ui/ExpandButton.h"
#include <QAction>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUrl>

namespace {

QStringList lines(const QPlainTextEdit *edit) {
  QStringList result;
  for (const QString &line : edit->toPlainText().split('\n')) {
    QString value = line.trimmed();
    if (!value.isEmpty())
      result.append(value);
  }
  return result;
}

constexpr int kNoneServiceId = 0;
constexpr int kServiceIdOffset = 1;

QList<Account::Kind> serviceKinds() {
  return {Account::GitHub, Account::GitLab, Account::Bitbucket,
          Account::Beanstalk, Account::Gitea};
}

} // namespace

RemoteConfigDialog::RemoteConfigDialog(const git::Repository &repo,
                                       const git::Remote &remote,
                                       const QString &name, QWidget *parent)
    : QDialog(parent), mRepo(repo), mRemote(remote),
      mEditing(remote.isValid()) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(mEditing ? tr("Configure Remote") : tr("Add Remote"));

  mName = new QLineEdit(mEditing ? remote.name() : name, this);
  mName->setObjectName(QStringLiteral("RemoteName"));
  mName->setPlaceholderText(tr("e.g. origin"));
  mFetchUrl = new QLineEdit(mEditing ? remote.url() : QString(), this);
  mFetchUrl->setObjectName(QStringLiteral("RemoteFetchUrl"));
  mFetchUrl->setPlaceholderText(mEditing ? QString()
                                         : tr("Enter the fetch URL"));
  mPushUrl = new QLineEdit(mEditing ? remote.pushUrl() : QString(), this);
  mPushUrl->setObjectName(QStringLiteral("RemotePushUrl"));
  mPushUrl->setPlaceholderText(mEditing ? QString() : tr("Optional push URL"));

  auto *pushUrlHint =
      new QLabel(tr("Optional; leave blank to use the fetch URL."), this);
  pushUrlHint->setObjectName(QStringLiteral("RemotePushUrlHint"));
  pushUrlHint->setWordWrap(true);

  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  if (!mEditing) {
    auto *services = new QWidget(this);
    services->setObjectName(QStringLiteral("RemoteServices"));
    auto *serviceLayout = new QHBoxLayout(services);
    serviceLayout->setContentsMargins(0, 0, 0, 0);
    serviceLayout->setSpacing(4);
    mServiceButtons = new QButtonGroup(this);

    auto *none = new QToolButton(services);
    none->setObjectName(QStringLiteral("RemoteServiceNone"));
    none->setText(tr("None"));
    none->setToolButtonStyle(Qt::ToolButtonTextOnly);
    none->setCheckable(true);
    none->setAccessibleName(tr("No hosting service"));
    mServiceButtons->addButton(none, kNoneServiceId);
    serviceLayout->addWidget(none);

    for (Account::Kind kind : serviceKinds()) {
      auto *button = new QToolButton(services);
      const QString name = Account::name(kind);
      button->setObjectName(QStringLiteral("RemoteService%1").arg(name));
      button->setIcon(Account::icon(kind));
      button->setToolTip(name);
      button->setAccessibleName(name);
      button->setIconSize(QSize(20, 20));
      button->setCheckable(true);
      button->setToolButtonStyle(Qt::ToolButtonIconOnly);
      mServiceButtons->addButton(button,
                                 static_cast<int>(kind) + kServiceIdOffset);
      serviceLayout->addWidget(button);
    }
    serviceLayout->addStretch();
    none->setChecked(true);
    form->addRow(tr("Service:"), services);

    mProtocol = new QComboBox(this);
    mProtocol->setObjectName(QStringLiteral("RemoteProtocol"));
    mProtocol->addItem(tr("HTTPS"), Repository::Https);
    mProtocol->addItem(tr("SSH"), Repository::Ssh);
    mProtocol->setCurrentIndex(mProtocol->findData(Repository::Ssh));
    form->addRow(tr("Protocol:"), mProtocol);

    connect(mServiceButtons, &QButtonGroup::idClicked, this, [this](int id) {
      const int service = id == kNoneServiceId ? -1 : id - kServiceIdOffset;
      setService(service);
    });
    connect(mProtocol, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
              if (mHasRepositoryUrls) {
                const QString fetch =
                    protocol() == Repository::Https ? mHttpsUrl : mSshUrl;
                const QString push = fetch;
                if (!mFetchUrlEdited && !fetch.isEmpty()) {
                  mGeneratedFetchUrl = fetch;
                  const QSignalBlocker blocker(mFetchUrl);
                  mFetchUrl->setText(fetch);
                }
                if (!mPushUrlEdited && !push.isEmpty()) {
                  mGeneratedPushUrl = push;
                  const QSignalBlocker blocker(mPushUrl);
                  mPushUrl->setText(push);
                }
              }
              updateUrlExamples();
            });

    auto *fetchLabel = new QWidget(this);
    auto *fetchLabelLayout = new QHBoxLayout(fetchLabel);
    fetchLabelLayout->setContentsMargins(0, 0, 0, 0);
    fetchLabelLayout->setSpacing(4);
    auto *fetchText = new QLabel(tr("Fetch URL:"), fetchLabel);
    fetchText->setBuddy(mFetchUrl);
    fetchLabelLayout->addWidget(fetchText);
    mAccountMenu = new QToolButton(fetchLabel);
    mAccountMenu->setObjectName(QStringLiteral("RemoteAccountMenu"));
    mAccountMenu->setArrowType(Qt::DownArrow);
    mAccountMenu->setToolTip(
        tr("Choose a repository from a connected account"));
    mAccountMenu->setAccessibleName(
        tr("Choose a repository or create a new repository"));
    mAccountMenu->hide();
    mAccountMenu->setPopupMode(QToolButton::InstantPopup);
    mAccountMenu->setMenu(new QMenu(mAccountMenu));
    connect(mAccountMenu->menu(), &QMenu::aboutToShow, this,
            &RemoteConfigDialog::updateAccountMenu);
    fetchLabelLayout->addWidget(mAccountMenu);
    fetchLabelLayout->addStretch();

    mFetchUrlExample = new QLabel(this);
    mFetchUrlExample->setObjectName(QStringLiteral("RemoteFetchUrlExample"));
    mFetchUrlExample->setWordWrap(true);
    mPushUrlExample = new QLabel(this);
    mPushUrlExample->setObjectName(QStringLiteral("RemotePushUrlExample"));
    mPushUrlExample->setWordWrap(true);
    mRepositoryStatus = new QLabel(this);
    mRepositoryStatus->setObjectName(QStringLiteral("RemoteRepositoryStatus"));
    mRepositoryStatus->setWordWrap(true);
    mRepositoryStatus->hide();

    form->addRow(tr("Name:"), mName);
    form->addRow(fetchLabel, mFetchUrl);
    form->addRow(QString(), mFetchUrlExample);
    form->addRow(tr("Push URL:"), mPushUrl);
    form->addRow(QString(), mPushUrlExample);
    form->addRow(QString(), pushUrlHint);
    form->addRow(QString(), mRepositoryStatus);
    updateUrlExamples();
  } else {
    form->addRow(tr("Name:"), mName);
    form->addRow(tr("Fetch URL:"), mFetchUrl);
    form->addRow(tr("Push URL:"), mPushUrl);
    form->addRow(QString(), pushUrlHint);
  }

  auto *expand = new ExpandButton(this);
  expand->setObjectName(QStringLiteral("RemoteRefspecsToggle"));
  expand->setText(tr("Refspecs"));
  expand->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto *advanced = new QWidget(this);
  advanced->setObjectName(QStringLiteral("RemoteAdvancedSettings"));
  advanced->setVisible(false);

  mFetchRefspecs = new QPlainTextEdit(advanced);
  mFetchRefspecs->setObjectName(QStringLiteral("RemoteFetchRefspecs"));
  mFetchRefspecs->setTabChangesFocus(true);
  mFetchRefspecs->setPlaceholderText(
      tr("One fetch refspec per line, for example:\n"
         "+refs/heads/*:refs/remotes/origin/*"));
  mPushRefspecs = new QPlainTextEdit(advanced);
  mPushRefspecs->setObjectName(QStringLiteral("RemotePushRefspecs"));
  mPushRefspecs->setTabChangesFocus(true);
  mPushRefspecs->setPlaceholderText(
      tr("One push refspec per line, for example:\n"
         "refs/heads/main:refs/heads/main"));

  auto *advancedForm = new QFormLayout(advanced);
  advancedForm->setContentsMargins(0, 0, 0, 0);
  advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  advancedForm->addRow(tr("Fetch refspecs:"), mFetchRefspecs);
  advancedForm->addRow(tr("Push refspecs:"), mPushRefspecs);
  connect(expand, &QToolButton::toggled, advanced, &QWidget::setVisible);

  if (mEditing) {
    const QStringList fetchRefspecs = remote.fetchRefspecs();
    const QStringList pushRefspecs = remote.pushRefspecs();
    mFetchRefspecs->setPlainText(fetchRefspecs.join('\n'));
    mPushRefspecs->setPlainText(pushRefspecs.join('\n'));
    expand->setChecked(!fetchRefspecs.isEmpty() || !pushRefspecs.isEmpty());
  } else {
    mGeneratedFetchRefspec = defaultFetchRefspec(name);
    mFetchRefspecs->setPlainText(mGeneratedFetchRefspec);
    connect(mName, &QLineEdit::textChanged, this, [this] {
      if (!mFetchRefspecEdited) {
        mGeneratedFetchRefspec = defaultFetchRefspec(mName->text());
        QSignalBlocker blocker(mFetchRefspecs);
        mFetchRefspecs->setPlainText(mGeneratedFetchRefspec);
      }
      updateSaveButton();
    });
    connect(mFetchRefspecs, &QPlainTextEdit::textChanged, this, [this] {
      mFetchRefspecEdited =
          mFetchRefspecs->toPlainText() != mGeneratedFetchRefspec;
    });
  }

  auto *buttons = new QDialogButtonBox(this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  buttons->addButton(QDialogButtonBox::Cancel);
  mSave = buttons->addButton(mEditing ? tr("Save Remote") : tr("Add Remote"),
                             QDialogButtonBox::AcceptRole);
  mSave->setObjectName(QStringLiteral("SaveRemote"));
  connect(mSave, &QPushButton::clicked, this, &RemoteConfigDialog::accept);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(expand);
  layout->addWidget(advanced);
  layout->addWidget(buttons);

  connect(mFetchUrl, &QLineEdit::textChanged, this,
          [this](const QString &text) {
            if (mHasRepositoryUrls && text != mGeneratedFetchUrl)
              mFetchUrlEdited = true;
            updateSaveButton();
          });
  connect(mPushUrl, &QLineEdit::textChanged, this, [this](const QString &text) {
    if (mHasRepositoryUrls && text != mGeneratedPushUrl)
      mPushUrlEdited = true;
  });
  connect(mName, &QLineEdit::textChanged, this,
          &RemoteConfigDialog::updateSaveButton);
  updateSaveButton();

  if (!mEditing)
    resize(sizeHint().width() * 2, sizeHint().height());
}

QString RemoteConfigDialog::name() const { return mName->text().trimmed(); }

QString RemoteConfigDialog::fetchUrl() const {
  return mFetchUrl->text().trimmed();
}

QString RemoteConfigDialog::pushUrl() const {
  return mPushUrl->text().trimmed();
}

QStringList RemoteConfigDialog::fetchRefspecs() const {
  return lines(mFetchRefspecs);
}

QStringList RemoteConfigDialog::pushRefspecs() const {
  return lines(mPushRefspecs);
}

Repository::Protocol RemoteConfigDialog::protocol() const {
  if (!mProtocol)
    return Repository::Ssh;
  return static_cast<Repository::Protocol>(mProtocol->currentData().toInt());
}

void RemoteConfigDialog::setCreatedRepositoryUrls(
    const QString &httpsUrl, const QString &sshUrl,
    Repository::Protocol selectedProtocol) {
  if ((selectedProtocol == Repository::Https && httpsUrl.isEmpty()) ||
      (selectedProtocol == Repository::Ssh && sshUrl.isEmpty()))
    selectedProtocol = httpsUrl.isEmpty() ? Repository::Ssh : Repository::Https;
  mProtocol->setCurrentIndex(mProtocol->findData(selectedProtocol));
  setRepositoryUrls(httpsUrl, sshUrl);
  if (mRepositoryStatus) {
    mRepositoryStatus->setText(
        tr("Repository created. Add Remote will save it locally; the hosted "
           "repository has not been pushed."));
    mRepositoryStatus->show();
  }
}

void RemoteConfigDialog::setService(int service) {
  if (mService == service)
    return;

  mService = service;
  mHasRepositoryUrls = false;
  if (mAccountMenu)
    mAccountMenu->setVisible(service >= 0);
  updateUrlExamples();
}

void RemoteConfigDialog::updateUrlExamples() {
  if (!mFetchUrlExample || !mPushUrlExample)
    return;

  mFetchUrlExample->setText(tr("Example: %1").arg(exampleUrl(false)));
  mPushUrlExample->setText(tr("Example: %1").arg(exampleUrl(true)));
}

QString RemoteConfigDialog::exampleUrl(bool push) const {
  Q_UNUSED(push);

  if (mService < 0) {
    return protocol() == Repository::Https
               ? QStringLiteral("https://<host>/<owner>/<repository>.git")
               : QStringLiteral("git@<host>:<owner>/<repository>.git");
  }

  const auto kind = static_cast<Account::Kind>(mService);
  QString host;
  QString username = QStringLiteral("<username>");
  Accounts *accounts = Accounts::instance();
  for (int i = 0; i < accounts->count(); ++i) {
    Account *account = accounts->account(i);
    if (account->kind() != kind)
      continue;
    username = account->username();
    if (account->hasCustomUrl())
      host = QUrl(account->url()).host();
    if (!host.isEmpty())
      break;
  }

  QString path = QStringLiteral("<owner>/<repository>");
  if (kind == Account::GitLab)
    path = QStringLiteral("<namespace>/<repository>");
  else if (kind == Account::Bitbucket)
    path = QStringLiteral("<workspace>/<repository>");
  else if (kind == Account::Beanstalk) {
    if (protocol() == Repository::Https) {
      host = host.isEmpty()
                 ? QStringLiteral("%1.git.beanstalkapp.com").arg(username)
                 : host;
      return QStringLiteral("https://%1/<repository>.git").arg(host);
    }
    host = host.isEmpty() ? QStringLiteral("%1.beanstalkapp.com").arg(username)
                          : host;
    return QStringLiteral("git@%1:/<repository>.git").arg(host);
  }

  if (host.isEmpty()) {
    switch (kind) {
      case Account::GitHub:
        host = QStringLiteral("github.com");
        break;
      case Account::GitLab:
        host = QStringLiteral("gitlab.com");
        break;
      case Account::Bitbucket:
        host = QStringLiteral("bitbucket.org");
        break;
      case Account::Beanstalk:
        break;
      case Account::Gitea:
        host = QStringLiteral("try.gitea.io");
        break;
    }
  }

  if (protocol() == Repository::Https) {
    if (kind == Account::Bitbucket)
      return QStringLiteral("https://%1@%2/%3.git").arg(username, host, path);
    return QStringLiteral("https://%1/%2.git").arg(host, path);
  }

  return QStringLiteral("git@%1:%2.git").arg(host, path);
}

void RemoteConfigDialog::updateAccountMenu() {
  if (!mAccountMenu || !mAccountMenu->menu() || mService < 0)
    return;

  QMenu *menu = mAccountMenu->menu();
  menu->clear();

  const auto kind = static_cast<Account::Kind>(mService);
  bool hasRepositories = false;
  Accounts *accounts = Accounts::instance();
  for (int i = 0; i < accounts->count(); ++i) {
    Account *account = accounts->account(i);
    if (account->kind() != kind)
      continue;

    if (account->repositoryCount() == 0)
      continue;

    QMenu *accountMenu = menu->addMenu(
        Account::icon(kind), QStringLiteral("%1 (%2)").arg(
                                 Account::name(kind), account->username()));
    for (int j = 0; j < account->repositoryCount(); ++j) {
      Repository *repository = account->repository(j);
      QAction *action = accountMenu->addAction(repository->fullName());
      QPointer<Repository> repositoryGuard(repository);
      connect(action, &QAction::triggered, this, [this, repositoryGuard] {
        if (!repositoryGuard)
          return;
        setRepositoryUrls(repositoryGuard->url(Repository::Https),
                          repositoryGuard->url(Repository::Ssh));
      });
    }
    hasRepositories = true;
  }

  if (!hasRepositories) {
    QAction *empty = menu->addAction(tr("No repositories available"));
    empty->setEnabled(false);
  }

  menu->addSeparator();
  QAction *create = menu->addAction(tr("Create new repository…"));
  connect(create, &QAction::triggered, this, [this] {
    if (mService >= 0)
      emit createRepositoryRequested(mService, static_cast<int>(protocol()));
  });
}

void RemoteConfigDialog::setRepositoryUrls(const QString &httpsUrl,
                                           const QString &sshUrl) {
  mHttpsUrl = httpsUrl;
  mSshUrl = sshUrl;
  mHasRepositoryUrls = true;
  mFetchUrlEdited = false;
  mPushUrlEdited = false;

  const QString selected =
      protocol() == Repository::Https ? mHttpsUrl : mSshUrl;
  if (selected.isEmpty())
    return;

  mGeneratedFetchUrl = selected;
  mGeneratedPushUrl = selected;
  {
    const QSignalBlocker blocker(mFetchUrl);
    mFetchUrl->setText(selected);
  }
  {
    const QSignalBlocker blocker(mPushUrl);
    mPushUrl->setText(selected);
  }
  updateSaveButton();
}

void RemoteConfigDialog::accept() {
  if (name().isEmpty() || fetchUrl().isEmpty())
    return;

  git::Result result;
  if (mEditing) {
    result = mRemote.configure(name(), fetchUrl(), pushUrl(), fetchRefspecs(),
                               pushRefspecs());
  } else {
    mRemote = mRepo.addRemote(name(), fetchUrl(), &result);
    if (mRemote) {
      result = mRemote.configure(name(), fetchUrl(), pushUrl(), fetchRefspecs(),
                                 pushRefspecs());
      if (result.error()) {
        mRepo.deleteRemote(name());
        mRemote = git::Remote();
      }
    }
  }

  if (result.error()) {
    QMessageBox::warning(
        this, tr("Unable to Save Remote"),
        result.errorString(tr("The remote configuration could not be saved.")));
    return;
  }

  QDialog::accept();
}

void RemoteConfigDialog::updateSaveButton() {
  mSave->setEnabled(!name().isEmpty() && !fetchUrl().isEmpty());
}

QString
RemoteConfigDialog::defaultFetchRefspec(const QString &remoteName) const {
  QString name = remoteName.trimmed();
  if (name.isEmpty())
    name = QStringLiteral("origin");
  return QStringLiteral("+refs/heads/*:refs/remotes/%1/*").arg(name);
}

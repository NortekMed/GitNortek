//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "StartDialog.h"
#include "AccountDialog.h"
#include "CloneDialog.h"
#include "app/Application.h"
#include "host/Accounts.h"
#include "host/Repository.h"
#include "ui/Footer.h"
#include "ui/MainWindow.h"
#include "ui/ProgressIndicator.h"
#include "ui/RepoView.h"
#include "ui/TabWidget.h"
#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

const QString kGeometryKey = "geometry";
const QString kStartGroup = "start";

const QString kCloudIcon = ":/cloud.png";

enum Role { KindRole = Qt::UserRole, AccountRole, RepositoryRole };

class AccountModel : public QAbstractListModel {
  Q_OBJECT

public:
  AccountModel(QObject *parent = nullptr) : QAbstractListModel(parent) {
    Accounts *accounts = Accounts::instance();
    connect(accounts, &Accounts::accountAboutToBeAdded, this,
            &AccountModel::beginResetModel);
    connect(accounts, &Accounts::accountAdded, this,
            &AccountModel::endResetModel);
    connect(accounts, &Accounts::accountAboutToBeRemoved, this,
            &AccountModel::beginResetModel);
    connect(accounts, &Accounts::accountRemoved, this,
            &AccountModel::endResetModel);
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override {
    return parent.isValid() ? 0 : Accounts::instance()->count();
  }

  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= Accounts::instance()->count())
      return QVariant();

    Account *account = Accounts::instance()->account(index.row());
    switch (role) {
      case Qt::DisplayRole:
        return account->username();
      case Qt::DecorationRole:
        return Account::icon(account->kind());
      case KindRole:
        return account->kind();
      case AccountRole:
        return QVariant::fromValue(account);
    }

    return QVariant();
  }
};

class RemoteRepositoryModel : public QAbstractListModel {
  Q_OBJECT

public:
  RemoteRepositoryModel(QStyle *style, QObject *parent = nullptr)
      : QAbstractListModel(parent),
        mErrorIcon(style->standardIcon(QStyle::SP_MessageBoxCritical)) {
    Accounts *accounts = Accounts::instance();
    connect(accounts, &Accounts::progress, this, [this](int accountIndex, int) {
      if (isCurrentAccount(accountIndex) && rowCount() > 0)
        emit dataChanged(index(0, 0), index(0, 0),
                         {Qt::DecorationRole, Qt::FontRole});
    });
    connect(accounts, &Accounts::started, this,
            [this](int accountIndex) { resetIfCurrentAccount(accountIndex); });
    connect(accounts, &Accounts::finished, this,
            [this](int accountIndex) { resetIfCurrentAccount(accountIndex); });
    connect(accounts, &Accounts::repositoryAboutToBeAdded, this,
            [this](int accountIndex) {
              if (isCurrentAccount(accountIndex))
                beginResetModel();
            });
    connect(accounts, &Accounts::repositoryAdded, this,
            [this](int accountIndex) {
              if (isCurrentAccount(accountIndex))
                endResetModel();
            });
    connect(accounts, &Accounts::repositoryPathChanged, this,
            [this](int accountIndex, int repoIndex, const QString &) {
              if (!isCurrentAccount(accountIndex) || repoIndex < 0 ||
                  repoIndex >= rowCount())
                return;

              QModelIndex changed = index(repoIndex, 0);
              emit dataChanged(changed, changed,
                               {Qt::DisplayRole, Qt::DecorationRole});
            });
  }

  Account *account() const { return mAccount; }

  void setAccount(Account *account) {
    if (mAccount == account)
      return;

    beginResetModel();
    mAccount = account;
    endResetModel();
  }

  void setShowFullName(bool enabled) {
    if (mShowFullName == enabled)
      return;

    beginResetModel();
    mShowFullName = enabled;
    endResetModel();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override {
    if (parent.isValid() || !mAccount)
      return 0;

    if (mAccount->repositoryCount() > 0)
      return mAccount->repositoryCount();

    return (mAccount->error()->isValid() || mAccount->progress()->isValid())
               ? 1
               : 0;
  }

  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
      return QVariant();

    if (mAccount->repositoryCount() <= 0) {
      AccountError *error = mAccount->error();
      if (error->isValid()) {
        switch (role) {
          case Qt::DisplayRole:
            return error->text();
          case Qt::DecorationRole:
            return mErrorIcon;
          case Qt::ToolTipRole:
            return error->detailedText();
        }
      }

      switch (role) {
        case Qt::DisplayRole:
          return tr("Connecting");
        case Qt::DecorationRole:
          return mAccount->progress()->value();
        case Qt::FontRole: {
          QFont font = static_cast<QWidget *>(QObject::parent())->font();
          font.setItalic(true);
          return font;
        }
      }

      return QVariant();
    }

    Repository *repo = mAccount->repository(index.row());
    switch (role) {
      case Qt::DisplayRole:
        return mShowFullName ? repo->fullName() : repo->name();
      case Qt::DecorationRole:
        return mAccount->repositoryPath(index.row()).isEmpty()
                   ? QIcon(kCloudIcon)
                   : QIcon();
      case RepositoryRole:
        return QVariant::fromValue(repo);
    }

    return QVariant();
  }

  Qt::ItemFlags flags(const QModelIndex &index) const override {
    Qt::ItemFlags itemFlags = QAbstractListModel::flags(index);
    if (!index.data(RepositoryRole).isValid())
      itemFlags &= ~Qt::ItemIsSelectable;
    return itemFlags;
  }

private:
  bool isCurrentAccount(int accountIndex) const {
    Accounts *accounts = Accounts::instance();
    return mAccount && accountIndex >= 0 && accountIndex < accounts->count() &&
           accounts->account(accountIndex) == mAccount;
  }

  void resetIfCurrentAccount(int accountIndex) {
    if (!isCurrentAccount(accountIndex))
      return;

    beginResetModel();
    endResetModel();
  }

  Account *mAccount = nullptr;
  bool mShowFullName = false;
  QIcon mErrorIcon;
};

class ProgressDelegate : public QStyledItemDelegate {
public:
  ProgressDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Draw background.
    QStyledItemDelegate::paint(painter, opt, index);

    // Draw busy indicator.
    QVariant progress = index.data(Qt::DecorationRole);
    if (!progress.canConvert<int>())
      return;

    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    QStyle::SubElement se = QStyle::SE_ItemViewItemDecoration;
    QRect rect = style->subElementRect(se, &opt, opt.widget);
    ProgressIndicator::paint(painter, rect, "#808080", progress.toInt());
  }

protected:
  void initStyleOption(QStyleOptionViewItem *option,
                       const QModelIndex &index) const override {
    QStyledItemDelegate::initStyleOption(option, index);
    if (index.data(Qt::DecorationRole).canConvert<int>()) {
      option->decorationSize = ProgressIndicator::size();
    } else if (index.data(RepositoryRole).isValid()) {
      option->features |= QStyleOptionViewItem::HasDecoration;
      option->decorationSize = QSize(20, 20);
    }
  }
};

} // namespace

StartDialog::StartDialog(QWidget *parent) : QDialog(parent) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(tr("Choose Repository"));

  mAccountList = new QListView(this);
  mAccountList->setObjectName("RemoteAccounts");
  mAccountList->setIconSize(QSize(32, 32));
  mAccountList->setSelectionMode(QAbstractItemView::SingleSelection);
  auto accountModel = new AccountModel(mAccountList);
  mAccountList->setModel(accountModel);

  mAccountFooter = new Footer(mAccountList);
  mAccountFooter->setObjectName("AccountFooter");
  connect(mAccountFooter, &Footer::plusClicked, this, [this] { edit(); });
  connect(mAccountFooter, &Footer::minusClicked, this, &StartDialog::remove);

  QMenu *accountOptionsMenu = new QMenu(this);
  mAccountFooter->setContextMenu(accountOptionsMenu);

  QAction *refresh = accountOptionsMenu->addAction(tr("Refresh"));
  connect(refresh, &QAction::triggered, this, [] {
    Accounts *accounts = Accounts::instance();
    for (int i = 0; i < accounts->count(); ++i)
      accounts->account(i)->connect();
  });

  QSettings settings;
  QAction *showFullName = accountOptionsMenu->addAction(tr("Show Full Name"));
  bool remoteChecked = settings.value("start/remote/fullname").toBool();
  showFullName->setCheckable(true);
  showFullName->setChecked(remoteChecked);

  mRemoteRepoList = new QListView(this);
  mRemoteRepoList->setObjectName("RemoteRepositories");
  mRemoteRepoList->setIconSize(QSize(32, 32));
  mRemoteRepoList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  auto repositoryModel = new RemoteRepositoryModel(style(), mRemoteRepoList);
  repositoryModel->setShowFullName(remoteChecked);
  mRemoteRepoList->setModel(repositoryModel);
  mRemoteRepoList->setItemDelegate(new ProgressDelegate(this));

  mRemoteRepoFooter = new Footer(mRemoteRepoList);
  mRemoteRepoFooter->setObjectName("RemoteRepositoryFooter");
  mRemoteRepoFooter->setPlusVisible(false);
  connect(mRemoteRepoFooter, &Footer::minusClicked, this,
          &StartDialog::removeRepositoryAssociation);

  connect(mAccountList, &QListView::doubleClicked, this, &StartDialog::edit);
  connect(mRemoteRepoList, &QListView::doubleClicked, this,
          [this](const QModelIndex &index) {
            Repository *repo = index.data(RepositoryRole).value<Repository *>();
            int repoIndex = repo ? repo->account()->indexOf(repo) : -1;
            if (repo && repoIndex >= 0 &&
                !repo->account()->repositoryPath(repoIndex).isEmpty()) {
              accept();
              return;
            }
            if (repo)
              edit(index);
          });

  connect(
      mAccountList->selectionModel(), &QItemSelectionModel::currentChanged,
      this,
      [this, repositoryModel](const QModelIndex &current, const QModelIndex &) {
        repositoryModel->setAccount(
            current.data(AccountRole).value<Account *>());
        mRemoteRepoList->clearSelection();
        updateButtons();
      });
  connect(accountModel, &QAbstractItemModel::modelReset, this,
          [this, accountModel, repositoryModel] {
            if (accountModel->rowCount() > 0) {
              if (!mAccountList->currentIndex().isValid())
                mAccountList->selectionModel()->setCurrentIndex(
                    accountModel->index(0, 0),
                    QItemSelectionModel::ClearAndSelect);
            } else {
              mAccountList->selectionModel()->setCurrentIndex(
                  QModelIndex(), QItemSelectionModel::Clear);
              repositoryModel->setAccount(nullptr);
            }
            updateButtons();
          });
  connect(repositoryModel, &QAbstractItemModel::modelReset, this, [this] {
    mRemoteRepoList->clearSelection();
    updateButtons();
  });
  connect(repositoryModel, &QAbstractItemModel::dataChanged, this,
          [this] { updateButtons(); });
  connect(mRemoteRepoList->selectionModel(),
          &QItemSelectionModel::selectionChanged, this,
          [this] { updateButtons(); });

  connect(showFullName, &QAction::triggered, this,
          [repositoryModel](bool checked) {
            QSettings().setValue("start/remote/fullname", checked);
            repositoryModel->setShowFullName(checked);
          });

  auto *accountsColumn = new QVBoxLayout;
  accountsColumn->setSpacing(0);
  accountsColumn->addWidget(new QLabel(tr("Remote Accounts:"), this));
  accountsColumn->addSpacing(8);
  accountsColumn->addWidget(mAccountList);
  accountsColumn->addWidget(mAccountFooter);

  auto *repositoriesColumn = new QVBoxLayout;
  repositoriesColumn->setSpacing(0);
  repositoriesColumn->addWidget(
      new QLabel(tr("Repositories for selected account:"), this));
  repositoriesColumn->addSpacing(8);
  repositoriesColumn->addWidget(mRemoteRepoList);
  repositoriesColumn->addWidget(mRemoteRepoFooter);

  auto *top = new QHBoxLayout;
  top->addLayout(accountsColumn, 1);
  top->addSpacing(12);
  top->addLayout(repositoriesColumn, 2);

  QDialogButtonBox::StandardButtons buttons =
      QDialogButtonBox::Open | QDialogButtonBox::Cancel;
  mButtonBox = new QDialogButtonBox(buttons, this);
  connect(mButtonBox, &QDialogButtonBox::accepted, this, &StartDialog::accept);
  connect(mButtonBox, &QDialogButtonBox::rejected, this, &StartDialog::reject);

  auto *cloneOtherRepository = mButtonBox->addButton(
      tr("Clone other repository"), QDialogButtonBox::ResetRole);
  cloneOtherRepository->setObjectName("CloneOtherRepository");
  connect(cloneOtherRepository, &QPushButton::clicked, this, [this] {
    CloneDialog *dialog = new CloneDialog(CloneDialog::Clone, this);
    connect(dialog, &CloneDialog::accepted, this, [this, dialog] {
      if (MainWindow *window =
              openWindow(dialog->path(), dialog->updateSubmodules()))
        window->currentView()->addLogEntry(dialog->message(),
                                           dialog->messageTitle());
    });
    dialog->open();
  });

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(mButtonBox);

  if (accountModel->rowCount() > 0)
    mAccountList->selectionModel()->setCurrentIndex(
        accountModel->index(0, 0), QItemSelectionModel::ClearAndSelect);
  updateButtons();
}

void StartDialog::accept() {
  QModelIndexList repoIndexes =
      mRemoteRepoList->selectionModel()->selectedIndexes();

  QList<QPair<QString, MainWindow::OpenSource>> paths;
  QModelIndexList uncloned;
  foreach (const QModelIndex &index, repoIndexes) {
    Repository *repo = index.data(RepositoryRole).value<Repository *>();
    if (!repo)
      continue;

    Account *account = repo->account();
    int repoIndex = account->indexOf(repo);
    if (repoIndex < 0)
      continue;

    QString path = account->repositoryPath(repoIndex);
    if (path.isEmpty())
      uncloned.append(index);
    else
      paths.append({path, MainWindow::OpenSource::Other});
  }

  if (!uncloned.isEmpty()) {
    edit(uncloned.first());
    return;
  }

  if (paths.isEmpty())
    return;

  QDialog::accept();

  auto request = paths.takeFirst();
  auto updateSubmodules = [this](const QString &path) -> std::optional<bool> {
    auto override = mSubmoduleUpdateOverrides.constFind(path);
    return override == mSubmoduleUpdateOverrides.cend()
               ? std::nullopt
               : std::optional<bool>(*override);
  };
  MainWindow *window = MainWindow::open(request.first, true, request.second,
                                        updateSubmodules(request.first));
  while (!window && !paths.isEmpty()) {
    request = paths.takeFirst();
    window = MainWindow::open(request.first, true, request.second,
                              updateSubmodules(request.first));
  }

  if (!window)
    return;

  foreach (const auto &request, paths)
    window->addTab(request.first, request.second, QString(),
                   updateSubmodules(request.first));
}

StartDialog *StartDialog::openSharedInstance() {
  static QPointer<StartDialog> dialog;
  if (dialog) {
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return dialog;
  }

  dialog = new StartDialog;
  dialog->show();
  return dialog;
}

void StartDialog::showEvent(QShowEvent *event) {
  // Restore geometry.
  QSettings settings;
  settings.beginGroup(kStartGroup);
  if (settings.contains(kGeometryKey))
    restoreGeometry(settings.value(kGeometryKey).toByteArray());
  settings.endGroup();

  QDialog::showEvent(event);
}

void StartDialog::hideEvent(QHideEvent *event) {
  QSettings settings;
  settings.beginGroup(kStartGroup);
  settings.setValue(kGeometryKey, saveGeometry());
  settings.endGroup();

  QDialog::hideEvent(event);
}

void StartDialog::updateButtons() {
  QModelIndexList repoIndexes =
      mRemoteRepoList->selectionModel()->selectedIndexes();
  bool canOpen = !repoIndexes.isEmpty();
  bool clone = false;

  foreach (const QModelIndex &index, repoIndexes) {
    Repository *repo = index.data(RepositoryRole).value<Repository *>();
    if (!repo) {
      canOpen = false;
      break;
    }

    Account *account = repo->account();
    int repoIndex = account->indexOf(repo);
    if (repoIndex < 0) {
      canOpen = false;
      break;
    }
    clone |= account->repositoryPath(repoIndex).isEmpty();
  }

  QPushButton *open = mButtonBox->button(QDialogButtonBox::Open);
  open->setEnabled(canOpen);
  open->setText((clone && canOpen) ? tr("Clone") : tr("Open"));

  QModelIndex accountIndex = mAccountList->currentIndex();
  mAccountFooter->setMinusEnabled(accountIndex.isValid());

  bool canForgetAssociation = false;
  if (repoIndexes.size() == 1) {
    Repository *repo =
        repoIndexes.first().data(RepositoryRole).value<Repository *>();
    if (repo) {
      int repoIndex = repo->account()->indexOf(repo);
      canForgetAssociation =
          repoIndex >= 0 &&
          !repo->account()->repositoryPath(repoIndex).isEmpty();
    }
  }
  mRemoteRepoFooter->setMinusEnabled(canForgetAssociation);
}

void StartDialog::edit(const QModelIndex &index) {
  if (!index.isValid() || index.model() == mAccountList->model()) {
    Account *account =
        index.isValid() ? index.data(AccountRole).value<Account *>() : nullptr;
    (new AccountDialog(account, this))->open();
    return;
  }

  Repository *repo = index.data(RepositoryRole).value<Repository *>();
  if (!repo)
    return;

  Account *account = repo->account();
  QPointer<Account> accountGuard(account);
  CloneDialog *dialog = new CloneDialog(CloneDialog::Clone, this, repo);
  connect(
      dialog, &CloneDialog::accepted, this, [this, dialog, accountGuard, repo] {
        if (!accountGuard)
          return;

        int repoIndex = accountGuard->indexOf(repo);
        if (repoIndex < 0)
          return;
        accountGuard->setRepositoryPath(repoIndex, dialog->path());
        if (std::optional<bool> updateSubmodules = dialog->updateSubmodules())
          mSubmoduleUpdateOverrides.insert(dialog->path(), *updateSubmodules);

        QCoreApplication::processEvents();
        accept();
      });
  dialog->open();
}

void StartDialog::remove() {
  QModelIndex index = mAccountList->currentIndex();
  if (!index.isValid())
    return;

  Account *account = index.data(AccountRole).value<Account *>();
  if (!account)
    return;

  QString fmt =
      tr("<p>Are you sure you want to remove the %1 account for '%2'?</p>"
         "<p>Only the account association will be removed. Remote "
         "configurations and local clones will not be affected.</p>");

  QMessageBox mb(QMessageBox::Warning, tr("Remove Account?"),
                 fmt.arg(Account::name(account->kind()), account->username()),
                 QMessageBox::Cancel, this);
  QPushButton *remove = mb.addButton(tr("Remove"), QMessageBox::AcceptRole);
  remove->setFocus();

  mb.exec();
  if (mb.clickedButton() != remove)
    return;

  mAccountList->selectionModel()->setCurrentIndex(QModelIndex(),
                                                  QItemSelectionModel::Clear);
  static_cast<RemoteRepositoryModel *>(mRemoteRepoList->model())
      ->setAccount(nullptr);
  Accounts::instance()->removeAccount(account);
}

void StartDialog::removeRepositoryAssociation() {
  QModelIndexList indexes =
      mRemoteRepoList->selectionModel()->selectedIndexes();
  if (indexes.size() != 1)
    return;

  Repository *repo = indexes.first().data(RepositoryRole).value<Repository *>();
  if (!repo)
    return;

  Account *account = repo->account();
  int repoIndex = account->indexOf(repo);
  if (repoIndex < 0 || account->repositoryPath(repoIndex).isEmpty())
    return;

  QString fmt =
      tr("<p>Are you sure you want to remove the remote repository association "
         "for %1?</p><p>The local clone itself will not be affected.</p>");
  QMessageBox mb(QMessageBox::Warning, tr("Remove Repository Association?"),
                 fmt.arg(repo->fullName()), QMessageBox::Cancel, this);
  QPushButton *remove = mb.addButton(tr("Remove"), QMessageBox::AcceptRole);
  remove->setFocus();

  mb.exec();
  if (mb.clickedButton() == remove)
    account->setRepositoryPath(repoIndex, QString());

  updateButtons();
}

MainWindow *StartDialog::openWindow(const QString &repo,
                                    std::optional<bool> updateSubmodules) {
  // This dialog has to be hidden before opening another window so that it
  // doesn't automatically move to a position that still shows the dialog.
  hide();
  if (MainWindow *window = MainWindow::open(
          repo, true, MainWindow::OpenSource::Other, updateSubmodules)) {
    // Dismiss this dialog without trying to open the selection.
    reject();
    return window;
  } else {
    show();
  }

  return nullptr;
}

#include "StartDialog.moc"

//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "LocalWorkspaceDialog.h"
#include "DirectorySelectionDialog.h"
#include "git/Repository.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

namespace {

bool pathsMatch(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
  return left.compare(right, Qt::CaseInsensitive) == 0;
#else
  return left == right;
#endif
}

bool containsPath(const QStringList &paths, const QString &path) {
  for (const QString &candidate : paths) {
    if (pathsMatch(candidate, path))
      return true;
  }
  return false;
}

} // namespace

LocalWorkspaceDialog::LocalWorkspaceDialog(QWidget *parent)
    : LocalWorkspaceDialog(std::nullopt, parent) {}

LocalWorkspaceDialog::LocalWorkspaceDialog(
    std::optional<LocalWorkspace> workspace, QWidget *parent)
    : QDialog(parent), mWorkspace(workspace.value_or(LocalWorkspace())) {
  const bool editing = workspace.has_value();
  if (mWorkspace.id.isEmpty())
    mWorkspace.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!mWorkspace.color.isValid())
    mWorkspace.color = QColor(QStringLiteral("#3daee9"));

  setWindowTitle(editing ? tr("Edit Local Workspace")
                         : tr("Create Local Workspace"));
  setMinimumSize(520, 560);

  mName = new QLineEdit(mWorkspace.name, this);
  mName->setObjectName(QStringLiteral("LocalWorkspaceName"));
  mName->setAccessibleName(tr("Workspace name"));
  mName->setPlaceholderText(tr("Workspace name"));

  mIcon = new QComboBox(this);
  mIcon->setObjectName(QStringLiteral("LocalWorkspaceIcon"));
  mIcon->setAccessibleName(tr("Workspace icon"));
  const QList<QPair<QString, QString>> icons = {
      {QStringLiteral("folder-symbolic"), tr("Folder")},
      {QStringLiteral("applications-development-symbolic"), tr("Development")},
      {QStringLiteral("document-open-symbolic"), tr("Project")},
      {QStringLiteral("network-server-symbolic"), tr("Server")}};
  for (const auto &[name, label] : icons)
    mIcon->addItem(QIcon::fromTheme(name), label, name);
  int iconIndex = mIcon->findData(mWorkspace.iconName);
  if (iconIndex < 0 && !mWorkspace.iconName.isEmpty()) {
    mIcon->addItem(QIcon::fromTheme(mWorkspace.iconName), mWorkspace.iconName,
                   mWorkspace.iconName);
    iconIndex = mIcon->count() - 1;
  }
  mIcon->setCurrentIndex(iconIndex < 0 ? 0 : iconIndex);

  mColor = new QPushButton(this);
  mColor->setObjectName(QStringLiteral("LocalWorkspaceColor"));
  mColor->setAccessibleName(tr("Workspace color"));
  updateColorButton();

  mDescription = new QPlainTextEdit(mWorkspace.description, this);
  mDescription->setObjectName(QStringLiteral("LocalWorkspaceDescription"));
  mDescription->setAccessibleName(tr("Workspace description"));
  mDescription->setPlaceholderText(tr("Description"));
  mDescription->setMaximumHeight(100);

  QFormLayout *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(tr("Name:"), mName);
  form->addRow(tr("Icon:"), mIcon);
  form->addRow(tr("Color:"), mColor);
  form->addRow(tr("Description:"), mDescription);

  mRepositories = new QListWidget(this);
  mRepositories->setObjectName(QStringLiteral("LocalWorkspaceRepositories"));
  mRepositories->setAccessibleName(tr("Repositories"));
  mRepositories->setSelectionMode(QAbstractItemView::SingleSelection);
  for (const QString &path : std::as_const(mWorkspace.repositories)) {
    QListWidgetItem *item = new QListWidgetItem(path, mRepositories);
    item->setData(Qt::UserRole,
                  containsPath(mWorkspace.synchronizedRepositories, path));
    item->setData(Qt::UserRole + 1,
                  containsPath(mWorkspace.manualRepositories, path));
  }

  QPushButton *browseRepositories =
      new QPushButton(tr("Browse Repositories..."), this);
  browseRepositories->setObjectName(
      QStringLiteral("LocalWorkspaceBrowseRepositories"));
  mRemoveRepository = new QPushButton(tr("Remove Selected"), this);
  mRemoveRepository->setObjectName(
      QStringLiteral("LocalWorkspaceRemoveRepository"));
  mRemoveRepository->setEnabled(false);

  QHBoxLayout *repositoryActions = new QHBoxLayout;
  repositoryActions->addWidget(browseRepositories);
  repositoryActions->addWidget(mRemoveRepository);
  repositoryActions->addStretch();

  mSync = new QCheckBox(tr("Sync with local directory"), this);
  mSync->setObjectName(QStringLiteral("LocalWorkspaceSyncEnabled"));
  mSync->setChecked(mWorkspace.syncEnabled);
  mSyncDirectory = new QLineEdit(mWorkspace.syncDirectory, this);
  mSyncDirectory->setObjectName(
      QStringLiteral("LocalWorkspaceSyncDirectory"));
  mSyncDirectory->setAccessibleName(tr("Synchronized directory"));
  mSyncDirectory->setReadOnly(true);
  mSyncDirectory->setPlaceholderText(tr("No directory selected"));
  mBrowseSyncDirectory = new QPushButton(tr("Browse..."), this);
  mBrowseSyncDirectory->setObjectName(
      QStringLiteral("LocalWorkspaceBrowseSyncDirectory"));

  QHBoxLayout *syncDirectory = new QHBoxLayout;
  syncDirectory->addWidget(mSyncDirectory, 1);
  syncDirectory->addWidget(mBrowseSyncDirectory);

  QDialogButtonBox *buttons = new QDialogButtonBox(this);
  buttons->addButton(QDialogButtonBox::Cancel);
  mSave = buttons->addButton(editing ? tr("Save") : tr("Create"),
                             QDialogButtonBox::AcceptRole);
  mSave->setObjectName(QStringLiteral("LocalWorkspaceSave"));
  mSave->setDefault(true);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(new QLabel(tr("Repositories:"), this));
  layout->addWidget(mRepositories, 1);
  layout->addLayout(repositoryActions);
  layout->addWidget(mSync);
  layout->addLayout(syncDirectory);
  layout->addWidget(buttons);

  connect(mName, &QLineEdit::textChanged, this,
          &LocalWorkspaceDialog::updateState);
  connect(mColor, &QPushButton::clicked, this,
          &LocalWorkspaceDialog::chooseColor);
  connect(browseRepositories, &QPushButton::clicked, this,
          &LocalWorkspaceDialog::browseRepository);
  connect(mRemoveRepository, &QPushButton::clicked, this, [this] {
    delete mRepositories->takeItem(mRepositories->currentRow());
    updateState();
  });
  connect(mRepositories, &QListWidget::itemSelectionChanged, this, [this] {
    updateState();
  });
  connect(mSync, &QCheckBox::toggled, this,
          &LocalWorkspaceDialog::updateState);
  connect(mBrowseSyncDirectory, &QPushButton::clicked, this,
          &LocalWorkspaceDialog::browseSyncDirectory);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  updateState();
}

LocalWorkspace LocalWorkspaceDialog::workspace() const {
  LocalWorkspace result = mWorkspace;
  result.name = mName->text().trimmed();
  result.iconName = mIcon->currentData().toString();
  result.color = mWorkspace.color;
  result.description = mDescription->toPlainText().trimmed();
  result.repositories.clear();
  result.manualRepositories.clear();
  for (int i = 0; i < mRepositories->count(); ++i) {
    result.repositories.append(mRepositories->item(i)->text());
    if (mRepositories->item(i)->data(Qt::UserRole + 1).toBool())
      result.manualRepositories.append(mRepositories->item(i)->text());
  }
  result.syncDirectory = mSyncDirectory->text();
  result.syncEnabled = mSync->isChecked();
  return result;
}

void LocalWorkspaceDialog::browseRepository() {
  const QStringList paths = DirectorySelectionDialog::getExistingDirectories(
      this, tr("Select Git Repositories"));
  if (paths.isEmpty())
    return;

  QStringList invalid;
  QStringList duplicates;
  for (const QString &path : paths) {
    const git::Repository repository = git::Repository::open(path, true);
    if (!repository.isValid()) {
      invalid.append(path);
      continue;
    }

    const QString root = repository.dir(false).path();
    bool duplicate = false;
    for (int i = 0; i < mRepositories->count(); ++i) {
      if (pathsMatch(mRepositories->item(i)->text(), root)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      duplicates.append(root);
      continue;
    }

    QListWidgetItem *item = new QListWidgetItem(root, mRepositories);
    item->setData(Qt::UserRole, false);
    item->setData(Qt::UserRole + 1, true);
  }

  QStringList skipped;
  if (!invalid.isEmpty())
    skipped.append(tr("Not Git repositories:\n%1").arg(invalid.join('\n')));
  if (!duplicates.isEmpty())
    skipped.append(
        tr("Already in the workspace:\n%1").arg(duplicates.join('\n')));
  if (!skipped.isEmpty())
    QMessageBox::warning(
        this, tr("Some Folders Were Skipped"),
        tr("Some selected folders were skipped.\n\n%1")
            .arg(skipped.join(QStringLiteral("\n\n"))));
  updateState();
}

void LocalWorkspaceDialog::browseSyncDirectory() {
  const QString path = QFileDialog::getExistingDirectory(
      this, tr("Select Synchronized Directory"), mSyncDirectory->text(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (path.isEmpty())
    return;

  mSyncDirectory->setText(path);
  mSync->setChecked(true);
  updateState();
}

void LocalWorkspaceDialog::chooseColor() {
  const QColor color =
      QColorDialog::getColor(mWorkspace.color, this, tr("Workspace Color"));
  if (!color.isValid())
    return;

  mWorkspace.color = color;
  updateColorButton();
}

void LocalWorkspaceDialog::updateColorButton() {
  const QString color = mWorkspace.color.name();
  mColor->setText(color);
  mColor->setStyleSheet(QStringLiteral("background-color: %1;").arg(color));
}

void LocalWorkspaceDialog::updateState() {
  const bool synchronized = mSync->isChecked();
  mSyncDirectory->setEnabled(synchronized);
  mBrowseSyncDirectory->setEnabled(synchronized);
  const bool hasLocation =
      mRepositories->count() > 0 || !mSyncDirectory->text().isEmpty();
  mSave->setEnabled(!mName->text().trimmed().isEmpty() && hasLocation);
  QListWidgetItem *item = mRepositories->currentItem();
  mRemoveRepository->setEnabled(
      item && !item->data(Qt::UserRole).toBool());
}

//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "DirectorySelectionDialog.h"
#include "util/Path.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

DirectorySelectionDialog::DirectorySelectionDialog(const QString &title,
                                                   QWidget *parent)
    : QDialog(parent), mModel(new QFileSystemModel(this)),
      mLocation(new QLineEdit(this)),
      mTree(new QTreeView(this)) {
  setWindowTitle(title);
  resize(760, 520);

  mModel->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives |
                    QDir::Hidden);
  mModel->setRootPath(QString());

  mTree->setObjectName(QStringLiteral("DirectorySelectionTree"));
  mTree->setAccessibleName(tr("Directories"));
  mTree->setModel(mModel);
  mTree->setRootIndex({});
  mTree->setSelectionBehavior(QAbstractItemView::SelectRows);
  mTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  mTree->setSortingEnabled(true);
  mTree->sortByColumn(0, Qt::AscendingOrder);
  mTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int column = 1; column < mModel->columnCount(); ++column)
    mTree->hideColumn(column);

  const QModelIndex home = mModel->index(QDir::homePath());
  for (QModelIndex parentIndex = home.parent(); parentIndex.isValid();
       parentIndex = parentIndex.parent())
    mTree->expand(parentIndex);
  if (home.isValid())
    mTree->scrollTo(home, QAbstractItemView::PositionAtCenter);

  mLocation->setObjectName(QStringLiteral("DirectorySelectionLocation"));
  mLocation->setAccessibleName(tr("Directory location"));
  mLocation->setPlaceholderText(tr("Enter a directory path"));
  mLocation->setText(QDir::toNativeSeparators(QDir::homePath()));
  QPushButton *go = new QPushButton(tr("Go"), this);
  go->setObjectName(QStringLiteral("DirectorySelectionGo"));

  QHBoxLayout *locationLayout = new QHBoxLayout;
  locationLayout->addWidget(new QLabel(tr("Location:"), this));
  locationLayout->addWidget(mLocation, 1);
  locationLayout->addWidget(go);

  QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel,
                                                   Qt::Horizontal, this);
  mSelect = buttons->addButton(tr("Select"), QDialogButtonBox::AcceptRole);
  mSelect->setObjectName(QStringLiteral("DirectorySelectionAccept"));
  mSelect->setEnabled(false);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(
      new QLabel(tr("Select one or more folders. Use Ctrl/Command or Shift to "
                    "select multiple folders."),
                 this));
  layout->addLayout(locationLayout);
  layout->addWidget(mTree, 1);
  layout->addWidget(buttons);

  connect(mTree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          [this] {
            const QStringList selected = selectedDirectories();
            if (!selected.isEmpty())
              mLocation->setText(QDir::toNativeSeparators(selected.first()));
            updateSelectButton();
          });
  connect(mLocation, &QLineEdit::textChanged, this,
          &DirectorySelectionDialog::updateSelectButton);
  connect(mLocation, &QLineEdit::returnPressed, this,
          &DirectorySelectionDialog::navigateToLocation);
  connect(go, &QPushButton::clicked, this,
          &DirectorySelectionDialog::navigateToLocation);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QStringList DirectorySelectionDialog::selectedDirectories() const {
  QStringList paths;
  const QModelIndexList rows = mTree->selectionModel()->selectedRows(0);
  for (const QModelIndex &index : rows) {
    const QString path = QDir::cleanPath(mModel->filePath(index));
    if (!path.isEmpty() && !util::containsPath(paths, path))
      paths.append(path);
  }
  if (paths.isEmpty()) {
    const QFileInfo info(QDir::fromNativeSeparators(mLocation->text()));
    if (info.exists() && info.isDir()) {
      const QString path = QDir::cleanPath(info.absoluteFilePath());
      if (!util::containsPath(paths, path))
        paths.append(path);
    }
  }
  return paths;
}

void DirectorySelectionDialog::navigateToLocation() {
  const QString path = QDir::cleanPath(
      QDir::fromNativeSeparators(mLocation->text().trimmed()));
  const QFileInfo info(path);
  if (!info.exists() || !info.isDir()) {
    QMessageBox::warning(this, tr("Invalid Directory"),
                         tr("The entered path is not an existing directory:\n%1")
                             .arg(mLocation->text()));
    updateSelectButton();
    return;
  }

  const QModelIndex index = mModel->index(info.absoluteFilePath());
  if (index.isValid()) {
    for (QModelIndex parentIndex = index.parent(); parentIndex.isValid();
         parentIndex = parentIndex.parent())
      mTree->expand(parentIndex);
    mTree->scrollTo(index, QAbstractItemView::PositionAtCenter);
    mTree->selectionModel()->select(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  }
  updateSelectButton();
}

void DirectorySelectionDialog::updateSelectButton() {
  mSelect->setEnabled(!selectedDirectories().isEmpty());
}

QStringList DirectorySelectionDialog::getExistingDirectories(
    QWidget *parent, const QString &title) {
  DirectorySelectionDialog dialog(title, parent);
  return dialog.exec() == QDialog::Accepted ? dialog.selectedDirectories()
                                             : QStringList();
}

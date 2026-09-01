//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "DirectorySelectionDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

DirectorySelectionDialog::DirectorySelectionDialog(const QString &title,
                                                   QWidget *parent)
    : QDialog(parent), mModel(new QFileSystemModel(this)),
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
  layout->addWidget(mTree, 1);
  layout->addWidget(buttons);

  connect(mTree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          [this] { mSelect->setEnabled(!selectedDirectories().isEmpty()); });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QStringList DirectorySelectionDialog::selectedDirectories() const {
  QStringList paths;
  const QModelIndexList rows = mTree->selectionModel()->selectedRows(0);
  for (const QModelIndex &index : rows) {
    const QString path = QDir::cleanPath(mModel->filePath(index));
    if (!path.isEmpty() && !paths.contains(path))
      paths.append(path);
  }
  return paths;
}

QStringList DirectorySelectionDialog::getExistingDirectories(
    QWidget *parent, const QString &title) {
  DirectorySelectionDialog dialog(title, parent);
  return dialog.exec() == QDialog::Accepted ? dialog.selectedDirectories()
                                             : QStringList();
}

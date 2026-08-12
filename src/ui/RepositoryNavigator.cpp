//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryNavigator.h"
#include "RepositoryNavigatorModel.h"
#include "RepoView.h"
#include <QMetaEnum>
#include <QSettings>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

const QString kExpandedGroup = "sidebar/repositoryNavigator/expanded";

QString sectionKey(const QModelIndex &index) {
  int section = index.data(RepositoryNavigatorModel::SectionRole).toInt();
  QMetaEnum meta = QMetaEnum::fromType<RepositoryNavigatorModel::Section>();
  return QString::fromLatin1(meta.valueToKey(section));
}

} // namespace

RepositoryNavigator::RepositoryNavigator(QWidget *parent) : QWidget(parent) {
  setObjectName("RepositoryNavigator");
  setAccessibleName(tr("Repository navigation"));

  mView = new QTreeView(this);
  mView->setObjectName("RepositoryNavigationTree");
  mView->setAccessibleName(tr("Repository references"));
  mView->setHeaderHidden(true);
  mView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  mView->setRootIsDecorated(true);
  mView->setItemsExpandable(true);
  mView->setExpandsOnDoubleClick(false);

  mModel = new RepositoryNavigatorModel(mView);
  mView->setModel(mModel);

  connect(mModel, &RepositoryNavigatorModel::modelReset, this,
          &RepositoryNavigator::restoreExpansion);
  connect(mView, &QTreeView::expanded, this,
          [this](const QModelIndex &index) { storeExpansion(index, true); });
  connect(mView, &QTreeView::collapsed, this,
          [this](const QModelIndex &index) { storeExpansion(index, false); });

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(mView);

  restoreExpansion();
}

void RepositoryNavigator::setRepository(const git::Repository &repo) {
  mModel->setRepository(repo);
}

void RepositoryNavigator::setRepoView(RepoView *view) {
  disconnect(mSubmodulesConnection);
  setRepository(view ? view->repo() : git::Repository());
  if (view) {
    mSubmodulesConnection =
        connect(view, &RepoView::submodulesChanged, mModel,
                &RepositoryNavigatorModel::refresh);
  }
}

RepositoryNavigatorModel *RepositoryNavigator::model() const { return mModel; }

QTreeView *RepositoryNavigator::view() const { return mView; }

void RepositoryNavigator::restoreExpansion() {
  QSettings settings;
  settings.beginGroup(kExpandedGroup);
  for (int row = 0; row < mModel->rowCount(); ++row) {
    QModelIndex index = mModel->index(row, 0);
    bool defaultExpanded =
        index.data(RepositoryNavigatorModel::AvailableRole).toBool();
    mView->setExpanded(
        index, settings.value(sectionKey(index), defaultExpanded).toBool());
  }
  settings.endGroup();
}

void RepositoryNavigator::storeExpansion(const QModelIndex &index,
                                         bool expanded) {
  if (!index.isValid() || index.parent().isValid())
    return;
  QSettings settings;
  settings.beginGroup(kExpandedGroup);
  settings.setValue(sectionKey(index), expanded);
  settings.endGroup();
}

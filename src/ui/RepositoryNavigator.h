//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REPOSITORYNAVIGATOR_H
#define REPOSITORYNAVIGATOR_H

#include "git/Repository.h"
#include <QWidget>

class QTreeView;
class RepositoryNavigatorModel;
class RepoView;

class RepositoryNavigator : public QWidget {
  Q_OBJECT

public:
  explicit RepositoryNavigator(QWidget *parent = nullptr);

  void setRepository(const git::Repository &repo);
  void setRepoView(RepoView *view);
  RepositoryNavigatorModel *model() const;
  QTreeView *view() const;

private:
  void restoreExpansion();
  void storeExpansion(const QModelIndex &index, bool expanded);

  QTreeView *mView;
  RepositoryNavigatorModel *mModel;
  QMetaObject::Connection mSubmodulesConnection;
};

#endif

//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Bryan Williams
//

#include "RemoteTableModel.h"
#include "git/Remote.h"

RemoteTableModel::RemoteTableModel(QObject *parent, const git::Repository &repo)
    : QAbstractTableModel(parent), mRepo(repo) {
  git::RepositoryNotifier *notifier = repo.notifier();
  connect(notifier, &git::RepositoryNotifier::remoteAboutToBeAdded, this,
          &RemoteTableModel::beginResetModel);
  connect(notifier, &git::RepositoryNotifier::remoteAdded, this,
          &RemoteTableModel::endResetModel);
  connect(notifier, &git::RepositoryNotifier::remoteAboutToBeRemoved, this,
          &RemoteTableModel::beginResetModel);
  connect(notifier, &git::RepositoryNotifier::remoteRemoved, this,
          &RemoteTableModel::endResetModel);
  connect(notifier, &git::RepositoryNotifier::remoteUpdated, this, [this] {
    beginResetModel();
    endResetModel();
  });
}

int RemoteTableModel::rowCount(const QModelIndex &parent) const {
  return mRepo.remotes().count();
}

int RemoteTableModel::columnCount(const QModelIndex &parent) const { return 3; }

QVariant RemoteTableModel::data(const QModelIndex &index, int role) const {
  git::Remote remote = mRepo.remotes().at(index.row());
  switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
      switch (index.column()) {
        case Name:
          return remote.name();
        case FetchUrl:
          return remote.url();
        case PushUrl:
          return remote.pushUrl().isEmpty() ? tr("Same as fetch")
                                            : remote.pushUrl();
        default:
          return QVariant();
      }
  }

  return QVariant();
}

QVariant RemoteTableModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return QVariant();

  switch (section) {
    case Name:
      return tr("Name");
    case FetchUrl:
      return tr("Fetch URL");
    case PushUrl:
      return tr("Push URL");
    default:
      return QVariant();
  }
}

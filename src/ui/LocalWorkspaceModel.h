//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef LOCALWORKSPACEMODEL_H
#define LOCALWORKSPACEMODEL_H

#include "conf/LocalWorkspace.h"
#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>

class LocalWorkspaces;

class LocalWorkspaceModel : public QAbstractItemModel {
  Q_OBJECT

public:
  enum ItemKind { WorkspaceItem, RepositoryItem };
  Q_ENUM(ItemKind)

  enum Column {
    RepositoryColumn,
    BranchColumn,
    RemoteColumn,
    ChangesColumn,
    DetailsColumn,
    RemoveColumn,
    ColumnCount
  };
  Q_ENUM(Column)

  enum Role {
    ItemKindRole = Qt::UserRole + 1,
    WorkspaceIdRole,
    PathRole,
    AvailableRole,
    ColorRole,
    SynchronizedRole,
    UpstreamRole,
    AheadRole,
    BehindRole,
    TrackingReadyRole,
    ModifiedRole,
    AddedRole,
    RemovedRole,
    UntrackedRole,
    ConflictedRole,
    StatusReadyRole,
    StatusErrorRole,
    OriginFetchActiveRole
  };

  explicit LocalWorkspaceModel(QObject *parent = nullptr);
  ~LocalWorkspaceModel() override;

  QModelIndex index(int row, int column,
                    const QModelIndex &parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex &index) const override;
  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index,
                 int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;
  QHash<int, QByteArray> roleNames() const override;
  QStringList repositoryPaths() const;
  void setOriginFetchActive(const QString &path, bool active);
  void refreshRepositories();

private:
  struct RepositoryState {
    bool available = false;
    QString branch;
    QString upstream;
    int ahead = 0;
    int behind = 0;
    bool trackingReady = false;
    int modified = 0;
    int added = 0;
    int removed = 0;
    int untracked = 0;
    int conflicted = 0;
    bool statusReady = false;
    bool statusError = false;
  };

  bool statesEqual(const RepositoryState &lhs,
                   const RepositoryState &rhs) const;
  void applyRepositoryStates(const QHash<QString, RepositoryState> &states);
  RepositoryState repositoryState(const QString &path) const;
  void reload();

  LocalWorkspaces *mWorkspaces;
  QList<LocalWorkspace> mSnapshot;
  QHash<QString, RepositoryState> mRepositoryStates;
  QSet<QString> mActiveOriginFetches;
  QFutureWatcher<QHash<QString, RepositoryState>> *mRefreshWatcher;
  bool mRefreshPending = false;
};

#endif

//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "LocalWorkspaceModel.h"

#include "conf/LocalWorkspace.h"
#include "conf/LocalWorkspaces.h"
#include "git/Branch.h"
#include "git/Diff.h"
#include "git/Index.h"
#include "git/Reference.h"
#include "git/Remote.h"
#include "git/Repository.h"
#include "git/Result.h"

#include <QFileInfo>
#include <QIcon>
#include <QSet>
#include <QTimer>
#include <QtConcurrent>

namespace {

QString commitCount(int count) {
  return count == 1 ? LocalWorkspaceModel::tr("1 commit")
                    : LocalWorkspaceModel::tr("%1 commits").arg(count);
}

} // namespace

LocalWorkspaceModel::LocalWorkspaceModel(QObject *parent)
    : QAbstractItemModel(parent), mWorkspaces(LocalWorkspaces::instance()),
      mRefreshWatcher(
          new QFutureWatcher<QHash<QString, RepositoryState>>(this)) {
  reload();
  connect(mRefreshWatcher,
          &QFutureWatcher<QHash<QString, RepositoryState>>::finished, this,
          [this] {
            if (mRefreshWatcher->future().resultCount())
              applyRepositoryStates(mRefreshWatcher->result());
            if (mRefreshPending) {
              mRefreshPending = false;
              QTimer::singleShot(0, this,
                                 &LocalWorkspaceModel::refreshRepositories);
            }
          });
  connect(mWorkspaces, &LocalWorkspaces::workspacesChanged, this, [this] {
    beginResetModel();
    reload();
    endResetModel();
    refreshRepositories();
  });
  refreshRepositories();
}

LocalWorkspaceModel::~LocalWorkspaceModel() {
  if (mRefreshWatcher->isRunning())
    mRefreshWatcher->waitForFinished();
}

QModelIndex LocalWorkspaceModel::index(int row, int column,
                                        const QModelIndex &parent) const {
  if (row < 0 || column < 0 || column >= ColumnCount)
    return {};

  if (!parent.isValid()) {
    if (row >= mSnapshot.size())
      return {};
    return createIndex(row, column, quintptr(0));
  }

  if (parent.column() != 0 || parent.internalId() != 0)
    return {};
  if (parent.row() < 0 || parent.row() >= mSnapshot.size() ||
      row >= mSnapshot.at(parent.row()).repositories.size())
    return {};
  return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex LocalWorkspaceModel::parent(const QModelIndex &index) const {
  if (!index.isValid() || index.internalId() == 0)
    return {};

  const int workspaceRow = int(index.internalId()) - 1;
  if (workspaceRow < 0 || workspaceRow >= mSnapshot.size())
    return {};
  return createIndex(workspaceRow, 0, quintptr(0));
}

int LocalWorkspaceModel::rowCount(const QModelIndex &parent) const {
  if (!parent.isValid())
    return mSnapshot.size();
  if (parent.column() != 0 || parent.internalId() != 0)
    return 0;

  return parent.row() >= 0 && parent.row() < mSnapshot.size()
             ? mSnapshot.at(parent.row()).repositories.size()
             : 0;
}

int LocalWorkspaceModel::columnCount(const QModelIndex &) const {
  return ColumnCount;
}

QVariant LocalWorkspaceModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid())
    return {};

  const bool repositoryItem = index.internalId() != 0;
  const int workspaceRow =
      repositoryItem ? int(index.internalId()) - 1 : index.row();
  if (workspaceRow < 0 || workspaceRow >= mSnapshot.size())
    return {};
  const LocalWorkspace *workspace = &mSnapshot.at(workspaceRow);

  if (!repositoryItem) {
    if (index.column() != RepositoryColumn)
      return {};
    switch (role) {
    case Qt::DisplayRole: {
      const QString name =
          tr("%1 (%2)").arg(workspace->name).arg(workspace->repositories.size());
      if (workspace->syncDirectory.isEmpty())
        return name;
      return workspace->syncEnabled ? tr("%1  Local - synced").arg(name)
                                    : tr("%1  Local - sync paused").arg(name);
    }
    case Qt::ToolTipRole:
      return workspace->description;
    case Qt::DecorationRole:
      return QIcon::fromTheme(workspace->iconName);
    case Qt::ForegroundRole:
    case ColorRole:
      return workspace->color.isValid() ? QVariant(workspace->color) : QVariant();
    case ItemKindRole:
      return WorkspaceItem;
    case WorkspaceIdRole:
      return workspace->id;
    case AvailableRole:
      return true;
    default:
      return {};
    }
  }

  if (index.row() < 0 || index.row() >= workspace->repositories.size())
    return {};
  const QString path = workspace->repositories.at(index.row());
  const bool synchronized =
      workspace->synchronizedRepositories.contains(path);
  const RepositoryState state = mRepositoryStates.value(path);

  switch (role) {
  case PathRole:
    return path;
  case ItemKindRole:
    return RepositoryItem;
  case WorkspaceIdRole:
    return workspace->id;
  case AvailableRole:
    return state.available;
  case SynchronizedRole:
    return synchronized;
  case UpstreamRole:
    return state.upstream;
  case AheadRole:
    return state.trackingReady ? QVariant(state.ahead) : QVariant();
  case BehindRole:
    return state.trackingReady ? QVariant(state.behind) : QVariant();
  case TrackingReadyRole:
    return state.trackingReady;
  case ModifiedRole:
    return state.modified;
  case AddedRole:
    return state.added;
  case RemovedRole:
    return state.removed;
  case UntrackedRole:
    return state.untracked;
  case ConflictedRole:
    return state.conflicted;
  case StatusReadyRole:
    return state.statusReady;
  case StatusErrorRole:
    return state.statusError;
  case OriginFetchActiveRole:
    return mActiveOriginFetches.contains(path);
  case OriginCheckEligibleRole:
    return state.originCheckEligible;
  case OriginCheckFreshRole:
    return mFreshOriginChecks.contains(path);
  case OriginCheckFailedRole:
    return mFailedOriginChecks.contains(path);
  case OriginInitialPendingRole:
    return mInitialPendingOrigins.contains(path);
  case Qt::ForegroundRole:
    if (!state.available)
      return QColor(Qt::gray);
    return {};
  default:
    break;
  }

  switch (index.column()) {
  case RepositoryColumn:
    if (role == Qt::DisplayRole) {
      const QFileInfo info(path);
      return info.fileName().isEmpty() ? path : info.fileName();
    }
    if (role == Qt::ToolTipRole)
      return path;
    break;
  case BranchColumn:
    if (role == Qt::TextAlignmentRole)
      return int(Qt::AlignLeft | Qt::AlignVCenter);
    if (role == Qt::DecorationRole && state.available)
      return QIcon(QStringLiteral(":/branches.png"));
    if (role == Qt::DisplayRole && state.available)
      return state.branch;
    if (role == Qt::ToolTipRole)
      return state.available ? tr("Checked out branch")
                             : tr("Repository unavailable");
    break;
  case RemoteColumn:
    if (role == Qt::TextAlignmentRole)
      return int(Qt::AlignLeft | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) {
      if (mActiveOriginFetches.contains(path))
        return tr("Synchronization is running.");
      if (mInitialPendingOrigins.contains(path))
        return tr("Waiting for origin check.");
      if (mFailedOriginChecks.contains(path))
        return tr("The last origin check failed.");
      if (!state.available)
        return tr("Repository unavailable.");
      if (!state.trackingReady)
        return tr("No upstream branch is configured.");
      if (!state.originCheckEligible)
        return tr("The configured upstream cannot be checked through origin.");
      if (!mFreshOriginChecks.contains(path))
        return tr("Waiting for origin check.");
      if (!state.ahead && !state.behind)
        return tr("The local branch matches %1.").arg(state.upstream);
      if (state.ahead && state.behind) {
        return tr("The local branch and %1 have diverged: %2 ahead and %3 "
                  "behind.")
            .arg(state.upstream, commitCount(state.ahead),
                 commitCount(state.behind));
      }
      if (state.ahead) {
        return tr("The local branch is %1 ahead of %2.")
            .arg(commitCount(state.ahead), state.upstream);
      }
      return tr("The local branch is %1 behind %2.")
          .arg(commitCount(state.behind), state.upstream);
    }
    break;
  case ChangesColumn:
    if (role == Qt::TextAlignmentRole)
      return int(Qt::AlignLeft | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) {
      if (!state.available)
        return tr("Repository unavailable.");
      if (!state.statusReady)
        return state.statusError ? tr("Unable to read working-tree status.")
                                 : tr("Reading working-tree status.");
      if (!state.modified && !state.added && !state.removed &&
          !state.untracked && !state.conflicted)
        return tr("No uncommitted changes.");
    }
    break;
  case DetailsColumn:
    if (role == Qt::ToolTipRole)
      return tr("show details");
    if (role == Qt::TextAlignmentRole)
      return Qt::AlignCenter;
    break;
  case RemoveColumn:
    if (role == Qt::ToolTipRole) {
      return synchronized
                 ? tr("Synchronized repositories cannot be removed")
                 : tr("Remove repository from workspace");
    }
    if (role == Qt::TextAlignmentRole)
      return Qt::AlignCenter;
    break;
  default:
    break;
  }
  return {};
}

Qt::ItemFlags LocalWorkspaceModel::flags(const QModelIndex &index) const {
  if (!index.isValid())
    return Qt::NoItemFlags;

  Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  if (index.internalId() != 0) {
    if (index.column() == DetailsColumn &&
        !index.data(AvailableRole).toBool())
      flags &= ~Qt::ItemIsEnabled;
    if (index.column() == RemoveColumn &&
        index.data(SynchronizedRole).toBool())
      flags &= ~Qt::ItemIsEnabled;
  }
  return flags;
}

QVariant LocalWorkspaceModel::headerData(int section,
                                         Qt::Orientation orientation,
                                         int role) const {
  if (orientation != Qt::Horizontal)
    return {};
  if (role == Qt::TextAlignmentRole)
    return int(Qt::AlignLeft | Qt::AlignVCenter);
  if (role != Qt::DisplayRole)
    return {};

  switch (section) {
  case RepositoryColumn:
    return tr("Repository");
  case BranchColumn:
    return tr("Branch");
  case RemoteColumn:
    return tr("Remote");
  case ChangesColumn:
    return tr("Changes");
  default:
    return {};
  }
}

QHash<int, QByteArray> LocalWorkspaceModel::roleNames() const {
  QHash<int, QByteArray> roles = QAbstractItemModel::roleNames();
  roles.insert(ItemKindRole, "itemKind");
  roles.insert(WorkspaceIdRole, "workspaceId");
  roles.insert(PathRole, "path");
  roles.insert(AvailableRole, "available");
  roles.insert(ColorRole, "color");
  roles.insert(SynchronizedRole, "synchronized");
  roles.insert(UpstreamRole, "upstream");
  roles.insert(AheadRole, "ahead");
  roles.insert(BehindRole, "behind");
  roles.insert(TrackingReadyRole, "trackingReady");
  roles.insert(ModifiedRole, "modified");
  roles.insert(AddedRole, "added");
  roles.insert(RemovedRole, "removed");
  roles.insert(UntrackedRole, "untracked");
  roles.insert(ConflictedRole, "conflicted");
  roles.insert(StatusReadyRole, "statusReady");
  roles.insert(StatusErrorRole, "statusError");
  roles.insert(OriginFetchActiveRole, "originFetchActive");
  roles.insert(OriginCheckEligibleRole, "originCheckEligible");
  roles.insert(OriginCheckFreshRole, "originCheckFresh");
  roles.insert(OriginCheckFailedRole, "originCheckFailed");
  roles.insert(OriginInitialPendingRole, "originInitialPending");
  return roles;
}

QStringList LocalWorkspaceModel::repositoryPaths() const {
  QStringList paths;
  for (const LocalWorkspace &workspace : mSnapshot) {
    for (const QString &path : workspace.repositories) {
      if (!paths.contains(path))
        paths.append(path);
    }
  }
  return paths;
}

bool LocalWorkspaceModel::isOriginCheckFresh(const QString &path) const {
  return mFreshOriginChecks.contains(path);
}

bool LocalWorkspaceModel::isOriginCheckEligible(const QString &path) const {
  return mRepositoryStates.value(path).originCheckEligible;
}

void LocalWorkspaceModel::setOriginFetchActive(const QString &path,
                                               bool active) {
  if (active == mActiveOriginFetches.contains(path))
    return;
  if (active)
    mActiveOriginFetches.insert(path);
  else
    mActiveOriginFetches.remove(path);

  for (int workspaceRow = 0; workspaceRow < mSnapshot.size(); ++workspaceRow) {
    const QModelIndex workspaceIndex = index(workspaceRow, 0);
    const QStringList &repositories = mSnapshot.at(workspaceRow).repositories;
    for (int repositoryRow = 0; repositoryRow < repositories.size();
         ++repositoryRow) {
      if (repositories.at(repositoryRow) != path)
        continue;
      const QModelIndex remote =
          index(repositoryRow, RemoteColumn, workspaceIndex);
      emit dataChanged(remote, remote,
                       {OriginFetchActiveRole, Qt::ToolTipRole});
    }
  }
}

void LocalWorkspaceModel::setOriginCheckFresh(const QString &path, bool fresh) {
  setPathState(mFreshOriginChecks, path, fresh, OriginCheckFreshRole);
}

void LocalWorkspaceModel::setOriginCheckFailed(const QString &path,
                                               bool failed) {
  setPathState(mFailedOriginChecks, path, failed, OriginCheckFailedRole);
}

void LocalWorkspaceModel::setOriginInitialPending(const QString &path,
                                                  bool pending) {
  setPathState(mInitialPendingOrigins, path, pending,
               OriginInitialPendingRole);
}

void LocalWorkspaceModel::setPathState(QSet<QString> &paths,
                                       const QString &path, bool enabled,
                                       int role) {
  if (enabled == paths.contains(path))
    return;
  if (enabled)
    paths.insert(path);
  else
    paths.remove(path);

  for (int workspaceRow = 0; workspaceRow < mSnapshot.size(); ++workspaceRow) {
    const QModelIndex workspaceIndex = index(workspaceRow, 0);
    const QStringList &repositories = mSnapshot.at(workspaceRow).repositories;
    for (int repositoryRow = 0; repositoryRow < repositories.size();
         ++repositoryRow) {
      if (repositories.at(repositoryRow) != path)
        continue;
      const QModelIndex remote =
          index(repositoryRow, RemoteColumn, workspaceIndex);
      emit dataChanged(remote, remote, {role, Qt::ToolTipRole});
    }
  }
}

void LocalWorkspaceModel::refreshRepositories() {
  if (mRefreshWatcher->isRunning()) {
    mRefreshPending = true;
    return;
  }

  QStringList paths;
  QSet<QString> uniquePaths;
  for (const LocalWorkspace &workspace : std::as_const(mSnapshot)) {
    for (const QString &path : workspace.repositories) {
      if (uniquePaths.contains(path))
        continue;
      uniquePaths.insert(path);
      paths.append(path);
    }
  }

  mRefreshWatcher->setFuture(QtConcurrent::run([paths] {
    QHash<QString, RepositoryState> states;
    for (const QString &path : paths) {
      RepositoryState state;
      const git::Repository repository = git::Repository::open(path, true);
      state.available = repository.isValid();
      if (!state.available) {
        states.insert(path, state);
        continue;
      }

      const git::Reference head = repository.head();
      state.branch =
          head.isValid() ? head.name() : repository.unbornHeadName();
      if (head.isValid() && head.isLocalBranch()) {
        const git::Branch upstream = git::Branch(head).upstream();
        if (upstream.isValid()) {
          state.upstream = upstream.name();
          state.ahead = head.difference(upstream);
          state.behind = upstream.difference(head);
          state.trackingReady = true;
          state.originCheckEligible =
              state.upstream.section('/', 0, 0) == QStringLiteral("origin") &&
              repository.lookupRemote(QStringLiteral("origin")).isValid();
        }
      }

      git::Result result(0);
      const QList<unsigned int> statuses = repository.statusFlags(&result);
      state.statusReady = bool(result);
      state.statusError = !result;
      if (result) {
        for (unsigned int status : statuses) {
          if (status & GIT_STATUS_CONFLICTED) {
            ++state.conflicted;
            continue;
          }
          if (status & (GIT_STATUS_INDEX_RENAMED | GIT_STATUS_WT_RENAMED)) {
            ++state.added;
            ++state.removed;
            continue;
          }
          if (status & GIT_STATUS_INDEX_NEW)
            ++state.added;
          if (status & GIT_STATUS_WT_NEW)
            ++state.untracked;
          if (status & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED))
            ++state.removed;
          if (status & (GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_WT_MODIFIED |
                        GIT_STATUS_INDEX_TYPECHANGE |
                        GIT_STATUS_WT_TYPECHANGE))
            ++state.modified;
        }
      }
      states.insert(path, state);
    }
    return states;
  }));
}

bool LocalWorkspaceModel::statesEqual(const RepositoryState &lhs,
                                      const RepositoryState &rhs) const {
  return lhs.available == rhs.available && lhs.branch == rhs.branch &&
         lhs.upstream == rhs.upstream && lhs.ahead == rhs.ahead &&
         lhs.behind == rhs.behind &&
         lhs.trackingReady == rhs.trackingReady &&
         lhs.originCheckEligible == rhs.originCheckEligible &&
         lhs.modified == rhs.modified && lhs.added == rhs.added &&
         lhs.removed == rhs.removed && lhs.untracked == rhs.untracked &&
         lhs.conflicted == rhs.conflicted &&
         lhs.statusReady == rhs.statusReady &&
         lhs.statusError == rhs.statusError;
}

void LocalWorkspaceModel::applyRepositoryStates(
    const QHash<QString, RepositoryState> &states) {
  QSet<QString> changed;
  for (auto it = states.cbegin(); it != states.cend(); ++it) {
    if (!mRepositoryStates.contains(it.key()))
      continue;
    if (!statesEqual(mRepositoryStates.value(it.key()), it.value())) {
      mRepositoryStates.insert(it.key(), it.value());
      changed.insert(it.key());
    }
  }

  if (changed.isEmpty())
    return;
  for (int workspaceRow = 0; workspaceRow < mSnapshot.size(); ++workspaceRow) {
    const QModelIndex workspaceIndex = index(workspaceRow, RepositoryColumn);
    const int repositories = rowCount(workspaceIndex);
    for (int repositoryRow = 0; repositoryRow < repositories; ++repositoryRow) {
      const QString path =
          mSnapshot.at(workspaceRow).repositories.at(repositoryRow);
      if (!changed.contains(path))
        continue;
      emit dataChanged(index(repositoryRow, RepositoryColumn, workspaceIndex),
                       index(repositoryRow, RemoveColumn, workspaceIndex));
    }
  }
}

LocalWorkspaceModel::RepositoryState
LocalWorkspaceModel::repositoryState(const QString &path) const {
  RepositoryState state;
  const git::Repository repository = git::Repository::open(path, true);
  state.available = repository.isValid();
  if (state.available) {
    const git::Reference head = repository.head();
    state.branch =
        head.isValid() ? head.name() : repository.unbornHeadName();
  }
  return state;
}

void LocalWorkspaceModel::reload() {
  mSnapshot.clear();
  mRepositoryStates.clear();
  QSet<QString> currentPaths;
  for (int i = 0; i < mWorkspaces->count(); ++i) {
    const LocalWorkspace workspace = *mWorkspaces->workspace(i);
    mSnapshot.append(workspace);
    for (const QString &path : workspace.repositories) {
      currentPaths.insert(path);
      mRepositoryStates.insert(path, repositoryState(path));
    }
  }
  mActiveOriginFetches.intersect(currentPaths);
  mFreshOriginChecks.intersect(currentPaths);
  mFailedOriginChecks.intersect(currentPaths);
  mInitialPendingOrigins.intersect(currentPaths);
}

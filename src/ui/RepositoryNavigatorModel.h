//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REPOSITORYNAVIGATORMODEL_H
#define REPOSITORYNAVIGATORMODEL_H

#include "git/Commit.h"
#include "git/Reference.h"
#include "git/Repository.h"
#include "git/Submodule.h"
#include "host/GitHub.h"
#include <QAbstractItemModel>
#include <QHash>
#include <QTimer>

class RepositoryNavigatorModel : public QAbstractItemModel {
  Q_OBJECT

public:
  enum class Section {
    Local,
    Remote,
    Worktrees,
    Stashes,
    CloudPatches,
    PullRequests,
    GitHubIssues,
    Teams,
    Tags,
    Submodules,
    Count
  };
  Q_ENUM(Section)

  enum class ItemKind {
    Section,
    Reference,
    Worktree,
    Stash,
    Submodule,
    GitHubIssuesFilter,
    GitHubIssue,
    Status
  };
  Q_ENUM(ItemKind)

  enum class LoadState { Unavailable, Loading, Ready, Refreshing, Failed, Stale };
  Q_ENUM(LoadState)

  enum class OriginState { Hidden, Pending, Failed, Ready };
  Q_ENUM(OriginState)

  enum Role {
    SectionRole = Qt::UserRole + 1,
    ItemKindRole,
    CountRole,
    AvailableRole,
    CurrentRole,
    MainWorktreeRole,
    WorktreeRole,
    AheadRole,
    BehindRole,
    ReferenceRole,
    CommitRole,
    StashIndexRole,
    SubmoduleRole,
    PathRole,
    UrlRole,
    BranchRole,
    InitializedRole,
    PinnedAheadRole,
    PinnedBehindRole,
    OriginAheadRole,
    OriginBehindRole,
    OriginStateRole,
    OriginTargetRole,
    SubmoduleBusyRole,
    LoadStateRole,
    IssueNumberRole,
    IssueAuthorRole
  };

  explicit RepositoryNavigatorModel(QObject *parent = nullptr);

  void setRepository(const git::Repository &repo);
  void clear();
  git::Repository repository() const;
  void setSubmoduleUpdateStatuses(
      const QList<git::Submodule::UpdateStatus> &statuses);
  void setBusySubmodulePaths(const QStringList &paths);
  void setGitHubIssuesAvailable(bool available);
  void setGitHubIssuesFilter(const QString &filter);
  void beginGitHubIssuesLoad(bool refresh);
  void setGitHubIssues(const GitHub::Issues &issues);
  void failGitHubIssues(const QString &message, bool preserveIssues);

  QModelIndex sectionIndex(Section section) const;

  QModelIndex index(int row, int column,
                    const QModelIndex &parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex &index) const override;
  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  int columnCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex &index) const override;

public slots:
  void refresh();

private:
  struct Row {
    ItemKind kind = ItemKind::Reference;
    git::Reference reference;
    git::Commit commit;
    git::Submodule submodule;
    git::Worktree worktree;
    int stashIndex = -1;
    QString display;
    QString tooltip;
    QString path;
    QString url;
    QString branch;
    bool initialized = false;
    bool available = true;
    bool current = false;
    bool mainWorktree = false;
    int ahead = -1;
    int behind = -1;
    int pinnedAhead = -1;
    int pinnedBehind = -1;
    int originAhead = -1;
    int originBehind = -1;
    OriginState originState = OriginState::Hidden;
    git::Id originTarget;
    int issueNumber = 0;
    QString author;
  };

  struct SectionData {
    Section section;
    QString display;
    QString tooltip;
    bool available;
    QList<Row> rows;
  };

  static bool lessThan(const Row &lhs, const Row &rhs);
  bool isSection(const QModelIndex &index) const;
  bool isItem(const QModelIndex &index) const;
  const SectionData *sectionData(const QModelIndex &index) const;
  const Row *rowData(const QModelIndex &index) const;
  void disconnectRepository();
  void connectRepository();
  void rebuild();
  void rebuildGitHubIssuesSection();

  git::Repository mRepo;
  QHash<QString, git::Submodule::UpdateStatus> mSubmoduleUpdateStatuses;
  QStringList mBusySubmodulePaths;
  QList<SectionData> mSections;
  QList<QMetaObject::Connection> mConnections;
  QTimer mRefreshTimer;
  bool mGitHubIssuesAvailable = false;
  LoadState mGitHubIssuesState = LoadState::Unavailable;
  GitHub::Issues mGitHubIssues;
  QString mGitHubIssuesError;
  QString mGitHubIssuesFilter;
};

#endif

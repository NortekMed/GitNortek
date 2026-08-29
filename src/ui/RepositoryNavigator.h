//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REPOSITORYNAVIGATOR_H
#define REPOSITORYNAVIGATOR_H

#include "git/Reference.h"
#include "git/Repository.h"
#include "host/GitHub.h"
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QToolButton;
class QTreeView;
class RepositoryNavigatorModel;
class RepoView;

class RepositoryNavigator : public QWidget {
  Q_OBJECT

public:
  using IssuesRequest = std::function<void(
      GitHub *, const QString &, const QString &,
      const GitHub::IssuesCallback &)>;
  using Clock = std::function<qint64()>;

  explicit RepositoryNavigator(QWidget *parent = nullptr,
                               const IssuesRequest &request = IssuesRequest(),
                               const Clock &clock = Clock());

  void setRepository(const git::Repository &repo);
  void setRepoView(RepoView *view);
  RepositoryNavigatorModel *model() const;
  QTreeView *view() const;
  QTreeView *issuesView() const;
  QComboBox *issuesRemoteFilter() const;
  void setBodyFont(const QFont &font);

private:
  void restoreExpansion();
  void storeExpansion(const QModelIndex &index, bool expanded);
  void selectReference(const git::Reference &ref);
  void activate(const QModelIndex &index, bool checkout);
  void showContextMenu(const QPoint &point);
  void discoverGitHubIssuesRepositories();
  void selectGitHubIssuesRepository(int index);
  void requestGitHubIssues(bool force);
  void applyGitHubIssuesCache(const QString &key);
  void updateGitHubIssuesPanel();
  QString currentGitHubIssuesKey() const;
  void openIssue(const QModelIndex &index);
  void showIssuesRepositoryMenu(const QModelIndex &index);

  struct IssuesCandidate {
    QString remote;
    QString host;
    QString owner;
    QString repository;
    QString key;
    QPointer<GitHub> account;
  };

  struct IssuesCacheEntry {
    GitHub::Issues issues;
    QString error;
    qint64 lastSuccess = 0;
    qint64 lastAttempt = 0;
    qint64 retryAfter = 0;
    int generation = 0;
    bool hasValue = false;
    bool inFlight = false;
  };

  QTreeView *mView;
  QWidget *mIssuesPanel;
  QLabel *mIssuesTitle;
  QToolButton *mIssuesRefresh;
  QTreeView *mIssuesView;
  QComboBox *mIssuesRemoteFilter;
  RepositoryNavigatorModel *mModel;
  QPointer<RepoView> mRepoView;
  QMetaObject::Connection mSubmodulesConnection;
  QMetaObject::Connection mSubmoduleStatusesConnection;
  QMetaObject::Connection mReferenceConnection;
  QMetaObject::Connection mReferenceSelectedConnection;
  git::Reference mReferenceBeforeReset;
  QList<QMetaObject::Connection> mRemoteConnections;
  QList<IssuesCandidate> mIssuesCandidates;
  QHash<QString, IssuesCacheEntry> mIssuesCache;
  int mIssuesScrollBeforeReset = 0;
  IssuesRequest mIssuesRequest;
  Clock mClock;
  QPointer<GitHub> mAnonymousGitHub;
};

#endif

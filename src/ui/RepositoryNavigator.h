//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REPOSITORYNAVIGATOR_H
#define REPOSITORYNAVIGATOR_H

#include "RepositoryNavigatorModel.h"
#include "git/Reference.h"
#include "git/Repository.h"
#include "host/GitHub.h"
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QSplitter;
class QTreeView;
class QToolButton;
class RepoView;
class StatePushButton;

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
  QTreeView *sectionView(RepositoryNavigatorModel::Section section) const;
  QComboBox *issuesRemoteFilter() const;
  void setBodyFont(const QFont &font);
  void refresh();

signals:
  void openRepositoryRequested(const QString &path);

private:
  struct SectionPanel;

  void restoreExpansion();
  void storeExpansion(RepositoryNavigatorModel::Section section,
                      bool expanded);
  void setPanelExpanded(RepositoryNavigatorModel::Section section,
                        bool expanded);
  bool allAvailablePanelsExpanded() const;
  void toggleAllPanels();
  void updateExpandCollapseAllButton();
  void updatePanels();
  void promptToCreateWorktree();
  void clearOtherSelections(QTreeView *selected);
  SectionPanel *panel(RepositoryNavigatorModel::Section section);
  const SectionPanel *panel(RepositoryNavigatorModel::Section section) const;
  void selectReference(const git::Reference &ref);
  void activate(const QModelIndex &index, bool checkout);
  void showContextMenu(const QPoint &point);
  void discoverGitHubIssuesRepositories();
  void selectGitHubIssuesRepository(int index);
  void requestGitHubIssues(bool force);
  void applyGitHubIssuesCache(const QString &key);
  QString currentGitHubIssuesKey() const;
  void openIssue(const QModelIndex &index);

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

  struct SectionPanel {
    RepositoryNavigatorModel::Section section;
    QWidget *container;
    QWidget *header;
    QToolButton *toggle;
    QWidget *icon;
    QLabel *title;
    QToolButton *action;
    QWidget *body;
    QTreeView *view;
    int scrollBeforeReset = 0;
    int expandedSize = 96;
  };

  QSplitter *mSectionSplitter;
  StatePushButton *mExpandCollapseAllButton;
  QToolButton *mWorktreeAdd;
  QWidget *mCollapsedSpacer;
  QList<SectionPanel> mPanels;
  QComboBox *mIssuesRemoteFilter;
  RepositoryNavigatorModel *mModel;
  QPointer<RepoView> mRepoView;
  QMetaObject::Connection mSubmodulesConnection;
  QMetaObject::Connection mSubmoduleStatusesConnection;
  QMetaObject::Connection mReferenceConnection;
  QMetaObject::Connection mReferenceSelectedConnection;
  QMetaObject::Connection mRefreshConnection;
  git::Reference mReferenceBeforeReset;
  QList<QMetaObject::Connection> mRemoteConnections;
  QList<IssuesCandidate> mIssuesCandidates;
  QHash<QString, IssuesCacheEntry> mIssuesCache;
  IssuesRequest mIssuesRequest;
  Clock mClock;
  QPointer<GitHub> mAnonymousGitHub;
};

#endif

//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef LOCALREPOSITORYMANAGEMENT_H
#define LOCALREPOSITORYMANAGEMENT_H

#include <QDateTime>
#include <QFutureWatcher>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>

class LocalWorkspaceModel;
class LocalWorkspaces;
class RemoteCallbacks;
class QEvent;
class QLineEdit;
class QLabel;
class QModelIndex;
class QPoint;
class QPushButton;
class QSplitter;
class QSortFilterProxyModel;
class QTextBrowser;
class QTimer;
class QThreadPool;
class QTreeView;
class QWidget;

class LocalRepositoryManagement : public QWidget {
  Q_OBJECT

public:
  explicit LocalRepositoryManagement(QWidget *parent = nullptr);
  ~LocalRepositoryManagement() override;

  void checkOriginsIfStale();
  QString selectedRepositoryPath() const;

signals:
  void openRepositoryRequested(const QString &path);
  void openRepositoryDialogRequested();
  void openWorkspaceRequested(const QStringList &paths);
  void originCheckStarted(int repositoryCount);
  void originCheckFinished(int successful, int failed);
  void originFetchStarted(const QString &path);
  void originFetchFinished(const QString &path, bool successful);
  void selectedRepositoryChanged(const QString &path);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  struct OriginCheckRequest {
    QString path;
    qsizetype callbackIndex = -1;
  };

  struct OriginCheckEvent {
    QString path;
    qsizetype callbackIndex = -1;
    bool attempted = false;
    bool successful = false;
  };

  QModelIndex currentSourceIndex() const;
  void showContextMenu(const QPoint &position);
  void activate(const QModelIndex &index);
  void createWorkspace();
  void editWorkspace(const QString &id);
  void deleteWorkspace(const QString &id);
  void addRepository(const QString &id);
  void removeRepository(const QString &id, const QString &path);
  void openWorkspace(const QString &id);
  void rescanWorkspace(const QString &id);
  void deleteCurrentItem();
  void showDetails(const QString &path);
  void updateWorkspaceSpans();
  void updateSelectedRepository();
  void restoreWorkspaceExpansion();
  void clearPendingWorkspaceClick(bool restore = false);
  bool isWorkspaceDisclosure(const QModelIndex &index,
                             const QPoint &position) const;
  void updateOriginCheckStates();
  void toggleWorkspaceExpansion();
  void updateExpansionButton();
  void checkOrigins(bool force);
  void checkOrigin(const QString &path);
  void startOriginCheck(const QStringList &paths, bool force);
  void handleOriginCheckEvent(int index);
  void finishOriginCheck();
  void updateOriginCheckButton();
  void showError(const QString &error);

  LocalWorkspaces *mWorkspaces;
  LocalWorkspaceModel *mModel;
  QSortFilterProxyModel *mProxy;
  QLineEdit *mSearch;
  QPushButton *mExpansionToggle;
  QPushButton *mOriginCheck;
  QTreeView *mTree;
  QSplitter *mSplitter;
  QWidget *mDetailsPane;
  QLabel *mDetailsTitle;
  QTextBrowser *mReadme;
  QThreadPool *mOriginCheckPool = nullptr;
  QPointer<QFutureWatcher<OriginCheckEvent>> mOriginCheckWatcher;
  QTimer *mOriginCooldownTimer;
  QTimer *mOriginAnimationTimer;
  QTimer *mWorkspaceClickTimer;
  QList<RemoteCallbacks *> mOriginCallbacks;
  QSet<QString> mActiveOriginFetches;
  QSet<QString> mUntrustedOriginFetches;
  QSet<QString> mExpandedWorkspaceIds;
  QString mSelectedRepositoryPath;
  QPersistentModelIndex mPendingWorkspaceClick;
  QDateTime mOriginCooldownDeadline;
  bool mFirstOriginOpen = true;
  bool mResolveOriginAfterPaint = false;
  bool mOriginResolutionScheduled = false;
  bool mRestoringWorkspaceExpansion = false;
  bool mIgnoreNextWorkspaceRelease = false;
  bool mPendingWorkspaceWasExpanded = false;
  bool mDetailsInitialized = false;
};

#endif

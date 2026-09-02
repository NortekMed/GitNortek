//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef COMMITLIST_H
#define COMMITLIST_H

#include "git/Diff.h"
#include "git/Reference.h"
#include "git/WorkingTreeStatus.h"
#include <QByteArray>
#include <QFutureWatcher>
#include <QListView>

class Index;
class CommitAvatarProvider;
class QHeaderView;
class QScrollBar;
class QShowEvent;
class QStandardItemModel;
class QTimer;
class QToolButton;

namespace git {
class Commit;
class Diff;
} // namespace git

class CommitList : public QListView {
  Q_OBJECT

public:
  enum Role {
    DiffRole = Qt::UserRole,
    CommitRole,
    GraphRole,
    GraphLaneCountRole,
    GraphColorRole,
    GraphBaseColorRole,
    GraphStyleRole,
    GraphNodeRole,
    StashIndexRole
  };
  enum class GraphNode { Commit, Stash };
  Q_ENUM(GraphNode)

  enum Column { ReferencesColumn, GraphColumn, SummaryColumn, AuthorColumn,
                DateColumn, IdColumn, ColumnCount };

  enum class RefsFilter {
    AllRefs,
    SelectedRef,
    SelectedRefIgnoreMerge,
  };

  CommitList(Index *index, CommitAvatarProvider *avatars,
             QWidget *parent = nullptr);

  // Get the status diff item.
  git::Diff status() const;
  bool hasStatusChanges() const;
  bool hasTrackedStatusChanges() const;
  QStringList untrackedStatusPaths() const;

  // Get the current selection.
  QString selectedRange() const;
  git::Diff selectedDiff() const;
  QList<git::Commit> selectedCommits() const;

  // Cancel background status diff.
  void cancelStatus();

  void setReference(const git::Reference &ref);
  void setFilter(const QString &filter);
  void setPathspec(const QString &pathspec, bool index = false);
  void setCommits(const QList<git::Commit> &commits);

  void selectReference(const git::Reference &ref);
  void resetSelection(bool spontaneous = false);
  void selectFirstCommit(bool spontaneous = false);
  void selectCommitRelative(int offset);
  bool selectRange(const QString &range, const QString &file = QString(),
                   bool spontaneous = false);
  void suppressResetWalker(bool suppress);
  bool isResetWalkerSuppressed();

  void resetSettings();
  void resetReference(const git::Reference &ref);
  void preserveSelectionOnRefresh();

  void setModel(QAbstractItemModel *model) override;

signals:
  void statusChanged(bool dirty);
  void statusError(const QString &error);
  void selectedRangeChanged(const QString &range);
  void diffSelected(const git::Diff diff, const QString &file = QString(),
                    bool spontaneous = false);
  void statusSelected(const git::WorkingTreeStatusSnapshot status,
                      const QString &file = QString(),
                      bool spontaneous = false);

protected:
  void contextMenuEvent(QContextMenuEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *) override;
  void resizeEvent(QResizeEvent *event) override;
  void showEvent(QShowEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void setupHeader();
  void updateHeader(bool saveState = true);
  void resetHeader(bool saveState = true);
  int defaultReferencesWidth() const;
  int minimumColumnWidth(int column) const;
  void updateGraphColumnWidth(int first = -1, int last = -1);
  void updateGraphScrollBar();
  QRect graphViewportRect() const;
  void scheduleHistoryPrefetch();
  void prefetchHistory();
  void resizeHeaderToFit(int protectedColumn = -1);
  void saveHeaderState();
  void storeSelection();
  void restoreSelection();
  void updateModel();

  QModelIndexList sortedIndexes() const;

  QModelIndex findCommit(const git::Commit &commit);
  void selectIndexes(const QItemSelection &selection,
                     const QString &file = QString(), bool spontaneous = false);

  void notifySelectionChanged();

  bool isDecoration(const QModelIndex &index, const QPoint &pos);
  bool isStar(const QModelIndex &index, const QPoint &pos);

  QString mFile;
  QModelIndex mStar;
  QModelIndex mCancel;
  bool mSpontaneous = true;

  Index *mIndex;
  QString mFilter;

  QAbstractListModel *mList;
  QAbstractListModel *mModel;

  QHeaderView *mHeader = nullptr;
  QScrollBar *mGraphScrollBar = nullptr;
  QTimer *mHistoryPrefetchTimer = nullptr;
  QStandardItemModel *mHeaderModel = nullptr;
  QToolButton *mHeaderOptions = nullptr;
  bool mUpdatingHeader = false;
  bool mHeaderStateReady = false;
  bool mResetHeaderOnShow = false;
  bool mHeaderInteraction = false;
  bool mMigrateReferencesWidth = false;
  QByteArray mPendingHeaderState;
  int mReferencesPreferredWidth = 0;
  int mGraphContentWidth = 0;
  int mGraphPreferredWidth = 0;
  int mHistoryPrefetchTarget = 0;

  bool mRestoreSelection{true};
  bool mPreserveSelectionDetails = false;
  bool mSuppressSelectionNotification = false;

  QFutureWatcher<git::Diff> mSelectionDiff;
  QString mSelectionDiffFile;
  bool mSelectionDiffSpontaneous = false;
  quint64 mSelectionDiffGeneration = 0;
  quint64 mActiveSelectionDiffGeneration = 0;
  QString mSelectedRange;
};

#endif

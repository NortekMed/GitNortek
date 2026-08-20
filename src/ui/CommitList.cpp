//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "CommitList.h"
#include "CommitAvatarProvider.h"
#include "Badge.h"
#include "ContextMenuButton.h"
#include "Location.h"
#include "MainWindow.h"
#include "ProgressIndicator.h"
#include "RepoView.h"
#include "Debug.h"
#include "ConfigKeys.h"
#include "app/Application.h"
#include "conf/Settings.h"
#include "dialogs/MergeDialog.h"
#include "index/Index.h"
#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Config.h"
#include "git/Diff.h"
#include "git/Index.h"
#include "git/Patch.h"
#include "git/RevWalk.h"
#include "git/Signature.h"
#include "git/TagRef.h"
#include "git/Tree.h"
#include "ui/HotkeyManager.h"
#include <QAbstractListModel>
#include <QApplication>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QSettings>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QtConcurrent>

namespace {

constexpr int kGraphNodeSize = 16;
constexpr int kCommitHeaderHeight = 24;
constexpr int kCommitHeaderInset = 8;
constexpr int kCommitHeaderOptionsWidth = 28;
constexpr int kCompactColumnPadding = 8;
constexpr int kShortIdMargin = 8;
constexpr int kReferencesMinimumWidth = 55;
constexpr int kGraphMinimumWidth = 50;
constexpr int kSummaryMinimumWidth = 24;
constexpr int kAuthorMinimumWidth = 70;
constexpr int kDateMinimumWidth = 100;
const char kCommitHeaderStateKey[] = "commit/columns/headerStateV10";

// FIXME: Factor out into theme?
const QColor kTaintedColor = Qt::gray;

const QString kPathspecFmt = "pathspec:%1";

// Use fixed short id size in compact mode.
// FIXME: Use 'core.abbrev' config instead?
const int kShortIdSize = 7;

QFont compactFont(QFont font) {
  if (font.pointSizeF() > 1.0) {
    font.setPointSizeF(font.pointSizeF() - 1.0);
  } else if (font.pixelSize() > 1) {
    font.setPixelSize(font.pixelSize() - 1);
  }
  return font;
}

int shortIdTextWidth(const QFont &font, const QPaintDevice *device) {
  QFontMetrics fm(font, device);
  const QString chars = "0123456789abcdef";
  int maxCharacter = 0;
  int maxPairAdjustment = 0;
  for (QChar first : chars) {
    int firstWidth = fm.horizontalAdvance(first);
    maxCharacter = qMax(maxCharacter, firstWidth);
    for (QChar second : chars) {
      int pairWidth = fm.horizontalAdvance(QString(first) + second);
      int secondWidth = fm.horizontalAdvance(second);
      maxPairAdjustment =
          qMax(maxPairAdjustment, pairWidth - firstWidth - secondWidth);
    }
  }
  return kShortIdSize * maxCharacter +
         (kShortIdSize - 1) * maxPairAdjustment;
}

enum GraphSegment {
  Dot,
  Top,
  Middle,
  Bottom,
  Cross,
  LeftIn,
  LeftOut,
  RightIn,
  RightOut,
  MergeCross,
  MergeLeftIn,
  MergeLeftOut,
  MergeRightIn,
  MergeRightOut,
  ForkCross,
  ForkLeftIn,
  ForkLeftOut,
  ForkRightIn,
  ForkRightOut
};

class DiffCallbacks : public git::Diff::Callbacks {
public:
  void setCanceled(bool canceled) { mCanceled = canceled; }

  bool progress(const QString &oldPath, const QString &newPath) override {
    return !mCanceled;
  }

private:
  bool mCanceled = false;
};

/*!
 * \brief The CommitModel class
 * Model showing all commits as timeline
 */
class CommitModel : public QAbstractListModel {
  Q_OBJECT

public:
  CommitModel(const git::Repository &repo, QObject *parent = nullptr)
      : QAbstractListModel(parent), mRepo(repo) {
    // Connect progress timer.
    connect(&mTimer, &QTimer::timeout, [this] {
      ++mProgress;
      QModelIndex idx = index(0, 0);
      emit dataChanged(idx, idx, {Qt::DisplayRole});
    });

    // Connect watcher to signal when the status diff finishes.
    connect(&mStatus, &QFutureWatcher<git::Diff>::finished, [this] {
      mTimer.stop();
      resetWalker();
      emit statusFinished(!mRows.isEmpty() && !mRows.first().commit.isValid());
    });

    resetSettings();
  }

  git::Reference reference() const { return mRef; }

  bool isHeadDetached() const { return mRepo.isHeadDetached(); }

  git::Diff status() const {
    if (!mStatus.isFinished())
      return git::Diff();

    QFuture<git::Diff> future = mStatus.future();
    if (!future.resultCount())
      return git::Diff();

    return future.result();
  }

  void startStatus() {
    // Cancel existing status diff.
    cancelStatus();

    // Reload the index before starting the status thread. Allowing
    // it to reload on the thread frequently corrupts the index.
    mRepo.index().read();

    // Check for uncommitted changes asynchronously.
    mProgress = 0;
    mTimer.start(50);
    mStatus.setFuture(QtConcurrent::run([this] {
      // Pass the repo's index to suppress reload.
      bool ignoreWhitespace = Settings::instance()->isWhitespaceIgnored();
      return mRepo.status(mRepo.index(), &mStatusCallbacks, ignoreWhitespace);
    }));
  }

  void cancelStatus() {
    if (!mStatus.isRunning())
      return;

    mStatusCallbacks.setCanceled(true);
    mStatus.waitForFinished();
    mStatus.setFuture(QFuture<git::Diff>());
    mStatusCallbacks.setCanceled(false);
  }

  void setPathspec(const QString &pathspec) {
    if (mPathspec == pathspec)
      return;

    mPathspec = pathspec;
    resetWalker();
  }

  void suppressResetWalker(bool suppress) { mSuppressResetWalker = suppress; }

  bool isResetWalkerSuppressed() { return mSuppressResetWalker; }

  void setReference(const git::Reference &ref) {
    mRef = ref;
    if (!mSuppressResetWalker) {
      resetWalker();
    }
  }

  void resetReference(const git::Reference &ref) {
    // Reset selected ref to updated ref.
    if (ref.isValid() && mRef.isValid() &&
        ref.qualifiedName() == mRef.qualifiedName())
      mRef = ref;

    // Status is invalid after HEAD changes.
    if (!ref.isValid() || ref.isHead())
      startStatus();
    else if (!mSuppressResetWalker) {
      // reset walker will be done when status finished
      resetWalker();
    }
  }

  void resetWalker() {
    beginResetModel();

    // Reset state.
    mParents.clear();
    mNextLane = 1;
    mRows.clear();
    mStashIndexes.clear();
    mStashAuxiliaryCommits.clear();
    DebugRefresh("");

    // Update status row.
    bool head = (!mRef.isValid() || mRef.isHead());
    bool valid = (!mStatus.isFinished() || status().isValid());
    if (mShowCleanStatus && head && valid && mPathspec.isEmpty()) {
      QVector<Column> row;
      if (mGraphVisible && mRef.isValid() && mStatus.isFinished()) {
        row.append({Segment(Bottom, kTaintedColor), Segment(Dot, QColor())});
        mParents.append(
            Parent(mRef.target(), nextColor(), true, Qt::SolidLine,
                   mNextLane++));
      }
      DebugRefresh("mRows append invalid commit");
      mRows.append(Row(git::Commit(), row)); // Uncommitted changes
    }

    // Begin walking commits.
    if (mRef.isValid()) {
      int sort = GIT_SORT_NONE;
      if (mGraphVisible) {
        sort |= GIT_SORT_TOPOLOGICAL;
        if (mSortDate)
          sort |= GIT_SORT_TIME;
      } else if (!mSortDate) {
        sort |= GIT_SORT_TOPOLOGICAL;
      }

      mWalker = mRef.walker(
          sort, mRefsFilter == CommitList::RefsFilter::SelectedRefIgnoreMerge);
      if (mRef.isLocalBranch()) {
        // Add the upstream branch.
        if (git::Branch upstream = git::Branch(mRef).upstream())
          mWalker.push(upstream);
      }

      if (mRef.isHead()) {
        // Add merge head.
        if (git::Reference mergeHead = mRepo.lookupRef("MERGE_HEAD"))
          mWalker.push(mergeHead);
      }

      if (mRefsFilter == CommitList::RefsFilter::AllRefs) {
        foreach (const git::Reference ref, mRepo.refs()) {
          if (!ref.isStash())
            mWalker.push(ref);
        }

        const QList<git::Commit> stashes = mRepo.stashes();
        for (int i = 0; i < stashes.size(); ++i) {
          const git::Commit &stash = stashes.at(i);
          mStashIndexes.insert(stash.id(), i);
          mWalker.push(stash);

          // A stash is a merge commit whose additional parents represent the
          // index and untracked state. They are implementation details, not
          // branches in the repository topology.
          const QList<git::Commit> parents = stash.parents();
          for (int j = 1; j < parents.size(); ++j)
            mStashAuxiliaryCommits.insert(parents.at(j).id());
        }
      }
    }

    if (canFetchMore(QModelIndex()))
      fetchMore(QModelIndex());

    endResetModel();
  }

  void resetSettings(bool walk = false) {
    git::Config config = mRepo.appConfig();
    mRefsFilter = static_cast<CommitList::RefsFilter>(config.value<int>(
        ConfigKeys::kRefsKey, (int)CommitList::RefsFilter::AllRefs));
    mSortDate = config.value<bool>(ConfigKeys::kSortKey, true);
    mShowCleanStatus = config.value<bool>(ConfigKeys::kStatusKey, true);
    mGraphVisible = config.value<bool>(ConfigKeys::kGraphKey, true);

    if (walk)
      resetWalker();
  }

  bool canFetchMore(const QModelIndex &parent) const {
    return mWalker.isValid();
  }

  void fetchMore(const QModelIndex &parent) {
    // Load commits.
    int i = 0;
    QList<Row> rows;
    git::Commit commit = nextCommit();
    while (commit.isValid()) {
      // Add root commits.
      bool root = false;
      if (indexOf(commit) < 0) {
        root = true;
        Qt::PenStyle style = isStash(commit) ? Qt::DotLine : Qt::SolidLine;
        mParents.append(
            Parent(commit, nextColor(), false, style, mNextLane++));
      }

      // Calculate graph columns.
      // Remember current row.
      QList<Parent> parents = mParents;

      // Replace commit with its parents.
      QList<git::Commit> commitParents = graphParents(commit);
      QList<git::Commit> replacements;
      foreach (const git::Commit &parent, commitParents) {
        // FIXME: Mark commits that point to existing parent?
        if (indexOf(parent) < 0 && !contains(parent, rows))
          replacements.append(parent);
        if (mRefsFilter == CommitList::RefsFilter::SelectedRefIgnoreMerge) {
          break;
        }
      }

      // Set parents for next row.
      int index = indexOf(commit);
      if (index >= 0) {
        Parent parent = mParents.takeAt(index);
        bool deferJoin = !isStash(commit) && commitParents.size() == 1 &&
                         indexOf(commitParents.constFirst()) >= 0;
        if (deferJoin) {
          git::Commit target = commitParents.constFirst();
          mParents.insert(index,
                          Parent(target, parent.color, false, Qt::SolidLine,
                                 parent.lane, target));
        } else if (!replacements.isEmpty()) {
          git::Commit replacement = replacements.takeFirst();
          Qt::PenStyle style = isStash(commit) ? parent.style : Qt::SolidLine;
          mParents.insert(
              index,
              Parent(replacement, parent.color, false, style, parent.lane));
          foreach (const git::Commit &replacement, replacements)
            mParents.append(
                Parent(replacement, nextColor(), false, Qt::SolidLine,
                       mNextLane++));
        }
      }

      // Deferred lanes converge only when their shared parent is drawn.
      for (int j = mParents.size() - 1; j >= 0; --j) {
        if (mParents.at(j).isDeferred() &&
            mParents.at(j).joinTarget == commit) {
          mParents.removeAt(j);
        }
      }

      // Add graph row.
      QVector<Column> row;
      if (mGraphVisible && mPathspec.isEmpty())
        row = columns(commit, parents, root);

      rows.append(Row(commit, row));
      DebugRefresh("Append commit: " << commit.shortId());

      // Bail out.
      if (i++ >= 64)
        break;

      commit = nextCommit();
    }

    // Update the model.
    if (!rows.isEmpty()) {
      int first = mRows.size();
      int last = first + rows.size() - 1;
      beginInsertRows(QModelIndex(), first, last);
      mRows.append(rows);
      endInsertRows();
    }

    // Invalidate walker.
    if (!commit.isValid())
      mWalker = git::RevWalk();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const {
    return mRows.size();
  }

  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const {
    if (index.row() >= mRows.size())
      return QVariant();
    const Row &row = mRows.at(index.row());
    bool status = !row.commit.isValid();
    switch (role) {
      case Qt::DisplayRole:
        if (!status)
          return QVariant();

        return mStatus.isFinished() ? tr("Uncommitted changes")
                                    : tr("Checking for uncommitted changes");

      case Qt::FontRole: {
        if (!status)
          return QVariant();

        QFont font = static_cast<QWidget *>(QObject::parent())->font();
        font.setItalic(true);
        return font;
      }

      case Qt::TextAlignmentRole:
        if (!status)
          return QVariant();

        return QVariant(Qt::AlignHCenter | Qt::AlignVCenter);

      case Qt::DecorationRole:
        if (!status)
          return QVariant();

        return mStatus.isFinished() ? QVariant() : mProgress;

      case CommitList::Role::DiffRole: {
        if (status)
          return QVariant::fromValue(this->status());

        bool ignoreWhitespace = Settings::instance()->isWhitespaceIgnored();
        git::Diff diff = row.commit.diff(git::Commit(), -1, ignoreWhitespace);
        diff.findSimilar();
        return QVariant::fromValue(diff);
      }

      case CommitList::Role::CommitRole:
        return status ? QVariant() : QVariant::fromValue(row.commit);

      case CommitList::Role::GraphRole: {
        QVariantList columns;
        foreach (const Column &column, row.columns) {
          QVariantList segments;
          foreach (const Segment &segment, column)
            segments.append(segment.segment);
          columns.append(QVariant(segments));
        }

        return columns;
      }

      case CommitList::Role::GraphColorRole: {
        QVariantList columns;
        foreach (const Column &column, row.columns) {
          QVariantList segments;
          foreach (const Segment &segment, column)
            segments.append(segment.color);
          columns.append(QVariant(segments));
        }

        return columns;
      }

      case CommitList::Role::GraphStyleRole: {
        QVariantList columns;
        foreach (const Column &column, row.columns) {
          QVariantList segments;
          foreach (const Segment &segment, column)
            segments.append(static_cast<int>(segment.style));
          columns.append(QVariant(segments));
        }

        return columns;
      }

      case CommitList::Role::GraphNodeRole:
        return QVariant::fromValue(isStash(row.commit)
                                       ? CommitList::GraphNode::Stash
                                       : CommitList::GraphNode::Commit);

      case CommitList::Role::StashIndexRole: {
        auto stash = mStashIndexes.constFind(row.commit.id());
        return stash == mStashIndexes.cend() ? QVariant() : QVariant(*stash);
      }
    }

    return QVariant();
  }

signals:
  void statusFinished(bool visible);

private:
  struct Parent {
    Parent(const git::Commit &commit, const QColor &color, bool tainted = false,
            Qt::PenStyle style = Qt::SolidLine, quint64 lane = 0,
            const git::Commit &joinTarget = git::Commit())
        : commit(commit), joinTarget(joinTarget), color(color), lane(lane),
          tainted(tainted), style(style) {}

    bool isDeferred() const { return joinTarget.isValid(); }

    QColor taintedColor(const git::Commit &commit = git::Commit()) const {
      return (tainted && this->commit != commit) ? kTaintedColor : color;
    }

    git::Commit commit;
    git::Commit joinTarget;
    QColor color;
    quint64 lane;
    bool tainted;
    Qt::PenStyle style;
  };

  struct Segment {
    Segment(GraphSegment segment, QColor color,
            Qt::PenStyle style = Qt::SolidLine)
        : segment(segment), color(color), style(style) {}

    GraphSegment segment;
    QColor color;
    Qt::PenStyle style;
  };

  using Column = QList<Segment>;

  struct Row {
    Row(const git::Commit &commit, const QVector<Column> &columns)
        : commit(commit), columns(columns) {}

    git::Commit commit;
    QVector<Column> columns;
  };

  int indexOf(const git::Commit &commit) const {
    int count = mParents.size();
    for (int i = 0; i < count; ++i) {
      if (!mParents.at(i).isDeferred() && mParents.at(i).commit == commit)
        return i;
    }

    return -1;
  }

  int indexOfLane(quint64 lane) const {
    for (int i = 0; i < mParents.size(); ++i) {
      if (mParents.at(i).lane == lane)
        return i;
    }
    return -1;
  }

  bool contains(const git::Commit &commit, const QList<Row> &rows) const {
    foreach (const Row &row, mRows) {
      if (row.commit == commit)
        return true;
    }

    foreach (const Row &row, rows) {
      if (row.commit == commit)
        return true;
    }

    return false;
  }

  bool isStash(const git::Commit &commit) const {
    return commit.isValid() && mStashIndexes.contains(commit.id());
  }

  QList<git::Commit> graphParents(const git::Commit &commit) const {
    QList<git::Commit> parents = commit.parents();
    if (isStash(commit) && !parents.isEmpty())
      return {parents.first()};
    return parents;
  }

  git::Commit nextCommit() {
    git::Commit commit;
    do {
      commit = mWalker.next(mPathspec);
    } while (commit.isValid() &&
             mStashAuxiliaryCommits.contains(commit.id()));
    return commit;
  }

  // The commit and parents parameters represent the current row.
  // The mParents member represents the next row after this one.
  QVector<Column> columns(const git::Commit &commit,
                          const QList<Parent> &parents, bool root) {
    int count = parents.size();
    QVector<Column> columns(count);
    auto resolvesHere = [&commit](const Parent &parent) {
      return parent.isDeferred() && parent.joinTarget == commit;
    };

    int nodeIndex = -1;
    for (int i = 0; i < count; ++i) {
      if (!parents.at(i).isDeferred() && parents.at(i).commit == commit) {
        nodeIndex = i;
        break;
      }
    }

    // Add incoming paths.
    int incoming = root ? count - 1 : count;
    for (int i = 0; i < incoming; ++i) {
      if (!resolvesHere(parents.at(i))) {
        columns[i] << Segment(Top, parents.at(i).taintedColor(),
                              parents.at(i).style);
      }
    }

    // Resolve shared-parent lanes at the actual parent bubble.
    if (nodeIndex >= 0) {
      for (int i = 0; i < count; ++i) {
        const Parent &alias = parents.at(i);
        if (!resolvesHere(alias))
          continue;

        if (i < nodeIndex) {
          columns[i] << Segment(ForkRightIn, alias.color, alias.style);
          for (int j = i + 1; j < nodeIndex; ++j)
            columns[j] << Segment(ForkCross, alias.color, alias.style);
          columns[nodeIndex]
              << Segment(ForkLeftOut, alias.color, alias.style);
        } else if (i > nodeIndex) {
          columns[nodeIndex]
              << Segment(ForkRightOut, alias.color, alias.style);
          for (int j = nodeIndex + 1; j < i; ++j)
            columns[j] << Segment(ForkCross, alias.color, alias.style);
          columns[i] << Segment(ForkLeftIn, alias.color, alias.style);
        }
      }
    }

    // Add outgoing paths.
    for (int i = 0; i < count; ++i) {
      // Get the successors of this column.
      QList<git::Commit> successors;
      const Parent &parent = parents.at(i);
      if (resolvesHere(parent))
        continue;
      if (parent.commit == commit) {
        successors = graphParents(parent.commit);
      } else {
        successors.append(parent.commit);
      }

      // Add a path to each successor.
      foreach (const git::Commit &successor, successors) {
        // Find index of parent in next row.
        int index = -1;
        if (successors.size() == 1) {
          int laneIndex = indexOfLane(parent.lane);
          if (laneIndex >= 0 && mParents.at(laneIndex).commit == successor)
            index = laneIndex;
        }
        if (index < 0)
          index = indexOf(successor);
        if (index < 0)
          continue;

        // Match each end of a lateral edge to the lane it touches.
        bool merge = parent.commit == commit && successors.size() > 1;
        QColor sourceColor = parent.taintedColor(commit);
        QColor targetColor = mParents.at(index).taintedColor();
        QColor edgeColor = merge ? targetColor : sourceColor;
        Qt::PenStyle style =
            parent.commit == commit && !isStash(commit) ? Qt::SolidLine
                                                         : parent.style;

        if (index < i) {
          // out to the left
          columns[index]
              << Segment(merge ? MergeRightIn : RightIn, edgeColor, style);
          for (int j = index + 1; j < i; ++j)
            columns[j]
                << Segment(merge ? MergeCross : Cross, edgeColor, style);
          columns[i]
              << Segment(merge ? MergeLeftOut : LeftOut, edgeColor, style);

        } else if (index > i) {
          // out to the right
          columns[i]
              << Segment(merge ? MergeRightOut : RightOut, edgeColor, style);
          for (int j = i + 1; j < index; ++j)
            columns[j]
                << Segment(merge ? MergeCross : Cross, edgeColor, style);
          if (index == columns.size())
            columns.append(Column());
          columns[index]
              << Segment(merge ? MergeLeftIn : LeftIn, edgeColor, style);

        } else { // index == i
          // out the bottom
          columns[index] << Segment(Bottom, edgeColor, style);
        }
      }
    }

    // Add middle section last.
    for (int i = 0; i < count; ++i) {
      const Parent &parent = parents.at(i);
      if (resolvesHere(parent))
        continue;
      bool dot = (!parent.isDeferred() && parent.commit == commit);
      columns[i] << Segment(dot ? Dot : Middle, parent.taintedColor(),
                            parent.style);
    }

    return columns;
  }

  QColor nextColor() {
    // Get the first unused (or least used) color.
    QMap<QString, int> counts;
    foreach (const Parent &parent, mParents)
      counts[parent.color.name()]++;

    int count = 0;
    QList<QColor> colors = Application::theme()->branchTopologyEdges();
    forever {
      foreach (const QColor &color, colors) {
        if (counts.value(color.name()) == count)
          return color;
      }

      ++count;
    }

    Q_UNREACHABLE();
    return QColor();
  }

  QTimer mTimer;
  int mProgress = 0;

  DiffCallbacks mStatusCallbacks;
  QFutureWatcher<git::Diff> mStatus;

  QString mPathspec;
  git::Reference mRef;
  git::RevWalk mWalker;
  git::Repository mRepo;

  QList<Row> mRows;
  QList<Parent> mParents;
  quint64 mNextLane = 1;
  QMap<git::Id, int> mStashIndexes;
  QSet<git::Id> mStashAuxiliaryCommits;

  // walker settings
  bool mSuppressResetWalker{false};
  CommitList::RefsFilter mRefsFilter{CommitList::RefsFilter::AllRefs};
  bool mSortDate = true;
  bool mShowCleanStatus = true;
  bool mGraphVisible = true;
};

/*!
 * \brief The ListModel class
 * Used to show a list of commits. This is used when a filter is used
 */
class ListModel : public QAbstractListModel {
public:
  ListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

  void setList(const QList<git::Commit> &commits) {
    beginResetModel();
    mCommits = commits;
    endResetModel();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override {
    return mCommits.size();
  }

  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override {
    switch (role) {
      case CommitList::Role::DiffRole: {
        git::Commit commit = mCommits.at(index.row());
        bool ignoreWhitespace = Settings::instance()->isWhitespaceIgnored();
        git::Diff diff = commit.diff(git::Commit(), -1, ignoreWhitespace);
        diff.findSimilar();
        return QVariant::fromValue(diff);
      }

      case CommitList::Role::CommitRole:
        return QVariant::fromValue(mCommits.at(index.row()));
    }

    return QVariant();
  }

private:
  QList<git::Commit> mCommits;
};

class CommitDelegate : public QStyledItemDelegate {
  struct CompactLayout {
    QRect refs;
    QRect graph;
    QRect summary;
    QRect author;
    QRect timestamp;
    QRect id;
    QRect star;
  };

public:
  CommitDelegate(const git::Repository &repo, CommitAvatarProvider *avatars,
                 QHeaderView *header, QObject *parent = nullptr)
      : QStyledItemDelegate(parent), mRepo(repo), mAvatars(avatars),
        mHeader(header) {
    updateRefs();

    git::RepositoryNotifier *notifier = repo.notifier();
    connect(notifier, &git::RepositoryNotifier::referenceUpdated, this,
            &CommitDelegate::updateRefs);
    connect(notifier, &git::RepositoryNotifier::referenceAdded, this,
            &CommitDelegate::updateRefs);
    connect(notifier, &git::RepositoryNotifier::referenceRemoved, this,
            &CommitDelegate::updateRefs);
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    bool compact = Settings::instance()
                       ->value(Setting::Id::ShowCommitsInCompactMode)
                       .toBool();
    if (compact) {
      opt.font = compactFont(opt.font);
      opt.fontMetrics = QFontMetrics(opt.font, opt.widget);
    }
    bool showAuthor = Settings::instance()
                          ->value(Setting::Id::ShowCommitsAuthor, true)
                          .toBool();
    bool showDate = Settings::instance()
                        ->value(Setting::Id::ShowCommitsDate, true)
                        .toBool();
    bool showId =
        Settings::instance()->value(Setting::Id::ShowCommitsId, true).toBool();
    LayoutConstants constants = layoutConstants(compact);

    bool active = (opt.state & QStyle::State_Active);
    bool selected = (opt.state & QStyle::State_Selected);
    auto group = active ? QPalette::Active : QPalette::Inactive;
    auto textRole = selected ? QPalette::HighlightedText : QPalette::Text;
    auto brightRole = selected ? QPalette::WindowText : QPalette::BrightText;
    QPalette palette = Application::theme()->commitList();
    QColor text = palette.color(group, textRole);
    QColor bright = palette.color(group, brightRole);
    QColor highlight = palette.color(group, QPalette::Highlight);

    painter->save();
    painter->setRenderHints(QPainter::Antialiasing);
    painter->setFont(opt.font);

    // Draw background.
    if (selected) {
      painter->fillRect(opt.rect, highlight);
    }

    // Draw busy indicator.
    if (opt.features & QStyleOptionViewItem::HasDecoration) {
      QRect rect = decorationRect(option, index);
      int progress = index.data(Qt::DecorationRole).toInt();
      ProgressIndicator::paint(painter, rect, bright, progress, opt.widget);
    }

    // Set default foreground color.
    painter->setPen(text);

    // Use default pen color for dot.
    QPen dot = painter->pen();
    dot.setWidth(2);

    // Copy content rect.
    QRect rect = opt.rect;
    rect.setX(rect.x() + 2);

    git::Commit commit =
        index.data(CommitList::Role::CommitRole).value<git::Commit>();
    bool stashNode = index.data(CommitList::Role::GraphNodeRole)
                         .value<CommitList::GraphNode>() ==
                     CommitList::GraphNode::Stash;
    bool avatarsEnabled =
        Settings::instance()->value(Setting::Id::ShowAvatars).toBool() &&
        mAvatars && mAvatars->isAvailable();
    QPixmap avatar;
    if (avatarsEnabled && commit.isValid() && !stashNode)
      avatar = mAvatars->avatar(commit, kGraphNodeSize,
                                opt.widget ? opt.widget->devicePixelRatioF()
                                           : qApp->devicePixelRatio());

    QDateTime date;
    QString timestamp;
    if (commit.isValid()) {
      date = commit.committer().date().toLocalTime();
      if (compact) {
        timestamp =
            QString("%1 @ %2")
                .arg(QLocale().toString(date.date(), QLocale::ShortFormat),
                     QLocale().toString(date.time(), QLocale::ShortFormat));
      } else {
        timestamp = (date.date() == QDate::currentDate())
                        ? QLocale().toString(date.time(), QLocale::ShortFormat)
                        : QLocale().toString(date.date(), QLocale::ShortFormat);
      }
    }
    CompactLayout compactColumns;
    if (compact) {
      compactColumns = compactLayout(opt.rect);
      rect = compactColumns.graph;
    }

    // Draw graph.
    painter->save();
    if (compact)
      painter->setClipRect(compactColumns.graph);
    QVariantList columns = index.data(CommitList::Role::GraphRole).toList();
    QVariantList colorColumns =
        index.data(CommitList::Role::GraphColorRole).toList();
    QVariantList styleColumns =
        index.data(CommitList::Role::GraphStyleRole).toList();
    for (int i = 0; i < columns.size(); ++i) {
      int x = rect.x();
      int y = rect.y();
      int w = qMax(opt.fontMetrics.ascent(), kGraphNodeSize + 4);
      int h = opt.rect.height();
      int h_2 = h / 2;

      // radius
      int r =
          commit.isValid() ? kGraphNodeSize / 2 : opt.fontMetrics.ascent() / 3;

      // xs
      int x1 = x + (w / 2);
      int x2 = x + w;

      // ys
      int y1 = y + h_2 - r;
      int y2 = y + h_2;
      int y3 = y + h_2 + r;
      int y5 = y + h;
      int y4 = y3 + (y5 - y3) / 2;

      QVariantList segments = columns.at(i).toList();
      QVariantList colors = colorColumns.at(i).toList();
      QVariantList styles = styleColumns.at(i).toList();
      for (int j = 0; j < segments.size(); ++j) {
        QColor color = colors.at(j).value<QColor>();
        QPen pen(color, 2);
        pen.setStyle(static_cast<Qt::PenStyle>(styles.at(j).toInt()));
        if (pen.style() == Qt::DotLine) {
          pen.setCapStyle(Qt::RoundCap);
        } else {
          pen.setCapStyle(Qt::FlatCap);
        }
        if (color == kTaintedColor) {
          pen.setStyle(Qt::DashLine);
          pen.setDashPattern({2, 2});
        }

        painter->setPen(pen);
        switch (segments.at(j).toInt()) {
          case Dot:
            if (stashNode) {
              pen.setStyle(Qt::SolidLine);
              painter->setPen(pen);
              painter->drawRect(QRect(x1 - r, y2 - r, 2 * r, 2 * r));
            } else if (!avatar.isNull()) {
              QRect avatarRect(x1 - r, y2 - r, 2 * r, 2 * r);
              painter->drawPixmap(avatarRect, avatar);
              pen.setStyle(Qt::SolidLine);
              painter->setPen(pen);
              painter->drawEllipse(avatarRect);
            } else {
              painter->setPen(dot);
              painter->drawEllipse(QPoint(x1, y2), r, r);
            }
            break;

          case Top:
            painter->drawLine(x1, y, x1, y1);
            break;

          case Middle:
            painter->drawLine(x1, y1, x1, y3);
            break;

          case Bottom:
            painter->drawLine(x1, y3, x1, y5);
            break;

          case Cross:
            painter->drawLine(x, y4, x2, y4);
            break;

          case RightOut: {
            QPainterPath path;
            path.moveTo(x1, y3);
            path.quadTo(x1, y4, x2, y4);
            painter->drawPath(path);
            break;
          }

          case LeftOut: {
            QPainterPath path;
            path.moveTo(x1, y3);
            path.quadTo(x1, y4, x, y4);
            painter->drawPath(path);
            break;
          }

          case RightIn: {
            QPainterPath path;
            path.moveTo(x1, y5);
            path.quadTo(x1, y4, x2, y4);
            painter->drawPath(path);
            break;
          }

          case LeftIn: {
            QPainterPath path;
            path.moveTo(x1, y5);
            path.quadTo(x1, y4, x, y4);
            painter->drawPath(path);
            break;
          }

          case MergeCross:
            painter->drawLine(x, y2, x2, y2);
            break;

          case MergeRightOut:
            painter->drawLine(x1 + r, y2, x2, y2);
            break;

          case MergeLeftOut:
            painter->drawLine(x1 - r, y2, x, y2);
            break;

          case MergeLeftIn: {
            QPainterPath path;
            path.moveTo(x, y2);
            path.cubicTo(x1, y2, x1, y4, x1, y5);
            painter->drawPath(path);
            break;
          }

          case MergeRightIn: {
            QPainterPath path;
            path.moveTo(x2, y2);
            path.cubicTo(x1, y2, x1, y4, x1, y5);
            painter->drawPath(path);
            break;
          }

          case ForkCross:
            painter->drawLine(x, y2, x2, y2);
            break;

          case ForkRightOut:
            painter->drawLine(x1 + r, y2, x2, y2);
            break;

          case ForkLeftOut:
            painter->drawLine(x1 - r, y2, x, y2);
            break;

          case ForkLeftIn: {
            QPainterPath path;
            path.moveTo(x, y2);
            path.cubicTo(x1, y2, x1, y1, x1, y);
            painter->drawPath(path);
            break;
          }

          case ForkRightIn: {
            QPainterPath path;
            path.moveTo(x2, y2);
            path.cubicTo(x1, y2, x1, y1, x1, y);
            painter->drawPath(path);
            break;
          }
        }
      }

      rect.setX(x + w);

      // Finish early if the graph exceeds its available column.
      if ((compact && rect.x() >= compactColumns.graph.right()) ||
          (!compact && rect.x() - opt.rect.x() > opt.rect.width() / 3))
        break;
    }

    painter->restore();

    // Adjust margins.
    if (compact) {
      rect = compactColumns.summary;
    } else {
      rect.setY(rect.y() + constants.vMargin);
      rect.setX(rect.x() + constants.hMargin);
    }

    // Star has enough padding in compact mode.
    if (!compact)
      rect.setWidth(rect.width() - constants.hMargin);

    // Draw content.
    if (!commit.isValid()) {
      // special case for uncommitted changes
      QString message = index.model()->data(index).toString();
      painter->save();
      QFont italic = opt.font;
      italic.setItalic(true);
      if (compact) {
        message = QFontMetrics(italic, opt.widget).elidedText(
            message, Qt::ElideRight, compactColumns.summary.width());
      }
      painter->setFont(italic);
      painter->drawText(compact ? compactColumns.summary : opt.rect,
                        compact ? Qt::AlignVCenter | Qt::AlignLeft
                                : Qt::AlignCenter,
                        message);
      painter->restore();
    } else {
      const QFontMetrics &fm = opt.fontMetrics;
      QRect star = rect;
      int timestampWidth = fm.horizontalAdvance(timestamp);

      if (compact) {
        star = compactColumns.star;

        // Draw references before the graph.
        QList<Badge::Label> refs = mRefs.value(commit.id());
        if (!refs.isEmpty())
          Badge::paint(painter, refs, compactColumns.refs, &opt, Qt::AlignLeft);

        // Draw message.
        painter->save();
        painter->setPen(bright);
        QString msg = commit.summary(git::Commit::SubstituteEmoji);
        QString elidedText =
            fm.elidedText(msg, Qt::ElideRight, compactColumns.summary.width());
        painter->drawText(compactColumns.summary,
                          Qt::AlignVCenter | Qt::AlignLeft, elidedText);
        painter->restore();

        // Draw aligned metadata columns in a muted color.
        painter->save();
        painter->setPen(text);
        if (compactColumns.author.isValid()) {
          QString author = fm.elidedText(commit.author().name(), Qt::ElideRight,
                                         compactColumns.author.width());
          painter->drawText(compactColumns.author,
                            Qt::AlignVCenter | Qt::AlignLeft, author);
        }
        if (compactColumns.timestamp.isValid()) {
          QString elidedTimestamp = fm.elidedText(
              timestamp, Qt::ElideRight, compactColumns.timestamp.width());
          painter->drawText(compactColumns.timestamp,
                            Qt::AlignVCenter | Qt::AlignLeft, elidedTimestamp);
        }
        if (compactColumns.id.isValid()) {
          QString id = commit.id().toString().left(kShortIdSize);
          id = fm.elidedText(id, Qt::ElideRight, compactColumns.id.width());
          painter->drawText(compactColumns.id, Qt::AlignVCenter | Qt::AlignLeft,
                            id);
        }
        painter->restore();

      } else {

        // Draw Name.
        QString name = "";
        if (showAuthor) {
          name = commit.author().name();
          painter->save();
          QFont bold = opt.font;
          bold.setBold(true);
          painter->setFont(bold);
          painter->drawText(rect, Qt::AlignLeft, name);
          painter->restore();
        }

        // Draw date.
        if (showDate &&
            rect.width() > fm.horizontalAdvance(name) + timestampWidth + 8) {
          painter->save();
          painter->setPen(bright);
          if (showAuthor) {
            painter->drawText(rect, Qt::AlignRight, timestamp);
          } else {
            painter->drawText(rect, Qt::AlignLeft, timestamp);
          }
          painter->restore();
        }

        // Draw id.
        QString id = "";
        if (showId) {
          QRect idRect = rect;
          if (showAuthor || showDate) {
            idRect.setY(idRect.y() + constants.lineSpacing + constants.vMargin);
          }
          id = commit.shortId();
          painter->save();
          painter->drawText(idRect, Qt::AlignLeft, id);
          painter->restore();
        }

        // Draw references.
        QList<Badge::Label> refs = mRefs.value(commit.id());
        if (!refs.isEmpty()) {
          QRect refsRect = rect;
          QString leftText = "";

          if (showDate && showAuthor) {
            refsRect.setY(refsRect.y() + constants.lineSpacing +
                          constants.vMargin);
            if (showId) {
              leftText = id;
            }
          } else {
            if (showDate) {
              leftText = timestamp;
            } else if (showAuthor) {
              leftText = name;
            } else if (showId) {
              leftText = id;
            }
          }
          refsRect.setX(refsRect.x() + fm.boundingRect(leftText).width() + 6);
          Badge::paint(painter, refs, refsRect, &opt);
        }

        int numOptional = 0;
        if (showId)
          ++numOptional;
        if (showAuthor)
          ++numOptional;
        if (showDate)
          ++numOptional;
        if (numOptional > 1) {
          rect.setY(rect.y() + constants.lineSpacing + constants.vMargin);
        }

        rect.setY(rect.y() + constants.lineSpacing + constants.vMargin);

        // Divide remaining rectangle.
        star = rect;
        star.setX(star.x() + star.width() - star.height());
        QRect text = rect;
        text.setWidth(text.width() - star.width());

        // Draw message.
        painter->save();
        painter->setPen(bright);
        QString msg = commit.summary(git::Commit::SubstituteEmoji);
        QTextLayout layout(msg, painter->font());
        layout.beginLayout();

        QTextLine line = layout.createLine();
        if (line.isValid()) {
          int width = text.width();
          line.setLineWidth(width);
          int len = line.textLength();
          painter->drawText(text, Qt::AlignLeft, msg.left(len));

          if (len < msg.length()) {
            text.setY(text.y() + constants.lineSpacing);
            QString elided = fm.elidedText(msg.mid(len), Qt::ElideRight, width);
            painter->drawText(text, Qt::AlignLeft, elided);
          }
        }

        layout.endLayout();
        painter->restore();
      }

      // Draw star.
      bool starred = commit.isStarred();
      const QAbstractItemView *view =
          static_cast<const QAbstractItemView *>(opt.widget);
      QPoint pos = view->viewport()->mapFromGlobal(QCursor::pos());
      if (starred || (view->underMouse() && view->indexAt(pos) == index)) {
        painter->save();

        // Calculate outer radius and vertices.
        qreal r = (star.height() / 2.0) - constants.starPadding;
        qreal x = star.x() + (star.width() / 2.0);
        qreal y = star.y() + (star.height() / 2.0);
        qreal x1 = r * qCos(M_PI / 10.0);
        qreal y1 = -r * qSin(M_PI / 10.0);
        qreal x2 = r * qCos(17.0 * M_PI / 10.0);
        qreal y2 = -r * qSin(17.0 * M_PI / 10.0);

        // Calculate inner radius and vertices.
        qreal xi = ((y1 + r) * x2) / (y2 + r);
        qreal ri = qSqrt(qPow(xi, 2.0) + qPow(y1, 2.0));
        qreal xi1 = ri * qCos(3.0 * M_PI / 10.0);
        qreal yi1 = -ri * qSin(3.0 * M_PI / 10.0);
        qreal xi2 = ri * qCos(19.0 * M_PI / 10.0);
        qreal yi2 = -ri * qSin(19.0 * M_PI / 10.0);

        QPolygonF polygon({QPointF(0, -r), QPointF(xi1, yi1), QPointF(x1, y1),
                           QPointF(xi2, yi2), QPointF(x2, y2), QPointF(0, ri),
                           QPointF(-x2, y2), QPointF(-xi2, yi2),
                           QPointF(-x1, y1), QPointF(-xi1, yi1)});

        if (starred)
          painter->setBrush(Application::theme()->star());

        painter->setPen(QPen(bright, 1.25));
        painter->drawPolygon(polygon.translated(x, y));
        painter->restore();
      }
    }

    // Is the next index selected?
    bool nextSelected = false;

#ifndef Q_OS_WIN
    // Draw separator between selected indexes.
    QModelIndex next = index.sibling(index.row() + 1, 0);
    if (next.isValid()) {
      const QAbstractItemView *view =
          static_cast<const QAbstractItemView *>(opt.widget);
      nextSelected = view->selectionModel()->isSelected(next);
    }
#endif

    // Draw separator line.
    if (!compact && selected == nextSelected) {
      painter->save();
      painter->setRenderHints(QPainter::Antialiasing, false);
      painter->setPen(selected ? text : opt.palette.color(QPalette::Dark));
      painter->drawLine(rect.bottomLeft(), rect.bottomRight());
      painter->restore();
    }

    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    bool compact = Settings::instance()
                       ->value(Setting::Id::ShowCommitsInCompactMode)
                       .toBool();
    LayoutConstants constants = layoutConstants(compact);

    int lineHeight = constants.lineSpacing + constants.vMargin;
    int width = compact && mHeader
                    ? kCommitHeaderInset + mHeader->length() +
                          kCommitHeaderOptionsWidth
                    : 0;
    return QSize(width, lineHeight * (compact ? 1 : 4));
  }

  bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                 const QStyleOptionViewItem &option,
                 const QModelIndex &index) override {
    bool compact = Settings::instance()
                       ->value(Setting::Id::ShowCommitsInCompactMode)
                       .toBool();
    git::Commit commit =
        index.data(CommitList::Role::CommitRole).value<git::Commit>();
    QRect refsRect = compactLayout(option.rect).refs;
    QList<Badge::Label> refs = mRefs.value(commit.id());
    if (compact && commit.isValid() && refsRect.contains(event->pos()) &&
        !refs.isEmpty() &&
        Badge::size(compactFont(option.font), refs).width() >
            refsRect.width()) {
      QStringList names;
      for (const Badge::Label &ref : refs)
        names.append(ref.text.toHtmlEscaped());
      QToolTip::showText(event->globalPos(),
                         QString("<qt>%1</qt>").arg(names.join("<br>")), view);
      return true;
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
  }

  QRect decorationRect(const QStyleOptionViewItem &option,
                       const QModelIndex &index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    QStyle::SubElement se = QStyle::SE_ItemViewItemDecoration;
    return style->subElementRect(se, &opt, opt.widget);
  }

  QRect starRect(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const {
    bool compact = Settings::instance()
                       ->value(Setting::Id::ShowCommitsInCompactMode)
                       .toBool();
    if (compact)
      return compactLayout(option.rect).star;

    LayoutConstants constants = layoutConstants(compact);

    QRect rect = option.rect;
    int length = constants.lineSpacing * 2;
    rect.setX(rect.x() + rect.width() - length);
    rect.setY(rect.y() + rect.height() - length);
    rect.setWidth(rect.width() - constants.starPadding);
    rect.setHeight(rect.height() - constants.starPadding);
    return rect;
  }

protected:
  void initStyleOption(QStyleOptionViewItem *option,
                       const QModelIndex &index) const override {
    QStyledItemDelegate::initStyleOption(option, index);
    if (index.data(Qt::DecorationRole).canConvert<int>())
      option->decorationSize = ProgressIndicator::size();
  }

private:
  struct LayoutConstants {
    const int starPadding;
    const int lineSpacing;
    const int vMargin;
    const int hMargin;
  };

  LayoutConstants layoutConstants(bool compact) const {
    return {compact ? 7 : 8, compact ? 23 : 16, compact ? 5 : 2, 4};
  }

  QRect compactColumn(const QRect &row, int column) const {
    if (!mHeader || mHeader->isSectionHidden(column))
      return QRect();
    int x = row.x() + kCommitHeaderInset +
            mHeader->sectionPosition(column);
    return QRect(x, row.y(), mHeader->sectionSize(column), row.height());
  }

  CompactLayout compactLayout(const QRect &row) const {
    CompactLayout layout;
    layout.refs = compactColumn(row, CommitList::ReferencesColumn);
    layout.graph = compactColumn(row, CommitList::GraphColumn);
    layout.summary = compactColumn(row, CommitList::SummaryColumn);
    layout.author = compactColumn(row, CommitList::AuthorColumn);
    layout.timestamp = compactColumn(row, CommitList::DateColumn);
    layout.id = compactColumn(row, CommitList::IdColumn);
    layout.star = QRect(row.right() - row.height() + 1, row.y(), row.height(),
                        row.height());

    int top = layoutConstants(true).vMargin;
    for (QRect *rect : {&layout.refs, &layout.summary, &layout.author,
                        &layout.timestamp, &layout.id}) {
      if (rect->isValid())
        rect->adjust(4, top, -4, -top);
    }
    return layout;
  }

  void updateRefs() {
    mRefs.clear();

    if (mRepo.isHeadDetached()) {
      git::Reference head = mRepo.head();
      mRefs[head.target().id()].append(
          {Badge::Label::Type::Ref, head.name(), true});
    }

    foreach (const git::Reference &ref, mRepo.refs()) {
      if (git::Commit target = ref.target())
        mRefs[target.id()].append(
            {Badge::Label::Type::Ref, ref.name(), ref.isHead(), ref.isTag()});
    }
  }

  git::Repository mRepo;
  CommitAvatarProvider *mAvatars;
  QHeaderView *mHeader;
  QMap<git::Id, QList<Badge::Label>> mRefs;

};

class SelectionModel : public QItemSelectionModel {
public:
  SelectionModel(QAbstractItemModel *model) : QItemSelectionModel(model) {}

  void select(const QItemSelection &selection,
              QItemSelectionModel::SelectionFlags command) {
    if ((command == QItemSelectionModel::Select ||
         command == QItemSelectionModel::SelectCurrent ||
         command == (QItemSelectionModel::Current |
                     QItemSelectionModel::ClearAndSelect)) &&
        (selectedIndexes().size() >= 2 || selection.indexes().size() > 1))
      return;

    QItemSelectionModel::select(selection, command);
  }
};

} // namespace

static Hotkey selectCommitDownHotKey = HotkeyManager::registerHotkey(
    "j", "commitList/selectCommitDown", "CommitList/Select Next Commit Down");

static Hotkey selectCommitUpHotKey = HotkeyManager::registerHotkey(
    "k", "commitList/selectCommitUp", "CommitList/Select Next Commit Up");

CommitList::CommitList(Index *index, CommitAvatarProvider *avatars,
                       QWidget *parent)
    : QListView(parent), mIndex(index) {
  Theme *theme = Application::theme();
  setPalette(theme->commitList());

#ifdef Q_OS_MAC
  QFont font = this->font();
  font.setPointSize(13);
  setFont(font);
#endif

  git::Repository repo = index->repo();
  mList = new ListModel(this);
  mModel = new CommitModel(repo, this);
  setupHeader();
  viewport()->installEventFilter(this);
  connect(Settings::instance(), &Settings::settingsChanged, this,
          [this] { updateHeader(false); });

  setMouseTracking(true);
  setUniformItemSizes(true);
  setAttribute(Qt::WA_MacShowFocusRect, false);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

  setModel(mModel);
  setItemDelegate(new CommitDelegate(repo, avatars, mHeader, this));
  if (avatars) {
    connect(avatars, &CommitAvatarProvider::avatarReady, viewport(),
            qOverload<>(&QWidget::update));
    connect(avatars, &CommitAvatarProvider::avatarsChanged, viewport(),
            qOverload<>(&QWidget::update));
  }

  connect(mModel, &QAbstractItemModel::modelAboutToBeReset, this,
          &CommitList::storeSelection);
  connect(mModel, &QAbstractItemModel::modelReset, this,
           &CommitList::restoreSelection);
  connect(mList, &QAbstractItemModel::modelAboutToBeReset, this,
          &CommitList::storeSelection);
  connect(mList, &QAbstractItemModel::modelReset, this,
           &CommitList::restoreSelection);
  for (QAbstractItemModel *model : {mModel, mList}) {
    connect(model, &QAbstractItemModel::rowsInserted, this,
            [this] { updateGraphColumnWidth(); });
    connect(model, &QAbstractItemModel::modelReset, this,
            &CommitList::updateGraphColumnWidth);
  }
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            mHeader->setOffset(value);
            viewport()->update();
          });

  CommitModel *model = static_cast<CommitModel *>(mModel);
  connect(model, &CommitModel::statusFinished, [this, model](bool visible) {
    mRestoreSelection = true; // Reset to default

    // Select the detached HEAD commit when opening a repository that is not on
    // the tip. Otherwise the fallback below selects the newest visible commit.
    if (selectedIndexes().isEmpty() && model->isHeadDetached()) {
      git::Commit commit = model->reference().target();
      if (commit.isValid() && selectRange(commit.id().toString()))
        return;
    }

    // Select the first commit if the selection was cleared.
    if (selectedIndexes().isEmpty())
      selectFirstCommit();

    // Notify main window.
    emit statusChanged(visible);
  });

  git::RepositoryNotifier *notifier = repo.notifier();
  connect(notifier, &git::RepositoryNotifier::referenceUpdated,
          [this](const git::Reference &ref, bool restoreSelection) {
            mRestoreSelection = restoreSelection;
            resetReference(ref);
          });
  connect(notifier, &git::RepositoryNotifier::workdirChanged, [this] {
    resetReference(static_cast<const CommitModel *>(mModel)->reference());
  });

  connect(this, &CommitList::entered,
          [this](const QModelIndex &index) { update(index); });

  QShortcut *shortcut = new QShortcut(this);
  selectCommitDownHotKey.use(shortcut);
  connect(shortcut, &QShortcut::activated, [this] { selectCommitRelative(1); });

  shortcut = new QShortcut(this);
  selectCommitUpHotKey.use(shortcut);
  connect(shortcut, &QShortcut::activated,
          [this] { selectCommitRelative(-1); });

}

void CommitList::setupHeader() {
  mHeaderModel = new QStandardItemModel(0, ColumnCount, this);
  mHeaderModel->setHeaderData(ReferencesColumn, Qt::Horizontal,
                              tr("Branch / Tag"));
  mHeaderModel->setHeaderData(GraphColumn, Qt::Horizontal, tr("Graph"));
  mHeaderModel->setHeaderData(SummaryColumn, Qt::Horizontal,
                              tr("Commit Message"));
  mHeaderModel->setHeaderData(AuthorColumn, Qt::Horizontal, tr("Author"));
  mHeaderModel->setHeaderData(DateColumn, Qt::Horizontal, tr("Date / Time"));
  mHeaderModel->setHeaderData(IdColumn, Qt::Horizontal, tr("SHA"));

  mHeader = new QHeaderView(Qt::Horizontal, this);
  mHeader->installEventFilter(this);
  mHeader->setModel(mHeaderModel);
  mHeader->setSectionsMovable(true);
  mHeader->setSectionsClickable(false);
  mHeader->setHighlightSections(false);
  mHeader->setMinimumSectionSize(kSummaryMinimumWidth);
  mHeader->setDefaultAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  mHeader->setFixedHeight(kCommitHeaderHeight);
  mHeader->setFont(compactFont(mHeader->font()));
  for (int column = 0; column < ColumnCount; ++column)
    mHeader->setSectionResizeMode(column, QHeaderView::Interactive);

  mHeaderOptions = new ContextMenuButton(this);
  mHeaderOptions->setAccessibleName(tr("Configure commit columns"));
  QMenu *menu = new QMenu(mHeaderOptions);
  mHeaderOptions->setMenu(menu);
  for (int column = 0; column < ColumnCount; ++column) {
    QAction *action = menu->addAction(
        mHeaderModel->headerData(column, Qt::Horizontal).toString());
    action->setCheckable(true);
    action->setData(column);
    connect(action, &QAction::triggered, this, [this, column](bool visible) {
      mUpdatingHeader = true;
      mHeader->setSectionHidden(column, !visible);
      mUpdatingHeader = false;
      if (column == AuthorColumn)
        Settings::instance()->setValue(Setting::Id::ShowCommitsAuthor, visible);
      else if (column == DateColumn)
        Settings::instance()->setValue(Setting::Id::ShowCommitsDate, visible);
      else if (column == IdColumn)
        Settings::instance()->setValue(Setting::Id::ShowCommitsId, visible);
      resizeEvent(nullptr);
      saveHeaderState();
      doItemsLayout();
      viewport()->update();
    });
  }
  menu->addSeparator();
  menu->addAction(tr("Reset columns"), this, [this] {
    Settings::instance()->setValue(Setting::Id::ShowCommitsAuthor, true);
    Settings::instance()->setValue(Setting::Id::ShowCommitsDate, true);
    Settings::instance()->setValue(Setting::Id::ShowCommitsId, true);
    resetHeader();
  });
  connect(menu, &QMenu::aboutToShow, this, [this, menu] {
    for (QAction *action : menu->actions()) {
      if (action->isCheckable()) {
        int column = action->data().toInt();
        action->setChecked(!mHeader->isSectionHidden(column));
      }
    }
  });

  connect(mHeader, &QHeaderView::sectionResized, this,
          [this](int column, int, int size) {
            if (mUpdatingHeader)
              return;
            if (column == GraphColumn && mHeaderInteraction)
              mGraphPreferredWidth = size;
            resizeHeaderToFit(column);
            if (mHeaderInteraction)
              saveHeaderState();
            doItemsLayout();
            viewport()->update();
          });
  connect(mHeader, &QHeaderView::sectionMoved, this, [this] {
    saveHeaderState();
    doItemsLayout();
    viewport()->update();
  });

  QByteArray state = QSettings().value(kCommitHeaderStateKey).toByteArray();
  resetHeader(false);
  mPendingHeaderState = state;
  mResetHeaderOnShow = true;
  updateHeader(false);
}

void CommitList::resetHeader(bool saveState) {
  if (!mHeader)
    return;
  mUpdatingHeader = true;
  for (int column = 0; column < ColumnCount; ++column) {
    int visual = mHeader->visualIndex(column);
    if (visual != column)
      mHeader->moveSection(visual, column);
    mHeader->showSection(column);
  }

  int width = qMax(240, viewport()->width() - kCommitHeaderInset -
                            kCommitHeaderOptionsWidth);
  int refs = qBound(kReferencesMinimumWidth, width * 19 / 100, 360);
  int graph = qBound(kGraphMinimumWidth, width * 7 / 100, 160);
  int author = qBound(kAuthorMinimumWidth, width * 7 / 100, 120);
  int date = qBound(kDateMinimumWidth, width * 11 / 100, 160);
  int id = minimumColumnWidth(IdColumn);
  int summary = qMax(60, width - refs - graph - author - date - id);
  const int sizes[] = {refs, graph, summary, author, date, id};
  for (int column = 0; column < ColumnCount; ++column)
    mHeader->resizeSection(column, sizes[column]);
  mGraphPreferredWidth = graph;
  mUpdatingHeader = false;
  if (saveState)
    saveHeaderState();
  updateHeader(saveState);
}

void CommitList::saveHeaderState() {
  if (!mHeaderStateReady || mUpdatingHeader || !mHeader)
    return;

  int graphWidth = mHeader->sectionSize(GraphColumn);
  if (mGraphPreferredWidth > 0 && graphWidth != mGraphPreferredWidth) {
    mUpdatingHeader = true;
    mHeader->resizeSection(GraphColumn, mGraphPreferredWidth);
    QByteArray state = mHeader->saveState();
    mHeader->resizeSection(GraphColumn, graphWidth);
    mUpdatingHeader = false;
    QSettings().setValue(kCommitHeaderStateKey, state);
  } else {
    QSettings().setValue(kCommitHeaderStateKey, mHeader->saveState());
  }
}

int CommitList::minimumColumnWidth(int column) const {
  switch (column) {
    case ReferencesColumn:
      return kReferencesMinimumWidth;
    case GraphColumn:
      return qMax(kGraphMinimumWidth, mGraphMinimumWidth);
    case SummaryColumn:
      return kSummaryMinimumWidth;
    case AuthorColumn:
      return kAuthorMinimumWidth;
    case DateColumn:
      return kDateMinimumWidth;
    case IdColumn:
      return shortIdTextWidth(compactFont(font()), this) +
             kCompactColumnPadding + kShortIdMargin;
    default:
      return mHeader->minimumSectionSize();
  }
}

void CommitList::updateGraphColumnWidth() {
  QAbstractItemModel *graphModel = model();
  if (!graphModel)
    return;

  int laneWidth = qMax(QFontMetrics(compactFont(font()), this).ascent(),
                       kGraphNodeSize + 4);
  int minimum = kGraphMinimumWidth;
  for (int row = 0; row < graphModel->rowCount(); ++row) {
    int lanes = graphModel->index(row, 0).data(GraphRole).toList().size();
    minimum = qMax(minimum, lanes * laneWidth);
  }

  mGraphMinimumWidth = minimum;
  if (!mHeader || mHeader->isSectionHidden(GraphColumn))
    return;

  int current = mHeader->sectionSize(GraphColumn);
  int target = qMax(mGraphPreferredWidth, minimum);
  if (current == target)
    return;

  mUpdatingHeader = true;
  mHeader->resizeSection(GraphColumn, target);
  mUpdatingHeader = false;
  resizeHeaderToFit(GraphColumn);
  doItemsLayout();
  viewport()->update();
}

void CommitList::resizeHeaderToFit(int protectedColumn) {
  if (!mHeader || mHeader->width() <= 0)
    return;

  auto updateScrollPolicy = [this] {
    setHorizontalScrollBarPolicy(mHeader->length() > mHeader->width()
                                     ? Qt::ScrollBarAsNeeded
                                     : Qt::ScrollBarAlwaysOff);
  };

  bool updating = mUpdatingHeader;
  mUpdatingHeader = true;
  for (int column = 0; column < ColumnCount; ++column) {
    if (!mHeader->isSectionHidden(column) &&
        mHeader->sectionSize(column) < minimumColumnWidth(column)) {
      mHeader->resizeSection(column, minimumColumnWidth(column));
    }
  }

  int delta = mHeader->width() - mHeader->length();
  if (delta == 0) {
    updateScrollPolicy();
    mUpdatingHeader = updating;
    return;
  }

  QList<int> columns;
  if (SummaryColumn != protectedColumn &&
      !mHeader->isSectionHidden(SummaryColumn))
    columns.append(SummaryColumn);
  for (int column = 0; column < ColumnCount; ++column) {
    if (column != protectedColumn && column != SummaryColumn &&
        !mHeader->isSectionHidden(column))
      columns.append(column);
  }
  if (protectedColumn >= 0 && !mHeader->isSectionHidden(protectedColumn))
    columns.append(protectedColumn);
  if (columns.isEmpty()) {
    updateScrollPolicy();
    mUpdatingHeader = updating;
    return;
  }

  if (delta > 0) {
    int column = columns.constFirst();
    mHeader->resizeSection(column, mHeader->sectionSize(column) + delta);
  } else {
    int remaining = -delta;
    for (int column : columns) {
      int available =
          mHeader->sectionSize(column) - minimumColumnWidth(column);
      int shrink = qMin(remaining, qMax(0, available));
      if (shrink > 0)
        mHeader->resizeSection(column, mHeader->sectionSize(column) - shrink);
      remaining -= shrink;
      if (remaining == 0)
        break;
    }
  }
  updateScrollPolicy();
  mUpdatingHeader = updating;
}

void CommitList::updateHeader(bool saveState) {
  if (!mHeader)
    return;
  bool updating = mUpdatingHeader;
  mUpdatingHeader = true;
  bool compact = Settings::instance()
                     ->value(Setting::Id::ShowCommitsInCompactMode)
                     .toBool();
  setViewportMargins(0, compact ? kCommitHeaderHeight : 0, 0, 0);
  mHeader->setVisible(compact);
  mHeaderOptions->setVisible(compact);
  if (compact) {
    mHeader->setSectionHidden(
        AuthorColumn,
        !Settings::instance()->value(Setting::Id::ShowCommitsAuthor, true).toBool());
    mHeader->setSectionHidden(
        DateColumn,
        !Settings::instance()->value(Setting::Id::ShowCommitsDate, true).toBool());
    mHeader->setSectionHidden(
        IdColumn,
        !Settings::instance()->value(Setting::Id::ShowCommitsId, true).toBool());
  }
  resizeEvent(nullptr);
  resizeHeaderToFit();
  mUpdatingHeader = updating;
  if (saveState)
    saveHeaderState();
  doItemsLayout();
  viewport()->update();
}

git::Diff CommitList::status() const {
  return static_cast<CommitModel *>(mModel)->status();
}

QString CommitList::selectedRange() const {
  QList<git::Commit> commits = selectedCommits();
  if (commits.isEmpty())
    return !selectedIndexes().isEmpty() ? "status" : QString();

  git::Commit first = commits.first();
  if (commits.size() == 1)
    return first.id().toString();

  git::Commit last = commits.last();
  return QString("%1..%2").arg(last.id().toString(), first.id().toString());
}

git::Diff CommitList::selectedDiff() const {
  QModelIndexList indexes = sortedIndexes();
  DebugRefresh("Selected indices count: " << indexes.count());
  for (const auto &index : indexes) {
    const auto &id = index.data(CommitRole).value<git::Commit>().shortId();
    (void)id; // Unused in release builds
    DebugRefresh("Commit: " << id);
  }
  if (indexes.isEmpty())
    return git::Diff();

  if (indexes.size() == 1) {
    auto first = indexes.first().data(DiffRole);
    return first.isValid() ? first.value<git::Diff>() : git::Diff();
  }

  git::Commit first = indexes.first().data(CommitRole).value<git::Commit>();
  if (!first.isValid())
    return git::Diff();

  git::Commit last = indexes.last().data(CommitRole).value<git::Commit>();
  bool ignoreWhitespace = Settings::instance()->isWhitespaceIgnored();
  git::Diff diff = first.diff(last, -1, ignoreWhitespace);
  diff.findSimilar();
  return diff;
}

QList<git::Commit> CommitList::selectedCommits() const {
  QList<git::Commit> selectedCommits;
  foreach (const QModelIndex &index, sortedIndexes()) {
    git::Commit commit = index.data(CommitRole).value<git::Commit>();
    if (commit.isValid())
      selectedCommits.append(commit);
  }

  return selectedCommits;
}

void CommitList::cancelStatus() {
  static_cast<CommitModel *>(mModel)->cancelStatus();
}

void CommitList::setReference(const git::Reference &ref) {
  static_cast<CommitModel *>(mModel)->setReference(ref);
  if (!isResetWalkerSuppressed())
    updateModel();
  setFocus();
}

void CommitList::setFilter(const QString &filter) {
  mFilter = filter.simplified();
  updateModel();
}

void CommitList::setPathspec(const QString &pathspec, bool index) {
  if (index) {
    setFilter(!pathspec.isEmpty() ? kPathspecFmt.arg(pathspec) : QString());
  } else {
    static_cast<CommitModel *>(mModel)->setPathspec(pathspec);
  }
}

void CommitList::setCommits(const QList<git::Commit> &commits) {
  setModel(mList);
  static_cast<ListModel *>(mList)->setList(commits);
}

void CommitList::selectReference(const git::Reference &ref) {
  if (!ref.isValid())
    return;

  QModelIndex index = model()->index(0, 0);
  if (ref.isHead() && !index.data(CommitRole).isValid()) {
    selectFirstCommit();
  } else {
    selectRange(ref.target().id().toString());
  }
}

void CommitList::resetSelection(bool spontaneous) {
  // Just notify.
  mSpontaneous = spontaneous;
  notifySelectionChanged();
  mSpontaneous = true;
}

void CommitList::selectFirstCommit(bool spontaneous) {
  QModelIndex index = model()->index(0, 0);
  const auto commit = index.data(CommitRole).value<git::Commit>();
  if (commit.isValid())
    DebugRefresh("Commit id: " << commit.shortId());
  else
    DebugRefresh("Invalid commit");
  if (index.isValid()) {
    selectIndexes(QItemSelection(index, index), QString(), spontaneous);
  } else {
    emit diffSelected(git::Diff());
  }
}

void CommitList::selectCommitRelative(int offset) {
  QModelIndexList indices = selectionModel()->selectedIndexes();
  QModelIndex index = indices[0];
  if (!index.isValid()) {
    return;
  }
  QModelIndex new_index = model()->index(index.row() + offset, index.column());
  if (!new_index.isValid()) {
    return;
  }
  selectIndexes(QItemSelection(new_index, new_index), QString(), true);
}

bool CommitList::selectRange(const QString &range, const QString &file,
                             bool spontaneous) {
  // Try to select the "status" index.
  QModelIndex index = model()->index(0, 0);
  if (range == "status" && !index.data(CommitRole).isValid()) {
    return true;
  }

  QStringList ids = range.split("..");
  if (ids.size() > 2)
    return false;

  // Invert range.
  bool one = (ids.size() == 1);
  git::Repository repo = RepoView::parentView(this)->repo();
  git::Commit firstCommit = repo.lookupCommit(ids.last());
  git::Commit lastCommit = one ? firstCommit : repo.lookupCommit(ids.first());

  // Check for already selected range.
  QModelIndexList indexes = sortedIndexes();
  if (indexes.size() >= 2) {
    git::Commit first = indexes.first().data(CommitRole).value<git::Commit>();
    git::Commit last = indexes.last().data(CommitRole).value<git::Commit>();
    if (first.isValid() && first == firstCommit && last.isValid() &&
        last == lastCommit)
      return false;
  }

  // Find indexes.
  QItemSelection selection;
  QModelIndex first = findCommit(firstCommit);
  if (!first.isValid())
    return false;
  selection.select(first, first);

  if (lastCommit != firstCommit) {
    QModelIndex last = findCommit(lastCommit);
    if (!last.isValid())
      return false;
    selection.select(last, last);
  }

  selectIndexes(selection, file, spontaneous);
  return true;
}

void CommitList::suppressResetWalker(bool suppress) {
  static_cast<CommitModel *>(mModel)->suppressResetWalker(suppress);
}

void CommitList::resetReference(const git::Reference &ref) {
  static_cast<CommitModel *>(mModel)->resetReference(ref);
}

bool CommitList::isResetWalkerSuppressed() {
  return static_cast<CommitModel *>(mModel)->isResetWalkerSuppressed();
}

void CommitList::resetSettings() {
  updateHeader();
  static_cast<CommitModel *>(mModel)->resetSettings(true);
}

void CommitList::setModel(QAbstractItemModel *model) {
  if (model == this->model())
    return;

  storeSelection();

  // Destroy the previous selection model.
  delete selectionModel();

  QListView::setModel(model);
  updateGraphColumnWidth();

  // Destroy the selection model created by Qt.
  delete selectionModel();

  SelectionModel *selectionModel = new SelectionModel(model);
  connect(
      selectionModel, &QItemSelectionModel::selectionChanged,
      [this](const QItemSelection &selected, const QItemSelection &deselected) {
        // Update the index before each selected/deselected range.
        foreach (const QItemSelectionRange &range, selected + deselected) {
          if (int row = range.top())
            update(this->model()->index(row - 1, 0));
        }

        notifySelectionChanged();
      });

  setSelectionModel(selectionModel);

  restoreSelection();
}

/// @brief Helper function to add a list of items to a menu.
/// A single item is added directly to the menu, whereas multiple items will
/// be added to a sub-menu.
static void addMenuEntries(QMenu &menu, const QString &operation,
                           const QList<git::Reference> &items,
                           std::function<void(const git::Reference &)> action) {
  QMenu *submenu = &menu;
  QString entryName(operation + " %1");
  if (items.count() > 1) {
    submenu = menu.addMenu(operation);
    entryName = QString("%1");
  }
  for (const git::Reference &ref : items) {
    submenu->addAction(entryName.arg(ref.name()),
                       [action, ref] { action(ref); });
  }
}

void CommitList::contextMenuEvent(QContextMenuEvent *event) {
  QModelIndex index = indexAt(event->pos());
  if (!index.isValid())
    return;

  RepoView *view = RepoView::parentView(this);
  git::Commit commit = index.data(CommitRole).value<git::Commit>();

  if (!commit.isValid()) {
    QMenu menu;

    // clean
    QStringList untracked;
    if (git::Diff diff = status()) {
      for (int i = 0; i < diff.count(); i++) {
        if (diff.status(i) == GIT_DELTA_UNTRACKED)
          untracked.append(diff.name(i));
      }
    }

    QAction *clean =
        menu.addAction(tr("Remove Untracked Files"),
                       [view, untracked] { view->clean(untracked); });

    clean->setEnabled(!untracked.isEmpty());

    menu.exec(event->globalPos());
    return;
  }

  QMenu menu;
  menu.setToolTipsVisible(true);

  // stash
  git::Reference ref = static_cast<CommitModel *>(mModel)->reference();
  QVariant integratedStash = index.data(StashIndexRole);
  if (integratedStash.isValid() || (ref.isValid() && ref.isStash())) {
    int stashIndex =
        integratedStash.isValid() ? integratedStash.toInt() : index.row();
    menu.addAction(tr("Apply"),
                   [view, stashIndex] { view->applyStash(stashIndex); });

    menu.addAction(tr("Pop"),
                   [view, stashIndex] { view->popStash(stashIndex); });

    menu.addAction(tr("Drop"),
                   [view, stashIndex] { view->dropStash(stashIndex); });

  } else {
    // multiple selection
    bool anyStarred = false;
    foreach (const QModelIndex &index, selectionModel()->selectedIndexes()) {
      if (index.data(CommitRole).isValid() &&
          index.data(CommitRole).value<git::Commit>().isStarred()) {
        anyStarred = true;
        break;
      }
    }

    menu.addAction(anyStarred ? tr("Unstar") : tr("Star"), [this, anyStarred] {
      foreach (const QModelIndex &index, selectionModel()->selectedIndexes())
        if (index.data(CommitRole).isValid())
          index.data(CommitRole).value<git::Commit>().setStarred(!anyStarred);
    });

    // single selection
    if (selectionModel()->selectedIndexes().size() <= 1) {
      menu.addSeparator();

      menu.addAction(tr("Add Tag..."),
                     [view, commit] { view->promptToAddTag(commit); });

      menu.addAction(tr("New Branch..."),
                     [view, commit] { view->promptToCreateBranch(commit); });

      // Add operations on existing references; there may be 0, 1, or multiple
      // of each type of reference on a commit.
      QList<git::Reference> rename_branches;
      QList<git::Reference> tags;
      QList<git::Reference> delete_branches;
      QList<git::Reference> all_branches; // used later
      for (const git::Reference &ref : commit.refs()) {
        if (ref.isTag()) {
          tags.append(ref);
        } else if (ref.isBranch()) {
          all_branches.append(ref);
          if (ref.isLocalBranch()) {
            rename_branches.append(ref);
            if (view->repo().head().name() != ref.name()) {
              delete_branches.append(ref);
            }
          }
        }
      }

      if (rename_branches.count() > 0 || delete_branches.count() > 0 ||
          tags.count() > 0) {
        menu.addSeparator();
      }
      addMenuEntries(menu, tr("Rename Branch"), rename_branches,
                     std::bind(&RepoView::promptToRenameBranch, view,
                               std::placeholders::_1));

      addMenuEntries(menu, tr("Delete Branch"), delete_branches,
                     std::bind(&RepoView::promptToDeleteBranch, view,
                               std::placeholders::_1));

      addMenuEntries(
          menu, tr("Delete Tag"), tags,
          std::bind(&RepoView::promptToDeleteTag, view, std::placeholders::_1));
      menu.addSeparator();

      menu.addAction(tr("Merge..."), [view, commit] {
        MergeDialog *dialog =
            new MergeDialog(RepoView::Merge, view->repo(), view);
        connect(dialog, &QDialog::accepted, [view, dialog] {
          git::AnnotatedCommit upstream;
          git::Reference ref = dialog->reference();
          if (!ref.isValid())
            upstream = dialog->target().annotatedCommit();
          view->merge(dialog->flags(), ref, upstream);
        });

        dialog->setCommit(commit);
        dialog->open();
      });

      menu.addAction(tr("Rebase..."), [view, commit] {
        MergeDialog *dialog =
            new MergeDialog(RepoView::Rebase, view->repo(), view);
        connect(dialog, &QDialog::accepted, [view, dialog] {
          git::AnnotatedCommit upstream;
          git::Reference ref = dialog->reference();
          if (!ref.isValid())
            upstream = dialog->target().annotatedCommit();
          view->merge(dialog->flags(), ref, upstream);
        });

        dialog->setCommit(commit);
        dialog->open();
      });

      menu.addAction(tr("Squash..."), [view, commit] {
        MergeDialog *dialog =
            new MergeDialog(RepoView::Squash, view->repo(), view);
        connect(dialog, &QDialog::accepted, [view, dialog] {
          git::AnnotatedCommit upstream;
          git::Reference ref = dialog->reference();
          if (!ref.isValid())
            upstream = dialog->target().annotatedCommit();
          view->merge(dialog->flags(), ref, upstream);
        });

        dialog->setCommit(commit);
        dialog->open();
      });

      menu.addSeparator();

      menu.addAction(tr("Revert"), [view, commit] { view->revert(commit); });

      menu.addAction(tr("Cherry-pick"),
                     [view, commit] { view->cherryPick(commit); });

      menu.addSeparator();

      git::Reference head = view->repo().head();
      auto submenu = &menu;
      auto entryName = tr("Checkout %1");
      if (all_branches.count() > 1) {
        submenu = menu.addMenu(tr("Checkout"));
        entryName = QString("%1");
      }
      for (const git::Reference &ref : all_branches) {
        if (ref.isLocalBranch()) {
          QAction *checkout = submenu->addAction(
              entryName.arg(ref.name()), [view, ref] { view->checkout(ref); });

          checkout->setEnabled(head.isValid() &&
                               head.qualifiedName() != ref.qualifiedName() &&
                               !view->repo().isBare());
        } else if (ref.isRemoteBranch()) {
          QAction *checkout = submenu->addAction(
              entryName.arg(ref.name()), [view, ref] { view->checkout(ref); });

          // Calculate local branch name in the same way as checkout() does
          QString local = ref.name().section('/', 1);
          if (!head.isValid()) { // I'm not sure when this can happen
            checkout->setEnabled(false);
          } else if (head.name() == local) {
            checkout->setEnabled(false);
            checkout->setToolTip(tr("Local branch is already checked out"));
          } else if (view->repo().isBare()) {
            checkout->setEnabled(false);
            checkout->setToolTip(tr("This is a bare repository"));
          }
        }
      }

      QString name = commit.detachedHeadName();
      QAction *checkout =
          menu.addAction(tr("Checkout %1").arg(name),
                         [view, commit] { view->checkout(commit); });

      checkout->setEnabled(head.isValid() && head.target() != commit &&
                           !view->repo().isBare());

      menu.addSeparator();

      QMenu *reset = menu.addMenu(tr("Reset"));
      reset->addAction(tr("Soft"))->setData(GIT_RESET_SOFT);
      reset->addAction(tr("Mixed"))->setData(GIT_RESET_MIXED);
      reset->addAction(tr("Hard"))->setData(GIT_RESET_HARD);
      connect(reset, &QMenu::triggered, [view, commit](QAction *action) {
        git_reset_t type = static_cast<git_reset_t>(action->data().toInt());
        view->promptToReset(commit, type);
      });

      reset->setEnabled(head.isValid() && head.isLocalBranch());
    }
  }

  menu.exec(event->globalPos());
}

void CommitList::mouseMoveEvent(QMouseEvent *event) {
  if (mStar.isValid() || mCancel.isValid())
    return;

  QListView::mouseMoveEvent(event);
}

void CommitList::mousePressEvent(QMouseEvent *event) {
  QPoint pos = event->pos();
  QModelIndex index = indexAt(pos);
  mStar = isStar(index, pos) ? index : QModelIndex();
  mCancel = isDecoration(index, pos) ? index : QModelIndex();

  if (mStar.isValid() || mCancel.isValid())
    return;

  DebugRefresh("time: " << QDateTime::currentDateTime());

  QListView::mousePressEvent(event);
}

void CommitList::mouseReleaseEvent(QMouseEvent *event) {
  QPoint pos = event->pos();
  QModelIndex index = indexAt(pos);
  if (mStar == index && isStar(index, pos)) {
    if (git::Commit commit = index.data(CommitRole).value<git::Commit>()) {
      commit.setStarred(!commit.isStarred());
      update(index); // FIXME: Add signal?
    }
  } else if (mCancel == index && isDecoration(index, pos)) {
    static_cast<CommitModel *>(model())->cancelStatus();
  }

  mStar = QModelIndex();
  mCancel = QModelIndex();

  QListView::mouseReleaseEvent(event);
}

void CommitList::leaveEvent(QEvent *event) {
  viewport()->update();
  QListView::leaveEvent(event);
}

void CommitList::resizeEvent(QResizeEvent *event) {
  if (event)
    QListView::resizeEvent(event);
  if (!mHeader)
    return;

  int frame = frameWidth();
  int available = qMax(1, viewport()->width() - kCommitHeaderInset -
                               kCommitHeaderOptionsWidth);
  int headerX = frame + kCommitHeaderInset;
  mHeader->setGeometry(headerX, frame, available, kCommitHeaderHeight);
  mHeader->setOffset(horizontalScrollBar()->value());
  mHeaderOptions->setGeometry(headerX + available, frame,
                              kCommitHeaderOptionsWidth,
                              kCommitHeaderHeight);

  if (!mUpdatingHeader) {
    resizeHeaderToFit();
  }
}

void CommitList::showEvent(QShowEvent *event) {
  QListView::showEvent(event);
  if (!mResetHeaderOnShow)
    return;

  mResetHeaderOnShow = false;
  QTimer::singleShot(100, this, [this] {
    mHeaderStateReady = true;
    if (mPendingHeaderState.isEmpty()) {
      resetHeader();
      return;
    }

    mUpdatingHeader = true;
    mHeader->restoreState(mPendingHeaderState);
    mUpdatingHeader = false;
    mPendingHeaderState.clear();
    mGraphPreferredWidth = mHeader->sectionSize(GraphColumn);
    updateHeader(false);
  });
}

bool CommitList::eventFilter(QObject *watched, QEvent *event) {
  if (watched == mHeader) {
    if (event->type() == QEvent::MouseButtonPress) {
      mHeaderInteraction = true;
    } else if (event->type() == QEvent::MouseButtonRelease) {
      mHeaderInteraction = false;
      saveHeaderState();
    }
  }
  if (watched == viewport() && event->type() == QEvent::Resize && mHeader)
    resizeEvent(nullptr);
  return QListView::eventFilter(watched, event);
}

void CommitList::storeSelection() {
  mSelectedRange = selectedRange();
  DebugRefresh("Selected Range: " << mSelectedRange);
  Debug(mSelectedRange);
}

void CommitList::restoreSelection() {
  // Restore selection.
  DebugRefresh(mSelectedRange);
  if (!mRestoreSelection ||
      (!mSelectedRange.isEmpty() && mSelectedRange != "status" &&
       !selectRange(mSelectedRange))) {
    DebugRefresh("Failed to restore");
    emit diffSelected(git::Diff());
  }

  mSelectedRange = QString();
  mRestoreSelection = true;
}

void CommitList::updateModel() {
  if (!mFilter.isEmpty()) {
    setCommits(mIndex->commits(mFilter));
    return;
  }

  git::Reference ref = static_cast<CommitModel *>(mModel)->reference();
  if (ref.isValid() && ref.isStash()) {
    setCommits(ref.repo().stashes());
    return;
  }

  // Reset model.
  setModel(mModel);
}

QModelIndexList CommitList::sortedIndexes() const {
  QModelIndexList indexes = selectedIndexes();
  std::sort(indexes.begin(), indexes.end(),
            [](const QModelIndex &lhs, const QModelIndex &rhs) {
              return lhs.row() < rhs.row();
            });

  return indexes;
}

QModelIndex CommitList::findCommit(const git::Commit &commit) {
  // Get the 'uncommitted changes' index.
  QAbstractItemModel *model = this->model();
  if (!commit.isValid()) {
    QModelIndex index = model->index(0, 0);
    git::Commit tmp = index.data(CommitRole).value<git::Commit>();
    return !tmp.isValid() ? index : QModelIndex();
  }

  // Find the id.
  QDateTime date = commit.committer().date();
  for (int i = 0; i < model->rowCount(); ++i) {
    QModelIndex index = model->index(i, 0);
    if (git::Commit tmp = index.data(CommitRole).value<git::Commit>()) {
      if (tmp == commit)
        return index;

      // Cut off search if we find an older commit.
      if (tmp.committer().date() < date)
        return QModelIndex();
    }

    // Load more commits.
    if (i == model->rowCount() - 1 && model->canFetchMore(QModelIndex()))
      model->fetchMore(QModelIndex());
  }

  return QModelIndex();
}

void CommitList::selectIndexes(const QItemSelection &selection,
                               const QString &file, bool spontaneous) {
  mFile = file;
  mSpontaneous = spontaneous;
  selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
  mSpontaneous = true;
  mFile = QString();

  QModelIndexList indexes = selection.indexes();
  if (!indexes.isEmpty())
    scrollTo(indexes.first());
}

void CommitList::notifySelectionChanged() {
  // Multiple selection means that the selected parameter
  // could be empty when there are still indexes selected.
  QModelIndexList indexes = selectedIndexes();
  if (indexes.isEmpty())
    return;

  // Redraw all selected indexes. Separators may have changed.
  foreach (const QModelIndex &index, indexes)
    update(index);
  git::Diff diff = selectedDiff();
  emit diffSelected(diff, mFile, mSpontaneous);
}

bool CommitList::isDecoration(const QModelIndex &index, const QPoint &pos) {
  if (!index.isValid())
    return false;

  CommitDelegate *delegate = static_cast<CommitDelegate *>(itemDelegate());
  QStyleOptionViewItem options;
  initViewItemOption(&options);
  options.rect = visualRect(index);
  return delegate->decorationRect(options, index).contains(pos);
}

bool CommitList::isStar(const QModelIndex &index, const QPoint &pos) {
  if (!index.isValid() || !index.data(CommitRole).isValid())
    return false;

  CommitDelegate *delegate = static_cast<CommitDelegate *>(itemDelegate());
  QStyleOptionViewItem options;
  initViewItemOption(&options);
  options.rect = visualRect(index);
  return delegate->starRect(options, index).contains(pos);
}

#include "CommitList.moc"

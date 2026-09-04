//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryNavigatorModel.h"
#include "git/Branch.h"
#include "git/TagRef.h"
#include "git/Worktree.h"
#include <QUrl>
#include <algorithm>

namespace {

bool compareCommits(const git::Repository &repo, const git::Id &checkoutId,
                    const git::Id &referenceId, int &ahead, int &behind) {
  git::Commit checkout = repo.lookupCommit(checkoutId);
  git::Commit reference = repo.lookupCommit(referenceId);
  if (!checkout.isValid() || !reference.isValid())
    return false;

  ahead = checkout.difference(reference);
  behind = reference.difference(checkout);
  return true;
}

QString commitCount(int count) {
  return count == 1
             ? RepositoryNavigatorModel::tr("1 commit")
             : RepositoryNavigatorModel::tr("%1 commits").arg(count);
}

QString comparisonText(const QString &reference, int ahead, int behind) {
  if (ahead < 0 || behind < 0)
    return RepositoryNavigatorModel::tr("Comparison is unavailable.");
  if (!ahead && !behind)
    return RepositoryNavigatorModel::tr("The local checkout matches %1.")
        .arg(reference);
  if (ahead && behind)
    return RepositoryNavigatorModel::tr(
               "The local checkout and %1 have diverged (%2 ahead, %3 "
               "behind).")
        .arg(reference, commitCount(ahead), commitCount(behind));
  if (ahead)
    return RepositoryNavigatorModel::tr(
               "The local checkout is %1 ahead of %2.")
        .arg(commitCount(ahead), reference);
  return RepositoryNavigatorModel::tr(
             "The local checkout is %1 behind %2.")
      .arg(commitCount(behind), reference);
}

QString coloredText(const QString &text, const QString &color) {
  return QString("<span style=\"color:%1; font-weight:600\">%2</span>")
      .arg(color, text.toHtmlEscaped());
}

} // namespace

bool RepositoryNavigatorModel::lessThan(
    const RepositoryNavigatorModel::Row &lhs,
    const RepositoryNavigatorModel::Row &rhs) {
  return QString::localeAwareCompare(lhs.display, rhs.display) < 0;
}

RepositoryNavigatorModel::RepositoryNavigatorModel(QObject *parent)
    : QAbstractItemModel(parent) {
  mRefreshTimer.setSingleShot(true);
  mRefreshTimer.setInterval(50);
  connect(&mRefreshTimer, &QTimer::timeout, this,
          &RepositoryNavigatorModel::refresh);
  rebuild();
}

void RepositoryNavigatorModel::setRepository(const git::Repository &repo) {
  mRefreshTimer.stop();
  disconnectRepository();
  beginResetModel();
  mRepo = repo;
  mSubmoduleUpdateStatuses.clear();
  mGitHubIssuesAvailable = false;
  mGitHubIssuesState = LoadState::Unavailable;
  mGitHubIssues.clear();
  mGitHubIssuesError.clear();
  rebuild();
  endResetModel();
  connectRepository();
}

void RepositoryNavigatorModel::setGitHubIssuesAvailable(bool available) {
  if (mGitHubIssuesAvailable == available)
    return;
  beginResetModel();
  mGitHubIssuesAvailable = available;
  if (!available) {
    mGitHubIssues.clear();
    mGitHubIssuesError.clear();
    mGitHubIssuesFilter.clear();
    mGitHubIssuesState = LoadState::Unavailable;
  }
  rebuildGitHubIssuesSection();
  endResetModel();
}

void RepositoryNavigatorModel::setGitHubIssuesFilter(const QString &filter) {
  if (mGitHubIssuesFilter == filter)
    return;
  beginResetModel();
  mGitHubIssuesFilter = filter;
  rebuildGitHubIssuesSection();
  endResetModel();
}

void RepositoryNavigatorModel::beginGitHubIssuesLoad(bool refresh) {
  beginResetModel();
  if (!refresh)
    mGitHubIssues.clear();
  mGitHubIssuesState = refresh ? LoadState::Refreshing : LoadState::Loading;
  mGitHubIssuesError.clear();
  rebuildGitHubIssuesSection();
  endResetModel();
}

void RepositoryNavigatorModel::setGitHubIssues(const GitHub::Issues &issues) {
  beginResetModel();
  mGitHubIssues = issues;
  mGitHubIssuesError.clear();
  mGitHubIssuesState = LoadState::Ready;
  rebuildGitHubIssuesSection();
  endResetModel();
}

void RepositoryNavigatorModel::failGitHubIssues(const QString &message,
                                                bool preserveIssues) {
  beginResetModel();
  if (!preserveIssues)
    mGitHubIssues.clear();
  mGitHubIssuesError = message;
  mGitHubIssuesState = preserveIssues ? LoadState::Stale : LoadState::Failed;
  rebuildGitHubIssuesSection();
  endResetModel();
}

void RepositoryNavigatorModel::clear() { setRepository(git::Repository()); }

git::Repository RepositoryNavigatorModel::repository() const { return mRepo; }

void RepositoryNavigatorModel::setSubmoduleUpdateStatuses(
    const QList<git::Submodule::UpdateStatus> &statuses) {
  if (statuses.isEmpty() && mSubmoduleUpdateStatuses.isEmpty())
    return;

  QHash<QString, git::Submodule::UpdateStatus> updated;
  for (const git::Submodule::UpdateStatus &status : statuses)
    updated.insert(status.name, status);

  beginResetModel();
  mSubmoduleUpdateStatuses = updated;
  rebuild();
  endResetModel();
}

void RepositoryNavigatorModel::setBusySubmodulePaths(const QStringList &paths) {
  if (mBusySubmodulePaths == paths)
    return;

  mBusySubmodulePaths = paths;
  const QModelIndex section = sectionIndex(Section::Submodules);
  const int rows = rowCount(section);
  if (rows > 0)
    emit dataChanged(index(0, 0, section), index(rows - 1, 0, section),
                     {SubmoduleBusyRole});
}

QModelIndex RepositoryNavigatorModel::sectionIndex(
    RepositoryNavigatorModel::Section section) const {
  int row = static_cast<int>(section);
  return row >= 0 && row < mSections.size() ? index(row, 0) : QModelIndex();
}

QModelIndex RepositoryNavigatorModel::index(int row, int column,
                                             const QModelIndex &parent) const {
  if (column != 0 || row < 0)
    return QModelIndex();

  if (!parent.isValid()) {
    if (row >= mSections.size())
      return QModelIndex();
    return createIndex(row, column, quintptr(0));
  }

  if (!isSection(parent) || row >= mSections.at(parent.row()).rows.size())
    return QModelIndex();

  return createIndex(row, column, quintptr(parent.row() + 1));
}

QModelIndex RepositoryNavigatorModel::parent(const QModelIndex &index) const {
  if (!isItem(index))
    return QModelIndex();

  return createIndex(static_cast<int>(index.internalId()) - 1, 0, quintptr(0));
}

int RepositoryNavigatorModel::rowCount(const QModelIndex &parent) const {
  if (!parent.isValid())
    return mSections.size();
  return isSection(parent) ? mSections.at(parent.row()).rows.size() : 0;
}

int RepositoryNavigatorModel::columnCount(const QModelIndex &parent) const {
  return 1;
}

QVariant RepositoryNavigatorModel::data(const QModelIndex &index,
                                        int role) const {
  const SectionData *section = sectionData(index);
  if (!section)
    return QVariant();

  const Row *row = rowData(index);
  if (!row) {
    switch (role) {
      case Qt::DisplayRole:
        return section->display;
      case Qt::ToolTipRole:
        return section->tooltip;
      case SectionRole:
        return static_cast<int>(section->section);
      case ItemKindRole:
        return static_cast<int>(ItemKind::Section);
      case CountRole:
        if (section->section == Section::GitHubIssues)
          return mGitHubIssues.size();
        return section->rows.size();
      case AvailableRole:
        return section->available;
      case LoadStateRole:
        return section->section == Section::GitHubIssues
                   ? QVariant::fromValue(mGitHubIssuesState)
                   : QVariant();
      default:
        return QVariant();
    }
  }

  switch (role) {
    case Qt::DisplayRole:
      return row->display;
    case Qt::ToolTipRole:
      return row->tooltip;
    case SectionRole:
      return static_cast<int>(section->section);
    case ItemKindRole:
      return static_cast<int>(row->kind);
    case AvailableRole:
      return row->available && row->kind != ItemKind::Status;
    case CurrentRole:
      return row->current;
    case MainWorktreeRole:
      return row->kind == ItemKind::Worktree ? QVariant(row->mainWorktree)
                                             : QVariant();
    case WorktreeRole:
      return row->kind == ItemKind::Worktree
                 ? QVariant::fromValue(row->worktree)
                 : QVariant();
    case AheadRole:
      return row->ahead >= 0 ? QVariant(row->ahead) : QVariant();
    case BehindRole:
      return row->behind >= 0 ? QVariant(row->behind) : QVariant();
    case ReferenceRole:
      return row->reference.isValid() ? QVariant::fromValue(row->reference)
                                      : QVariant();
    case CommitRole:
      return row->commit.isValid() ? QVariant::fromValue(row->commit)
                                   : QVariant();
    case StashIndexRole:
      return row->stashIndex >= 0 ? QVariant(row->stashIndex) : QVariant();
    case SubmoduleRole:
      return row->submodule.isValid() ? QVariant::fromValue(row->submodule)
                                      : QVariant();
    case PathRole:
      return row->path;
    case UrlRole:
      return row->url;
    case BranchRole:
      return row->branch;
    case InitializedRole:
      return row->kind == ItemKind::Submodule ? QVariant(row->initialized)
                                              : QVariant();
    case PinnedAheadRole:
      return row->pinnedAhead >= 0 ? QVariant(row->pinnedAhead) : QVariant();
    case PinnedBehindRole:
      return row->pinnedBehind >= 0 ? QVariant(row->pinnedBehind) : QVariant();
    case OriginAheadRole:
      return row->originAhead >= 0 ? QVariant(row->originAhead) : QVariant();
    case OriginBehindRole:
      return row->originBehind >= 0 ? QVariant(row->originBehind) : QVariant();
    case OriginStateRole:
      return row->kind == ItemKind::Submodule
                 ? QVariant::fromValue(row->originState)
                 : QVariant();
    case OriginTargetRole:
      return row->originTarget.isValid()
                  ? QVariant::fromValue(row->originTarget)
                   : QVariant();
    case SubmoduleBusyRole:
      return row->kind == ItemKind::Submodule &&
             mBusySubmodulePaths.contains(row->path);
    case LoadStateRole:
      return section->section == Section::GitHubIssues
                 ? QVariant::fromValue(mGitHubIssuesState)
                 : QVariant();
    case IssueNumberRole:
      return row->kind == ItemKind::GitHubIssue ? QVariant(row->issueNumber)
                                                : QVariant();
    case IssueAuthorRole:
      return row->kind == ItemKind::GitHubIssue ? QVariant(row->author)
                                                : QVariant();
    default:
      return QVariant();
  }
}

Qt::ItemFlags RepositoryNavigatorModel::flags(const QModelIndex &index) const {
  const SectionData *section = sectionData(index);
  if (!section)
    return Qt::NoItemFlags;
  if (isSection(index))
    return section->available ? Qt::ItemIsEnabled : Qt::NoItemFlags;
  const Row *row = rowData(index);
  if (row && (row->kind == ItemKind::Status || !row->available))
    return Qt::ItemIsEnabled;
  return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void RepositoryNavigatorModel::refresh() {
  beginResetModel();
  rebuild();
  endResetModel();
}

bool RepositoryNavigatorModel::isSection(const QModelIndex &index) const {
  return index.isValid() && index.model() == this && index.internalId() == 0 &&
         index.row() >= 0 && index.row() < mSections.size();
}

bool RepositoryNavigatorModel::isItem(const QModelIndex &index) const {
  if (!index.isValid() || index.model() != this || index.internalId() == 0)
    return false;
  int section = static_cast<int>(index.internalId()) - 1;
  return section >= 0 && section < mSections.size() && index.row() >= 0 &&
         index.row() < mSections.at(section).rows.size();
}

const RepositoryNavigatorModel::SectionData *
RepositoryNavigatorModel::sectionData(const QModelIndex &index) const {
  if (isSection(index))
    return &mSections.at(index.row());
  if (isItem(index))
    return &mSections.at(static_cast<int>(index.internalId()) - 1);
  return nullptr;
}

const RepositoryNavigatorModel::Row *
RepositoryNavigatorModel::rowData(const QModelIndex &index) const {
  const SectionData *section = sectionData(index);
  return section && isItem(index) ? &section->rows.at(index.row()) : nullptr;
}

void RepositoryNavigatorModel::disconnectRepository() {
  for (const QMetaObject::Connection &connection : mConnections)
    disconnect(connection);
  mConnections.clear();
}

void RepositoryNavigatorModel::connectRepository() {
  if (!mRepo.isValid())
    return;

  git::RepositoryNotifier *notifier = mRepo.notifier();
  auto scheduleRefresh = [this] { mRefreshTimer.start(); };
  mConnections.append(connect(notifier,
                              &git::RepositoryNotifier::referenceAdded, this,
                              scheduleRefresh));
  mConnections.append(connect(notifier,
                              &git::RepositoryNotifier::referenceRemoved, this,
                              scheduleRefresh));
  mConnections.append(connect(notifier,
                              &git::RepositoryNotifier::referenceUpdated, this,
                              scheduleRefresh));
}

void RepositoryNavigatorModel::rebuild() {
  mSections = {
      {Section::Local, tr("Local"), QString(), false, {}},
      {Section::Remote, tr("Remote"), QString(), false, {}},
      {Section::Worktrees, tr("Worktrees"), QString(), false, {}},
      {Section::Stashes, tr("Stashes"), QString(), false, {}},
      {Section::CloudPatches, tr("Cloud Patches"),
       tr("Cloud Patches are not available."), false, {}},
      {Section::PullRequests, tr("Pull Requests"),
       tr("Pull Request listing is not available."), false, {}},
      {Section::GitHubIssues, tr("GitHub Issues"),
       tr("No eligible GitHub repository was found."), false, {}},
      {Section::Teams, tr("Teams"), tr("Team integration is not available."),
       false, {}},
      {Section::Tags, tr("Tags"), QString(), false, {}},
      {Section::Submodules, tr("Submodules"), QString(), false, {}}};

  rebuildGitHubIssuesSection();

  if (!mRepo.isValid())
    return;

  SectionData &local = mSections[static_cast<int>(Section::Local)];
  for (const git::Branch &branch : mRepo.branches(GIT_BRANCH_LOCAL)) {
    Row row;
    row.reference = branch;
    row.display = branch.name();
    row.tooltip = branch.qualifiedName();
    row.current = branch.isHead();
    git::Branch upstream = branch.upstream();
    if (upstream.isValid()) {
      row.ahead = branch.difference(upstream);
      row.behind = upstream.difference(branch);
    }
    local.rows.append(row);
  }
  std::sort(local.rows.begin(), local.rows.end(), lessThan);
  local.available = !local.rows.isEmpty();

  SectionData &remote = mSections[static_cast<int>(Section::Remote)];
  for (const git::Branch &branch : mRepo.branches(GIT_BRANCH_REMOTE)) {
    if (branch.isRemoteHead())
      continue;
    Row row;
    row.reference = branch;
    row.display = branch.name();
    row.tooltip = branch.qualifiedName();
    remote.rows.append(row);
  }
  std::sort(remote.rows.begin(), remote.rows.end(), lessThan);
  remote.available = !remote.rows.isEmpty();

  SectionData &worktrees = mSections[static_cast<int>(Section::Worktrees)];
  if (!mRepo.isBare()) {
    for (const git::Worktree &worktree : mRepo.worktrees()) {
      Row row;
      row.kind = ItemKind::Worktree;
      row.worktree = worktree;
      row.display = worktree.branch();
      if (row.display.isEmpty())
        row.display = worktree.name();
      row.path = worktree.path();
      row.branch = worktree.branch();
      row.current = worktree.isCurrent();
      row.mainWorktree = worktree.isMain();
      row.available = worktree.isValid() && !worktree.path().isEmpty();
      QStringList tooltip{tr("Path: %1").arg(row.path)};
      if (!row.branch.isEmpty())
        tooltip.append(tr("Branch: %1").arg(row.branch));
      if (!row.available)
        tooltip.append(tr("This worktree is unavailable."));
      row.tooltip = tooltip.join('\n');
      worktrees.rows.append(row);
    }
  }
  worktrees.available = !worktrees.rows.isEmpty();

  SectionData &stashes = mSections[static_cast<int>(Section::Stashes)];
  const QList<git::Commit> commits = mRepo.stashes();
  for (int i = 0; i < commits.size(); ++i) {
    Row row;
    row.kind = ItemKind::Stash;
    row.commit = commits.at(i);
    row.stashIndex = i;
    QString summary = row.commit.summary(git::Commit::SubstituteEmoji);
    row.display = summary.isEmpty() ? tr("stash@{%1}").arg(i) : summary;
    row.tooltip = row.commit.message(git::Commit::SubstituteEmoji);
    stashes.rows.append(row);
  }
  stashes.available = !stashes.rows.isEmpty();

  SectionData &tags = mSections[static_cast<int>(Section::Tags)];
  for (const git::TagRef &tag : mRepo.tags()) {
    Row row;
    row.reference = tag;
    row.display = tag.name();
    row.tooltip = tag.qualifiedName();
    tags.rows.append(row);
  }
  std::sort(tags.rows.begin(), tags.rows.end(), lessThan);
  tags.available = !tags.rows.isEmpty();

  SectionData &submodules = mSections[static_cast<int>(Section::Submodules)];
  for (const git::Submodule &submodule : mRepo.submodules()) {
    Row row;
    row.kind = ItemKind::Submodule;
    row.submodule = submodule;
    row.display = submodule.name();
    row.path = submodule.path();
    row.url = submodule.url();
    row.branch = submodule.branch();
    row.initialized = submodule.isInitialized();
    QStringList tooltip{tr("Path: %1").arg(row.path)};
    git::Id pinnedId = submodule.indexId();
    auto status = mSubmoduleUpdateStatuses.constFind(row.display);
    bool matchingStatus =
        status != mSubmoduleUpdateStatuses.cend() && status->path == row.path &&
        (status->url == row.url || QUrl(row.url).isRelative()) &&
        status->branch == row.branch;
    if (!row.branch.isEmpty()) {
      row.originState =
          matchingStatus ? OriginState::Failed : OriginState::Pending;
    }
    if (row.initialized) {
      git::Repository repo = submodule.open();
      git::Id checkoutId = submodule.workdirId();
      if (repo.isValid() && checkoutId.isValid()) {
        tooltip.append(tr("Local checkout: %1").arg(checkoutId.shortId()));
        if (pinnedId.isValid()) {
          compareCommits(repo, checkoutId, pinnedId, row.pinnedAhead,
                         row.pinnedBehind);
          QString reference =
              tr("the commit recorded by the parent repository");
          QString text =
              tr("Pin %1: %2")
                  .arg(pinnedId.shortId(),
                       comparisonText(reference, row.pinnedAhead,
                                      row.pinnedBehind));
          tooltip.append(text);
        } else {
          tooltip.append(tr("Pin: The parent repository does not record a "
                            "submodule commit."));
        }

        if (matchingStatus && status->targetId.isValid()) {
          if (compareCommits(repo, checkoutId, status->targetId,
                             row.originAhead, row.originBehind)) {
            row.originState = OriginState::Ready;
            row.originTarget = status->targetId;
            QString reference =
                tr("the latest fetched commit on origin/%1").arg(row.branch);
            QString text =
                tr("Origin %1 (%2): %3")
                    .arg(row.branch, status->targetId.shortId(),
                         comparisonText(reference, row.originAhead,
                                        row.originBehind));
            tooltip.append(text);
          }
        }
      } else {
        tooltip.append(tr("Local checkout: unavailable because the submodule "
                          "repository could not be read."));
      }
    } else {
      tooltip.append(tr("Local checkout: unavailable because the submodule is "
                        "not initialized."));
    }

    if (row.branch.isEmpty()) {
      tooltip.append(
          tr("Origin: Not shown because no remote branch is configured. "
             "Configure a branch in the submodule settings to enable this "
             "comparison."));
    } else if (row.originState == OriginState::Pending) {
      tooltip.append(
          tr("Origin %1: Waiting for a submodule update check.")
              .arg(row.branch));
    } else if (row.originState == OriginState::Failed) {
      QString reason =
          matchingStatus ? status->message : QString();
      tooltip.append(
          reason.isEmpty()
              ? tr("Origin %1: The comparison failed.").arg(row.branch)
              : tr("Origin %1: The comparison failed - %2")
                    .arg(row.branch, reason));
    }

    if (row.pinnedAhead >= 0 || row.originState == OriginState::Ready) {
      tooltip.append(
          tr("↑ means local-only commits; ↓ means commits missing locally."));
    }
    QString details = tooltip.join('\n').toHtmlEscaped();
    details.replace('\n', "<br>");
    QStringList legend;
    legend.append(QString("<b>%1</b>").arg(tr("Indicators:").toHtmlEscaped()));
    legend.append(
        tr("%1 Left icon: Pin summary.")
            .arg(coloredText(QString::fromUtf8("✓"), "#36c96b")));
    legend.append(
        tr("%1 matches; %2 differs; %3 unavailable; %4 uninitialized.")
            .arg(coloredText(QString::fromUtf8("✓"), "#36c96b"),
                 coloredText(QString::fromUtf8("↕"), "#f0a020"),
                 coloredText("?", "#8c8c8c"),
                 coloredText(QString::fromUtf8("○"), "#8c8c8c")));
    legend.append(
        tr("%1 = Pin delta; %2 = Origin delta.")
            .arg(coloredText(QString::fromUtf8("P 1↓"), "#f0a020"),
                 coloredText(QString::fromUtf8("O 1↓"), "#f0a020")));
    legend.append(
        tr("An empty delta means no difference when comparison is available."));
    legend.append(
        tr("%1 synchronized; %2 difference; %3 unavailable, pending, or not "
           "configured; %4 failed.")
            .arg(coloredText("P", "#36c96b"),
                 coloredText(QString::fromUtf8("P 1↓"), "#f0a020"),
                 coloredText("O", "#8c8c8c"), coloredText("O", "#e25555")));
    legend.append(tr("%1 local-only; %2 missing locally.")
                      .arg(coloredText(QString::fromUtf8("↑"), "#4aa3ff"),
                           coloredText(QString::fromUtf8("↓"), "#f0a020")));
    row.tooltip = QString("<qt>%1<br><br>%2</qt>")
                      .arg(details, legend.join("<br>"));
    submodules.rows.append(row);
  }
  submodules.available = !submodules.rows.isEmpty();
  std::sort(submodules.rows.begin(), submodules.rows.end(), lessThan);
}

void RepositoryNavigatorModel::rebuildGitHubIssuesSection() {
  if (mSections.size() <= static_cast<int>(Section::GitHubIssues))
    return;

  SectionData &section = mSections[static_cast<int>(Section::GitHubIssues)];
  section.available = mGitHubIssuesAvailable;
  section.rows.clear();
  if (!mGitHubIssuesAvailable)
    return;

  Row filter;
  filter.kind = ItemKind::GitHubIssuesFilter;
  filter.display = mGitHubIssuesFilter.isEmpty()
                       ? tr("Issues repository")
                       : tr("Issues repository: %1").arg(mGitHubIssuesFilter);
  section.rows.append(filter);

  for (const GitHub::Issue &issue : mGitHubIssues) {
    Row row;
    row.kind = ItemKind::GitHubIssue;
    row.issueNumber = issue.number;
    row.author = issue.author;
    row.url = issue.url.toString();
    row.display = tr("#%1 %2").arg(issue.number).arg(issue.title);
    QStringList details;
    if (!issue.author.isEmpty())
      details.append(tr("Author: %1").arg(issue.author));
    if (!row.url.isEmpty())
      details.append(row.url);
    row.tooltip = details.join('\n');
    section.rows.append(row);
  }

  QString status;
  switch (mGitHubIssuesState) {
    case LoadState::Loading:
      status = tr("Loading issues...");
      break;
    case LoadState::Refreshing:
      status = tr("Refreshing issues...");
      break;
    case LoadState::Ready:
      if (mGitHubIssues.isEmpty())
        status = tr("No open issues.");
      break;
    case LoadState::Failed:
      status = mGitHubIssuesError.isEmpty()
                   ? tr("Unable to load issues.")
                   : tr("Unable to load issues: %1").arg(mGitHubIssuesError);
      break;
    case LoadState::Stale:
      status = mGitHubIssuesError.isEmpty()
                   ? tr("Refresh failed. Showing previously loaded issues.")
                   : tr("Refresh failed: %1").arg(mGitHubIssuesError);
      break;
    case LoadState::Unavailable:
      break;
  }
  if (!status.isEmpty()) {
    Row row;
    row.kind = ItemKind::Status;
    row.display = status;
    row.tooltip = status;
    section.rows.append(row);
  }
}

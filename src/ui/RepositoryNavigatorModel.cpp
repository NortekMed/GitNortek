//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryNavigatorModel.h"
#include "git/Branch.h"
#include "git/TagRef.h"
#include <algorithm>

bool RepositoryNavigatorModel::lessThan(
    const RepositoryNavigatorModel::Row &lhs,
    const RepositoryNavigatorModel::Row &rhs) {
  return QString::localeAwareCompare(lhs.display, rhs.display) < 0;
}

RepositoryNavigatorModel::RepositoryNavigatorModel(QObject *parent)
    : QAbstractItemModel(parent) {
  rebuild();
}

void RepositoryNavigatorModel::setRepository(const git::Repository &repo) {
  disconnectRepository();
  beginResetModel();
  mRepo = repo;
  rebuild();
  endResetModel();
  connectRepository();
}

void RepositoryNavigatorModel::clear() { setRepository(git::Repository()); }

git::Repository RepositoryNavigatorModel::repository() const { return mRepo; }

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
        return section->rows.size();
      case AvailableRole:
        return section->available;
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
      return true;
    case CurrentRole:
      return row->current;
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
  mConnections.append(connect(notifier, &git::RepositoryNotifier::referenceAdded,
                              this, &RepositoryNavigatorModel::refresh));
  mConnections.append(connect(notifier,
                              &git::RepositoryNotifier::referenceRemoved, this,
                              &RepositoryNavigatorModel::refresh));
  mConnections.append(connect(notifier,
                              &git::RepositoryNotifier::referenceUpdated, this,
                              &RepositoryNavigatorModel::refresh));
}

void RepositoryNavigatorModel::rebuild() {
  mSections = {
      {Section::Local, tr("Local"), QString(), true, {}},
      {Section::Remote, tr("Remote"), QString(), true, {}},
      {Section::Stashes, tr("Stashes"), QString(), true, {}},
      {Section::CloudPatches, tr("Cloud Patches"),
       tr("Cloud Patches are not available."), false, {}},
      {Section::PullRequests, tr("Pull Requests"),
       tr("Pull Request listing is not available."), false, {}},
      {Section::GitHubIssues, tr("GitHub Issues"),
       tr("GitHub Issue listing is not available."), false, {}},
      {Section::Teams, tr("Teams"), tr("Team integration is not available."),
       false, {}},
      {Section::Tags, tr("Tags"), QString(), true, {}},
      {Section::Submodules, tr("Submodules"), QString(), true, {}}};

  if (!mRepo.isValid())
    return;

  SectionData &local = mSections[static_cast<int>(Section::Local)];
  for (const git::Branch &branch : mRepo.branches(GIT_BRANCH_LOCAL)) {
    Row row;
    row.reference = branch;
    row.display = branch.name();
    row.tooltip = branch.qualifiedName();
    row.current = branch.isHead();
    local.rows.append(row);
  }
  std::sort(local.rows.begin(), local.rows.end(), lessThan);

  SectionData &remote = mSections[static_cast<int>(Section::Remote)];
  for (const git::Branch &branch : mRepo.branches(GIT_BRANCH_REMOTE)) {
    if (branch.name().endsWith("HEAD"))
      continue;
    Row row;
    row.reference = branch;
    row.display = branch.name();
    row.tooltip = branch.qualifiedName();
    remote.rows.append(row);
  }
  std::sort(remote.rows.begin(), remote.rows.end(), lessThan);

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

  SectionData &tags = mSections[static_cast<int>(Section::Tags)];
  for (const git::TagRef &tag : mRepo.tags()) {
    Row row;
    row.reference = tag;
    row.display = tag.name();
    row.tooltip = tag.qualifiedName();
    tags.rows.append(row);
  }
  std::sort(tags.rows.begin(), tags.rows.end(), lessThan);

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
    row.tooltip = row.path;
    submodules.rows.append(row);
  }
  std::sort(submodules.rows.begin(), submodules.rows.end(), lessThan);
}

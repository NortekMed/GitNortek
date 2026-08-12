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
#include <QAbstractItemModel>

class RepositoryNavigatorModel : public QAbstractItemModel {
  Q_OBJECT

public:
  enum class Section {
    Local,
    Remote,
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

  enum class ItemKind { Section, Reference, Stash, Submodule };
  Q_ENUM(ItemKind)

  enum Role {
    SectionRole = Qt::UserRole + 1,
    ItemKindRole,
    CountRole,
    AvailableRole,
    CurrentRole,
    ReferenceRole,
    CommitRole,
    StashIndexRole,
    SubmoduleRole,
    PathRole,
    UrlRole,
    BranchRole,
    InitializedRole
  };

  explicit RepositoryNavigatorModel(QObject *parent = nullptr);

  void setRepository(const git::Repository &repo);
  void clear();
  git::Repository repository() const;

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
    int stashIndex = -1;
    QString display;
    QString tooltip;
    QString path;
    QString url;
    QString branch;
    bool initialized = false;
    bool current = false;
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

  git::Repository mRepo;
  QList<SectionData> mSections;
  QList<QMetaObject::Connection> mConnections;
};

#endif

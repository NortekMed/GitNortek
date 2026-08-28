//
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Martin Marmsoler
//

#ifndef TREEPROXY_H
#define TREEPROXY_H

#include "git/Diff.h"
#include "git/Index.h"
#include "git/Tree.h"
#include "git/Repository.h"
#include <QSortFilterProxyModel>
#include <QFileIconProvider>

class QAbstractItemModel;
class TreeModel;

class TreeProxy : public QSortFilterProxyModel {
  Q_OBJECT

public:
  TreeProxy(bool staged, QAbstractItemModel *model, QObject *parent);
  virtual ~TreeProxy();
  bool setData(const QModelIndex &index, const QVariant &value,
               int role = Qt::EditRole, bool ignoreIndexChanges = false);
  bool staged() { return mStaged; }

  QVariant data(const QModelIndex &index,
                int role = Qt::DisplayRole) const override;

  void enableFilter(bool enable) { mFilter = enable; }
  void setUnresolvedOnly(bool enabled) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    mUnresolvedOnly = enabled;
    endFilterChange();
#else
    mUnresolvedOnly = enabled;
    invalidateFilter();
#endif
  }
  void setConflictMode(bool enabled) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    mConflictMode = enabled;
    endFilterChange();
#else
    mConflictMode = enabled;
    invalidateFilter();
#endif
  }

  int columnCount(const QModelIndex &parent = QModelIndex()) const override {
    return sourceModel()->columnCount();
  }

private:
  using QSortFilterProxyModel::setData;
  bool filterAcceptsRow(int source_row,
                        const QModelIndex &source_parent) const override;
  bool acceptsConflictMode(const QModelIndex &index) const;
  bool mStaged{
      true}; // indicates, if only staged or only unstages files should be shown
  bool mFilter = true;
  bool mUnresolvedOnly = false;
  bool mConflictMode = false;
};

#endif // TREEPROXY_H

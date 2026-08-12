//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryNavigator.h"
#include "RepositoryNavigatorModel.h"
#include "RepoView.h"
#include <QMenu>
#include <QMetaEnum>
#include <QPainter>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

const QString kExpandedGroup = "sidebar/repositoryNavigator/expanded";

QString sectionKey(const QModelIndex &index) {
  int section = index.data(RepositoryNavigatorModel::SectionRole).toInt();
  QMetaEnum meta = QMetaEnum::fromType<RepositoryNavigatorModel::Section>();
  return QString::fromLatin1(meta.valueToKey(section));
}

class NavigatorDelegate : public QStyledItemDelegate {
public:
  NavigatorDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    return QSize(size.width(), index.parent().isValid() ? 24 : 28);
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    painter->save();
    QPalette palette = option.palette;
    bool section = !index.parent().isValid();
    bool selected = option.state & QStyle::State_Selected;
    QColor background = selected ? palette.highlight().color()
                                 : palette.base().color();
    painter->fillRect(option.rect, background);

    QColor text = selected ? palette.highlightedText().color()
                           : palette.text().color();
    if (section)
      text = palette.brightText().color();
    if (!index.data(RepositoryNavigatorModel::AvailableRole).toBool())
      text = palette.color(QPalette::Disabled, QPalette::Text);

    QRect content = option.rect.adjusted(section ? 8 : 6, 0, -8, 0);
    QFont font = option.font;
    if (section) {
      font.setBold(true);
      font.setCapitalization(QFont::SmallCaps);
    }
    painter->setFont(font);
    painter->setPen(text);

    QString prefix;
    if (!section &&
        index.data(RepositoryNavigatorModel::CurrentRole).toBool())
      prefix = QString::fromUtf8("✓  ");
    QString label = prefix + index.data().toString();

    QString trailing;
    if (section) {
      trailing = QString::number(
          index.data(RepositoryNavigatorModel::CountRole).toInt());
    } else {
      QVariant ahead = index.data(RepositoryNavigatorModel::AheadRole);
      QVariant behind = index.data(RepositoryNavigatorModel::BehindRole);
      if (ahead.isValid() && ahead.toInt() > 0)
        trailing += tr("%1↑").arg(ahead.toInt());
      if (behind.isValid() && behind.toInt() > 0) {
        if (!trailing.isEmpty())
          trailing += " ";
        trailing += tr("%1↓").arg(behind.toInt());
      }
    }

    int trailingWidth = trailing.isEmpty()
                            ? 0
                            : painter->fontMetrics().horizontalAdvance(trailing);
    QRect textRect = content.adjusted(0, 0, -trailingWidth - 8, 0);
    painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                      painter->fontMetrics().elidedText(
                          label, Qt::ElideRight, textRect.width()));
    if (!trailing.isEmpty())
      painter->drawText(content, Qt::AlignVCenter | Qt::AlignRight, trailing);

    if (section) {
      painter->setPen(palette.mid().color());
      painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    }
    painter->restore();
  }
};

} // namespace

RepositoryNavigator::RepositoryNavigator(QWidget *parent) : QWidget(parent) {
  setObjectName("RepositoryNavigator");
  setAccessibleName(tr("Repository navigation"));

  mView = new QTreeView(this);
  mView->setObjectName("RepositoryNavigationTree");
  mView->setAccessibleName(tr("Repository references"));
  mView->setHeaderHidden(true);
  mView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  mView->setRootIsDecorated(true);
  mView->setItemsExpandable(true);
  mView->setExpandsOnDoubleClick(false);
  mView->setIndentation(14);
  mView->setUniformRowHeights(true);
  mView->setItemDelegate(new NavigatorDelegate(mView));

  mModel = new RepositoryNavigatorModel(mView);
  mView->setModel(mModel);

  connect(mModel, &RepositoryNavigatorModel::modelReset, this,
          &RepositoryNavigator::restoreExpansion);
  connect(mView, &QTreeView::expanded, this,
          [this](const QModelIndex &index) { storeExpansion(index, true); });
  connect(mView, &QTreeView::collapsed, this,
          [this](const QModelIndex &index) { storeExpansion(index, false); });
  connect(mView, &QTreeView::clicked, this,
          [this](const QModelIndex &index) { activate(index, false); });
  connect(mView, &QTreeView::doubleClicked, this,
          [this](const QModelIndex &index) { activate(index, true); });

  mView->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(mView, &QTreeView::customContextMenuRequested, this,
          [this](const QPoint &point) {
            if (!mRepoView)
              return;
            QModelIndex index = mView->indexAt(point);
            int stash = index.data(RepositoryNavigatorModel::StashIndexRole)
                            .toInt();
            if (!index.data(RepositoryNavigatorModel::StashIndexRole).isValid())
              return;
            QMenu menu;
            menu.addAction(tr("Apply Stash"), mRepoView,
                           [view = mRepoView, stash] {
                             if (view)
                               view->applyStash(stash);
                           });
            menu.addAction(tr("Pop Stash"), mRepoView,
                           [view = mRepoView, stash] {
                             if (view)
                               view->popStash(stash);
                           });
            menu.addAction(tr("Drop Stash"), mRepoView,
                           [view = mRepoView, stash] {
                             if (view)
                               view->dropStash(stash);
                           });
            menu.exec(mView->viewport()->mapToGlobal(point));
          });

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(mView);

  restoreExpansion();
}

void RepositoryNavigator::setRepository(const git::Repository &repo) {
  mModel->setRepository(repo);
}

void RepositoryNavigator::setRepoView(RepoView *view) {
  disconnect(mSubmodulesConnection);
  disconnect(mReferenceConnection);
  disconnect(mReferenceSelectedConnection);
  mRepoView = view;
  setRepository(view ? view->repo() : git::Repository());
  if (view) {
    mSubmodulesConnection =
        connect(view, &RepoView::submodulesChanged, mModel,
                &RepositoryNavigatorModel::refresh);
    mReferenceConnection = connect(view, &RepoView::referenceChanged, this,
                                   &RepositoryNavigator::selectReference);
    mReferenceSelectedConnection =
        connect(view, &RepoView::referenceSelected, this,
                &RepositoryNavigator::selectReference);
    selectReference(view->reference());
  }
}

RepositoryNavigatorModel *RepositoryNavigator::model() const { return mModel; }

QTreeView *RepositoryNavigator::view() const { return mView; }

void RepositoryNavigator::restoreExpansion() {
  QSettings settings;
  settings.beginGroup(kExpandedGroup);
  for (int row = 0; row < mModel->rowCount(); ++row) {
    QModelIndex index = mModel->index(row, 0);
    bool defaultExpanded =
        index.data(RepositoryNavigatorModel::AvailableRole).toBool();
    mView->setExpanded(
        index, settings.value(sectionKey(index), defaultExpanded).toBool());
  }
  settings.endGroup();
}

void RepositoryNavigator::storeExpansion(const QModelIndex &index,
                                         bool expanded) {
  if (!index.isValid() || index.parent().isValid())
    return;
  QSettings settings;
  settings.beginGroup(kExpandedGroup);
  settings.setValue(sectionKey(index), expanded);
  settings.endGroup();
}

void RepositoryNavigator::selectReference(const git::Reference &ref) {
  if (!ref.isValid()) {
    mView->clearSelection();
    return;
  }
  for (RepositoryNavigatorModel::Section section :
       {RepositoryNavigatorModel::Section::Local,
        RepositoryNavigatorModel::Section::Remote,
        RepositoryNavigatorModel::Section::Tags}) {
    QModelIndex parent = mModel->sectionIndex(section);
    for (int row = 0; row < mModel->rowCount(parent); ++row) {
      QModelIndex index = mModel->index(row, 0, parent);
      git::Reference candidate =
          index.data(RepositoryNavigatorModel::ReferenceRole)
              .value<git::Reference>();
      if (candidate.isValid() &&
          candidate.qualifiedName() == ref.qualifiedName()) {
        mView->setCurrentIndex(index);
        return;
      }
    }
  }
}

void RepositoryNavigator::activate(const QModelIndex &index, bool checkout) {
  if (!mRepoView || !index.isValid() || !index.parent().isValid())
    return;

  git::Reference ref =
      index.data(RepositoryNavigatorModel::ReferenceRole).value<git::Reference>();
  if (ref.isValid()) {
    if (checkout)
      mRepoView->checkout(ref);
    else
      mRepoView->navigateToReference(ref);
    return;
  }

  QVariant stash = index.data(RepositoryNavigatorModel::StashIndexRole);
  if (stash.isValid()) {
    mRepoView->selectStash(stash.toInt());
    return;
  }

  git::Submodule submodule =
      index.data(RepositoryNavigatorModel::SubmoduleRole).value<git::Submodule>();
  if (submodule.isValid() && checkout)
    mRepoView->openSubmodule(submodule);
}

//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryNavigator.h"
#include "FontUtils.h"
#include "RepositoryNavigatorModel.h"
#include "RepoView.h"
#include <QMenu>
#include <QMetaEnum>
#include <QPainter>
#include <QPainterPath>
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

QString comparisonDelta(const QVariant &ahead, const QVariant &behind) {
  if (!ahead.isValid() || !behind.isValid())
    return QString();

  int aheadCount = ahead.toInt();
  int behindCount = behind.toInt();
  QString badge;
  if (aheadCount)
    badge += QObject::tr("%1↑").arg(aheadCount);
  if (behindCount) {
    if (!badge.isEmpty())
      badge += " ";
    badge += QObject::tr("%1↓").arg(behindCount);
  }
  return badge;
}

struct SubmoduleVisual {
  QString pinIndicator;
  QColor pinColor;
  QString pinDelta;
  QColor originColor;
  QString originDelta;
};

struct TrailingPart {
  QString text;
  QColor color;
  int width = 0;
  Qt::Alignment alignment = Qt::AlignRight;
};

constexpr int kSubmoduleStatusWidth = 44;

SubmoduleVisual submoduleVisual(const QModelIndex &index,
                                const QPalette &palette) {
  const QColor green("#36c96b");
  const QColor amber("#f0a020");
  const QColor red("#e25555");
  const QColor gray = palette.color(QPalette::Disabled, QPalette::Text);

  SubmoduleVisual visual;
  if (!index.data(RepositoryNavigatorModel::InitializedRole).toBool()) {
    visual.pinIndicator = QString::fromUtf8("○");
    visual.pinColor = gray;
  } else {
    QVariant pinnedAhead =
        index.data(RepositoryNavigatorModel::PinnedAheadRole);
    QVariant pinnedBehind =
        index.data(RepositoryNavigatorModel::PinnedBehindRole);
    if (!pinnedAhead.isValid() || !pinnedBehind.isValid()) {
      visual.pinIndicator = "?";
      visual.pinColor = gray;
    } else {
      visual.pinDelta = comparisonDelta(pinnedAhead, pinnedBehind);
      visual.pinIndicator = visual.pinDelta.isEmpty() ? QString::fromUtf8("✓")
                                                      : QString::fromUtf8("↕");
      visual.pinColor = visual.pinDelta.isEmpty() ? green : amber;
    }
  }

  auto originState = index.data(RepositoryNavigatorModel::OriginStateRole)
                         .value<RepositoryNavigatorModel::OriginState>();
  if (originState == RepositoryNavigatorModel::OriginState::Hidden) {
    visual.originColor = gray;
  } else if (originState == RepositoryNavigatorModel::OriginState::Pending) {
    visual.originColor = gray;
  } else if (originState == RepositoryNavigatorModel::OriginState::Failed) {
    visual.originColor = red;
  } else if (originState == RepositoryNavigatorModel::OriginState::Ready) {
    visual.originDelta =
        comparisonDelta(index.data(RepositoryNavigatorModel::OriginAheadRole),
                        index.data(RepositoryNavigatorModel::OriginBehindRole));
    visual.originColor = visual.originDelta.isEmpty() ? green : amber;
  }

  return visual;
}

class NavigatorDelegate : public QStyledItemDelegate {
public:
  NavigatorDelegate(const QFont &sectionFont, QObject *parent)
      : QStyledItemDelegate(parent), mSectionFont(sectionFont) {}

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
    QFont font = section ? mSectionFont : option.font;
    if (section) {
      font.setBold(true);
      font.setCapitalization(QFont::SmallCaps);
    }
    painter->setFont(font);
    painter->setPen(text);

    const QColor green("#36c96b");
    bool localBranch =
        !section &&
        index.data(RepositoryNavigatorModel::SectionRole).toInt() ==
            static_cast<int>(RepositoryNavigatorModel::Section::Local);
    bool current =
        !section && index.data(RepositoryNavigatorModel::CurrentRole).toBool();
    if (localBranch) {
      if (current) {
        QRect marker(0, 0, 12, 12);
        marker.moveCenter(QPoint(content.x() + 7, content.center().y()));
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(green);
        painter->drawRoundedRect(marker, 1, 1);

        QPen check(Qt::white, 1.5);
        check.setCapStyle(Qt::RoundCap);
        check.setJoinStyle(Qt::RoundJoin);
        painter->setPen(check);
        QPainterPath path;
        path.moveTo(marker.left() + 3, marker.center().y());
        path.lineTo(marker.left() + 5, marker.bottom() - 3);
        path.lineTo(marker.right() - 2, marker.top() + 3);
        painter->drawPath(path);
      }
      content.adjust(18, 0, 0, 0);
      painter->setBrush(Qt::NoBrush);
      painter->setPen(text);
    }

    bool submodule =
        !section &&
        static_cast<RepositoryNavigatorModel::ItemKind>(
            index.data(RepositoryNavigatorModel::ItemKindRole).toInt()) ==
            RepositoryNavigatorModel::ItemKind::Submodule;
    SubmoduleVisual visual;
    if (submodule) {
      visual = submoduleVisual(index, palette);
      QRect indicatorRect = content;
      indicatorRect.setWidth(14);
      QFont indicatorFont = font;
      indicatorFont.setBold(true);
      painter->setFont(indicatorFont);
      painter->setPen(visual.pinColor);
      painter->drawText(indicatorRect, Qt::AlignCenter, visual.pinIndicator);
      content.adjust(18, 0, 0, 0);
      painter->setFont(font);
      painter->setPen(text);
    }

    QString label = index.data().toString();

    QList<TrailingPart> trailing;
    if (section) {
      trailing.append({QString::number(
                           index.data(RepositoryNavigatorModel::CountRole)
                               .toInt()),
                       text});
    } else {
      auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
          index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
      if (kind == RepositoryNavigatorModel::ItemKind::Submodule) {
        QString pin = "P";
        if (!visual.pinDelta.isEmpty())
          pin += " " + visual.pinDelta;
        trailing.append(
            {pin, visual.pinColor, kSubmoduleStatusWidth, Qt::AlignLeft});

        QString origin = "O";
        if (!visual.originDelta.isEmpty())
          origin += " " + visual.originDelta;
        trailing.append(
            {origin, visual.originColor, kSubmoduleStatusWidth, Qt::AlignLeft});
      } else {
        QVariant ahead = index.data(RepositoryNavigatorModel::AheadRole);
        QVariant behind = index.data(RepositoryNavigatorModel::BehindRole);
        if (ahead.isValid() && ahead.toInt() > 0)
          trailing.append(
              {RepositoryNavigator::tr("%1↑").arg(ahead.toInt()),
               QColor("#4aa3ff")});
        if (behind.isValid() && behind.toInt() > 0)
          trailing.append(
              {RepositoryNavigator::tr("%1↓").arg(behind.toInt()),
               QColor("#f0a020")});
      }
    }

    int trailingWidth = 0;
    for (const TrailingPart &part : trailing) {
      if (trailingWidth)
        trailingWidth += 6;
      trailingWidth += part.width > 0
                           ? part.width
                           : painter->fontMetrics().horizontalAdvance(part.text);
    }
    QRect textRect = content.adjusted(0, 0, -trailingWidth - 8, 0);
    painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                      painter->fontMetrics().elidedText(
                          label, Qt::ElideRight, textRect.width()));
    int left = content.right() - trailingWidth + 1;
    for (const TrailingPart &part : trailing) {
      int width = part.width > 0
                      ? part.width
                      : painter->fontMetrics().horizontalAdvance(part.text);
      QRect partRect(left, content.y(), width, content.height());
      painter->setPen(part.color);
      painter->drawText(partRect, Qt::AlignVCenter | part.alignment, part.text);
      left = partRect.right() + 7;
    }

    if (section) {
      painter->setPen(palette.mid().color());
      painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    }
    painter->restore();
  }

private:
  QFont mSectionFont;
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
  mView->setItemDelegate(new NavigatorDelegate(mView->font(), mView));

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
          &RepositoryNavigator::showContextMenu);

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
  disconnect(mSubmoduleStatusesConnection);
  disconnect(mReferenceConnection);
  disconnect(mReferenceSelectedConnection);
  mRepoView = view;
  setRepository(view ? view->repo() : git::Repository());
  if (view) {
    mModel->setSubmoduleUpdateStatuses(view->submoduleUpdateStatuses());
    mSubmodulesConnection =
        connect(view, &RepoView::submodulesChanged, mModel,
                &RepositoryNavigatorModel::refresh);
    mSubmoduleStatusesConnection = connect(
        view, &RepoView::submoduleUpdateStatusesChanged, mModel,
        &RepositoryNavigatorModel::setSubmoduleUpdateStatuses);
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

void RepositoryNavigator::setBodyFont(const QFont &font) {
  mView->setStyleSheet(
      mView->styleSheet() +
      QString("\nfont-size: %1pt;").arg(FontUtils::pointSize(font)));
  mView->setFont(FontUtils::copySize(mView->font(), font));
  mView->viewport()->update();
}

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

void RepositoryNavigator::showContextMenu(const QPoint &point) {
  if (!mRepoView)
    return;

  QModelIndex index = mView->indexAt(point);
  if (!index.isValid() || !index.parent().isValid())
    return;

  QMenu menu;
  git::Reference ref = index.data(RepositoryNavigatorModel::ReferenceRole)
                           .value<git::Reference>();
  if (ref.isValid()) {
    mRepoView->populateReferenceContextMenu(&menu, ref);
  } else {
    git::Submodule submodule =
        index.data(RepositoryNavigatorModel::SubmoduleRole)
            .value<git::Submodule>();
    if (submodule.isValid()) {
      bool initialized =
          index.data(RepositoryNavigatorModel::InitializedRole).toBool();
      QAction *open =
          menu.addAction(tr("Open"), mRepoView, [view = mRepoView, submodule] {
            if (view)
              view->openSubmodule(submodule);
          });
      open->setEnabled(initialized && submodule.open().isValid());
      QAction *commitChanges = menu.addAction(
          tr("Commit Changes"), mRepoView, [view = mRepoView, submodule] {
            if (view)
              view->commitSubmoduleChanges(submodule);
          });
      commitChanges->setEnabled(
          initialized && mRepoView->canCommitSubmoduleChanges(submodule));
      QAction *checkUpdates = menu.addAction(
          tr("Check for Updates"), mRepoView,
          [view = mRepoView, submodule] {
            if (view)
              view->checkSubmoduleUpdates(QList<git::Submodule>{submodule});
          });
      checkUpdates->setEnabled(initialized && !submodule.branch().isEmpty());
      RepositoryNavigatorModel::OriginState originState =
          index.data(RepositoryNavigatorModel::OriginStateRole)
              .value<RepositoryNavigatorModel::OriginState>();
      int originBehind =
          index.data(RepositoryNavigatorModel::OriginBehindRole).toInt();
      git::Id originTarget =
          index.data(RepositoryNavigatorModel::OriginTargetRole)
              .value<git::Id>();
      QString branch =
          index.data(RepositoryNavigatorModel::BranchRole).toString();
      if (initialized &&
          originState == RepositoryNavigatorModel::OriginState::Ready &&
          originBehind > 0 && originTarget.isValid()) {
        menu.addAction(
            tr("Checkout origin/%1").arg(branch), mRepoView,
            [view = mRepoView, name = submodule.name(), branch, originTarget] {
              if (view)
                view->checkoutSubmoduleOrigin(name, branch, originTarget);
            });
      }
      if (initialized) {
        menu.addAction(tr("Update"), mRepoView, [view = mRepoView, submodule] {
          if (view)
            view->updateSubmodules({submodule});
        });
      } else {
        menu.addAction(tr("Initialize and Update"), mRepoView,
                       [view = mRepoView, submodule] {
                         if (view)
                           view->updateSubmodules({submodule}, true, true);
                       });
      }
      menu.addSeparator();
      menu.addAction(tr("Modify..."), mRepoView, [view = mRepoView, submodule] {
        if (view)
          view->promptToModifySubmodule(submodule);
      });
      menu.addAction(tr("Delete Submodule..."), mRepoView,
                     [view = mRepoView, submodule] {
                       if (view)
                         view->promptToDeleteSubmodule(submodule);
                     });
    } else {
      QVariant stash =
          index.data(RepositoryNavigatorModel::StashIndexRole);
      if (stash.isValid()) {
        int stashIndex = stash.toInt();
        menu.addAction(tr("Apply Stash"), mRepoView,
                       [view = mRepoView, stashIndex] {
                         if (view)
                           view->applyStash(stashIndex);
                       });
        menu.addAction(tr("Pop Stash"), mRepoView,
                       [view = mRepoView, stashIndex] {
                         if (view)
                           view->popStash(stashIndex);
                       });
        menu.addAction(tr("Drop Stash"), mRepoView,
                       [view = mRepoView, stashIndex] {
                         if (view)
                           view->dropStash(stashIndex);
                       });
      }
    }
  }

  if (!menu.isEmpty())
    menu.exec(mView->viewport()->mapToGlobal(point));
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

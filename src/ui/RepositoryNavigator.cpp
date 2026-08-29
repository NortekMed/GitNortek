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
#include "git/Config.h"
#include "git/Remote.h"
#include "host/Accounts.h"
#include "host/Repository.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMetaEnum>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>

namespace {

const QString kExpandedGroup = "sidebar/repositoryNavigator/expanded";
const QString kIssuesRemoteKey = "sidebar.githubIssues.remote";
constexpr qint64 kIssuesCacheLifetimeMs = 5 * 60 * 1000;
constexpr qint64 kIssuesManualRefreshIntervalMs = 10 * 1000;
constexpr qint64 kIssuesRetryDelayMs = 60 * 1000;

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

RepositoryNavigator::RepositoryNavigator(QWidget *parent,
                                         const IssuesRequest &request,
                                         const Clock &clock)
    : QWidget(parent), mIssuesRequest(request),
      mClock(clock ? clock
                   : [] { return QDateTime::currentMSecsSinceEpoch(); }) {
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

  mIssuesPanel = new QWidget(this);
  mIssuesPanel->setObjectName("GitHubIssuesPanel");
  mIssuesTitle = new QLabel(tr("GitHub Issues"), mIssuesPanel);
  QFont issuesTitleFont = mIssuesTitle->font();
  issuesTitleFont.setBold(true);
  issuesTitleFont.setCapitalization(QFont::SmallCaps);
  mIssuesTitle->setFont(issuesTitleFont);
  mIssuesRefresh = new QToolButton(mIssuesPanel);
  mIssuesRefresh->setObjectName("GitHubIssuesRefresh");
  mIssuesRefresh->setAccessibleName(tr("Refresh Issues"));
  mIssuesRefresh->setToolTip(tr("Refresh Issues"));
  mIssuesRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
  mIssuesRefresh->setAutoRaise(true);

  QHBoxLayout *issuesHeader = new QHBoxLayout();
  issuesHeader->setContentsMargins(8, 2, 4, 2);
  issuesHeader->addWidget(mIssuesTitle);
  issuesHeader->addStretch();
  issuesHeader->addWidget(mIssuesRefresh);

  mIssuesRemoteFilter = new QComboBox(mIssuesPanel);
  mIssuesRemoteFilter->setObjectName("GitHubIssuesRemoteFilter");
  mIssuesRemoteFilter->setAccessibleName(tr("Issues repository"));
  mIssuesRemoteFilter->setPlaceholderText(tr("Issues repository"));

  mIssuesView = new QTreeView(mIssuesPanel);
  mIssuesView->setObjectName("GitHubIssuesView");
  mIssuesView->setAccessibleName(tr("GitHub Issues"));
  mIssuesView->setHeaderHidden(true);
  mIssuesView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  mIssuesView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  mIssuesView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  mIssuesView->setRootIsDecorated(false);
  mIssuesView->setIndentation(0);
  mIssuesView->setUniformRowHeights(true);
  mIssuesView->setItemDelegate(
      new NavigatorDelegate(mIssuesView->font(), mIssuesView));
  mIssuesView->setMinimumHeight(96);

  QVBoxLayout *issuesLayout = new QVBoxLayout(mIssuesPanel);
  issuesLayout->setContentsMargins(0, 0, 0, 0);
  issuesLayout->setSpacing(0);
  issuesLayout->addLayout(issuesHeader);
  issuesLayout->addWidget(mIssuesRemoteFilter);
  issuesLayout->addWidget(mIssuesView);

  mModel = new RepositoryNavigatorModel(mView);
  mView->setModel(mModel);
  mIssuesView->setModel(mModel);

  connect(mModel, &RepositoryNavigatorModel::modelAboutToBeReset, this, [this] {
    mReferenceBeforeReset =
        mView->currentIndex()
            .data(RepositoryNavigatorModel::ReferenceRole)
            .value<git::Reference>();
    mIssuesScrollBeforeReset = mIssuesView->verticalScrollBar()->value();
  });
  connect(mModel, &RepositoryNavigatorModel::modelReset, this, [this] {
    restoreExpansion();
    if (mReferenceBeforeReset.isValid())
      selectReference(mReferenceBeforeReset);
    mReferenceBeforeReset = git::Reference();
    updateGitHubIssuesPanel();
  });
  connect(mView, &QTreeView::expanded, this,
          [this](const QModelIndex &index) { storeExpansion(index, true); });
  connect(mView, &QTreeView::collapsed, this,
          [this](const QModelIndex &index) { storeExpansion(index, false); });
  connect(mView, &QTreeView::clicked, this,
          [this](const QModelIndex &index) { activate(index, false); });
  connect(mView, &QTreeView::doubleClicked, this,
          [this](const QModelIndex &index) { activate(index, true); });
  connect(mIssuesRemoteFilter, &QComboBox::currentIndexChanged, this,
          &RepositoryNavigator::selectGitHubIssuesRepository);
  connect(mIssuesRefresh, &QToolButton::clicked, this,
          [this] { requestGitHubIssues(true); });

  mView->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(mView, &QTreeView::customContextMenuRequested, this,
          &RepositoryNavigator::showContextMenu);
  mIssuesView->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(mIssuesView, &QTreeView::customContextMenuRequested, this,
          &RepositoryNavigator::showContextMenu);
  connect(mIssuesView, &QTreeView::doubleClicked, this,
          [this](const QModelIndex &index) { openIssue(index); });

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(mView, 2);
  layout->addWidget(mIssuesPanel, 1);

  Accounts *accounts = Accounts::instance();
  connect(accounts, &Accounts::accountAdded, this,
          &RepositoryNavigator::discoverGitHubIssuesRepositories);
  connect(accounts, &Accounts::accountRemoved, this,
          &RepositoryNavigator::discoverGitHubIssuesRepositories);
  connect(accounts, &Accounts::repositoryAdded, this,
          [this](int) { discoverGitHubIssuesRepositories(); });
  connect(accounts, &Accounts::finished, this,
          [this](int) { discoverGitHubIssuesRepositories(); });
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
              requestGitHubIssues(false);
          });

  restoreExpansion();
  updateGitHubIssuesPanel();
}

void RepositoryNavigator::setRepository(const git::Repository &repo) {
  for (const QMetaObject::Connection &connection : mRemoteConnections)
    disconnect(connection);
  mRemoteConnections.clear();
  mModel->setRepository(repo);
  if (repo.isValid()) {
    git::RepositoryNotifier *notifier = repo.notifier();
    auto rediscover = [this] { discoverGitHubIssuesRepositories(); };
    mRemoteConnections.append(connect(
        notifier, &git::RepositoryNotifier::remoteAdded, this, rediscover));
    mRemoteConnections.append(connect(
        notifier, &git::RepositoryNotifier::remoteRemoved, this, rediscover));
    mRemoteConnections.append(connect(
        notifier, &git::RepositoryNotifier::remoteUpdated, this, rediscover));
  }
  discoverGitHubIssuesRepositories();
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

QTreeView *RepositoryNavigator::issuesView() const { return mIssuesView; }

QComboBox *RepositoryNavigator::issuesRemoteFilter() const {
  return mIssuesRemoteFilter;
}

void RepositoryNavigator::setBodyFont(const QFont &font) {
  mView->setStyleSheet(
      mView->styleSheet() +
      QString("\nfont-size: %1pt;").arg(FontUtils::pointSize(font)));
  mView->setFont(FontUtils::copySize(mView->font(), font));
  mView->viewport()->update();
  mIssuesPanel->setFont(FontUtils::copySize(mIssuesPanel->font(), font));
  mIssuesView->setFont(FontUtils::copySize(mIssuesView->font(), font));
  mIssuesRemoteFilter->setFont(
      FontUtils::copySize(mIssuesRemoteFilter->font(), font));
  mIssuesView->viewport()->update();
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
  QTreeView *source = qobject_cast<QTreeView *>(sender());
  if (!source)
    source = mView;
  QModelIndex index = source->indexAt(point);
  if (!index.isValid())
    return;

  QMenu menu;
  auto section = static_cast<RepositoryNavigatorModel::Section>(
      index.data(RepositoryNavigatorModel::SectionRole).toInt());
  auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
      index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
  if (section == RepositoryNavigatorModel::Section::GitHubIssues) {
    if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssuesFilter) {
      showIssuesRepositoryMenu(index);
      return;
    }
    if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssue)
      menu.addAction(tr("Open in Browser"), this,
                     [this, index] { openIssue(index); });
    if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssue ||
        (kind == RepositoryNavigatorModel::ItemKind::Section &&
         index.data(RepositoryNavigatorModel::AvailableRole).toBool()))
      menu.addAction(tr("Refresh Issues"), this,
                     [this] { requestGitHubIssues(true); });
    if (!menu.isEmpty())
      menu.exec(source->viewport()->mapToGlobal(point));
    return;
  }

  if (!mRepoView || !index.parent().isValid())
    return;

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
    menu.exec(source->viewport()->mapToGlobal(point));
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
  if (!index.isValid() || !index.parent().isValid())
    return;

  auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
      index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
  if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssuesFilter) {
    showIssuesRepositoryMenu(index);
    return;
  }
  if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssue)
    return;

  if (!mRepoView)
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

void RepositoryNavigator::discoverGitHubIssuesRepositories() {
  QString previousKey;
  int previous = mIssuesRemoteFilter->currentIndex();
  if (previous >= 0 && previous < mIssuesCandidates.size())
    previousKey = mIssuesCandidates.at(previous).key;

  mIssuesCandidates.clear();
  git::Repository repo = mModel->repository();
  if (repo.isValid()) {
    Accounts *accounts = Accounts::instance();
    for (const git::Remote &remote : repo.remotes()) {
      GitHub::RemoteRepository parsed = GitHub::parseRemoteUrl(remote.url());
      if (!parsed.isValid())
        continue;

      GitHub *github = nullptr;
      if (::Repository *hostRepo = accounts->lookup(remote.url())) {
        if (hostRepo->account()->kind() == Account::GitHub)
          github = qobject_cast<GitHub *>(hostRepo->account());
      }
      bool publicGitHub =
          parsed.host.compare("github.com", Qt::CaseInsensitive) == 0;
      if (!publicGitHub && !github)
        continue;
      if (publicGitHub && github && github->hasCustomUrl())
        github = nullptr;
      if (publicGitHub && !github) {
        if (!mAnonymousGitHub) {
          mAnonymousGitHub = new GitHub(QString());
          mAnonymousGitHub->setParent(this);
        }
        github = mAnonymousGitHub;
      }

      IssuesCandidate candidate;
      candidate.remote = remote.name();
      candidate.host = parsed.host;
      candidate.owner = parsed.owner;
      candidate.repository = parsed.name;
      candidate.account = github;
      QString accountKey = github == mAnonymousGitHub
                               ? QStringLiteral("anonymous")
                               : QString::number(
                                     reinterpret_cast<quintptr>(github), 16);
      candidate.key = candidate.remote + '\n' + candidate.host + '\n' +
                      candidate.owner + '/' + candidate.repository + '\n' +
                      accountKey;
      mIssuesCandidates.append(candidate);
    }

    QString saved = repo.appConfig().value<QString>(kIssuesRemoteKey);
    QString defaultRemote = repo.defaultRemote().isValid()
                                ? repo.defaultRemote().name()
                                : QString();
    std::sort(mIssuesCandidates.begin(), mIssuesCandidates.end(),
              [saved, defaultRemote](const IssuesCandidate &lhs,
                                     const IssuesCandidate &rhs) {
                auto rank = [saved, defaultRemote](const IssuesCandidate &c) {
                  if (!saved.isEmpty() && c.remote == saved)
                    return 0;
                  if (!defaultRemote.isEmpty() && c.remote == defaultRemote)
                    return 1;
                  if (c.remote == "origin")
                    return 2;
                  if (c.remote == "upstream")
                    return 3;
                  return 4;
                };
                int lhsRank = rank(lhs);
                int rhsRank = rank(rhs);
                return lhsRank == rhsRank
                           ? QString::localeAwareCompare(lhs.remote,
                                                        rhs.remote) < 0
                           : lhsRank < rhsRank;
              });
  }

  int selected = -1;
  QString saved = repo.isValid()
                      ? repo.appConfig().value<QString>(kIssuesRemoteKey)
                      : QString();
  for (int i = 0; i < mIssuesCandidates.size(); ++i) {
    if ((!previousKey.isEmpty() && mIssuesCandidates.at(i).key == previousKey) ||
        (previousKey.isEmpty() && !saved.isEmpty() &&
         mIssuesCandidates.at(i).remote == saved)) {
      selected = i;
      break;
    }
  }
  if (selected < 0 && !mIssuesCandidates.isEmpty())
    selected = 0;

  QSignalBlocker blocker(mIssuesRemoteFilter);
  mIssuesRemoteFilter->clear();
  for (const IssuesCandidate &candidate : mIssuesCandidates) {
    mIssuesRemoteFilter->addItem(
        tr("%1 - %2/%3")
            .arg(candidate.remote, candidate.owner, candidate.repository));
  }
  mIssuesRemoteFilter->setCurrentIndex(selected);
  mModel->setGitHubIssuesAvailable(selected >= 0);
  mModel->setGitHubIssuesFilter(selected >= 0
                                    ? mIssuesRemoteFilter->itemText(selected)
                                    : QString());
  if (selected < 0) {
    updateGitHubIssuesPanel();
    return;
  }

  const QString key = mIssuesCandidates.at(selected).key;
  applyGitHubIssuesCache(key);
  requestGitHubIssues(false);
}

void RepositoryNavigator::selectGitHubIssuesRepository(int index) {
  if (index < 0 || index >= mIssuesCandidates.size())
    return;
  mModel->setGitHubIssuesFilter(mIssuesRemoteFilter->itemText(index));
  git::Repository repo = mModel->repository();
  if (repo.isValid())
    repo.appConfig().setValue(kIssuesRemoteKey,
                              mIssuesCandidates.at(index).remote);
  mIssuesView->verticalScrollBar()->setValue(0);
  applyGitHubIssuesCache(mIssuesCandidates.at(index).key);
  requestGitHubIssues(false);
}

void RepositoryNavigator::requestGitHubIssues(bool force) {
  int index = mIssuesRemoteFilter->currentIndex();
  if (index < 0 || index >= mIssuesCandidates.size())
    return;
  const IssuesCandidate candidate = mIssuesCandidates.at(index);
  if (!candidate.account)
    return;

  const qint64 now = mClock();
  IssuesCacheEntry &entry = mIssuesCache[candidate.key];
  if (entry.inFlight) {
    updateGitHubIssuesPanel();
    return;
  }
  if (force && entry.lastAttempt > 0 &&
      now - entry.lastAttempt < kIssuesManualRefreshIntervalMs)
    return;
  const bool fresh = entry.hasValue &&
                     now - entry.lastSuccess < kIssuesCacheLifetimeMs;
  if (!force && (fresh || now < entry.retryAfter))
    return;

  entry.inFlight = true;
  entry.lastAttempt = now;
  const int generation = ++entry.generation;
  mModel->beginGitHubIssuesLoad(entry.hasValue);
  updateGitHubIssuesPanel();
  QTimer::singleShot(kIssuesManualRefreshIntervalMs, this,
                     &RepositoryNavigator::updateGitHubIssuesPanel);
  QPointer<RepositoryNavigator> guard(this);
  GitHub::IssuesCallback callback =
      [guard, key = candidate.key, generation](
          bool success, const GitHub::Issues &issues, int,
          const QString &error) {
        if (!guard)
          return;
        auto it = guard->mIssuesCache.find(key);
        if (it == guard->mIssuesCache.end() || it->generation != generation)
          return;

        IssuesCacheEntry &entry = it.value();
        entry.inFlight = false;
        if (success) {
          entry.issues = issues;
          entry.error.clear();
          entry.lastSuccess = guard->mClock();
          entry.retryAfter = 0;
          entry.hasValue = true;
        } else {
          entry.error = error;
          entry.retryAfter = guard->mClock() + kIssuesRetryDelayMs;
        }

        if (guard->currentGitHubIssuesKey() == key) {
          if (success)
            guard->mModel->setGitHubIssues(entry.issues);
          else
            guard->mModel->failGitHubIssues(entry.error, entry.hasValue);
        }
        guard->updateGitHubIssuesPanel();
      };
  if (mIssuesRequest) {
    mIssuesRequest(candidate.account, candidate.owner, candidate.repository,
                   callback);
  } else {
    candidate.account->requestOpenIssues(candidate.owner, candidate.repository,
                                         callback);
  }
}

void RepositoryNavigator::applyGitHubIssuesCache(const QString &key) {
  auto it = mIssuesCache.constFind(key);
  if (it == mIssuesCache.cend())
    return;

  const IssuesCacheEntry &entry = it.value();
  if (entry.hasValue)
    mModel->setGitHubIssues(entry.issues);
  if (!entry.error.isEmpty())
    mModel->failGitHubIssues(entry.error, entry.hasValue);
  else if (entry.inFlight)
    mModel->beginGitHubIssuesLoad(entry.hasValue);
}

QString RepositoryNavigator::currentGitHubIssuesKey() const {
  const int index = mIssuesRemoteFilter->currentIndex();
  return index >= 0 && index < mIssuesCandidates.size()
             ? mIssuesCandidates.at(index).key
             : QString();
}

void RepositoryNavigator::updateGitHubIssuesPanel() {
  const QModelIndex section = mModel->sectionIndex(
      RepositoryNavigatorModel::Section::GitHubIssues);
  if (!section.isValid())
    return;

  mView->setRowHidden(section.row(), QModelIndex(), true);
  mIssuesView->setRootIndex(section);
  if (mModel->rowCount(section) > 0) {
    const QModelIndex first = mModel->index(0, 0, section);
    const auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
        first.data(RepositoryNavigatorModel::ItemKindRole).toInt());
    mIssuesView->setRowHidden(
        0, section,
        kind == RepositoryNavigatorModel::ItemKind::GitHubIssuesFilter);
  }

  const int count = section.data(RepositoryNavigatorModel::CountRole).toInt();
  mIssuesTitle->setText(tr("GitHub Issues (%1)").arg(count));
  const bool available = !currentGitHubIssuesKey().isEmpty();
  bool inFlight = false;
  bool manualRefreshReady = true;
  auto it = mIssuesCache.constFind(currentGitHubIssuesKey());
  if (it != mIssuesCache.cend()) {
    inFlight = it->inFlight;
    manualRefreshReady = it->lastAttempt == 0 ||
                         mClock() - it->lastAttempt >=
                             kIssuesManualRefreshIntervalMs;
  }
  mIssuesPanel->setVisible(available);
  mIssuesRemoteFilter->setVisible(available);
  mIssuesView->setVisible(available);
  mIssuesRemoteFilter->setEnabled(available);
  mIssuesRefresh->setEnabled(available && !inFlight && manualRefreshReady);
  mIssuesPanel->setSizePolicy(
      QSizePolicy::Preferred,
      available ? QSizePolicy::Preferred : QSizePolicy::Fixed);

  const int scroll = mIssuesScrollBeforeReset;
  QTimer::singleShot(0, this, [this, scroll] {
    mIssuesView->verticalScrollBar()->setValue(
        qMin(scroll, mIssuesView->verticalScrollBar()->maximum()));
  });
}

void RepositoryNavigator::openIssue(const QModelIndex &index) {
  QUrl url(index.data(RepositoryNavigatorModel::UrlRole).toString());
  QString scheme = url.scheme().toLower();
  if (url.isValid() && !url.host().isEmpty() &&
      (scheme == "http" || scheme == "https"))
    QDesktopServices::openUrl(url);
}

void RepositoryNavigator::showIssuesRepositoryMenu(const QModelIndex &index) {
  QMenu menu(this);
  for (int i = 0; i < mIssuesRemoteFilter->count(); ++i) {
    QAction *action = menu.addAction(mIssuesRemoteFilter->itemText(i));
    action->setCheckable(true);
    action->setChecked(i == mIssuesRemoteFilter->currentIndex());
    connect(action, &QAction::triggered, this,
            [this, i] { mIssuesRemoteFilter->setCurrentIndex(i); });
  }
  if (menu.isEmpty())
    return;

  QRect row = mView->visualRect(index);
  menu.exec(mView->viewport()->mapToGlobal(row.bottomLeft()));
}

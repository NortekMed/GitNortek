//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryNavigator.h"
#include "FontUtils.h"
#include "MainWindow.h"
#include "RepositoryNavigatorModel.h"
#include "RepoView.h"
#include "StatePushButton.h"
#include "TabWidget.h"
#include "WorktreeIcon.h"
#include "dialogs/WorktreeDialog.h"
#include "git/Branch.h"
#include "git/Config.h"
#include "git/Remote.h"
#include "git/Result.h"
#include "host/Accounts.h"
#include "host/Repository.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <functional>

namespace {

const QString kExpandedGroup = "sidebar/repositoryNavigator/expanded";
const QString kIssuesRemoteKey = "sidebar.githubIssues.remote";
constexpr qint64 kIssuesCacheLifetimeMs = 5 * 60 * 1000;
constexpr qint64 kIssuesManualRefreshIntervalMs = 10 * 1000;
constexpr qint64 kIssuesRetryDelayMs = 60 * 1000;
constexpr int kMaximumVisibleRows = 5;
constexpr int kSectionBottomSpacing = 24;

struct WorktreeCreationResult {
  QString path;
  git::Result result;
};

QString sectionKey(RepositoryNavigatorModel::Section section) {
  QMetaEnum meta = QMetaEnum::fromType<RepositoryNavigatorModel::Section>();
  return QString::fromLatin1(meta.valueToKey(static_cast<int>(section)));
}

bool hasScrollingBody(RepositoryNavigatorModel::Section section) {
  using Section = RepositoryNavigatorModel::Section;
  return section == Section::Local || section == Section::Remote ||
         section == Section::Worktrees || section == Section::Stashes ||
         section == Section::GitHubIssues || section == Section::Tags ||
         section == Section::Submodules;
}

QString canonicalPath(const QString &path) {
  QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                              : canonical);
}

QString sectionIconPath(RepositoryNavigatorModel::Section section) {
  using Section = RepositoryNavigatorModel::Section;
  switch (section) {
    case Section::Local:
      return ":/branches.png";
    case Section::Remote:
      return ":/remotes.png";
    case Section::Worktrees:
      return QString();
    case Section::Stashes:
      return ":/diff.png";
    case Section::CloudPatches:
      return ":/cloud.png";
    case Section::PullRequests:
      return ":/pull@2x.png";
    case Section::GitHubIssues:
      return ":/github.png";
    case Section::Teams:
      return ":/general.png";
    case Section::Tags:
      return ":/file.png";
    case Section::Submodules:
      return ":/submodules.png";
    case Section::Count:
      return QString();
  }
  return QString();
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

void drawHomeIcon(QPainter *painter, const QRect &rect, const QColor &color) {
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  QPen pen(color, 1.4);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  painter->setPen(pen);
  painter->setBrush(Qt::NoBrush);

  QPainterPath roof;
  roof.moveTo(rect.left() + 1, rect.top() + 6);
  roof.lineTo(rect.center().x(), rect.top() + 2);
  roof.lineTo(rect.right() - 1, rect.top() + 6);
  painter->drawPath(roof);
  painter->drawRect(rect.adjusted(3, 6, -3, -1));
  painter->drawLine(rect.center().x(), rect.bottom() - 4, rect.center().x(),
                    rect.bottom() - 1);
  painter->restore();
}

class SectionHeader : public QWidget {
public:
  SectionHeader(bool topDivider, QWidget *parent)
      : QWidget(parent), mTopDivider(topDivider) {}

  std::function<void()> clicked;

protected:
  void paintEvent(QPaintEvent *event) override {
    QWidget::paintEvent(event);
    if (!mTopDivider)
      return;

    QPainter painter(this);
    painter.setPen(palette().color(QPalette::ButtonText));
    painter.drawLine(0, 0, width() - 1, 0);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        rect().contains(event->position().toPoint()) && clicked) {
      clicked();
      event->accept();
      return;
    }
    QWidget::mouseReleaseEvent(event);
  }

private:
  bool mTopDivider;
};

class SectionSplitterHandle : public QSplitterHandle {
public:
  using QSplitterHandle::QSplitterHandle;

protected:
  void paintEvent(QPaintEvent *event) override {
    QSplitterHandle::paintEvent(event);
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::ButtonText));
    if (orientation() == Qt::Vertical)
      painter.drawLine(0, height() - 1, width() - 1, height() - 1);
    else
      painter.drawLine(width() - 1, 0, width() - 1, height() - 1);
  }
};

class SectionSplitter : public QSplitter {
public:
  using QSplitter::QSplitter;

  void setPreferredHeight(int height) {
    if (mPreferredHeight == height)
      return;
    mPreferredHeight = height;
    updateGeometry();
  }

  QSize sizeHint() const override {
    QSize size = QSplitter::sizeHint();
    size.setHeight(mPreferredHeight);
    return size;
  }

protected:
  QSplitterHandle *createHandle() override {
    return new SectionSplitterHandle(orientation(), this);
  }

private:
  int mPreferredHeight = 0;
};

class SectionIcon : public QWidget {
public:
  SectionIcon(RepositoryNavigatorModel::Section section, QWidget *parent)
      : QWidget(parent), mSection(section), mIcon(sectionIconPath(section)) {
    setFixedSize(16, 16);
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    if (mSection == RepositoryNavigatorModel::Section::Worktrees) {
      WorktreeIcon::paint(&painter, rect().adjusted(1, 1, -1, -1));
      return;
    }
    mIcon.paint(&painter, rect(), Qt::AlignCenter,
                isEnabled() ? QIcon::Normal : QIcon::Disabled);
  }

private:
  RepositoryNavigatorModel::Section mSection;
  QIcon mIcon;
};

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
                                 : palette.window().color();
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
    auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
        index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
    bool worktree = !section &&
                    kind == RepositoryNavigatorModel::ItemKind::Worktree;
    if (worktree) {
      QRect iconRect(0, 0, 14, 14);
      iconRect.moveCenter(QPoint(content.x() + 7, content.center().y()));
      if (index.data(RepositoryNavigatorModel::MainWorktreeRole).toBool())
        drawHomeIcon(painter, iconRect, text);
      else
        WorktreeIcon::paint(painter, iconRect);
      content.adjust(19, 0, 0, 0);
    }

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
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

  mModel = new RepositoryNavigatorModel(this);
  mSectionSplitter = new SectionSplitter(Qt::Vertical, this);
  mSectionSplitter->setObjectName("RepositorySectionSplitter");
  mSectionSplitter->setChildrenCollapsible(false);

  QWidget *actionBar = new QWidget(this);
  actionBar->setObjectName("RepositoryNavigationActionBar");
  actionBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  mExpandCollapseAllButton =
      new StatePushButton(QObject::tr("Collapse all"),
                          QObject::tr("Expand all"), actionBar);
  mExpandCollapseAllButton->setObjectName(
      "RepositoryNavigationExpandCollapseAll");
  QHBoxLayout *actionLayout = new QHBoxLayout(actionBar);
  actionLayout->setContentsMargins(4, 4, 8, 4);
  actionLayout->addStretch();
  actionLayout->addWidget(mExpandCollapseAllButton);
  connect(mExpandCollapseAllButton, &QPushButton::clicked, this,
          &RepositoryNavigator::toggleAllPanels);

  for (int value = 0;
       value < static_cast<int>(RepositoryNavigatorModel::Section::Count);
       ++value) {
    auto section = static_cast<RepositoryNavigatorModel::Section>(value);
    const QString key = sectionKey(section);
    QWidget *container = new QWidget(mSectionSplitter);
    container->setObjectName("RepositoryNavigation" + key + "Panel");
    auto *header = new SectionHeader(value == 0, container);
    header->setObjectName("RepositoryNavigation" + key + "Header");
    QToolButton *toggle = new QToolButton(header);
    toggle->setObjectName("RepositoryNavigation" + key + "Toggle");
    toggle->setCheckable(true);
    toggle->setAutoRaise(true);
    toggle->setArrowType(Qt::DownArrow);
    SectionIcon *icon = new SectionIcon(section, header);
    icon->setObjectName("RepositoryNavigation" + key + "Icon");
    QLabel *title = new QLabel(header);
    title->setObjectName("RepositoryNavigation" + key + "Title");
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setCapitalization(QFont::SmallCaps);
    title->setFont(titleFont);
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->setCursor(Qt::PointingHandCursor);
    header->clicked = [toggle] { toggle->click(); };

    QToolButton *action = nullptr;
    if (section == RepositoryNavigatorModel::Section::Worktrees) {
      action = new QToolButton(header);
      action->setObjectName("RepositoryNavigationWorktreesAdd");
      action->setAccessibleName(tr("Create worktree"));
      action->setToolTip(tr("Create worktree"));
      action->setText("+");
      action->setAutoRaise(true);
      mWorktreeAdd = action;
      connect(action, &QToolButton::clicked, this,
              &RepositoryNavigator::promptToCreateWorktree);
    }

    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 2, 8, 2);
    headerLayout->setSpacing(2);
    headerLayout->addWidget(toggle);
    headerLayout->addWidget(icon);
    headerLayout->addWidget(title, 1);
    if (action)
      headerLayout->addWidget(action);

    QWidget *body = new QWidget(container);
    QVBoxLayout *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    QTreeView *view = nullptr;
    if (hasScrollingBody(section)) {
      view = new QTreeView(body);
      view->setObjectName(section ==
                                  RepositoryNavigatorModel::Section::GitHubIssues
                              ? QStringLiteral("GitHubIssuesView")
                              : "RepositoryNavigation" + key + "View");
      view->setAccessibleName(tr("%1 items").arg(key));
      view->setHeaderHidden(true);
      view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
      view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
      view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
      view->setRootIsDecorated(false);
      view->setIndentation(0);
      view->setUniformRowHeights(true);
      view->setBackgroundRole(QPalette::Window);
      view->viewport()->setBackgroundRole(QPalette::Window);
      view->setItemDelegate(new NavigatorDelegate(view->font(), view));
      view->setMinimumHeight(0);
      view->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
      view->setModel(mModel);
    }

    QVBoxLayout *panelLayout = new QVBoxLayout(container);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);
    panelLayout->addWidget(header);
    panelLayout->addWidget(body, 1);
    if (view)
      bodyLayout->addWidget(view);
    QWidget *spacer = new QWidget(body);
    spacer->setObjectName("RepositoryNavigation" + key + "Spacer");
    spacer->setFixedHeight(kSectionBottomSpacing);
    bodyLayout->addWidget(spacer);

    body->setMinimumHeight(0);
    body->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    mPanels.append(
        {section, container, header, toggle, icon, title, action, body, view, 0});
    mSectionSplitter->addWidget(container);
    mSectionSplitter->setCollapsible(value, false);

    connect(toggle, &QToolButton::toggled, this, [this, section](bool expanded) {
      setPanelExpanded(section, expanded, expanded);
      storeExpansion(section, expanded);
      updateExpandCollapseAllButton();
    });
    if (view) {
      connect(view->selectionModel(), &QItemSelectionModel::currentChanged,
              this, [this, view](const QModelIndex &current) {
                if (current.isValid())
                  clearOtherSelections(view);
              });
      view->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(view, &QTreeView::customContextMenuRequested, this,
              &RepositoryNavigator::showContextMenu);
      connect(view, &QTreeView::clicked, this,
              [this, view](const QModelIndex &index) {
                clearOtherSelections(view);
                activate(index, false);
              });
      connect(view, &QTreeView::doubleClicked, this,
              [this, view](const QModelIndex &index) {
                clearOtherSelections(view);
                auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
                    index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
                if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssue)
                  openIssue(index);
                else
                  activate(index, true);
              });
    }
  }

  SectionPanel *issuesPanel =
      panel(RepositoryNavigatorModel::Section::GitHubIssues);
  mIssuesRemoteFilter = new QComboBox(issuesPanel->body);
  mIssuesRemoteFilter->setObjectName("GitHubIssuesRemoteFilter");
  mIssuesRemoteFilter->setAccessibleName(tr("Issues repository"));
  mIssuesRemoteFilter->setPlaceholderText(tr("Issues repository"));
  qobject_cast<QVBoxLayout *>(issuesPanel->body->layout())
      ->insertWidget(0, mIssuesRemoteFilter);

  connect(mModel, &RepositoryNavigatorModel::modelAboutToBeReset, this, [this] {
    mReferenceBeforeReset = git::Reference();
    mWorktreePathBeforeReset.clear();
    for (SectionPanel &panel : mPanels) {
      if (!panel.view)
        continue;
      QModelIndex current = panel.view->currentIndex();
      git::Reference ref;
      if (panel.view->selectionModel()->isSelected(current))
        ref = current.data(RepositoryNavigatorModel::ReferenceRole)
                  .value<git::Reference>();
      if (ref.isValid())
        mReferenceBeforeReset = ref;
      if (panel.view->selectionModel()->isSelected(current) &&
          static_cast<RepositoryNavigatorModel::ItemKind>(
              current.data(RepositoryNavigatorModel::ItemKindRole).toInt()) ==
              RepositoryNavigatorModel::ItemKind::Worktree)
        mWorktreePathBeforeReset =
            current.data(RepositoryNavigatorModel::PathRole).toString();
      panel.scrollBeforeReset = panel.view->verticalScrollBar()->value();
    }
  });
  connect(mModel, &RepositoryNavigatorModel::modelReset, this, [this] {
    restoreExpansion();
    updatePanels();
    if (mReferenceBeforeReset.isValid())
      selectReference(mReferenceBeforeReset);
    else if (!mWorktreePathBeforeReset.isEmpty())
      selectWorktree(mWorktreePathBeforeReset, false);
    mReferenceBeforeReset = git::Reference();
    mWorktreePathBeforeReset.clear();
    QTimer::singleShot(0, this, [this] {
      for (SectionPanel &panel : mPanels) {
        if (panel.view)
          panel.view->verticalScrollBar()->setValue(qMin(
              panel.scrollBeforeReset,
              panel.view->verticalScrollBar()->maximum()));
      }
    });
  });
  connect(mIssuesRemoteFilter, &QComboBox::currentIndexChanged, this,
          &RepositoryNavigator::selectGitHubIssuesRepository);
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(actionBar);
  layout->addWidget(mSectionSplitter);
  mCollapsedSpacer = new QWidget(this);
  mCollapsedSpacer->setSizePolicy(QSizePolicy::Preferred,
                                  QSizePolicy::Expanding);
  layout->addWidget(mCollapsedSpacer);

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
  updatePanels();
  QTimer::singleShot(0, this, &RepositoryNavigator::updatePanelSizes);
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
  disconnect(mRefreshConnection);
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
    mRefreshConnection = connect(view, &RepoView::manualRefreshRequested, this,
                                 &RepositoryNavigator::refresh);
    git::Reference head = view->repo().head();
    if (head.isLocalBranch())
      setPanelExpanded(RepositoryNavigatorModel::Section::Local, true, true);
    selectReference(head);
  }
}

RepositoryNavigatorModel *RepositoryNavigator::model() const { return mModel; }

void RepositoryNavigator::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  QTimer::singleShot(0, this, &RepositoryNavigator::updatePanelSizes);
}

QTreeView *RepositoryNavigator::sectionView(
    RepositoryNavigatorModel::Section section) const {
  const SectionPanel *sectionPanel = panel(section);
  return sectionPanel ? sectionPanel->view : nullptr;
}

QComboBox *RepositoryNavigator::issuesRemoteFilter() const {
  return mIssuesRemoteFilter;
}

void RepositoryNavigator::setBodyFont(const QFont &font) {
  for (SectionPanel &panel : mPanels) {
    panel.container->setFont(
        FontUtils::copySize(panel.container->font(), font));
    if (panel.view) {
      panel.view->setFont(FontUtils::copySize(panel.view->font(), font));
      panel.view->viewport()->update();
    }
  }
  mIssuesRemoteFilter->setFont(
      FontUtils::copySize(mIssuesRemoteFilter->font(), font));
  updatePanelSizes();
}

void RepositoryNavigator::restoreExpansion() {
  QSettings settings;
  settings.beginGroup(kExpandedGroup);
  for (int row = 0; row < mModel->rowCount(); ++row) {
    QModelIndex index = mModel->index(row, 0);
    auto section = static_cast<RepositoryNavigatorModel::Section>(
        index.data(RepositoryNavigatorModel::SectionRole).toInt());
    bool defaultExpanded =
        index.data(RepositoryNavigatorModel::AvailableRole).toBool();
    setPanelExpanded(
        section, settings.value(sectionKey(section), defaultExpanded).toBool());
  }
  settings.endGroup();
}

void RepositoryNavigator::storeExpansion(
    RepositoryNavigatorModel::Section section, bool expanded) {
  QSettings settings;
  settings.beginGroup(kExpandedGroup);
  settings.setValue(sectionKey(section), expanded);
  settings.endGroup();
}

void RepositoryNavigator::setPanelExpanded(
    RepositoryNavigatorModel::Section section, bool expanded, bool prioritize) {
  SectionPanel *sectionPanel = panel(section);
  if (!sectionPanel)
    return;

  QModelIndex index = mModel->sectionIndex(section);
  const bool available =
      index.data(RepositoryNavigatorModel::AvailableRole).toBool();
  const bool effectiveExpanded = expanded && available && sectionPanel->view;
  const bool showBody = effectiveExpanded;
  if (prioritize && effectiveExpanded)
    mPrioritizedSection = section;
  else if (!effectiveExpanded && mPrioritizedSection == section)
    mPrioritizedSection = RepositoryNavigatorModel::Section::Count;
  QSignalBlocker blocker(sectionPanel->toggle);
  sectionPanel->toggle->setChecked(effectiveExpanded);
  sectionPanel->toggle->setArrowType(effectiveExpanded ? Qt::DownArrow
                                                       : Qt::RightArrow);
  sectionPanel->body->setVisible(showBody);
  sectionPanel->container->setMaximumHeight(
      showBody ? expandedPanelHeight(*sectionPanel)
               : sectionPanel->header->sizeHint().height());
  updatePanelSizes();
  updateExpandCollapseAllButton();
}

bool RepositoryNavigator::allAvailablePanelsExpanded() const {
  bool found = false;
  for (const SectionPanel &panel : mPanels) {
    QModelIndex index = mModel->sectionIndex(panel.section);
    if (!panel.view ||
        !index.data(RepositoryNavigatorModel::AvailableRole).toBool())
      continue;
    found = true;
    if (!panel.toggle->isChecked())
      return false;
  }
  return found;
}

void RepositoryNavigator::toggleAllPanels() {
  const bool expand = !allAvailablePanelsExpanded();
  mPrioritizedSection = RepositoryNavigatorModel::Section::Count;
  for (SectionPanel &panel : mPanels) {
    QModelIndex index = mModel->sectionIndex(panel.section);
    if (panel.view &&
        index.data(RepositoryNavigatorModel::AvailableRole).toBool()) {
      setPanelExpanded(panel.section, expand);
      storeExpansion(panel.section, expand);
    }
  }
  updateExpandCollapseAllButton();
}

void RepositoryNavigator::updateExpandCollapseAllButton() {
  bool available = false;
  bool bodyVisible = false;
  int collapsedHeight = 0;
  for (const SectionPanel &panel : mPanels) {
    QModelIndex index = mModel->sectionIndex(panel.section);
    if (panel.view &&
        index.data(RepositoryNavigatorModel::AvailableRole).toBool()) {
      available = true;
    }
    bodyVisible |= !panel.body->isHidden();
    collapsedHeight += panel.header->sizeHint().height();
  }
  collapsedHeight += qMax(0, mPanels.size() - 1) *
                     mSectionSplitter->handleWidth();

  mExpandCollapseAllButton->setEnabled(available);
  mExpandCollapseAllButton->setState(allAvailablePanelsExpanded());
  mExpandCollapseAllButton->setAccessibleName(
      mExpandCollapseAllButton->text());
  mExpandCollapseAllButton->setToolTip(mExpandCollapseAllButton->text());
  if (!bodyVisible)
    mSectionSplitter->setMaximumHeight(collapsedHeight);
  mCollapsedSpacer->setVisible(true);
}

int RepositoryNavigator::expandedPanelHeight(
    const RepositoryNavigator::SectionPanel &panel) const {
  const int headerHeight = panel.header->sizeHint().height();
  if (!panel.view)
    return headerHeight;

  QModelIndex root = panel.view->rootIndex();
  int visibleRows = 0;
  int rowHeight = 0;
  for (int row = 0; row < mModel->rowCount(root); ++row) {
    if (panel.view->isRowHidden(row, root))
      continue;
    ++visibleRows;
    if (!rowHeight)
      rowHeight = panel.view->sizeHintForRow(row);
  }
  if (!rowHeight)
    rowHeight = panel.view->fontMetrics().height() + 6;

  int bodyHeight = qMin(visibleRows, kMaximumVisibleRows) * rowHeight +
                   2 * panel.view->frameWidth();
  bodyHeight += kSectionBottomSpacing;
  if (panel.section == RepositoryNavigatorModel::Section::GitHubIssues &&
      mIssuesRemoteFilter->isVisible())
    bodyHeight += mIssuesRemoteFilter->sizeHint().height();
  return headerHeight + bodyHeight;
}

void RepositoryNavigator::updatePanelSizes() {
  QList<int> sizes;
  sizes.reserve(mPanels.size());
  const int handlesHeight = qMax(0, mPanels.size() - 1) *
                            mSectionSplitter->handleWidth();
  int maximumHeight = handlesHeight;
  int collapsedHeight = handlesHeight;
  int priorityIndex = -1;
  for (SectionPanel &panel : mPanels) {
    const int headerHeight = panel.header->sizeHint().height();
    const int panelHeight =
        !panel.body->isHidden() ? expandedPanelHeight(panel) : headerHeight;
    panel.container->setMinimumHeight(headerHeight);
    panel.container->setMaximumHeight(panelHeight);
    sizes.append(panelHeight);
    maximumHeight += panelHeight;
    collapsedHeight += headerHeight;
    if (panel.section == mPrioritizedSection)
      priorityIndex = sizes.size() - 1;
  }
  const int actionHeight =
      mExpandCollapseAllButton->parentWidget()->sizeHint().height();
  const int availableHeight = qMax(0, height() - actionHeight);
  const int splitterHeight = qMin(maximumHeight, availableHeight);
  static_cast<SectionSplitter *>(mSectionSplitter)
      ->setPreferredHeight(splitterHeight);
  mSectionSplitter->setMaximumHeight(splitterHeight);

  if (priorityIndex >= 0 && splitterHeight < maximumHeight) {
    const int contentHeight = qMax(0, splitterHeight - handlesHeight);
    int remaining = qMax(0, contentHeight - (collapsedHeight - handlesHeight));
    const int priorityExtra =
        sizes.at(priorityIndex) -
        mPanels.at(priorityIndex).header->sizeHint().height();
    const int allocatedPriority = qMin(priorityExtra, remaining);
    remaining -= allocatedPriority;

    QList<int> extras;
    int totalExtras = 0;
    extras.reserve(mPanels.size());
    for (int index = 0; index < mPanels.size(); ++index) {
      const int extra =
          index == priorityIndex
              ? 0
              : sizes.at(index) -
                    mPanels.at(index).header->sizeHint().height();
      extras.append(extra);
      totalExtras += extra;
      sizes[index] = mPanels.at(index).header->sizeHint().height();
    }
    sizes[priorityIndex] += allocatedPriority;
    for (int index = 0; index < extras.size() && remaining > 0; ++index) {
      if (!extras.at(index) || !totalExtras)
        continue;
      const int allocated =
          qMin(extras.at(index), remaining * extras.at(index) / totalExtras);
      sizes[index] += allocated;
      remaining -= allocated;
      totalExtras -= extras.at(index);
    }
  }
  mSectionSplitter->setSizes(sizes);
}

void RepositoryNavigator::updatePanels() {
  for (SectionPanel &panel : mPanels) {
    QModelIndex index = mModel->sectionIndex(panel.section);
    if (!index.isValid())
      continue;

    const bool available =
        index.data(RepositoryNavigatorModel::AvailableRole).toBool();
    const int count = index.data(RepositoryNavigatorModel::CountRole).toInt();
    panel.title->setText(
        QString("%1 (%2)").arg(index.data().toString()).arg(count));
    const QString toggleText = tr("Toggle %1").arg(index.data().toString());
    panel.toggle->setAccessibleName(toggleText);
    panel.toggle->setToolTip(toggleText);
    panel.header->setAccessibleName(toggleText);
    panel.header->setToolTip(index.data(Qt::ToolTipRole).toString());
    panel.toggle->setEnabled(available && panel.view);
    panel.icon->setEnabled(available);
    panel.title->setEnabled(available);
    if (panel.action)
      panel.action->setEnabled(mModel->repository().isValid() &&
                               !mModel->repository().isBare() &&
                               !mCreatingWorktree);
    if (panel.view) {
      panel.view->setRootIndex(index);
      panel.view->setEnabled(available);
      panel.view->setAccessibleName(
          tr("%1 items").arg(index.data().toString()));
    }
    setPanelExpanded(panel.section, panel.toggle->isChecked());
  }
  updateExpandCollapseAllButton();

  SectionPanel *issues =
      panel(RepositoryNavigatorModel::Section::GitHubIssues);
  if (issues && issues->view && mModel->rowCount(issues->view->rootIndex()) > 0) {
    QModelIndex section = issues->view->rootIndex();
    QModelIndex first = mModel->index(0, 0, section);
    const auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
        first.data(RepositoryNavigatorModel::ItemKindRole).toInt());
    issues->view->setRowHidden(
        0, section,
        kind == RepositoryNavigatorModel::ItemKind::GitHubIssuesFilter);
  }
  mIssuesRemoteFilter->setVisible(!currentGitHubIssuesKey().isEmpty());
  updatePanelSizes();
}

void RepositoryNavigator::clearOtherSelections(QTreeView *selected) {
  for (SectionPanel &panel : mPanels) {
    if (panel.view && panel.view != selected) {
      panel.view->clearSelection();
      panel.view->setCurrentIndex(QModelIndex());
    }
  }
}

RepositoryNavigator::SectionPanel *RepositoryNavigator::panel(
    RepositoryNavigatorModel::Section section) {
  for (SectionPanel &panel : mPanels) {
    if (panel.section == section)
      return &panel;
  }
  return nullptr;
}

const RepositoryNavigator::SectionPanel *RepositoryNavigator::panel(
    RepositoryNavigatorModel::Section section) const {
  for (const SectionPanel &panel : mPanels) {
    if (panel.section == section)
      return &panel;
  }
  return nullptr;
}

void RepositoryNavigator::showContextMenu(const QPoint &point) {
  QTreeView *source = qobject_cast<QTreeView *>(sender());
  if (!source)
    return;
  QModelIndex index = source->indexAt(point);
  if (!index.isValid())
    return;

  QMenu menu;
  auto section = static_cast<RepositoryNavigatorModel::Section>(
      index.data(RepositoryNavigatorModel::SectionRole).toInt());
  auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
      index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
  if (section == RepositoryNavigatorModel::Section::GitHubIssues) {
    if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssue)
      menu.addAction(tr("Open in Browser"), this,
                     [this, index] { openIssue(index); });
    if (!menu.isEmpty())
      menu.exec(source->viewport()->mapToGlobal(point));
    return;
  }

  if (kind == RepositoryNavigatorModel::ItemKind::Worktree) {
    git::Worktree worktree = index.data(RepositoryNavigatorModel::WorktreeRole)
                                 .value<git::Worktree>();
    if (worktree.name().isEmpty())
      return;

    QAction *remove = menu.addAction(
        tr("Delete Worktree..."), this,
        [this, worktree] { promptToDeleteWorktree(worktree); });
    remove->setEnabled(!worktree.isMain());
    menu.exec(source->viewport()->mapToGlobal(point));
    return;
  }

  if (!mRepoView || !mRepoView->repo().isValid() ||
      !index.parent().isValid())
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
      if (!initialized) {
        menu.addAction(tr("Initialize"), mRepoView,
                       [view = mRepoView, submodule] {
                         if (view)
                           view->updateSubmodules({submodule}, true, true);
                       });
      }
      QList<git::Submodule> uninitialized;
      for (const git::Submodule &candidate : mRepoView->repo().submodules()) {
        if (!candidate.isInitialized())
          uninitialized.append(candidate);
      }
      if (!uninitialized.isEmpty()) {
        menu.addAction(
            tr("Initialize All Uninitialized"), mRepoView,
            [view = mRepoView, uninitialized] {
              if (view)
                view->updateSubmodules(uninitialized, true, true);
            });
      }
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

bool RepositoryNavigator::closeWorktreeTabs(const QString &path) {
  const QString target = canonicalPath(path);
  for (MainWindow *window : MainWindow::windows()) {
    for (int index = window->count() - 1; index >= 0; --index) {
      RepoView *view = window->view(index);
      if (view && canonicalPath(view->repo().workdir().path()) == target &&
          !window->tabWidget()->closeTab(view)) {
        return false;
      }
    }
  }
  return true;
}

void RepositoryNavigator::promptToDeleteWorktree(
    const git::Worktree &worktree) {
  if (worktree.isMain() || worktree.name().isEmpty())
    return;

  git::Repository linked = git::Repository::open(worktree.path());
  git::Result statusResult;
  const bool dirty = !linked.isValid() ||
                     linked.hasWorkdirChanges(&statusResult) || !statusResult;

  const QString text =
      tr("Delete worktree '%1' at '%2'?\n\nThe worktree folder and all of "
         "its contents will be permanently deleted. The branch itself will "
         "not be deleted.")
          .arg(worktree.name(), worktree.path());
  QMessageBox *message =
      new QMessageBox(QMessageBox::Warning, tr("Delete Worktree?"), text,
                      QMessageBox::Cancel, this);
  message->setAttribute(Qt::WA_DeleteOnClose);
  QPushButton *remove =
      message->addButton(tr("Delete Worktree"), QMessageBox::DestructiveRole);
  message->setDefaultButton(QMessageBox::Cancel);
  message->setEscapeButton(QMessageBox::Cancel);

  if (dirty) {
    message->setInformativeText(
        tr("This worktree contains uncommitted or untracked changes."));
    QCheckBox *acknowledge = new QCheckBox(
        tr("I understand that these changes will be permanently lost."),
        message);
    acknowledge->setObjectName(
        QStringLiteral("WorktreeDataLossAcknowledgment"));
    message->setCheckBox(acknowledge);
    remove->setEnabled(false);
    connect(acknowledge, &QCheckBox::toggled, remove, &QWidget::setEnabled);
  }

  const git::Repository repo = mModel->repository();
  connect(remove, &QPushButton::clicked, this, [this, repo, worktree] {
    if (!closeWorktreeTabs(worktree.path()))
      return;

    git::Result result = repo.removeWorktree(worktree);
    if (!result) {
      QMessageBox::critical(
          this, tr("Delete Worktree Failed"),
          tr("Could not delete worktree '%1'.\n\n%2")
              .arg(worktree.name(), result.errorString()));
    }
    mModel->refresh();
  });
  message->open();
}

void RepositoryNavigator::selectReference(const git::Reference &ref) {
  for (SectionPanel &panel : mPanels) {
    if (panel.view) {
      panel.view->clearSelection();
      panel.view->setCurrentIndex(QModelIndex());
    }
  }
  if (!ref.isValid()) {
    return;
  }
  for (RepositoryNavigatorModel::Section section :
       {RepositoryNavigatorModel::Section::Local,
        RepositoryNavigatorModel::Section::Remote,
        RepositoryNavigatorModel::Section::Tags}) {
    QTreeView *view = sectionView(section);
    if (!view)
      continue;
    QModelIndex parent = mModel->sectionIndex(section);
    for (int row = 0; row < mModel->rowCount(parent); ++row) {
      QModelIndex index = mModel->index(row, 0, parent);
      git::Reference candidate =
          index.data(RepositoryNavigatorModel::ReferenceRole)
              .value<git::Reference>();
      if (candidate.isValid() &&
          candidate.qualifiedName() == ref.qualifiedName()) {
        view->setCurrentIndex(index);
        view->selectionModel()->select(
            index,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        view->scrollTo(index);
        return;
      }
    }
  }
}

bool RepositoryNavigator::selectWorktree(const QString &path, bool focus) {
  QTreeView *view =
      sectionView(RepositoryNavigatorModel::Section::Worktrees);
  if (!view)
    return false;

  QModelIndex parent =
      mModel->sectionIndex(RepositoryNavigatorModel::Section::Worktrees);
  for (int row = 0; row < mModel->rowCount(parent); ++row) {
    QModelIndex index = mModel->index(row, 0, parent);
    if (index.data(RepositoryNavigatorModel::PathRole).toString() != path)
      continue;

    clearOtherSelections(view);
    view->setCurrentIndex(index);
    view->selectionModel()->select(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    view->scrollTo(index);
    if (focus)
      view->setFocus(Qt::MouseFocusReason);
    return true;
  }

  return false;
}

void RepositoryNavigator::activate(const QModelIndex &index, bool checkout) {
  if (!index.isValid() || !index.parent().isValid())
    return;

  auto kind = static_cast<RepositoryNavigatorModel::ItemKind>(
      index.data(RepositoryNavigatorModel::ItemKindRole).toInt());
  if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssuesFilter)
    return;
  if (kind == RepositoryNavigatorModel::ItemKind::GitHubIssue)
    return;
  if (kind == RepositoryNavigatorModel::ItemKind::Worktree) {
    if (!index.data(RepositoryNavigatorModel::AvailableRole).toBool())
      return;

    const QString path =
        index.data(RepositoryNavigatorModel::PathRole).toString();
    if (checkout) {
      emit openRepositoryRequested(path, false);
    } else {
      emit selectRepositoryRequested(path);
      selectWorktree(path, true);
    }
    return;
  }

  if (!mRepoView)
    return;

  git::Reference ref =
      index.data(RepositoryNavigatorModel::ReferenceRole).value<git::Reference>();
  if (ref.isValid()) {
    if (checkout && ref.isLocalBranch())
      mRepoView->checkoutFromNavigator(ref);
    else if (checkout)
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

void RepositoryNavigator::promptToCreateWorktree() {
  git::Repository repo = mModel->repository();
  if (!repo.isValid() || repo.isBare())
    return;
  const QString repositoryPath = repo.commonDir().path();

  WorktreeDialog *dialog = new WorktreeDialog(repo, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this, [this, dialog, repositoryPath] {
    const git::Branch selected = dialog->branch();
    const QString selectedName = selected.name();
    const git_branch_t branchType = selected.isRemoteBranch()
                                        ? GIT_BRANCH_REMOTE
                                        : GIT_BRANCH_LOCAL;
    const QString localBranchName = dialog->localBranchName();
    const QString worktreeName = dialog->worktreeName();
    const QString path = dialog->path();
    const bool initializeSubmodules = dialog->initializeSubmodules();
    const QString rootPath = QFileInfo(path).dir().absolutePath();
    const bool createdRoot = !QFileInfo::exists(rootPath);
    if (!QDir().mkpath(rootPath)) {
      QMessageBox::critical(this, tr("Create Worktree"),
                            tr("Unable to create worktree directory '%1'.")
                                .arg(rootPath));
      return;
    }

    mCreatingWorktree = true;
    mWorktreeAdd->setEnabled(false);
    auto *watcher = new QFutureWatcher<WorktreeCreationResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, createdRoot, rootPath, initializeSubmodules] {
              WorktreeCreationResult outcome = watcher->result();
              watcher->deleteLater();
              mCreatingWorktree = false;
              mWorktreeAdd->setEnabled(mModel->repository().isValid() &&
                                       !mModel->repository().isBare());
              if (outcome.path.isEmpty()) {
                if (createdRoot) {
                  QDir root(rootPath);
                  if (root.entryList(QDir::AllEntries | QDir::Hidden |
                                     QDir::NoDotAndDotDot)
                          .isEmpty())
                    QDir().rmdir(rootPath);
                }
                QMessageBox::critical(
                    this, tr("Create Worktree"),
                    outcome.result.errorString(tr("Unable to create worktree.")));
                return;
              }

              mModel->refresh();
              emit openRepositoryRequested(outcome.path, initializeSubmodules);
            });
    watcher->setFuture(QtConcurrent::run(
        [repositoryPath, selectedName, branchType, localBranchName, worktreeName,
         path] {
          WorktreeCreationResult outcome;
          git::Repository repo = git::Repository::open(repositoryPath);
          if (!repo.isValid()) {
            outcome.result = git::Result(-1);
            return outcome;
          }
          git::Branch branch = repo.lookupBranch(selectedName, branchType);
          git::Repository linked = repo.createWorktree(
              worktreeName, path, branch, localBranchName, &outcome.result);
          if (linked.isValid())
            outcome.path = linked.workdir().path();
          return outcome;
        }));
  });
  dialog->open();
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
    updatePanels();
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
  if (QTreeView *view =
          sectionView(RepositoryNavigatorModel::Section::GitHubIssues))
    view->verticalScrollBar()->setValue(0);
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
    updatePanels();
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
  updatePanels();
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
        guard->updatePanels();
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

void RepositoryNavigator::refresh() { requestGitHubIssues(true); }

void RepositoryNavigator::openIssue(const QModelIndex &index) {
  QUrl url(index.data(RepositoryNavigatorModel::UrlRole).toString());
  QString scheme = url.scheme().toLower();
  if (url.isValid() && !url.host().isEmpty() &&
      (scheme == "http" || scheme == "https"))
    QDesktopServices::openUrl(url);
}

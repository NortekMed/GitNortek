//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "LocalRepositoryManagement.h"

#include "LocalWorkspaceModel.h"
#include "RemoteCallbacks.h"
#include "conf/LocalWorkspace.h"
#include "conf/LocalWorkspaces.h"
#include "dialogs/LocalWorkspaceDialog.h"
#include "git/Branch.h"
#include "git/Remote.h"
#include "git/Repository.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QPromise>
#include <QSettings>
#include <QSplitter>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <functional>

namespace {

constexpr qint64 kMaximumReadmeSize = 2 * 1024 * 1024;
constexpr qint64 kMaximumReadmeImageSize = 5 * 1024 * 1024;
constexpr qint64 kMaximumReadmeImagePixels = 16 * 1024 * 1024;
constexpr int kOriginCacheSeconds = 5 * 60;
constexpr int kOriginCooldownSeconds = 2 * 60;
const char kOriginLastAttemptKey[] =
    "localRepositoryManagement/originLastAttempt";

QString originCacheKey(const QString &path) {
  QString normalized = QDir(path).canonicalPath();
  if (normalized.isEmpty())
    normalized = QDir(path).absolutePath();
  const QByteArray hash =
      QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256)
          .toHex();
  return QStringLiteral("localRepositoryManagement/originSuccess/%1")
      .arg(QString::fromLatin1(hash));
}

QString originFailureKey(const QString &path) {
  QString key = originCacheKey(path);
  return key.replace(QStringLiteral("originSuccess"),
                     QStringLiteral("originFailure"));
}

class WorkspaceFilterProxy : public QSortFilterProxyModel {
public:
  using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
  bool filterAcceptsRow(int sourceRow,
                        const QModelIndex &sourceParent) const override {
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    return index.data(Qt::DisplayRole)
               .toString()
               .contains(filterRegularExpression()) ||
           index.data(LocalWorkspaceModel::PathRole)
               .toString()
               .contains(filterRegularExpression());
  }
};

class RepositoryActionDelegate : public QStyledItemDelegate {
public:
  using Callback = std::function<void(const QModelIndex &)>;

  RepositoryActionDelegate(bool remove, Callback callback,
                           QObject *parent = nullptr)
      : QStyledItemDelegate(parent), mRemove(remove),
        mCallback(std::move(callback)) {}

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem background = option;
    initStyleOption(&background, index);
    background.text.clear();
    background.icon = {};
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

    if (index.data(LocalWorkspaceModel::ItemKindRole).toInt() !=
        LocalWorkspaceModel::RepositoryItem)
      return;

    const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled);
    const bool hovered = enabled && option.state.testFlag(QStyle::State_MouseOver);
    const int extent = qMin(16, qMin(option.rect.width(), option.rect.height()) - 6);
    if (extent <= 0)
      return;
    const QRect iconRect(option.rect.center().x() - extent / 2,
                         option.rect.center().y() - extent / 2, extent, extent);

    if (!mRemove) {
      const QIcon icon =
          style->standardIcon(QStyle::SP_FileDialogDetailedView, {}, widget);
      icon.paint(painter, iconRect, Qt::AlignCenter,
                 enabled ? (hovered ? QIcon::Active : QIcon::Normal)
                         : QIcon::Disabled);
      return;
    }

    painter->save();
    QPen pen(enabled ? (hovered ? QColor(235, 35, 35) : QColor(205, 45, 45))
                     : option.palette.color(QPalette::Mid));
    pen.setWidthF(hovered ? 2.4 : 2.0);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    const int inset = 3;
    painter->drawLine(iconRect.topLeft() + QPoint(inset, inset),
                      iconRect.bottomRight() - QPoint(inset, inset));
    painter->drawLine(iconRect.topRight() + QPoint(-inset, inset),
                      iconRect.bottomLeft() + QPoint(inset, -inset));
    painter->restore();
  }

  bool editorEvent(QEvent *event, QAbstractItemModel *,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
    const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled) &&
                         index.data(LocalWorkspaceModel::ItemKindRole).toInt() ==
                             LocalWorkspaceModel::RepositoryItem;
    if (event->type() == QEvent::MouseButtonPress) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      mPressedIndex = enabled && mouse->button() == Qt::LeftButton
                          ? QPersistentModelIndex(index)
                          : QPersistentModelIndex();
      return false;
    }
    if (!enabled)
      return false;

    bool activate = false;
    if (event->type() == QEvent::MouseButtonRelease) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      activate = mouse->button() == Qt::LeftButton &&
                 option.rect.contains(mouse->position().toPoint()) &&
                 mPressedIndex == index;
      mPressedIndex = QPersistentModelIndex();
    } else if (event->type() == QEvent::KeyPress) {
      const auto *key = static_cast<QKeyEvent *>(event);
      activate = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
                 key->key() == Qt::Key_Space;
    }
    if (!activate)
      return false;

    mCallback(index);
    return true;
  }

private:
  bool mRemove;
  Callback mCallback;
  QPersistentModelIndex mPressedIndex;
};

void paintItemBackground(QPainter *painter,
                         const QStyleOptionViewItem &option,
                         const QModelIndex &index) {
  QStyleOptionViewItem background = option;
  background.text.clear();
  background.icon = {};
  const QWidget *widget = option.widget;
  QStyle *style = widget ? widget->style() : QApplication::style();
  style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
}

class RemoteStatusDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    paintItemBackground(painter, option, index);
    if (index.data(LocalWorkspaceModel::ItemKindRole).toInt() !=
        LocalWorkspaceModel::RepositoryItem)
      return;

    const QColor green(QStringLiteral("#36c96b"));
    const QColor blue(QStringLiteral("#4aa3ff"));
    const QColor amber(QStringLiteral("#f0a020"));
    const QColor gray = option.palette.color(QPalette::Disabled, QPalette::Text);
    const bool ready =
        index.data(LocalWorkspaceModel::TrackingReadyRole).toBool();
    const QVariant aheadValue = index.data(LocalWorkspaceModel::AheadRole);
    const QVariant behindValue = index.data(LocalWorkspaceModel::BehindRole);
    const int ahead = aheadValue.toInt();
    const int behind = behindValue.toInt();
    const bool fetching =
        index.data(LocalWorkspaceModel::OriginFetchActiveRole).toBool();
    const bool eligible =
        index.data(LocalWorkspaceModel::OriginCheckEligibleRole).toBool();
    const bool fresh =
        index.data(LocalWorkspaceModel::OriginCheckFreshRole).toBool();
    const bool failed =
        index.data(LocalWorkspaceModel::OriginCheckFailedRole).toBool();
    const bool initialPending =
        index.data(LocalWorkspaceModel::OriginInitialPendingRole).toBool();

    painter->save();
    QFont font = option.font;
    font.setBold(true);
    painter->setFont(font);
    int x = option.rect.x() + 6;
    const int y = option.rect.y();
    const int height = option.rect.height();
    auto draw = [&](const QString &text, const QColor &color) {
      const int width = painter->fontMetrics().horizontalAdvance(text) + 8;
      painter->setPen(color);
      painter->drawText(QRect(x, y, width, height),
                        Qt::AlignLeft | Qt::AlignVCenter, text);
      x += width;
    };

    if (fetching) {
      const bool alternate =
          (QDateTime::currentMSecsSinceEpoch() / 250) % 2;
      draw(alternate ? QString::fromUtf8("⌛") : QString::fromUtf8("⏳"),
           amber);
    } else if (initialPending) {
      draw(QString::fromUtf8("◷"), gray);
    } else if (failed) {
      draw(QString::fromUtf8("◷"), QColor(QStringLiteral("#e25555")));
    } else if (!ready || !eligible) {
      draw(QStringLiteral("?"), gray);
    } else if (!fresh) {
      draw(QString::fromUtf8("◷"), gray);
    } else if (!ahead && !behind) {
      draw(QString::fromUtf8("✓"), green);
    } else {
      if (ahead)
        draw(LocalRepositoryManagement::tr("%1↑").arg(ahead), blue);
      if (behind)
        draw(LocalRepositoryManagement::tr("%1↓").arg(behind), amber);
    }
    painter->restore();
  }
};

class WorkingTreeStatusDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  enum Kind { Modified, Added, Removed, Untracked, Conflicted };

  struct Part {
    Kind kind;
    int count;
    QRect rect;
  };

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    paintItemBackground(painter, option, index);
    if (index.data(LocalWorkspaceModel::ItemKindRole).toInt() !=
        LocalWorkspaceModel::RepositoryItem)
      return;

    const QList<Part> values = parts(option, index);
    if (values.isEmpty()) {
      const bool ready = index.data(LocalWorkspaceModel::StatusReadyRole).toBool();
      const bool error = index.data(LocalWorkspaceModel::StatusErrorRole).toBool();
      painter->setPen(error ? QColor(QStringLiteral("#e25555"))
                            : option.palette.color(QPalette::Disabled,
                                                   QPalette::Text));
      painter->drawText(option.rect.adjusted(5, 0, 0, 0),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        ready ? QString::fromUtf8("✓")
                              : (error ? QStringLiteral("?")
                                       : QString::fromUtf8("…")));
      return;
    }

    for (const Part &part : values)
      paintPart(painter, option, part);
  }

  bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                 const QStyleOptionViewItem &option,
                 const QModelIndex &index) override {
    for (const Part &part : parts(option, index)) {
      if (!part.rect.contains(event->pos()))
        continue;
      QToolTip::showText(event->globalPos(), tooltip(part), view);
      return true;
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
  }

private:
  QList<Part> parts(const QStyleOptionViewItem &option,
                    const QModelIndex &index) const {
    const QList<QPair<Kind, int>> counts = {
        {Modified, index.data(LocalWorkspaceModel::ModifiedRole).toInt()},
        {Added, index.data(LocalWorkspaceModel::AddedRole).toInt()},
        {Removed, index.data(LocalWorkspaceModel::RemovedRole).toInt()},
        {Untracked, index.data(LocalWorkspaceModel::UntrackedRole).toInt()},
        {Conflicted, index.data(LocalWorkspaceModel::ConflictedRole).toInt()}};
    QList<Part> result;
    int x = option.rect.x() + 5;
    const QFontMetrics metrics(option.font);
    for (const auto &[kind, count] : counts) {
      if (!count)
        continue;
      const int width = 15 + metrics.horizontalAdvance(QString::number(count)) +
                        9;
      result.append({kind, count,
                     QRect(x, option.rect.y(), width, option.rect.height())});
      x += width;
    }
    return result;
  }

  void paintPart(QPainter *painter, const QStyleOptionViewItem &option,
                 const Part &part) const {
    const QColor orange(QStringLiteral("#f0a020"));
    const QColor green(QStringLiteral("#36c96b"));
    const QColor red(QStringLiteral("#e25555"));
    const QColor yellow(QStringLiteral("#e0c341"));
    const QRect icon(part.rect.x() + 1, part.rect.center().y() - 6, 12, 12);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QPen pen;
    pen.setWidthF(2.0);
    pen.setCapStyle(Qt::RoundCap);
    if (part.kind == Modified) {
      pen.setColor(orange);
      painter->setPen(pen);
      painter->drawLine(icon.bottomLeft() + QPoint(2, -1),
                        icon.topRight() + QPoint(-1, 2));
      painter->drawLine(icon.bottomLeft() + QPoint(1, -3),
                        icon.bottomLeft() + QPoint(3, -1));
    } else if (part.kind == Added) {
      pen.setColor(green);
      painter->setPen(pen);
      painter->drawLine(icon.center().x(), icon.top() + 2, icon.center().x(),
                        icon.bottom() - 2);
      painter->drawLine(icon.left() + 2, icon.center().y(), icon.right() - 2,
                        icon.center().y());
    } else if (part.kind == Removed) {
      pen.setColor(red);
      painter->setPen(pen);
      painter->drawLine(icon.left() + 2, icon.center().y(), icon.right() - 2,
                        icon.center().y());
    } else if (part.kind == Untracked) {
      QFont font = option.font;
      font.setBold(true);
      painter->setFont(font);
      painter->setPen(yellow);
      painter->drawText(icon, Qt::AlignCenter, QStringLiteral("?"));
    } else {
      pen.setColor(red);
      painter->setPen(pen);
      painter->drawLine(icon.center().x(), icon.top() + 1, icon.center().x(),
                        icon.bottom() - 1);
      painter->drawLine(icon.left() + 1, icon.center().y(), icon.right() - 1,
                        icon.center().y());
      painter->drawLine(icon.topLeft() + QPoint(2, 2),
                        icon.bottomRight() - QPoint(2, 2));
      painter->drawLine(icon.topRight() + QPoint(-2, 2),
                        icon.bottomLeft() + QPoint(2, -2));
    }

    painter->setPen(option.palette.color(QPalette::Text));
    painter->drawText(QRect(icon.right() + 3, part.rect.y(),
                            part.rect.right() - icon.right() - 3,
                            part.rect.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      QString::number(part.count));
    painter->restore();
  }

  QString tooltip(const Part &part) const {
    if (part.kind == Modified) {
      return part.count == 1
                 ? LocalRepositoryManagement::tr("1 modified file")
                 : LocalRepositoryManagement::tr("%1 modified files")
                       .arg(part.count);
    }
    if (part.kind == Added) {
      return part.count == 1
                 ? LocalRepositoryManagement::tr("1 added file")
                 : LocalRepositoryManagement::tr("%1 added files")
                       .arg(part.count);
    }
    if (part.kind == Removed) {
      return part.count == 1
                 ? LocalRepositoryManagement::tr("1 removed file")
                 : LocalRepositoryManagement::tr("%1 removed files")
                       .arg(part.count);
    }
    if (part.kind == Untracked) {
      return part.count == 1
                 ? LocalRepositoryManagement::tr("1 untracked file")
                 : LocalRepositoryManagement::tr("%1 untracked files")
                       .arg(part.count);
    }
    return part.count == 1
               ? LocalRepositoryManagement::tr("1 conflicted file")
               : LocalRepositoryManagement::tr("%1 conflicted files")
                     .arg(part.count);
  }
};

class ReadmeBrowser : public QTextBrowser {
public:
  explicit ReadmeBrowser(QWidget *parent = nullptr) : QTextBrowser(parent) {}

  void setRepositoryRoot(const QString &path) {
    mRoot = QDir(path).canonicalPath();
    if (mRoot.isEmpty())
      mRoot = QDir(path).absolutePath();
    document()->setBaseUrl(
        QUrl::fromLocalFile(mRoot + QDir::separator()));
  }

protected:
  QVariant loadResource(int type, const QUrl &name) override {
    if (type != QTextDocument::ImageResource)
      return QTextBrowser::loadResource(type, name);
    if (!name.isLocalFile())
      return {};

    const QFileInfo resource(name.toLocalFile());
    QString path = resource.canonicalFilePath();
    if (path.isEmpty())
      path = resource.absoluteFilePath();
    const QString relative = QDir(mRoot).relativeFilePath(path);
    if (QDir::isAbsolutePath(relative) || relative == QStringLiteral("..") ||
        relative.startsWith(QStringLiteral("../")))
      return {};
    if (resource.size() > kMaximumReadmeImageSize)
      return {};

    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid() ||
        qint64(size.width()) * qint64(size.height()) >
            kMaximumReadmeImagePixels)
      return {};
    return QTextBrowser::loadResource(type, name);
  }

private:
  QString mRoot;
};

} // namespace

LocalRepositoryManagement::LocalRepositoryManagement(QWidget *parent)
    : QWidget(parent), mWorkspaces(LocalWorkspaces::instance()),
      mModel(new LocalWorkspaceModel(this)),
      mProxy(new WorkspaceFilterProxy(this)), mSearch(new QLineEdit(this)),
      mExpansionToggle(new QPushButton(this)),
      mOriginCheck(new QPushButton(this)),
      mTree(new QTreeView(this)),
      mSplitter(new QSplitter(Qt::Horizontal, this)),
      mDetailsPane(new QWidget(mSplitter)),
      mDetailsTitle(new QLabel(mDetailsPane)),
      mReadme(new ReadmeBrowser(mDetailsPane)),
      mOriginCooldownTimer(new QTimer(this)),
      mOriginAnimationTimer(new QTimer(this)) {
  setObjectName(QStringLiteral("LocalRepositoryManagement"));

  QLabel *title = new QLabel(tr("Local Repository Management"), this);
  title->setObjectName(QStringLiteral("LocalRepositoryManagementTitle"));
  QFont titleFont = title->font();
  titleFont.setBold(true);
  titleFont.setPointSizeF(titleFont.pointSizeF() * 1.5);
  title->setFont(titleFont);

  QPushButton *openRepository = new QPushButton(tr("Open Repository"), this);
  openRepository->setObjectName(
      QStringLiteral("LocalRepositoryManagementOpenRepository"));
  QPushButton *newWorkspace = new QPushButton(tr("New Workspace"), this);
  newWorkspace->setObjectName(
      QStringLiteral("LocalRepositoryManagementNewWorkspace"));
  mOriginCheck->setObjectName(
      QStringLiteral("LocalRepositoryManagementCheckOrigin"));
  QHBoxLayout *primaryActions = new QHBoxLayout;
  primaryActions->addWidget(openRepository);
  primaryActions->addWidget(newWorkspace);
  primaryActions->addWidget(mOriginCheck);
  primaryActions->addStretch();

  mExpansionToggle->setObjectName(
      QStringLiteral("LocalRepositoryManagementExpansionToggle"));
  mSearch->setObjectName(QStringLiteral("LocalRepositoryManagementSearch"));
  mSearch->setPlaceholderText(tr("Search repository"));
  mSearch->setClearButtonEnabled(true);
  QHBoxLayout *tools = new QHBoxLayout;
  tools->addWidget(mExpansionToggle);
  tools->addStretch();
  tools->addWidget(mSearch, 1);

  mProxy->setSourceModel(mModel);
  mProxy->setFilterRole(Qt::DisplayRole);
  mProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
  mProxy->setRecursiveFilteringEnabled(true);
  mTree->setObjectName(QStringLiteral("LocalRepositoryManagementTree"));
  mTree->setModel(mProxy);
  mTree->setHeaderHidden(false);
  QHeaderView *header = mTree->header();
  header->setStretchLastSection(false);
  header->setSectionResizeMode(LocalWorkspaceModel::RepositoryColumn,
                                QHeaderView::Stretch);
  header->setSectionResizeMode(LocalWorkspaceModel::BranchColumn,
                                QHeaderView::ResizeToContents);
  header->setSectionResizeMode(LocalWorkspaceModel::RemoteColumn,
                               QHeaderView::Fixed);
  header->setSectionResizeMode(LocalWorkspaceModel::ChangesColumn,
                               QHeaderView::Fixed);
  header->setSectionResizeMode(LocalWorkspaceModel::DetailsColumn,
                               QHeaderView::Fixed);
  header->setSectionResizeMode(LocalWorkspaceModel::RemoveColumn,
                               QHeaderView::Fixed);
  mTree->setColumnWidth(LocalWorkspaceModel::RemoteColumn, 76);
  mTree->setColumnWidth(LocalWorkspaceModel::ChangesColumn, 172);
  mTree->setColumnWidth(LocalWorkspaceModel::DetailsColumn, 34);
  mTree->setColumnWidth(LocalWorkspaceModel::RemoveColumn, 34);
  mTree->setSelectionMode(QAbstractItemView::SingleSelection);
  mTree->setExpandsOnDoubleClick(false);
  mTree->setContextMenuPolicy(Qt::CustomContextMenu);
  mTree->viewport()->setMouseTracking(true);
  mTree->installEventFilter(this);
  mTree->viewport()->installEventFilter(this);

  mTree->setItemDelegateForColumn(
      LocalWorkspaceModel::RemoteColumn,
      new RemoteStatusDelegate(mTree));
  mTree->setItemDelegateForColumn(
      LocalWorkspaceModel::ChangesColumn,
      new WorkingTreeStatusDelegate(mTree));
  mTree->setItemDelegateForColumn(
      LocalWorkspaceModel::DetailsColumn,
      new RepositoryActionDelegate(
          false,
          [this](const QModelIndex &index) {
            showDetails(index.data(LocalWorkspaceModel::PathRole).toString());
          },
          mTree));
  mTree->setItemDelegateForColumn(
      LocalWorkspaceModel::RemoveColumn,
      new RepositoryActionDelegate(
          true,
          [this](const QModelIndex &index) {
            removeRepository(
                index.data(LocalWorkspaceModel::WorkspaceIdRole).toString(),
                index.data(LocalWorkspaceModel::PathRole).toString());
          },
          mTree));

  mDetailsPane->setObjectName(
      QStringLiteral("LocalRepositoryManagementDetails"));
  mDetailsTitle->setObjectName(
      QStringLiteral("LocalRepositoryManagementDetailsTitle"));
  QFont detailsTitleFont = mDetailsTitle->font();
  detailsTitleFont.setBold(true);
  mDetailsTitle->setFont(detailsTitleFont);
  QToolButton *closeDetails = new QToolButton(mDetailsPane);
  closeDetails->setObjectName(
      QStringLiteral("LocalRepositoryManagementCloseDetails"));
  closeDetails->setIcon(
      style()->standardIcon(QStyle::SP_TitleBarCloseButton));
  closeDetails->setToolTip(tr("Close details"));
  closeDetails->setAutoRaise(true);

  QHBoxLayout *detailsHeader = new QHBoxLayout;
  detailsHeader->addWidget(mDetailsTitle, 1);
  detailsHeader->addWidget(closeDetails);

  mReadme->setObjectName(
      QStringLiteral("LocalRepositoryManagementReadme"));
  mReadme->setOpenLinks(false);
  mReadme->document()->setDocumentMargin(16);
  QVBoxLayout *detailsLayout = new QVBoxLayout(mDetailsPane);
  detailsLayout->setContentsMargins(8, 0, 0, 0);
  detailsLayout->addLayout(detailsHeader);
  detailsLayout->addWidget(mReadme, 1);

  mSplitter->setObjectName(
      QStringLiteral("LocalRepositoryManagementSplitter"));
  mSplitter->addWidget(mTree);
  mSplitter->addWidget(mDetailsPane);
  mSplitter->setStretchFactor(0, 1);
  mSplitter->setStretchFactor(1, 1);
  mSplitter->setChildrenCollapsible(false);
  mDetailsPane->hide();

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(title);
  layout->addLayout(primaryActions);
  layout->addLayout(tools);
  layout->addWidget(mSplitter, 1);

  connect(openRepository, &QPushButton::clicked, this,
          &LocalRepositoryManagement::openRepositoryDialogRequested);
  connect(newWorkspace, &QPushButton::clicked, this,
          &LocalRepositoryManagement::createWorkspace);
  connect(mOriginCheck, &QPushButton::clicked, this,
          [this] { checkOrigins(true); });
  connect(mExpansionToggle, &QPushButton::clicked, this,
          &LocalRepositoryManagement::toggleWorkspaceExpansion);
  connect(mSearch, &QLineEdit::textChanged, this,
          [this](const QString &text) {
            mProxy->setFilterFixedString(text);
            updateExpansionButton();
          });
  connect(mTree, &QTreeView::expanded, this,
          [this] { updateExpansionButton(); });
  connect(mTree, &QTreeView::collapsed, this,
          [this] { updateExpansionButton(); });
  connect(mTree, &QAbstractItemView::entered, this,
          [this](const QModelIndex &index) {
            const bool action =
                index.isValid() &&
                index.column() >= LocalWorkspaceModel::DetailsColumn &&
                index.data(LocalWorkspaceModel::ItemKindRole).toInt() ==
                    LocalWorkspaceModel::RepositoryItem &&
                index.flags().testFlag(Qt::ItemIsEnabled);
            if (action)
              mTree->viewport()->setCursor(Qt::PointingHandCursor);
            else
              mTree->viewport()->unsetCursor();
          });
  connect(mTree, &QWidget::customContextMenuRequested, this,
           &LocalRepositoryManagement::showContextMenu);
  connect(closeDetails, &QToolButton::clicked, mDetailsPane, &QWidget::hide);
  connect(mProxy, &QAbstractItemModel::modelReset, this, [this] {
    updateWorkspaceSpans();
    updateExpansionButton();
    updateOriginCheckStates();
  });
  QTimer *branchRefresh = new QTimer(this);
  branchRefresh->setInterval(2000);
  connect(branchRefresh, &QTimer::timeout, mModel,
          &LocalWorkspaceModel::refreshRepositories);
  connect(branchRefresh, &QTimer::timeout, this,
          &LocalRepositoryManagement::updateOriginCheckStates);
  branchRefresh->start();
  mOriginCooldownTimer->setInterval(1000);
  connect(mOriginCooldownTimer, &QTimer::timeout, this,
          &LocalRepositoryManagement::updateOriginCheckButton);
  mOriginAnimationTimer->setObjectName(
      QStringLiteral("LocalRepositoryManagementOriginAnimation"));
  mOriginAnimationTimer->setInterval(250);
  connect(mOriginAnimationTimer, &QTimer::timeout, mTree->viewport(),
          QOverload<>::of(&QWidget::update));
  const QDateTime lastAttempt =
      QSettings().value(QLatin1String(kOriginLastAttemptKey)).toDateTime();
  if (lastAttempt.isValid())
    mOriginCooldownDeadline = lastAttempt.addSecs(kOriginCooldownSeconds);
  updateOriginCheckButton();
  updateWorkspaceSpans();
  updateExpansionButton();
  updateOriginCheckStates();
}

LocalRepositoryManagement::~LocalRepositoryManagement() = default;

void LocalRepositoryManagement::checkOriginsIfStale() {
  if (mFirstOriginOpen) {
    mFirstOriginOpen = false;
    mResolveOriginAfterPaint = true;
    for (const QString &path : mModel->repositoryPaths())
      mModel->setOriginInitialPending(path, true);
    mTree->viewport()->update();
    return;
  }
  updateOriginCheckStates();
  checkOrigins(false);
}

void LocalRepositoryManagement::checkOrigins(bool force) {
  updateOriginCheckStates();
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const QDateTime storedAttempt =
      QSettings().value(QLatin1String(kOriginLastAttemptKey)).toDateTime();
  if (storedAttempt.isValid() &&
      (!mOriginCooldownDeadline.isValid() ||
       storedAttempt.addSecs(kOriginCooldownSeconds) >
           mOriginCooldownDeadline))
    mOriginCooldownDeadline = storedAttempt.addSecs(kOriginCooldownSeconds);
  if ((mOriginCheckWatcher && mOriginCheckWatcher->isRunning()) ||
      (mOriginCooldownDeadline.isValid() &&
       now < mOriginCooldownDeadline))
    return;

  QStringList paths = mModel->repositoryPaths();
  if (!force)
    paths.removeIf([this](const QString &path) {
      return mModel->isOriginCheckFresh(path);
    });
  if (paths.isEmpty()) {
    updateOriginCheckButton();
    return;
  }

  mOriginCooldownDeadline = now.addSecs(kOriginCooldownSeconds);
  QSettings().setValue(QLatin1String(kOriginLastAttemptKey), now);
  auto *watcher = new QFutureWatcher<OriginCheckEvent>;
  mOriginCheckWatcher = watcher;
  mOriginCallbacks.reserve(paths.size());
  for (qsizetype i = 0; i < paths.size(); ++i) {
    mOriginCallbacks.append(new RemoteCallbacks(
        RemoteCallbacks::Receive, nullptr, QString(), QStringLiteral("origin"),
        watcher, git::Repository(), force, this));
  }

  const QList<RemoteCallbacks *> callbacks = mOriginCallbacks;
  QList<bool> untrusted;
  untrusted.reserve(paths.size());
  for (const QString &path : paths)
    untrusted.append(!mModel->isOriginCheckFresh(path));
  connect(watcher, &QFutureWatcher<OriginCheckEvent>::resultReadyAt, this,
          &LocalRepositoryManagement::handleOriginCheckEvent);
  connect(qApp, &QCoreApplication::aboutToQuit, watcher, [callbacks] {
    for (RemoteCallbacks *callback : callbacks)
      callback->setCanceled(true);
  });
  connect(watcher, &QFutureWatcher<OriginCheckEvent>::finished, watcher,
          [watcher, callbacks, untrusted] {
            const QList<OriginCheckEvent> events = watcher->future().results();
            QSettings settings;
            const QDateTime now = QDateTime::currentDateTimeUtc();
            for (const OriginCheckEvent &event : events) {
              if (event.type != FetchFinished)
                continue;
              if (event.successful) {
                settings.setValue(originCacheKey(event.path), now);
                settings.remove(originFailureKey(event.path));
                callbacks.at(event.callbackIndex)->storeDeferredCredentials();
              } else if (untrusted.at(event.callbackIndex)) {
                settings.setValue(originFailureKey(event.path), now);
              }
            }
            watcher->deleteLater();
          });
  connect(watcher, &QFutureWatcher<OriginCheckEvent>::finished, this,
          &LocalRepositoryManagement::finishOriginCheck);

  emit originCheckStarted(paths.size());
  updateOriginCheckButton();
  watcher->setFuture(QtConcurrent::run(
      [paths, callbacks](QPromise<OriginCheckEvent> &promise) {
        for (qsizetype i = 0; i < paths.size(); ++i) {
          const QString path = paths.at(i);
          const git::Repository repository =
              git::Repository::open(path, true);
          if (!repository.isValid())
            continue;

          const git::Reference head = repository.head();
          if (!head.isValid() || !head.isLocalBranch())
            continue;
          const git::Branch upstream = git::Branch(head).upstream();
          if (!upstream.isValid() ||
              upstream.name().section('/', 0, 0) !=
                  QStringLiteral("origin"))
            continue;

          git::Remote origin =
              repository.lookupRemote(QStringLiteral("origin"));
          if (!origin.isValid() || callbacks.at(i)->isCanceled())
            continue;
          callbacks.at(i)->setRepositoryForOperation(repository);
          promise.addResult({path, i, FetchStarted, false});
          const bool successful =
              bool(origin.fetch(callbacks.at(i), false, false));
          callbacks.at(i)->setRepositoryForOperation(git::Repository());
          promise.addResult({path, i, FetchFinished, successful});
        }
      }));
}

void LocalRepositoryManagement::handleOriginCheckEvent(int index) {
  const OriginCheckEvent event = mOriginCheckWatcher->resultAt(index);
  if (event.type == FetchStarted) {
    mActiveOriginFetches.insert(event.path);
    if (!mModel->isOriginCheckFresh(event.path))
      mUntrustedOriginFetches.insert(event.path);
    mModel->setOriginFetchActive(event.path, true);
    mOriginAnimationTimer->start();
    emit originFetchStarted(event.path);
    return;
  }

  mActiveOriginFetches.remove(event.path);
  if (event.successful) {
    QSettings settings;
    settings.setValue(originCacheKey(event.path),
                      QDateTime::currentDateTimeUtc());
    settings.remove(originFailureKey(event.path));
    mModel->setOriginCheckFailed(event.path, false);
    mModel->setOriginCheckFresh(event.path, true);
  } else if (mUntrustedOriginFetches.contains(event.path)) {
    QSettings().setValue(originFailureKey(event.path),
                         QDateTime::currentDateTimeUtc());
    mModel->setOriginCheckFresh(event.path, false);
    mModel->setOriginCheckFailed(event.path, true);
  }
  mUntrustedOriginFetches.remove(event.path);
  mModel->setOriginFetchActive(event.path, false);
  mModel->refreshRepositories();
  if (mActiveOriginFetches.isEmpty())
    mOriginAnimationTimer->stop();
  emit originFetchFinished(event.path, event.successful);
}

void LocalRepositoryManagement::finishOriginCheck() {
  const QList<OriginCheckEvent> events =
      mOriginCheckWatcher->future().results();
  int successful = 0;
  int failed = 0;
  for (const OriginCheckEvent &event : events) {
    if (event.type != FetchFinished)
      continue;
    if (event.successful)
      ++successful;
    else
      ++failed;
  }
  for (const QString &path : std::as_const(mActiveOriginFetches))
    mModel->setOriginFetchActive(path, false);
  mActiveOriginFetches.clear();
  mUntrustedOriginFetches.clear();
  mOriginAnimationTimer->stop();
  mOriginCallbacks.clear();
  mModel->refreshRepositories();
  updateOriginCheckButton();
  emit originCheckFinished(successful, failed);
}

void LocalRepositoryManagement::updateOriginCheckButton() {
  if (mOriginCheckWatcher && mOriginCheckWatcher->isRunning()) {
    mOriginCheck->setText(tr("Checking..."));
    mOriginCheck->setEnabled(false);
    mOriginCooldownTimer->start();
    return;
  }

  const QDateTime storedAttempt =
      QSettings().value(QLatin1String(kOriginLastAttemptKey)).toDateTime();
  if (storedAttempt.isValid() &&
      (!mOriginCooldownDeadline.isValid() ||
       storedAttempt.addSecs(kOriginCooldownSeconds) >
           mOriginCooldownDeadline))
    mOriginCooldownDeadline = storedAttempt.addSecs(kOriginCooldownSeconds);
  const qint64 remaining =
      QDateTime::currentDateTimeUtc().secsTo(mOriginCooldownDeadline);
  if (remaining > 0) {
    mOriginCheck->setText(
        tr("Check origin (%1:%2)")
            .arg(remaining / 60)
            .arg(remaining % 60, 2, 10, QLatin1Char('0')));
    mOriginCheck->setEnabled(false);
    mOriginCooldownTimer->start();
    return;
  }

  mOriginCooldownTimer->stop();
  mOriginCheck->setText(tr("Check origin"));
  mOriginCheck->setEnabled(true);
}

bool LocalRepositoryManagement::eventFilter(QObject *watched, QEvent *event) {
  if (watched == mTree && event->type() == QEvent::KeyPress) {
    const auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Delete) {
      deleteCurrentItem();
      return true;
    }
  }
  if (watched == mTree->viewport()) {
    if (event->type() == QEvent::Paint && mResolveOriginAfterPaint &&
        !mOriginResolutionScheduled) {
      mOriginResolutionScheduled = true;
      QTimer::singleShot(0, this, [this] {
        mResolveOriginAfterPaint = false;
        mOriginResolutionScheduled = false;
        updateOriginCheckStates();
        for (const QString &path : mModel->repositoryPaths())
          mModel->setOriginInitialPending(path, false);
        checkOrigins(false);
      });
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() != Qt::LeftButton)
        return false;
      activate(mTree->indexAt(mouse->position().toPoint()));
      return true;
    }
    if (event->type() == QEvent::MouseMove) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      const QModelIndex index = mTree->indexAt(mouse->position().toPoint());
      const bool action = index.isValid() &&
                          index.column() >= LocalWorkspaceModel::DetailsColumn &&
                          index.data(LocalWorkspaceModel::ItemKindRole).toInt() ==
                              LocalWorkspaceModel::RepositoryItem &&
                          index.flags().testFlag(Qt::ItemIsEnabled);
      QWidget *viewport = mTree->viewport();
      QTimer::singleShot(0, viewport, [viewport, action] {
        if (action)
          viewport->setCursor(Qt::PointingHandCursor);
        else
          viewport->unsetCursor();
      });
    } else if (event->type() == QEvent::Leave) {
      mTree->viewport()->unsetCursor();
    }
  }
  return QWidget::eventFilter(watched, event);
}

QModelIndex LocalRepositoryManagement::currentSourceIndex() const {
  return mProxy->mapToSource(mTree->currentIndex());
}

void LocalRepositoryManagement::activate(const QModelIndex &index) {
  if (!index.isValid() ||
      index.column() >= LocalWorkspaceModel::DetailsColumn)
    return;
  if (index.data(LocalWorkspaceModel::ItemKindRole).toInt() ==
      LocalWorkspaceModel::RepositoryItem) {
    emit openRepositoryRequested(
        index.data(LocalWorkspaceModel::PathRole).toString());
    return;
  }
  mTree->setExpanded(index, !mTree->isExpanded(index));
}

void LocalRepositoryManagement::toggleWorkspaceExpansion() {
  bool expanded = false;
  for (int row = 0; row < mProxy->rowCount(); ++row) {
    if (mTree->isExpanded(mProxy->index(row, 0))) {
      expanded = true;
      break;
    }
  }
  if (expanded)
    mTree->collapseAll();
  else
    mTree->expandAll();
  updateExpansionButton();
}

void LocalRepositoryManagement::updateExpansionButton() {
  bool expanded = false;
  for (int row = 0; row < mProxy->rowCount(); ++row) {
    if (mTree->isExpanded(mProxy->index(row, 0))) {
      expanded = true;
      break;
    }
  }
  mExpansionToggle->setText(expanded ? tr("Collapse") : tr("Expand"));
}

void LocalRepositoryManagement::showContextMenu(const QPoint &position) {
  const QModelIndex proxyIndex = mTree->indexAt(position);
  if (!proxyIndex.isValid())
    return;
  mTree->setCurrentIndex(proxyIndex);

  const QModelIndex index = mProxy->mapToSource(proxyIndex);
  const QString id =
      index.data(LocalWorkspaceModel::WorkspaceIdRole).toString();
  const bool repository =
      index.data(LocalWorkspaceModel::ItemKindRole).toInt() ==
      LocalWorkspaceModel::RepositoryItem;
  const QString path = index.data(LocalWorkspaceModel::PathRole).toString();
  QMenu menu(this);

  if (repository) {
    QAction *open = menu.addAction(tr("Open Repository"));
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("Remove Repository"));
    remove->setEnabled(
        !index.data(LocalWorkspaceModel::SynchronizedRole).toBool());
    connect(open, &QAction::triggered, this,
            [this, path] { emit openRepositoryRequested(path); });
    connect(remove, &QAction::triggered, this,
            [this, id, path] { removeRepository(id, path); });
  } else {
    QAction *open = menu.addAction(tr("Open Workspace"));
    QAction *add = menu.addAction(tr("Add Repository"));
    QAction *rescan = menu.addAction(tr("Rescan Synchronized Directory"));
    const LocalWorkspace *workspace = mWorkspaces->workspace(id);
    rescan->setEnabled(workspace && !workspace->syncDirectory.isEmpty());
    menu.addSeparator();
    QAction *edit = menu.addAction(tr("Edit Workspace"));
    QAction *remove = menu.addAction(tr("Delete Workspace"));
    connect(open, &QAction::triggered, this,
            [this, id] { openWorkspace(id); });
    connect(add, &QAction::triggered, this,
            [this, id] { addRepository(id); });
    connect(rescan, &QAction::triggered, this,
            [this, id] { rescanWorkspace(id); });
    connect(edit, &QAction::triggered, this,
            [this, id] { editWorkspace(id); });
    connect(remove, &QAction::triggered, this,
            [this, id] { deleteWorkspace(id); });
  }
  menu.exec(mTree->viewport()->mapToGlobal(position));
}

void LocalRepositoryManagement::createWorkspace() {
  LocalWorkspaceDialog dialog(this);
  while (dialog.exec() == QDialog::Accepted) {
    const LocalWorkspace workspace = dialog.workspace();
    QString error;
    if (!mWorkspaces->add(workspace, &error)) {
      showError(error);
      continue;
    }
    return;
  }
}

void LocalRepositoryManagement::editWorkspace(const QString &id) {
  const LocalWorkspace *stored = mWorkspaces->workspace(id);
  if (!stored)
    return;
  const LocalWorkspace workspace = *stored;
  LocalWorkspaceDialog dialog(workspace, this);
  while (dialog.exec() == QDialog::Accepted) {
    QString error;
    const LocalWorkspace updated = dialog.workspace();
    if (!mWorkspaces->update(updated, &error)) {
      showError(error);
      continue;
    }
    return;
  }
}

void LocalRepositoryManagement::deleteWorkspace(const QString &id) {
  const LocalWorkspace *workspace = mWorkspaces->workspace(id);
  if (!workspace)
    return;
  const QString name = workspace->name;
  if (QMessageBox::question(
          this, tr("Delete Workspace"),
          tr("Delete workspace \"%1\"?").arg(name)) != QMessageBox::Yes)
    return;

  QString error;
  if (!mWorkspaces->remove(id, &error))
    showError(error);
}

void LocalRepositoryManagement::addRepository(const QString &id) {
  const QString path = QFileDialog::getExistingDirectory(
      this, tr("Select Git Repository"), QString(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (path.isEmpty())
    return;

  QString error;
  if (!mWorkspaces->addRepository(id, path, &error))
    showError(error);
}

void LocalRepositoryManagement::removeRepository(const QString &id,
                                                 const QString &path) {
  if (QMessageBox::question(
          this, tr("Remove Repository"),
          tr("Remove repository \"%1\" from this workspace?").arg(path)) !=
      QMessageBox::Yes)
    return;

  QString error;
  if (!mWorkspaces->removeRepository(id, path, &error))
    showError(error);
}

void LocalRepositoryManagement::openWorkspace(const QString &id) {
  const LocalWorkspace *workspace = mWorkspaces->workspace(id);
  if (!workspace)
    return;
  const QStringList paths = workspace->repositories;
  emit openWorkspaceRequested(paths);
}

void LocalRepositoryManagement::rescanWorkspace(const QString &id) {
  QString error;
  if (!mWorkspaces->rescanSynchronizedDirectory(id, &error))
    showError(error);
}

void LocalRepositoryManagement::deleteCurrentItem() {
  const QModelIndex index = currentSourceIndex();
  if (!index.isValid())
    return;
  const QString id =
      index.data(LocalWorkspaceModel::WorkspaceIdRole).toString();
  if (index.data(LocalWorkspaceModel::ItemKindRole).toInt() ==
      LocalWorkspaceModel::RepositoryItem) {
    if (index.data(LocalWorkspaceModel::SynchronizedRole).toBool())
      return;
    removeRepository(id, index.data(LocalWorkspaceModel::PathRole).toString());
  } else {
    deleteWorkspace(id);
  }
}

void LocalRepositoryManagement::showDetails(const QString &path) {
  const QFileInfo repository(path);
  mDetailsTitle->setText(repository.fileName().isEmpty()
                             ? path
                             : repository.fileName());

  const QString readmePath = QDir(path).filePath(QStringLiteral("README.md"));
  QFile readme(readmePath);
  static_cast<ReadmeBrowser *>(mReadme)->setRepositoryRoot(path);
  if (!readme.exists()) {
    mReadme->setPlainText(tr("This repository has no README.md file."));
  } else if (!readme.open(QIODevice::ReadOnly)) {
    mReadme->setPlainText(tr("Unable to read README.md."));
  } else if (readme.size() > kMaximumReadmeSize) {
    mReadme->setPlainText(tr("README.md is too large to display."));
  } else {
    const QByteArray markdown = readme.read(kMaximumReadmeSize + 1);
    if (markdown.size() > kMaximumReadmeSize) {
      mReadme->setPlainText(tr("README.md is too large to display."));
    } else {
      QTextDocument::MarkdownFeatures features =
          QTextDocument::MarkdownDialectGitHub;
      features.setFlag(QTextDocument::MarkdownNoHTML);
      mReadme->document()->setMarkdown(QString::fromUtf8(markdown), features);
    }
  }

  mDetailsPane->show();
  if (!mDetailsInitialized) {
    mDetailsInitialized = true;
    QTimer::singleShot(0, this, [this] {
      const int available = mSplitter->width() - mSplitter->handleWidth();
      mSplitter->setSizes({available / 2, available - available / 2});
    });
  }
}

void LocalRepositoryManagement::updateWorkspaceSpans() {
  for (int row = 0; row < mProxy->rowCount(); ++row)
    mTree->setFirstColumnSpanned(row, {}, true);
}

void LocalRepositoryManagement::updateOriginCheckStates() {
  QSettings settings;
  const QDateTime now = QDateTime::currentDateTimeUtc();
  for (const QString &path : mModel->repositoryPaths()) {
    const QDateTime success =
        settings.value(originCacheKey(path)).toDateTime();
    const QDateTime failure =
        settings.value(originFailureKey(path)).toDateTime();
    const bool failed = failure.isValid() &&
                        (!success.isValid() || failure > success);
    const bool fresh = !failed && success.isValid() &&
                       success.secsTo(now) < kOriginCacheSeconds;
    mModel->setOriginCheckFresh(path, fresh);
    mModel->setOriginCheckFailed(path, failed);
    if (mResolveOriginAfterPaint)
      mModel->setOriginInitialPending(path, true);
  }
}

void LocalRepositoryManagement::showError(const QString &error) {
  QMessageBox::warning(this, tr("Local Repository Management"), error);
}

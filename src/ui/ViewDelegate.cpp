#include "ViewDelegate.h"
#include "TreeModel.h"
#include "Badge.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>

void ViewDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const {
  QStyleOptionViewItem opt = option;
  drawBackground(painter, opt, index);

  // Draw badges.
  QString status = index.data(TreeModel::StatusRole).toString();
  if (!status.isEmpty()) {
    QSize size =
        Badge::size(painter->font(), Badge::Label(Badge::Label::Type::Status));
    int width = size.width();
    int height = size.height();

    auto startIter = status.cbegin(), endIter = status.cend();
    int leftAdjust = 0, rightAdjust = -3, leftWidth = 0, rightWidth = -width;
    if (mMultiColumn) {
      leftAdjust = 3;
      rightAdjust = 0;
      leftWidth = width;
      rightWidth = 0;
      std::reverse(status.begin(), status.end());
    }

    // Add extra space.
    opt.rect.adjust(leftAdjust, 0, rightAdjust, 0);

    for (int i = 0; i < status.size(); ++i) {
      int x = opt.rect.x() + opt.rect.width();
      int y = opt.rect.y() + (opt.rect.height() / 2);
      QRect rect(mMultiColumn ? opt.rect.x() : x - width, y - (height / 2),
                 width, height);
      Badge::paint(painter,
                   {Badge::Label(Badge::Label::Type::Status, status.at(i))},
                   rect, &opt);

      // Adjust rect.
      opt.rect.adjust(leftWidth + leftAdjust, 0, rightWidth + rightAdjust, 0);
    }
  }

  QItemDelegate::paint(painter, opt, index);
}

QSize ViewDelegate::sizeHint(const QStyleOptionViewItem &option,
                             const QModelIndex &index) const {
  // Increase spacing.
  QSize size = QItemDelegate::sizeHint(option, index);
  size.setHeight(
      Badge::size(option.font, Badge::Label(Badge::Label::Type::Status))
          .height() +
      4);
  return size;
}

bool ViewDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                             const QStyleOptionViewItem &option,
                             const QModelIndex &index) {
  const QString status = index.data(TreeModel::StatusRole).toString();
  const QChar badge = statusBadgeAt(option, status, event->pos());
  if (!badge.isNull()) {
    QToolTip::showText(event->globalPos(), Badge::statusTooltip(badge), view);
    return true;
  }

  return QItemDelegate::helpEvent(event, view, option, index);
}

QChar ViewDelegate::statusBadgeAt(const QStyleOptionViewItem &option,
                                  const QString &status,
                                  const QPoint &position) const {
  if (status.isEmpty())
    return QChar();

  const QSize badgeSize =
      Badge::size(option.font, Badge::Label(Badge::Label::Type::Status));
  const int width = badgeSize.width();
  const int height = badgeSize.height();
  QStyleOptionViewItem adjusted = option;
  int leftAdjust = 0;
  int rightAdjust = -3;
  int leftWidth = 0;
  int rightWidth = -width;
  QString visualStatus = status;
  if (mMultiColumn) {
    leftAdjust = 3;
    rightAdjust = 0;
    leftWidth = width;
    rightWidth = 0;
    std::reverse(visualStatus.begin(), visualStatus.end());
  }
  adjusted.rect.adjust(leftAdjust, 0, rightAdjust, 0);

  for (const QChar badgeStatus : visualStatus) {
    const int x = adjusted.rect.x() + adjusted.rect.width();
    const int y = adjusted.rect.y() + (adjusted.rect.height() / 2);
    const QRect rect(mMultiColumn ? adjusted.rect.x() : x - width,
                     y - (height / 2), width, height);
    if (rect.contains(position))
      return badgeStatus;
    adjusted.rect.adjust(leftWidth + leftAdjust, 0, rightWidth + rightAdjust,
                         0);
  }

  return QChar();
}

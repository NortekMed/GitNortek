//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef WORKTREEICON_H
#define WORKTREEICON_H

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace WorktreeIcon {

inline QColor color() { return QColor("#36c96b"); }

inline void paint(QPainter *painter, const QRect &rect) {
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing);
  QPen pen(color(), 1.4);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  painter->setPen(pen);
  painter->setBrush(Qt::NoBrush);
  painter->drawLine(rect.center().x(), rect.bottom() - 1, rect.center().x(),
                    rect.top() + 5);
  painter->drawLine(rect.center().x(), rect.top() + 8, rect.left() + 3,
                    rect.top() + 5);
  painter->drawLine(rect.center().x(), rect.top() + 7, rect.right() - 3,
                    rect.top() + 4);
  painter->setPen(Qt::NoPen);
  painter->setBrush(color());
  painter->drawEllipse(QRect(rect.left() + 1, rect.top() + 2, 6, 6));
  painter->drawEllipse(QRect(rect.center().x() - 3, rect.top(), 7, 7));
  painter->drawEllipse(QRect(rect.right() - 6, rect.top() + 1, 6, 6));
  painter->restore();
}

inline QIcon icon() {
  static const QIcon icon = [] {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paint(&painter, pixmap.rect().adjusted(1, 1, -1, -1));
    return QIcon(pixmap);
  }();
  return icon;
}

} // namespace WorktreeIcon

#endif

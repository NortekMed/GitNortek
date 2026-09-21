#ifndef VIEWDELEGATE_H
#define VIEWDELEGATE_H

#include <QItemDelegate>

class QAbstractItemView;
class QHelpEvent;
class QModelIndex;
class QStyleOptionViewItem;

/*!
 * \brief The ViewDelegate class
 * Delegate used by the ColumnView and TreeView to paint the badges (M/A/?)
 */
class ViewDelegate : public QItemDelegate {
public:
  ViewDelegate(QObject *parent, bool multiColumn = false)
      : QItemDelegate(parent), mMultiColumn(multiColumn) {}

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override;

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override;

  bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                 const QStyleOptionViewItem &option,
                 const QModelIndex &index) override;

private:
  QChar statusBadgeAt(const QStyleOptionViewItem &option, const QString &status,
                      const QPoint &position) const;
  bool mMultiColumn = false;
};

#endif // VIEWDELEGATE_H

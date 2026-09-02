//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REPOSITORYTABSTRIP_H
#define REPOSITORYTABSTRIP_H

#include <QIcon>
#include <QVector>
#include <QWidget>

class QAbstractButton;
class QContextMenuEvent;
class QToolButton;

class RepositoryTabStrip : public QWidget {
  Q_OBJECT

public:
  explicit RepositoryTabStrip(QWidget *parent = nullptr);

  int addTab(const QIcon &icon, const QString &text);
  void removeTab(int index);
  void moveTab(int from, int to);

  int count() const;
  int currentIndex() const;
  void setCurrentIndex(int index);

  QIcon tabIcon(int index) const;
  void setTabIcon(int index, const QIcon &icon);
  QString tabText(int index) const;
  void setTabText(int index, const QString &text);
  QString tabToolTip(int index) const;
  void setTabToolTip(int index, const QString &toolTip);

  int rowCount() const;
  int visibleTabCount() const;
  int minimumTabWidth() const;
  QRect tabRect(int index) const;
  int tabAt(const QPoint &position) const;

signals:
  void currentChanged(int index);
  void closeRequested(int index);
  void contextMenuRequested(int index, const QPoint &globalPosition);
  void tabMoved(int from, int to);

protected:
  void contextMenuEvent(QContextMenuEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  struct Tab {
    QAbstractButton *button;
    QIcon icon;
    QString text;
    QString toolTip;
  };

  int indexOf(const QObject *object) const;
  int rowHeight() const;
  int naturalTabWidth(int index) const;
  void relayout();
  void showOverflow();
  void updateAccessibleDescriptions();
  void updateOverflow();

  QVector<Tab> mTabs;
  QToolButton *mOverflow;
  int mCurrentIndex = -1;
  int mFirstVisible = 0;
  int mRowCount = 1;
  int mVisibleCount = 0;
  bool mInRelayout = false;
};

#endif

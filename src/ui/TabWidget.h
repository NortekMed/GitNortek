//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef TABWIDGET_H
#define TABWIDGET_H

#include <QIcon>
#include <QVector>
#include <QWidget>

class QStackedWidget;
class RepositoryTabStrip;

enum class TabCloseScope { Left, Right, Others, All };

class TabWidget : public QWidget {
  Q_OBJECT

public:
  TabWidget(QWidget *parent = nullptr);
  ~TabWidget() override;

  int addTab(QWidget *widget, const QIcon &icon, const QString &text);
  void moveTab(int from, int to);
  int count() const;
  QWidget *widget(int index) const;
  int indexOf(QWidget *widget) const;

  int currentIndex() const;
  QWidget *currentWidget() const;
  void setCurrentIndex(int index);
  void setCurrentWidget(QWidget *widget);

  QIcon tabIcon(int index) const;
  void setTabIcon(int index, const QIcon &icon);
  QString tabText(int index) const;
  void setTabText(int index, const QString &text);
  QString tabToolTip(int index) const;
  void setTabToolTip(int index, const QString &toolTip);

  bool closeTab(int index);
  bool closeTab(QWidget *widget);
  bool closeTabs(int anchorIndex, TabCloseScope scope);

signals:
  void currentChanged(int index);
  void tabAboutToBeInserted();
  void tabAboutToBeRemoved();
  void tabRemovalCancelled();
  void tabInserted();
  void tabRemoved();

private:
  void moveTabFromStrip(int from, int to);
  void detachTab(QWidget *widget);
  void removeTab(QObject *object);

  RepositoryTabStrip *mTabStrip;
  QStackedWidget *mStack;
  QVector<QWidget *> mWidgets;
  int mCurrentIndex = -1;
  bool mDestroying = false;
};

#endif

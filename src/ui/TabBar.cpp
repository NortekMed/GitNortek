//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "TabBar.h"

TabBar::TabBar(QWidget *parent) : QTabBar(parent) {
  setAutoHide(false);
  setDocumentMode(true);
  setExpanding(false);
  setElideMode(Qt::ElideRight);
  setUsesScrollButtons(true);
}

QSize TabBar::tabSizeHint(int index) const {
  QSize size = QTabBar::tabSizeHint(index);
  size.setHeight(fontMetrics().lineSpacing() + 12);
  return size;
}

//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "CommitToolBar.h"
#include "CommitList.h"
#include "RepoView.h"
#include "ConfigKeys.h"
#include "git/Config.h"
#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleOption>
#include <QToolButton>

namespace {

const QString kStyleSheet = "QToolBar {"
                            "  border: none"
                            "}"
                            "QToolButton {"
                            "  border: none;"
                            "  border-radius: 4px;"
                            "  padding-right: 12px"
                            "}";
template <typename T> struct SettingsEntry {
  QString key;
  T value;
};

template <typename T> using SettingsMap = QMap<QString, SettingsEntry<T>>;

template <typename T> class ToolButton : public QToolButton {
public:
  ToolButton(const SettingsMap<T> &map, CommitToolBar *parent, T defaultValue)
      : QToolButton(parent) {
    setPopupMode(QToolButton::InstantPopup);

    QMenu *menu = new QMenu(this);
    QActionGroup *actions = new QActionGroup(menu);

    RepoView *view = RepoView::parentView(parent);
    git::Config config = view->repo().appConfig();
    foreach (const QString &key, map.keys()) {
      QAction *action = menu->addAction(key);
      action->setCheckable(true);
      actions->addAction(action);

      const SettingsEntry<T> &entry = map.value(key);
      if (config.value<T>(entry.key, defaultValue) == entry.value) {
        action->setChecked(true);
        setText(action->text());
      }

      connect(action, &QAction::triggered, [parent, entry] {
        RepoView *view = RepoView::parentView(parent);
        git::Config config = view->repo().appConfig();
        config.setValue(entry.key, entry.value);
        emit parent->settingsChanged();
      });
    }

    setMenu(menu);
    connect(menu, &QMenu::triggered,
            [this](QAction *action) { setText(action->text()); });

#ifdef Q_OS_MAC
    QFont font = this->font();
    font.setPointSize(11);
    setFont(font);
#endif
  }
};

class Style : public QProxyStyle {
public:
  Style(QStyle *style) : QProxyStyle(style) {}

  void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget = nullptr) const {
    if (element != QStyle::PE_IndicatorArrowDown) {
      QProxyStyle::drawPrimitive(element, option, painter, widget);
      return;
    }

    const QRect &rect = option->rect;
    int x = rect.x() + rect.width() - 16;
    int y = rect.y();

    QPainterPath path;
    path.moveTo(5, 4);
    path.lineTo(8, 7);
    path.lineTo(11, 4);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(option->palette.buttonText(), 1.5));
    painter->drawPath(path.translated(x, y));
    painter->restore();
  }
};

} // namespace

CommitToolBar::CommitToolBar(QWidget *parent) : QToolBar(parent) {
  setStyle(new Style(style()));
  setStyleSheet(kStyleSheet);
  setToolButtonStyle(Qt::ToolButtonTextOnly);

  SettingsMap<int> refsMap;
  refsMap.insert(tr("Show All Branches"),
                 {ConfigKeys::kRefsKey, (int)CommitList::RefsFilter::AllRefs});
  refsMap.insert(
      tr("Show Selected Branch"),
      {ConfigKeys::kRefsKey, (int)CommitList::RefsFilter::SelectedRef});
  refsMap.insert(tr("Show Selected Branch, First Parent Only"),
                 {ConfigKeys::kRefsKey,
                  (int)CommitList::RefsFilter::SelectedRefIgnoreMerge});
  addWidget(
      new ToolButton<int>(refsMap, this, (int)CommitList::RefsFilter::AllRefs));

  SettingsMap<bool> sortMap;
  sortMap.insert(tr("Sort by Date"), {ConfigKeys::kSortKey, true});
  sortMap.insert(tr("Sort Topologically"), {ConfigKeys::kSortKey, false});
  addWidget(new ToolButton(sortMap, this, true));

  QWidget *spacer = new QWidget(this);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  addWidget(spacer);

}

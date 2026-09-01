//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "ui/RepositoryTabStrip.h"
#include "ui/TabWidget.h"
#include <QAbstractButton>
#include <QAccessible>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QTimer>
#include <QSignalSpy>
#include <QToolButton>

using namespace QTest;

class TestRepositoryTabs : public QObject {
  Q_OBJECT

private slots:
  void wrapsAtMinimumWidth();
  void limitsRowsAndSearchesHiddenTabs();
  void keepsCurrentTabVisible();
  void keepsSelectedTabChecked();
  void avoidsNarrowOverflowOverlap();
  void rejectsDuplicateWidgets();
  void closesTabScopes();
  void routesTabContextMenu();
  void exposesBulkCloseActions();
};

void TestRepositoryTabs::wrapsAtMinimumWidth() {
  RepositoryTabStrip strip;
  strip.resize(800, 200);
  strip.show();
  QVERIFY(qWaitForWindowExposed(&strip));

  int minimum = strip.minimumTabWidth();
  strip.resize(2 * minimum + 2, 200);
  strip.addTab(QIcon(), "RepositoryAlphaWithLongTitle");
  strip.addTab(QIcon(), "RepositoryBetaWithLongTitle");
  QCOMPARE(strip.rowCount(), 1);
  QVERIFY(strip.tabRect(0).width() >= minimum);
  QVERIFY(strip.tabRect(1).width() >= minimum);

  strip.addTab(QIcon(), "RepositoryGammaWithLongTitle");
  QCOMPARE(strip.rowCount(), 2);
  QCOMPARE(strip.visibleTabCount(), 3);
}

void TestRepositoryTabs::limitsRowsAndSearchesHiddenTabs() {
  RepositoryTabStrip strip;
  strip.resize(420, 300);
  strip.show();
  QVERIFY(qWaitForWindowExposed(&strip));

  for (int i = 0; i < 30; ++i) {
    strip.addTab(QIcon(), QString("Repository %1").arg(i));
    strip.setTabToolTip(i, QString("/projects/repository-%1").arg(i));
  }

  QCOMPARE(strip.rowCount(), 4);
  QVERIFY(strip.visibleTabCount() < strip.count());
  QToolButton *overflow =
      strip.findChild<QToolButton *>("RepositoryTabOverflow");
  QVERIFY(overflow);
  QVERIFY(overflow->isVisible());

  mouseClick(overflow, Qt::LeftButton);
  QTRY_VERIFY(strip.findChild<QLineEdit *>("RepositoryTabSearch"));
  QLineEdit *search = strip.findChild<QLineEdit *>("RepositoryTabSearch");
  QListWidget *list =
      strip.findChild<QListWidget *>("RepositoryTabOverflowList");
  QVERIFY(list);
  QVERIFY(list->count() > 0);

  search->setText("repository-29");
  QCOMPARE(list->count(), 1);
  QCOMPARE(list->item(0)->text(), QString("Repository 29"));
}

void TestRepositoryTabs::keepsCurrentTabVisible() {
  RepositoryTabStrip strip;
  strip.resize(420, 300);
  for (int i = 0; i < 30; ++i)
    strip.addTab(QIcon(), QString("Repository %1").arg(i));

  QSignalSpy currentChanged(&strip, &RepositoryTabStrip::currentChanged);
  strip.setCurrentIndex(29);
  QCOMPARE(currentChanged.count(), 1);
  QVERIFY(!strip.tabRect(29).isEmpty());
  QVERIFY(strip.tabRect(29).intersects(strip.rect()));
  QVERIFY(strip.tabRect(0).isEmpty() ||
          !strip.tabRect(0).intersects(strip.rect()));
}

void TestRepositoryTabs::keepsSelectedTabChecked() {
  RepositoryTabStrip strip;
  strip.resize(420, 100);
  strip.addTab(QIcon(), "Repository");
  strip.show();
  QVERIFY(qWaitForWindowExposed(&strip));

  QAbstractButton *tab =
      strip.findChild<QAbstractButton *>("RepositoryTabButton");
  QVERIFY(tab);
  QCOMPARE(QAccessible::queryAccessibleInterface(&strip)->role(),
           QAccessible::PageTabList);
  QCOMPARE(QAccessible::queryAccessibleInterface(tab)->role(),
           QAccessible::PageTab);
  QVERIFY(QAccessible::queryAccessibleInterface(tab)->state().selected);
  QVERIFY(QAccessible::queryAccessibleInterface(tab)
              ->actionInterface()
              ->actionNames()
              .contains(QAccessibleActionInterface::pressAction()));
  QVERIFY(tab->isChecked());
  mouseClick(tab, Qt::LeftButton, Qt::NoModifier,
             QPoint(20, tab->height() / 2));
  QVERIFY(tab->isChecked());
}

void TestRepositoryTabs::avoidsNarrowOverflowOverlap() {
  RepositoryTabStrip strip;
  strip.resize(strip.minimumTabWidth() / 2, 300);
  for (int i = 0; i < 10; ++i)
    strip.addTab(QIcon(), QString("Repository %1").arg(i));
  strip.show();
  QVERIFY(qWaitForWindowExposed(&strip));

  QToolButton *overflow =
      strip.findChild<QToolButton *>("RepositoryTabOverflow");
  QVERIFY(overflow);
  QVERIFY(strip.rect().contains(overflow->geometry()));
  for (int i = 0; i < strip.count(); ++i) {
    if (!strip.tabRect(i).isEmpty()) {
      QVERIFY(strip.rect().contains(strip.tabRect(i)));
      QVERIFY(!strip.tabRect(i).intersects(overflow->geometry()));
    }
  }
}

void TestRepositoryTabs::rejectsDuplicateWidgets() {
  QMainWindow window;
  auto *tabs = new TabWidget(&window);
  window.setCentralWidget(tabs);
  auto *page = new QWidget;
  QSignalSpy inserted(tabs, &TabWidget::tabInserted);

  QCOMPARE(tabs->addTab(page, QIcon(), "Repository"), 0);
  QCOMPARE(tabs->addTab(page, QIcon(), "Duplicate"), 0);
  QCOMPARE(tabs->count(), 1);
  QCOMPARE(tabs->widget(0), page);
  QCOMPARE(tabs->tabText(0), QString("Repository"));
  QCOMPARE(inserted.count(), 1);
}

void TestRepositoryTabs::closesTabScopes() {
  QMainWindow window;
  auto *tabs = new TabWidget(&window);
  window.setCentralWidget(tabs);
  for (int i = 0; i < 4; ++i) {
    auto *page = new QWidget;
    page->setAttribute(Qt::WA_DeleteOnClose);
    tabs->addTab(page, QIcon(), QString("Repository %1").arg(i));
  }
  tabs->setCurrentIndex(2);

  QVERIFY(tabs->closeTabs(2, TabCloseScope::Left));
  QCOMPARE(tabs->count(), 2);
  QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("Repository 2"));

  QVERIFY(tabs->closeTabs(0, TabCloseScope::Right));
  QCOMPARE(tabs->count(), 1);
  QVERIFY(tabs->closeTabs(0, TabCloseScope::All));
  QCOMPARE(tabs->count(), 0);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void TestRepositoryTabs::routesTabContextMenu() {
  RepositoryTabStrip strip;
  strip.resize(600, 100);
  strip.addTab(QIcon(), "First");
  strip.addTab(QIcon(), "Second");
  strip.show();
  QVERIFY(qWaitForWindowExposed(&strip));

  QAbstractButton *button =
      strip.findChildren<QAbstractButton *>("RepositoryTabButton").at(1);
  QVERIFY(button);
  QSignalSpy requested(&strip,
                       &RepositoryTabStrip::contextMenuRequested);
  QPoint local(10, button->height() / 2);
  QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                          button->mapToGlobal(local));
  QApplication::sendEvent(button, &event);
  QCOMPARE(requested.count(), 1);
  QCOMPARE(requested.at(0).at(0).toInt(), 1);
}

void TestRepositoryTabs::exposesBulkCloseActions() {
  QMainWindow window;
  auto *tabs = new TabWidget(&window);
  window.setCentralWidget(tabs);
  tabs->addTab(new QWidget, QIcon(), "First");
  tabs->addTab(new QWidget, QIcon(), "Second");
  auto *strip = tabs->findChild<RepositoryTabStrip *>();
  QVERIFY(strip);
  strip->resize(600, 100);
  window.show();
  QVERIFY(qWaitForWindowExposed(&window));

  QAbstractButton *button =
      strip->findChildren<QAbstractButton *>("RepositoryTabButton").at(1);
  QVERIFY(button);
  QTimer::singleShot(0, [] {
    QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(menu);
    QCOMPARE(menu->actions().size(), 4);
    QCOMPARE(menu->actions().at(0)->text(), QString("Close tabs on left"));
    QCOMPARE(menu->actions().at(1)->text(), QString("Close tabs on right"));
    QCOMPARE(menu->actions().at(2)->text(), QString("Close others tabs"));
    QCOMPARE(menu->actions().at(3)->text(), QString("Close all tabs"));
    QVERIFY(menu->actions().at(0)->isEnabled());
    QVERIFY(!menu->actions().at(1)->isEnabled());
    QVERIFY(menu->actions().at(2)->isEnabled());
    QVERIFY(menu->actions().at(3)->isEnabled());
    menu->close();
  });
  QPoint local(10, button->height() / 2);
  QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                          button->mapToGlobal(local));
  QApplication::sendEvent(button, &event);
}

TEST_MAIN(TestRepositoryTabs)

#include "repository_tabs.moc"

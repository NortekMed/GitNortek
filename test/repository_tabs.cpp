//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "ui/RepositoryTabStrip.h"
#include <QAbstractButton>
#include <QAccessible>
#include <QLineEdit>
#include <QListWidget>
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

TEST_MAIN(TestRepositoryTabs)

#include "repository_tabs.moc"

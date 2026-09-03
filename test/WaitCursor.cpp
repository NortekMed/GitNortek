//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "Test.h"
#include "util/WaitCursor.h"
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPromise>

using namespace QTest;

class TestWaitCursor : public QObject {
  Q_OBJECT

private slots:
  void nesting();
  void outOfOrderRelease();
  void trackedWatcher();
  void watcherDestruction();
};

void TestWaitCursor::nesting() {
  QVERIFY(!QGuiApplication::overrideCursor());

  WaitCursor::Token outer = WaitCursor::acquire();
  QVERIFY(QGuiApplication::overrideCursor());
  QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::WaitCursor);

  {
    WaitCursor::Token inner = WaitCursor::acquire();
    QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::WaitCursor);
  }

  QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::WaitCursor);
  outer.reset();
  QVERIFY(!QGuiApplication::overrideCursor());
}

void TestWaitCursor::outOfOrderRelease() {
  WaitCursor::Token first = WaitCursor::acquire();
  WaitCursor::Token second = WaitCursor::acquire();
  WaitCursor::Token third = WaitCursor::acquire();

  second.reset();
  QVERIFY(QGuiApplication::overrideCursor());
  first.reset();
  QVERIFY(QGuiApplication::overrideCursor());
  third.reset();
  QVERIFY(!QGuiApplication::overrideCursor());
}

void TestWaitCursor::trackedWatcher() {
  QPromise<void> promise;
  promise.start();
  QFutureWatcher<void> watcher;
  WaitCursor::track(&watcher);
  watcher.setFuture(promise.future());

  QVERIFY(QGuiApplication::overrideCursor());
  watcher.cancel();
  QVERIFY(QGuiApplication::overrideCursor());

  promise.finish();
  QTRY_VERIFY(watcher.isFinished());
  QTRY_VERIFY(!QGuiApplication::overrideCursor());
}

void TestWaitCursor::watcherDestruction() {
  QPromise<void> promise;
  promise.start();
  auto *watcher = new QFutureWatcher<void>;
  WaitCursor::track(watcher);
  watcher->setFuture(promise.future());
  QVERIFY(QGuiApplication::overrideCursor());

  delete watcher;
  QVERIFY(!QGuiApplication::overrideCursor());
  promise.finish();
}

TEST_MAIN(TestWaitCursor)

#include "WaitCursor.moc"

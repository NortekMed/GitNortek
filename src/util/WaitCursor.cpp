//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "WaitCursor.h"
#include <QCursor>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QThread>

namespace {

int sCount = 0;

void assertGuiThread() {
  Q_ASSERT(QThread::currentThread() == qApp->thread());
}

} // namespace

WaitCursor::Token WaitCursor::acquire() {
  assertGuiThread();
  if (sCount++ == 0)
    QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
  return Token(new WaitCursor);
}

void WaitCursor::track(QFutureWatcherBase *watcher) {
  Q_ASSERT(watcher);
  Token token = acquire();
  QObject::connect(watcher, &QFutureWatcherBase::finished, watcher,
                   [token = std::move(token)]() mutable { token.reset(); });
}

WaitCursor::~WaitCursor() {
  assertGuiThread();
  Q_ASSERT(sCount > 0);
  if (--sCount == 0)
    QGuiApplication::restoreOverrideCursor();
}

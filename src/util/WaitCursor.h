//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#ifndef WAITCURSOR_H
#define WAITCURSOR_H

#include <memory>

class QFutureWatcherBase;

class WaitCursor {
public:
  using Token = std::shared_ptr<WaitCursor>;

  static Token acquire();
  static void track(QFutureWatcherBase *watcher);

  ~WaitCursor();

  WaitCursor(const WaitCursor &) = delete;
  WaitCursor &operator=(const WaitCursor &) = delete;

private:
  WaitCursor() = default;
};

#endif

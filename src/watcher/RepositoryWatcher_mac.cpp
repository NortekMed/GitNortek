//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "RepositoryWatcher.h"
#include <CoreServices/CoreServices.h>
#include <QStringList>

class RepositoryWatcherPrivate : public QObject {
  Q_OBJECT

public:
  RepositoryWatcherPrivate(const git::Repository &repo,
                           QObject *parent = nullptr)
      : QObject(parent), mRepo(repo) {
    // Create dispatch queue.
    mQueue = dispatch_queue_create("com.nortekmed.GitNortek.RepositoryWatcher",
                                   nullptr);

    // Watch both files and Git metadata, which may live outside a linked
    // worktree's directory.
    FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};
    QStringList roots = {repo.workdir().absolutePath(),
                         repo.dir().absolutePath(),
                         repo.commonDir().absolutePath()};
    roots.removeDuplicates();
    CFMutableArrayRef paths = CFArrayCreateMutable(
        nullptr, roots.size(), &kCFTypeArrayCallBacks);
    for (const QString &root : roots) {
      CFStringRef path = root.toCFString();
      CFArrayAppendValue(paths, path);
      CFRelease(path);
    }
    mStream = FSEventStreamCreate(nullptr, &notify, &context, paths,
                                  kFSEventStreamEventIdSinceNow, 0,
                                  kFSEventStreamCreateFlagNone);
    CFRelease(paths);

    // Register with queue.
    FSEventStreamSetDispatchQueue(mStream, mQueue);

    // Start the stream.
    FSEventStreamStart(mStream);
  }

  ~RepositoryWatcherPrivate() {
    // Stop stream.
    FSEventStreamStop(mStream);

    // Release stream.
    FSEventStreamInvalidate(mStream);
    FSEventStreamRelease(mStream);

    // Release queue
    dispatch_release(mQueue);
  }

  git::Repository repo() const { return mRepo; }

  static void notify(ConstFSEventStreamRef streamRef, void *clientCallBackInfo,
                     size_t numEvents, void *eventPaths,
                     const FSEventStreamEventFlags eventFlags[],
                     const FSEventStreamEventId eventIds[]) {
    RepositoryWatcherPrivate *watcher =
        static_cast<RepositoryWatcherPrivate *>(clientCallBackInfo);

    // Filter out ignored directories.
    git::Repository repo = watcher->repo();
    const QString gitDir = repo.dir().absolutePath();
    const QString commonDir = repo.commonDir().absolutePath();
    const char **paths = static_cast<const char **>(eventPaths);
    for (int i = 0; i < numEvents; ++i) {
      const QString path = QString::fromUtf8(paths[i]);
      const bool metadata = path == gitDir || path.startsWith(gitDir + '/') ||
                            path == commonDir ||
                            path.startsWith(commonDir + '/');
      if (metadata || !repo.isIgnored(path)) {
        emit watcher->notificationReceived();
        return;
      }
    }
  }

signals:
  void notificationReceived();

private:
  git::Repository mRepo;
  dispatch_queue_t mQueue;
  FSEventStreamRef mStream;
};

RepositoryWatcher::RepositoryWatcher(const git::Repository &repo,
                                     QObject *parent)
    : QObject(parent), d(new RepositoryWatcherPrivate(repo, this)) {
  init(repo);
  connect(d, &RepositoryWatcherPrivate::notificationReceived, &mTimer,
          QOverload<>::of(&QTimer::start));
}

RepositoryWatcher::~RepositoryWatcher() {}

#include "RepositoryWatcher_mac.moc"

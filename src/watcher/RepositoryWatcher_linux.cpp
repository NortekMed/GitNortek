//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "RepositoryWatcher.h"
#include "git/Submodule.h"
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <poll.h>
#include <unistd.h>
#include <sys/inotify.h>

namespace {

const uint kFlags = (IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                     IN_DELETE_SELF | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
                     IN_MOVE_SELF);

// FIXME: Include hidden and filter .git explicitly?
const QDir::Filters kFilters = (QDir::Dirs | QDir::NoDotAndDotDot);

} // namespace

class RepositoryWatcherPrivate : public QThread {
  Q_OBJECT

public:
  RepositoryWatcherPrivate(const git::Repository &repo,
                           QObject *parent = nullptr)
      : QThread(parent), mRepo(repo) {
    foreach (const git::Submodule &submodule, mRepo.submodules()) {
      git::Repository submoduleRepo = submodule.open();
      if (submoduleRepo.isValid())
        mSubmoduleGitDirs.append(submoduleRepo.dir().path());
    }

    mFd = inotify_init1(IN_NONBLOCK);
    if (mFd < 0)
      return; // FIXME: Report error?

    if (pipe(mPipe) < 0) {
      // FIXME: Report error?
      close(mFd);
      mFd = -1;
    }
  }

  ~RepositoryWatcherPrivate() {
    close(mPipe[1]);
    close(mPipe[0]);
    close(mFd);
  }

  bool isValid() const { return (mFd >= 0); }

  void run() override {
    // Watch the root directory.
    watchWorkdir(mRepo.workdir());
    watchGitMetadata();

    // Start listening for notifications.
    forever {
      pollfd pollFds[2];
      pollFds[0].fd = mPipe[0];
      pollFds[0].events = POLLIN;
      pollFds[1].fd = mFd;
      pollFds[1].events = POLLIN;
      if (poll(pollFds, 2, -1) < 0)
        return; // FIXME: Report error?

      // Check for signal to quit.
      if (pollFds[0].revents & POLLIN)
        return;

      // Check for notifications.
      if (!(pollFds[1].revents & POLLIN))
        continue;

      // Read notifications.
      bool ignored = true;
      forever {
        char buf[4096];
        int len = read(mFd, buf, sizeof(buf));
        if (len <= 0)
          break;

        const inotify_event *event = nullptr;
        for (char *ptr = buf; ptr < buf + len;
             ptr += sizeof(inotify_event) + event->len) {
          event = reinterpret_cast<inotify_event *>(ptr);
          if (event->len) {
            QString path = mWds.value(event->wd).filePath(event->name);
            bool gitMetadata = mGitWds.contains(event->wd);
            if (gitMetadata && isGitMetadataPath(path)) {
              ignored = false;

              uint32_t mask = (IN_CREATE | IN_ISDIR);
              if ((event->mask & mask) == mask)
                watchGitDirectory(QDir(path), true);
            } else if (!gitMetadata && !mRepo.isIgnored(path)) {
              ignored = false;

              // Start watching new directories.
              uint32_t mask = (IN_CREATE | IN_ISDIR);
              if ((event->mask & mask) == mask)
                watchWorkdir(path);
            }
          }
        }
      }

      if (!ignored)
        emit notificationReceived();
    }
  }

  bool watchDirectory(const QDir &dir, bool gitMetadata) {
    QString path = dir.path();
    if (mDirWds.contains(path)) {
      if (gitMetadata)
        mGitWds.insert(mDirWds.value(path));
      return true;
    }

    int wd = inotify_add_watch(mFd, path.toUtf8(), kFlags);
    if (wd < 0)
      return false; // FIXME: Report error?

    // Associate the dir with this watch descriptor.
    mWds[wd] = dir;
    mDirWds[path] = wd;
    if (gitMetadata)
      mGitWds.insert(wd);

    return true;
  }

  void watchWorkdir(const QDir &dir) {
    if (!watchDirectory(dir, false))
      return;

    // Watch subdirs.
    foreach (const QString &name, dir.entryList(kFilters)) {
      QString path = dir.filePath(name);
      if (!mRepo.isIgnored(path))
        watchWorkdir(path);
    }
  }

  void watchGitDirectory(const QDir &dir, bool recursive) {
    if (!dir.exists() || !watchDirectory(dir, true))
      return;

    if (!recursive)
      return;

    foreach (const QString &name, dir.entryList(kFilters))
      watchGitDirectory(QDir(dir.filePath(name)), true);
  }

  void watchGitMetadata() {
    QDir gitDir = mRepo.dir();
    QDir commonDir = mRepo.commonDir();
    watchGitDirectory(gitDir, false);
    watchGitMetadata(gitDir);
    if (commonDir.absolutePath() != gitDir.absolutePath()) {
      watchGitDirectory(commonDir, false);
      watchGitMetadata(commonDir);
    }
    watchGitDirectory(QDir(commonDir.filePath("worktrees")), true);
    foreach (const QString &path, mSubmoduleGitDirs) {
      QDir submoduleGitDir(path);
      watchGitDirectory(submoduleGitDir, false);
      watchGitMetadata(submoduleGitDir);
    }
  }

  void watchGitMetadata(const QDir &gitDir) {
    foreach (const QString &path,
             QStringList({"refs", "refs/heads", "refs/remotes", "refs/tags",
                          "logs", "logs/refs", "logs/refs/heads",
                          "logs/refs/remotes"}))
      watchGitDirectory(QDir(gitDir.filePath(path)), true);
  }

  bool isGitMetadataPath(const QString &path) const {
    QString relative = mRepo.dir().relativeFilePath(path);
    if (relative.startsWith("modules/") || isRepositoryMetadata(relative))
      return true;

    relative = mRepo.commonDir().relativeFilePath(path);
    if (relative.startsWith("modules/") || relative == "worktrees" ||
        relative.startsWith("worktrees/") || isRepositoryMetadata(relative))
      return true;

    foreach (const QString &gitDir, mSubmoduleGitDirs) {
      if (isRepositoryMetadata(QDir(gitDir).relativeFilePath(path)))
        return true;
    }
    return false;
  }

  bool isRepositoryMetadata(const QString &relative) const {
    return relative == "HEAD" || relative == "index" ||
           relative == "packed-refs" || relative == "refs" ||
           relative.startsWith("refs/") || relative == "logs" ||
           relative == "logs/HEAD" || relative == "logs/refs" ||
           relative == "logs/refs/heads" ||
           relative.startsWith("logs/refs/heads/") ||
           relative == "logs/refs/remotes" ||
           relative.startsWith("logs/refs/remotes/");
  }

  void stop() {
    if (write(mPipe[1], "\n", 1) < 0)
      terminate(); // FIXME: Report error?
  }

signals:
  void notificationReceived();

private:
  git::Repository mRepo;
  int mFd = -1;
  int mPipe[2] = {-1, -1};
  QMap<int, QDir> mWds;
  QMap<QString, int> mDirWds;
  QSet<int> mGitWds;
  QStringList mSubmoduleGitDirs;
};

RepositoryWatcher::RepositoryWatcher(const git::Repository &repo,
                                     QObject *parent)
    : QObject(parent), d(new RepositoryWatcherPrivate(repo, this)) {
  init(repo);
  connect(d, &RepositoryWatcherPrivate::notificationReceived, &mTimer,
          static_cast<void (QTimer::*)()>(&QTimer::start));

  if (d->isValid())
    d->start();
}

RepositoryWatcher::~RepositoryWatcher() {
  if (d->isValid()) {
    d->stop();
    d->wait();
  }
}

#include "RepositoryWatcher_linux.moc"

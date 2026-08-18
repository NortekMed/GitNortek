//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef COMMITAVATARPROVIDER_H
#define COMMITAVATARPROVIDER_H

#include "git/Repository.h"
#include <QCache>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <functional>

namespace git {
class Commit;
}

class CommitAvatarProvider : public QObject {
  Q_OBJECT

public:
  using AvatarMap = QMap<QString, QUrl>;
  using LookupCallback = std::function<void(const AvatarMap &avatars)>;
  using Lookup = std::function<void(const QStringList &oids, int size,
                                    const LookupCallback &callback)>;

  explicit CommitAvatarProvider(const git::Repository &repo,
                                QObject *parent = nullptr);
  CommitAvatarProvider(const git::Repository &repo, const Lookup &lookup,
                       QObject *parent = nullptr);

  QPixmap avatar(const git::Commit &commit, int logicalSize,
                 qreal devicePixelRatio);
  bool isAvailable() const;

signals:
  void avatarReady(const QString &oid);
  void avatarsChanged();

private:
  void initialize();
  void requestBatch();
  void requestImage(const QString &oid, const QUrl &url);
  void startDownloads();
  QPixmap render(const QString &oid, const QUrl &url, int logicalSize,
                 qreal devicePixelRatio);

  git::Repository mRepo;
  Lookup mLookup;
  QNetworkAccessManager mManager;
  QTimer mBatchTimer;
  QCache<QString, QImage> mImages;
  QCache<QString, QPixmap> mPixmaps;
  QHash<QString, QUrl> mAvatarUrls;
  QHash<QString, QSet<QString>> mUrlWaiters;
  QSet<QString> mQueued;
  QSet<QString> mLookups;
  QSet<QString> mDownloads;
  QSet<QString> mMissing;
  QQueue<QUrl> mDownloadQueue;
  int mActiveDownloads = 0;
};

#endif

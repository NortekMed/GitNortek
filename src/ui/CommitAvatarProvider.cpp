//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "CommitAvatarProvider.h"
#include "conf/Setting.h"
#include "conf/Settings.h"
#include "git/Commit.h"
#include "git/Remote.h"
#include "host/Accounts.h"
#include "host/GitHub.h"
#include "host/Repository.h"
#include <QBuffer>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QVariant>
#include <QtMath>
#include <climits>

namespace {

const int kAvatarSourceSize = 256;
const int kMaximumResponseSize = 2 * 1024 * 1024;
const int kMaximumDecodedSize = 2048;
const int kBatchSize = 24;
const int kMaximumDownloads = 6;

} // namespace

CommitAvatarProvider::CommitAvatarProvider(const git::Repository &repo,
                                           QObject *parent)
    : QObject(parent), mRepo(repo), mImages(32 * 1024 * 1024),
      mPixmaps(16 * 1024 * 1024) {
  git::Remote remote = mRepo.defaultRemote();
  Repository *remoteRepo =
      remote ? Accounts::instance()->lookup(remote.url()) : nullptr;
  GitHub *github =
      remoteRepo && remoteRepo->account()->kind() == Account::GitHub
          ? qobject_cast<GitHub *>(remoteRepo->account())
          : nullptr;
  if (github) {
    QPointer<GitHub> account(github);
    QPointer<Repository> repository(remoteRepo);
    mLookup = [account, repository](const QStringList &oids, int size,
                                    const LookupCallback &callback) {
      if (account && repository)
        account->requestCommitAvatars(repository, oids, size, callback);
      else
        callback({});
    };
  }
  initialize();
}

CommitAvatarProvider::CommitAvatarProvider(const git::Repository &repo,
                                           const Lookup &lookup,
                                           QObject *parent)
    : QObject(parent), mRepo(repo), mLookup(lookup), mImages(32 * 1024 * 1024),
      mPixmaps(16 * 1024 * 1024) {
  initialize();
}

void CommitAvatarProvider::initialize() {
  mBatchTimer.setSingleShot(true);
  mBatchTimer.setInterval(20);
  connect(&mBatchTimer, &QTimer::timeout, this,
          &CommitAvatarProvider::requestBatch);
  connect(Settings::instance(), &Settings::settingsChanged, this,
          &CommitAvatarProvider::avatarsChanged);
}

bool CommitAvatarProvider::isAvailable() const {
  return static_cast<bool>(mLookup);
}

QPixmap CommitAvatarProvider::avatar(const git::Commit &commit, int logicalSize,
                                     qreal devicePixelRatio) {
  if (!commit.isValid() || logicalSize <= 0 || devicePixelRatio <= 0 ||
      !Settings::instance()->value(Setting::Id::ShowAvatars).toBool() ||
      !isAvailable()) {
    return QPixmap();
  }

  QString oid = commit.id().toString();
  if (mMissing.contains(oid))
    return QPixmap();

  auto url = mAvatarUrls.constFind(oid);
  if (url != mAvatarUrls.cend()) {
    QPixmap pixmap = render(oid, *url, logicalSize, devicePixelRatio);
    if (!pixmap.isNull())
      return pixmap;
    requestImage(oid, *url);
    return QPixmap();
  }

  if (!mLookups.contains(oid)) {
    mQueued.insert(oid);
    if (!mBatchTimer.isActive())
      mBatchTimer.start();
  }
  return QPixmap();
}

void CommitAvatarProvider::requestBatch() {
  if (!Settings::instance()->value(Setting::Id::ShowAvatars).toBool()) {
    mQueued.clear();
    return;
  }
  if (!isAvailable() || mQueued.isEmpty())
    return;

  QStringList oids;
  while (!mQueued.isEmpty() && oids.size() < kBatchSize) {
    QString oid = *mQueued.cbegin();
    mQueued.remove(oid);
    mLookups.insert(oid);
    oids.append(oid);
  }
  if (!mQueued.isEmpty())
    mBatchTimer.start();

  QPointer<CommitAvatarProvider> guard(this);
  mLookup(oids, kAvatarSourceSize,
          [guard, oids](const QMap<QString, QUrl> &avatars) {
            if (!guard)
              return;
            for (const QString &oid : oids) {
              guard->mLookups.remove(oid);
              auto avatar = avatars.constFind(oid);
              if (avatar == avatars.cend()) {
                guard->mMissing.insert(oid);
                emit guard->avatarReady(oid);
                continue;
              }
              guard->mAvatarUrls.insert(oid, *avatar);
              guard->requestImage(oid, *avatar);
            }
          });
}

void CommitAvatarProvider::requestImage(const QString &oid, const QUrl &url) {
  if (url.scheme() != "https" && url.scheme() != "data") {
    mMissing.insert(oid);
    emit avatarReady(oid);
    return;
  }
  QString key = url.toString();
  if (mImages.contains(key)) {
    emit avatarReady(oid);
    return;
  }

  mUrlWaiters[key].insert(oid);
  if (mDownloads.contains(key))
    return;
  mDownloads.insert(key);
  mDownloadQueue.enqueue(url);
  startDownloads();
}

void CommitAvatarProvider::startDownloads() {
  while (mActiveDownloads < kMaximumDownloads && !mDownloadQueue.isEmpty()) {
    QUrl url = mDownloadQueue.dequeue();
    QString key = url.toString();
    ++mActiveDownloads;

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10000);
    QNetworkReply *reply = mManager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
      QByteArray data = reply->readAll();
      bool success = reply->error() == QNetworkReply::NoError &&
                     data.size() <= kMaximumResponseSize;
      QImage image;
      if (success) {
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        QSize size = reader.size();
        success = size.isValid() && size.width() <= kMaximumDecodedSize &&
                  size.height() <= kMaximumDecodedSize;
        if (success) {
          reader.setScaledSize(size.scaled(kAvatarSourceSize, kAvatarSourceSize,
                                           Qt::KeepAspectRatioByExpanding));
          image = reader.read().convertToFormat(QImage::Format_ARGB32);
          success = !image.isNull();
        }
      }

      if (success) {
        int cost = qMin<qsizetype>(image.sizeInBytes(), INT_MAX);
        mImages.insert(key, new QImage(image), cost);
      }

      const QSet<QString> waiters = mUrlWaiters.take(key);
      mDownloads.remove(key);
      --mActiveDownloads;
      for (const QString &oid : waiters) {
        if (!success)
          mMissing.insert(oid);
        emit avatarReady(oid);
      }
      reply->deleteLater();
      startDownloads();
    });
  }
}

QPixmap CommitAvatarProvider::render(const QString &oid, const QUrl &url,
                                     int logicalSize, qreal devicePixelRatio) {
  QString imageKey = url.toString();
  QImage *source = mImages.object(imageKey);
  if (!source)
    return QPixmap();

  int physicalSize = qCeil(logicalSize * devicePixelRatio);
  QString pixmapKey = QString("%1@%2").arg(oid).arg(physicalSize);
  if (QPixmap *cached = mPixmaps.object(pixmapKey))
    return *cached;

  QImage scaled =
      source->scaled(physicalSize, physicalSize, Qt::KeepAspectRatioByExpanding,
                     Qt::SmoothTransformation);
  QRect sourceRect((scaled.width() - physicalSize) / 2,
                   (scaled.height() - physicalSize) / 2, physicalSize,
                   physicalSize);
  QPixmap pixmap(physicalSize, physicalSize);
  pixmap.fill(Qt::transparent);
  QPainterPath path;
  path.addEllipse(QRect(0, 0, physicalSize, physicalSize));
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setClipPath(path);
  painter.drawImage(QRect(0, 0, physicalSize, physicalSize), scaled,
                    sourceRect);
  painter.end();
  pixmap.setDevicePixelRatio(devicePixelRatio);

  int cost = qMin<qint64>(qint64(physicalSize) * physicalSize * 4, INT_MAX);
  mPixmaps.insert(pixmapKey, new QPixmap(pixmap), cost);
  return pixmap;
}

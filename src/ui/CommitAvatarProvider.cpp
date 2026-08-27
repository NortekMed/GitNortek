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
#include "git/Signature.h"
#include "host/Accounts.h"
#include "host/GitHub.h"
#include "host/Repository.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QVariant>
#include <QtMath>
#include <climits>

namespace {

const int kAvatarSourceSize = 256;
const int kMaximumResponseSize = 2 * 1024 * 1024;
const int kMaximumDecodedSize = 2048;
const int kBatchSize = 24;
const int kMaximumDownloads = 6;
const int kMissingRetryMinutes = 5;

bool githubRepository(const QString &url, QString &owner, QString &name) {
  static const QRegularExpression expression(
      R"((?:^|[@/:])github\.com[:/]([^/]+)/([^/#]+?)(?:\.git)?/?$)",
      QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatch match = expression.match(url.trimmed());
  if (!match.hasMatch())
    return false;
  owner = match.captured(1);
  name = match.captured(2);
  if (name.endsWith(".git", Qt::CaseInsensitive))
    name.chop(4);
  return !owner.isEmpty() && !name.isEmpty();
}

} // namespace

CommitAvatarProvider::CommitAvatarProvider(const git::Repository &repo,
                                           QObject *parent)
    : QObject(parent), mRepo(repo), mImages(32 * 1024 * 1024),
      mPixmaps(16 * 1024 * 1024) {
  mFallback = [](const QString &email, int size) {
    QByteArray normalized = email.trimmed().toLower().toUtf8();
    if (normalized.isEmpty())
      return QUrl();
    QByteArray hash =
        QCryptographicHash::hash(normalized, QCryptographicHash::Md5).toHex();
    return QUrl(QString("https://www.gravatar.com/avatar/%1?s=%2&d=404")
                    .arg(QString::fromLatin1(hash))
                    .arg(size));
  };
  resolveGitHub();
  initialize();

  Accounts *accounts = Accounts::instance();
  connect(accounts, &Accounts::accountAdded, this,
          &CommitAvatarProvider::resolveGitHub);
  connect(accounts, &Accounts::repositoryAdded, this,
          [this](int) { resolveGitHub(); });
}

CommitAvatarProvider::CommitAvatarProvider(const git::Repository &repo,
                                           const Lookup &lookup,
                                           const Fallback &fallback,
                                           QObject *parent)
    : QObject(parent), mRepo(repo), mLookup(lookup), mFallback(fallback),
      mImages(32 * 1024 * 1024), mPixmaps(16 * 1024 * 1024) {
  initialize();
}

void CommitAvatarProvider::initialize() {
  mBatchTimer.setSingleShot(true);
  mBatchTimer.setInterval(20);
  connect(&mBatchTimer, &QTimer::timeout, this,
          &CommitAvatarProvider::requestBatch);
  connect(Settings::instance(), &Settings::settingsChanged, this, [this] {
    mMissing.clear();
    emit avatarsChanged();
  });
}

bool CommitAvatarProvider::isAvailable() const {
  return static_cast<bool>(mLookup) || static_cast<bool>(mFallback);
}

void CommitAvatarProvider::resolveGitHub() {
  QString owner;
  QString name;
  QList<git::Remote> remotes;
  if (git::Remote remote = mRepo.defaultRemote())
    remotes.append(remote);
  for (const git::Remote &remote : mRepo.remotes()) {
    bool duplicate = false;
    for (const git::Remote &candidate : remotes)
      duplicate |= candidate.name() == remote.name();
    if (!duplicate)
      remotes.append(remote);
  }
  GitHub *github = nullptr;
  Accounts *accounts = Accounts::instance();
  for (const git::Remote &remote : remotes) {
    Repository *repository = accounts->lookup(remote.url());
    if (repository && repository->account()->kind() == Account::GitHub &&
        githubRepository(remote.url(), owner, name)) {
      github = qobject_cast<GitHub *>(repository->account());
      break;
    }
  }
  for (const git::Remote &remote : remotes) {
    if (owner.isEmpty() && githubRepository(remote.url(), owner, name))
      break;
  }
  if (owner.isEmpty())
    return;
  mHasGitHubRemote = true;

  for (int i = 0; i < accounts->count(); ++i) {
    Account *account = accounts->account(i);
    if (!github && account->kind() == Account::GitHub) {
      github = qobject_cast<GitHub *>(account);
      break;
    }
  }
  if (!github)
    return;

  QPointer<GitHub> account(github);
  mLookup = [account, owner, name](const QStringList &oids, int size,
                                   const LookupCallback &callback) {
    if (account)
      account->requestCommitAvatars(owner, name, oids, size, callback);
    else
      callback(false, {});
  };
  mMissing.clear();
  emit avatarsChanged();
}

QPixmap CommitAvatarProvider::avatar(const git::Commit &commit, int logicalSize,
                                     qreal devicePixelRatio) {
  if (!commit.isValid() || logicalSize <= 0 || devicePixelRatio <= 0 ||
      !Settings::instance()->value(Setting::Id::ShowAvatars).toBool() ||
      !isAvailable()) {
    return QPixmap();
  }

  QString oid = commit.id().toString();
  mEmails.insert(oid, commit.author().email());
  mNames.insert(oid, commit.author().name());
  auto missing = mMissing.constFind(oid);
  if (missing != mMissing.cend()) {
    if (*missing > QDateTime::currentDateTimeUtc())
      return QPixmap();
    mMissing.erase(missing);
  }

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

  if (!mLookup) {
    for (const QString &oid : oids) {
      mLookups.remove(oid);
      resolveFallback(oid);
    }
    return;
  }

  QPointer<CommitAvatarProvider> guard(this);
  mLookup(oids, kAvatarSourceSize,
          [guard, oids](bool, const QMap<QString, QUrl> &avatars) {
            if (!guard)
              return;
            for (const QString &oid : oids) {
              guard->mLookups.remove(oid);
              auto avatar = avatars.constFind(oid);
              if (avatar == avatars.cend()) {
                guard->resolveFallback(oid);
                continue;
              }
              guard->mAvatarUrls.insert(oid, *avatar);
              guard->requestImage(oid, *avatar);
            }
          });
}

void CommitAvatarProvider::resolveFallback(const QString &oid) {
  QUrl url =
      mFallback ? mFallback(mEmails.value(oid), kAvatarSourceSize) : QUrl();
  if (!url.isValid()) {
    mMissing.insert(oid, QDateTime::currentDateTimeUtc().addSecs(
                             kMissingRetryMinutes * 60));
    emit avatarReady(oid);
    return;
  }
  mAvatarUrls.insert(oid, url);
  requestImage(oid, url);
}

void CommitAvatarProvider::resolveProfile(const QString &oid) {
  static const QRegularExpression username("^[A-Za-z0-9-]{1,39}$");
  QString name = mNames.value(oid).trimmed();
  if (!mHasGitHubRemote || !username.match(name).hasMatch()) {
    mMissing.insert(oid, QDateTime::currentDateTimeUtc().addSecs(
                             kMissingRetryMinutes * 60));
    emit avatarReady(oid);
    return;
  }

  QUrl url(QString("https://github.com/%1.png?size=%2")
               .arg(name)
               .arg(kAvatarSourceSize));
  mAvatarUrls.insert(oid, url);
  requestImage(oid, url);
}

void CommitAvatarProvider::requestImage(const QString &oid, const QUrl &url) {
  if (url.scheme() != "https" && url.scheme() != "data") {
    mMissing.insert(oid, QDateTime::currentDateTimeUtc().addSecs(
                             kMissingRetryMinutes * 60));
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
      bool success =
          reply->error() == QNetworkReply::NoError && reply->isOpen();
      QByteArray data;
      if (success)
        data = reply->readAll();
      success = success && data.size() <= kMaximumResponseSize;
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
        if (!success) {
          QUrl failed = mAvatarUrls.value(oid);
          if (failed.host().contains("gravatar.com")) {
            mAvatarUrls.remove(oid);
            resolveProfile(oid);
            continue;
          }
          if (failed.host() == "github.com") {
            mMissing.insert(oid, QDateTime::currentDateTimeUtc().addSecs(
                                     kMissingRetryMinutes * 60));
          } else {
            mAvatarUrls.remove(oid);
            resolveFallback(oid);
            continue;
          }
        }
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

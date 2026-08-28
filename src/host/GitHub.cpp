//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "GitHub.h"
#include "Repository.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <climits>

namespace {

const char *kPasswordProperty = "password";

const QString kScope = "repo";

const QString kAuthUrl =
    QStringLiteral("https://github.com/login/oauth/authorize");
const QString kAccessUrl =
    QStringLiteral("https://github.com/login/oauth/access_token");
const QString kGraphQlUrl = QStringLiteral("https://api.github.com/graphql");
const QString kIssuesUrl =
    QStringLiteral("https://api.github.com/search/issues");

QString graphqlString(QString value) {
  value.replace('\\', "\\\\");
  value.replace('"', "\\\"");
  return value;
}

} // namespace

GitHub::GitHub(const QString &username) : Account(username) {
  QObject::connect(
      mMgr, &QNetworkAccessManager::finished, this,
      [this](QNetworkReply *reply) {
        QString password = reply->property(kPasswordProperty).toString();
        if (password.isEmpty())
          return;

        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
          setErrorReply(*reply);
          mProgress->finish();
          return;
        }

        // Handle repositories.
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray array = doc.array();
        for (int i = 0; i < array.size(); ++i) {
          QJsonObject obj = array.at(i).toObject();

          // Add username to HTTPS URL.
          QUrl httpsUrl(obj.value("clone_url").toString());
          httpsUrl.setUserName(this->username());

          QString name = obj.value("name").toString();
          QString fullName = obj.value("full_name").toString();
          Repository *repo = addRepository(name, fullName);
          repo->setUrl(Repository::Https, httpsUrl.toString());
          repo->setUrl(Repository::Ssh, obj.value("ssh_url").toString());
        }

        // Check for additional pages.
        QString link = reply->rawHeader("Link");
        if (link.isEmpty()) {
          mProgress->finish();
          return;
        }

        QMap<QString, QString> map;
        QRegularExpression re("<(.*)>; rel=\"(\\w+)\"");
        foreach (const QString &record, link.split(", ")) {
          QRegularExpressionMatch match = re.match(record);
          if (match.isValid() && match.hasMatch())
            map.insert(match.captured(2), match.captured(1));
        }

        QString next = map.value("next");
        if (next.isEmpty()) {
          mProgress->finish();
          return;
        }

        // Request next page.
        QNetworkRequest request(next);
        if (setHeaders(request, password)) {
          QNetworkReply *reply = mMgr->get(request);
          reply->setProperty(kPasswordProperty, password);
          startProgress();
        }
      });
}

Account::Kind GitHub::kind() const { return Account::GitHub; }

bool GitHub::RemoteRepository::isValid() const {
  return !host.isEmpty() && !owner.isEmpty() && !name.isEmpty();
}

QString GitHub::name() const { return QStringLiteral("GitHub"); }

QString GitHub::host() const { return QStringLiteral("github.com"); }

void GitHub::connect(const QString &password) {
  clearRepos();

  QString suffix = hasCustomUrl() ? "/api/v3" : QString();
  QNetworkRequest request(url() + suffix + "/user/repos");
  if (setHeaders(request, password)) {
    QNetworkReply *reply = mMgr->get(request);
    reply->setProperty(kPasswordProperty,
                       !password.isEmpty() ? password : this->password());
    startProgress();
  }
}

void GitHub::requestForkParents(Repository *repo) {
  QString query = QString("query {"
                          "  repository(owner:\"%1\", name:\"%2\") {"
                          "    isFork"
                          "    parent {"
                          "      isFork"
                          "      nameWithOwner"
                          "      defaultBranchRef {"
                          "        name"
                          "      }"
                          "      parent {"
                          "        isFork"
                          "        nameWithOwner"
                          "        defaultBranchRef {"
                          "          name"
                          "        }"
                          "        parent{"
                          "          isFork"
                          "          nameWithOwner"
                          "          defaultBranchRef {"
                          "            name"
                          "          }"
                          "        }"
                          "      }"
                          "    }"
                          "  }"
                          "}")
                      .arg(repo->owner(), repo->name());

  graphql(query, [this](const QJsonObject &data) {
    QMap<QString, QString> map;
    QJsonObject repository = data.value("repository").toObject();
    while (repository.value("isFork").toBool()) {
      repository = repository.value("parent").toObject();

      QString nameWithOwner = repository.value("nameWithOwner").toString();
      QString branch = repository.value("defaultBranchRef")
                           .toObject()
                           .value("name")
                           .toString();

      map.insert(nameWithOwner, branch);
    }

    emit forkParentsReady(map);
  });
}

void GitHub::createPullRequest(Repository *repo, const QString &ownerRepo,
                               const QString &title, const QString &body,
                               const QString &head, const QString &base,
                               bool canModify) {
  QJsonDocument doc;
  doc.setObject({{"title", title},
                 {"body", body},
                 {"head", QString("%1:%2").arg(repo->owner(), head)},
                 {"base", base},
                 {"maintainer_can_modify", canModify}});

  QUrl url(QString("https://api.github.com/repos/%1/pulls").arg(ownerRepo));
  rest(url, doc, [this, title](const QJsonObject &obj) {
    foreach (const QJsonValue &error, obj.value("errors").toArray())
      emit pullRequestError(title,
                            error.toObject().value("message").toString());
  });
}

void GitHub::requestComments(Repository *repo, const QString &oid) {
  QString query = QString("query {"
                          "  repository(owner: \"%1\", name: \"%2\") {"
                          "    object(oid: \"%3\") {"
                          "      ... on Commit {"
                          "        comments(first: 50) {"
                          "          nodes {"
                          "            path"
                          "            position"
                          "            publishedAt"
                          "            body"
                          "            author {"
                          "              login"
                          "            }"
                          "          }"
                          "        }"
                          "      }"
                          "    }"
                          "  }"
                          "}")
                      .arg(repo->owner(), repo->name(), oid);

  graphql(query, [this, repo, oid](const QJsonObject &data) {
    QJsonArray nodes = data.value("repository")
                           .toObject()
                           .value("object")
                           .toObject()
                           .value("comments")
                           .toObject()
                           .value("nodes")
                           .toArray();

    if (nodes.isEmpty())
      return;

    CommitComments comments;
    foreach (const QJsonValue &value, nodes) {
      QJsonObject obj = value.toObject();
      QString path = obj.value("path").toString();
      int position = obj.value("position").toInt() - 1;

      QString raw = obj.value("body").toString();
      QString body = raw.trimmed().replace("\r\n", "\n");

      QJsonObject author = obj.value("author").toObject();
      QString login = author.value("login").toString();

      QString published = obj["publishedAt"].toString();
      QDateTime date = QDateTime::fromString(published, Qt::ISODate);

      Comments &map =
          path.isEmpty() ? comments.comments : comments.files[path][position];
      map.insert(date, {body, login});
    }

    emit commentsReady(repo, oid, comments);
  });
}

void GitHub::requestCommitAvatars(const QString &owner, const QString &name,
                                  const QStringList &oids, int size,
                                  const AvatarCallback &callback) {
  if (owner.isEmpty() || name.isEmpty() || oids.isEmpty()) {
    callback(false, {});
    return;
  }

  QStringList objects;
  for (int i = 0; i < oids.size(); ++i) {
    objects.append(QString("c%1: object(oid: \"%2\") {"
                           "  ... on Commit {"
                           "    author { avatarUrl(size: %3) }"
                           "  }"
                           "}")
                       .arg(i)
                       .arg(graphqlString(oids.at(i)))
                       .arg(size));
  }

  QString query =
      QString("query {"
              "  repository(owner: \"%1\", name: \"%2\") { %3 }"
              "}")
          .arg(graphqlString(owner), graphqlString(name), objects.join(' '));
  graphqlResult(query, [oids, callback](bool success, const QJsonObject &data) {
    QMap<QString, QUrl> avatars;
    if (!success) {
      callback(false, avatars);
      return;
    }
    QJsonObject repository = data.value("repository").toObject();
    for (int i = 0; i < oids.size(); ++i) {
      QJsonObject author = repository.value(QString("c%1").arg(i))
                               .toObject()
                               .value("author")
                               .toObject();
      QUrl url(author.value("avatarUrl").toString());
      if (url.isValid() && url.scheme() == "https")
        avatars.insert(oids.at(i), url);
    }
    callback(true, avatars);
  });
}

void GitHub::requestOpenIssues(const QString &owner,
                               const QString &repository,
                               const IssuesCallback &callback) {
  if (!callback)
    return;

  if (owner.isEmpty() || repository.isEmpty() || owner != owner.trimmed() ||
      repository != repository.trimmed() || owner.contains('/') ||
      repository.contains('/')) {
    callback(false, {}, 0, tr("Invalid repository owner or name."));
    return;
  }

  QUrl endpoint;
  if (hasCustomUrl()) {
    QString base = url();
    while (base.endsWith('/'))
      base.chop(1);
    endpoint = QUrl(base + "/api/v3/search/issues");
  } else {
    endpoint = QUrl(kIssuesUrl);
  }
  if (!endpoint.isValid() ||
      (endpoint.scheme() != "http" && endpoint.scheme() != "https") ||
      endpoint.host().isEmpty()) {
    callback(false, {}, 0, tr("Invalid GitHub API URL."));
    return;
  }

  QUrlQuery query;
  query.addQueryItem(
      "q", QString("repo:%1/%2 is:issue is:open").arg(owner, repository));
  query.addQueryItem("sort", "updated");
  query.addQueryItem("order", "desc");
  query.addQueryItem("per_page", "50");
  query.addQueryItem("page", "1");
  endpoint.setQuery(query);

  QNetworkRequest request(endpoint);
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
  request.setRawHeader("User-Agent", "GitNortek");
  request.setTransferTimeout(10000);

  QString token = mAccessToken;
  if (token.isEmpty() && !username().isEmpty())
    token = password();
  if (!token.isEmpty() && endpoint.scheme() != "https") {
    callback(false, {}, 0, tr("GitHub authentication requires HTTPS."));
    return;
  }
  if (!token.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + token.toUtf8());

  QNetworkReply *reply = mMgr->get(request);
  QObject::connect(reply, &QNetworkReply::finished, this,
                   [reply, callback] {
                     QByteArray body = reply->readAll();
                     int status = reply
                                      ->attribute(
                                          QNetworkRequest::HttpStatusCodeAttribute)
                                      .toInt();

                     QJsonParseError parseError;
                     QJsonDocument doc =
                         QJsonDocument::fromJson(body, &parseError);
                     QJsonObject object = doc.object();
                     QString apiError = object.value("message").toString();

                     auto fail = [&](const QString &error) {
                       callback(false, {}, 0, error);
                       reply->deleteLater();
                     };

                     if (status == 0 &&
                         reply->error() != QNetworkReply::NoError) {
                       fail(reply->errorString());
                       return;
                     }
                     if (status < 200 || status >= 300) {
                       QString error = apiError;
                       if (error.isEmpty())
                         error = QObject::tr("GitHub returned HTTP status %1.")
                                     .arg(status);
                       fail(error);
                       return;
                     }
                     if (reply->error() != QNetworkReply::NoError) {
                       fail(reply->errorString());
                       return;
                     }
                     if (parseError.error != QJsonParseError::NoError ||
                         !doc.isObject()) {
                       fail(QObject::tr("Invalid GitHub JSON response: %1")
                                .arg(parseError.errorString()));
                       return;
                     }

                     QJsonValue totalValue = object.value("total_count");
                     QJsonValue itemsValue = object.value("items");
                     if (!totalValue.isDouble() || !itemsValue.isArray()) {
                       fail(apiError.isEmpty()
                                ? QObject::tr("Invalid GitHub issues response.")
                                : apiError);
                       return;
                     }

                     double totalNumber = totalValue.toDouble();
                     if (totalNumber < 0 || totalNumber > INT_MAX ||
                         totalNumber != static_cast<int>(totalNumber)) {
                       fail(QObject::tr("Invalid GitHub issue count."));
                       return;
                     }

                     Issues issues;
                     QJsonArray items = itemsValue.toArray();
                     int count = qMin(items.size(), 50);
                     for (int i = 0; i < count; ++i) {
                       if (!items.at(i).isObject()) {
                         fail(QObject::tr("Invalid GitHub issue entry."));
                         return;
                       }
                       QJsonObject item = items.at(i).toObject();
                       QJsonValue number = item.value("number");
                       QJsonValue title = item.value("title");
                       QJsonValue htmlUrl = item.value("html_url");
                        QJsonValue user = item.value("user");
                        QString login;
                        if (user.isObject())
                          login = user.toObject().value("login").toString();
                        QUrl issueUrl(htmlUrl.toString());
                        if (!number.isDouble() || !title.isString() ||
                            !htmlUrl.isString() ||
                            (!user.isObject() && !user.isNull()) ||
                            number.toDouble() < 0 ||
                           number.toDouble() > INT_MAX ||
                           number.toDouble() !=
                               static_cast<int>(number.toDouble()) ||
                           !issueUrl.isValid() || issueUrl.host().isEmpty() ||
                           (issueUrl.scheme() != "http" &&
                            issueUrl.scheme() != "https")) {
                         fail(QObject::tr("Invalid GitHub issue entry."));
                         return;
                        }
                        issues.append({static_cast<int>(number.toDouble()),
                                       title.toString(), login, issueUrl});
                     }

                     callback(true, issues, static_cast<int>(totalNumber), {});
                     reply->deleteLater();
                   });
}

void GitHub::authorize() {
  mState = QString();
  for (int i = 0; i < 32; i++) {
    int value = QRandomGenerator::global()->bounded('a', 'z' + 1);
    mState.append(QChar::fromLatin1(value));
  }

  QUrlQuery query;
  query.addQueryItem("client_id", GITHUB_CLIENT_ID);
  query.addQueryItem("scope", kScope);
  query.addQueryItem("state", mState);

  QUrl url(kAuthUrl);
  url.setQuery(query);

  // Open in default browser.
  QDesktopServices::openUrl(url);
}

bool GitHub::isAuthorizeSupported() {
  QByteArray id(GITHUB_CLIENT_ID);
  QByteArray secret(GITHUB_CLIENT_SECRET);
  QByteArray env = qgetenv("GITNORTEK_OAUTH");
  if (env.isEmpty())
    env = qgetenv("GITTYUP_OAUTH");
  return (!id.isEmpty() && !secret.isEmpty() && !env.isEmpty());
}

QString GitHub::defaultUrl() {
  return QStringLiteral("https://api.github.com");
}

GitHub::RemoteRepository GitHub::parseRemoteUrl(const QString &remoteUrl) {
  QString value = remoteUrl.trimmed();
  QString host;
  QString path;

  QRegularExpression scp(
      QStringLiteral("^[^@/:\\s]+@([^/:\\s]+):(.+)$"));
  QRegularExpressionMatch scpMatch = scp.match(value);
  if (!value.contains("://") && scpMatch.hasMatch()) {
    host = scpMatch.captured(1);
    path = scpMatch.captured(2);
  } else {
    QUrl url(value, QUrl::StrictMode);
    QString scheme = url.scheme().toLower();
    if (!url.isValid() ||
        (scheme != "http" && scheme != "https" && scheme != "ssh" &&
         scheme != "git") ||
        url.host().isEmpty() || !url.query().isEmpty() ||
        !url.fragment().isEmpty()) {
      return {};
    }
    host = url.host();
    path = url.path();
  }

  while (path.endsWith('/'))
    path.chop(1);
  if (path.startsWith('/'))
    path.remove(0, 1);
  if (path.endsWith(".git", Qt::CaseInsensitive))
    path.chop(4);

  QStringList parts = path.split('/', Qt::KeepEmptyParts);
  if (parts.size() != 2)
    return {};

  auto malformed = [](const QString &part) {
    return part.isEmpty() || part == "." || part == ".." ||
           part.contains(QRegularExpression("\\s"));
  };
  if (malformed(parts.at(0)) || malformed(parts.at(1)))
    return {};

  return {host.toLower(), parts.at(0), parts.at(1)};
}

void GitHub::graphql(const QString &query, const Callback &callback) {
  graphqlResult(query,
                [callback](bool, const QJsonObject &data) { callback(data); });
}

void GitHub::graphqlResult(const QString &query,
                           const ResultCallback &callback) {
  QString token = !mAccessToken.isEmpty() ? mAccessToken : password();
  if (token.isEmpty()) {
    callback(false, {});
    return;
  }

  QJsonDocument doc;
  doc.setObject({{"query", query}});

  QUrl endpoint =
      hasCustomUrl() ? QUrl(url() + "/api/graphql") : QUrl(kGraphQlUrl);
  QNetworkRequest request(endpoint);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setTransferTimeout(10000);
  request.setRawHeader("Authorization",
                       QString("bearer %1").arg(token).toUtf8());

  QNetworkReply *reply = mMgr->post(request, doc.toJson());
  QObject::connect(reply, &QNetworkReply::finished, [reply, callback] {
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject object = doc.object();
    bool success = reply->error() == QNetworkReply::NoError &&
                   !object.contains("errors") && object.contains("data");
    callback(success, object.value("data").toObject());
    reply->deleteLater();
  });
}

void GitHub::rest(const QUrl &url, const QJsonDocument &doc,
                  const Callback &callback) {
  if (mAccessToken.isEmpty())
    return;

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("Accept", "application/vnd.github.v3+json");
  request.setRawHeader("Authorization",
                       QString("token %1").arg(mAccessToken).toUtf8());

  QNetworkReply *reply;
  if (doc.isEmpty()) {
    reply = mMgr->get(request);
  } else {
    reply = mMgr->post(request, doc.toJson());
  }

  QObject::connect(reply, &QNetworkReply::finished, [reply, callback] {
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    callback(doc.object());
    reply->deleteLater();
  });
}

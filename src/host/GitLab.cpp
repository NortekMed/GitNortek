//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "GitLab.h"
#include "Repository.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace {

const QString kContentType = "application/json";
const QString kProjectsFmt = "/projects?membership=true&private_token=%1";

} // namespace

GitLab::GitLab(const QString &username) : Account(username) {
  QObject::connect(
      mMgr, &QNetworkAccessManager::finished, this,
      [this](QNetworkReply *reply) {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
          setErrorReply(*reply);
          mProgress->finish();
          return;
        }

        // Read repositories.
        QJsonArray array = QJsonDocument::fromJson(reply->readAll()).array();
        for (int i = 0; i < array.size(); ++i) {
          QJsonObject obj = array.at(i).toObject();
          QString name = obj.value("path").toString();
          QString fullName = obj.value("path_with_namespace").toString();
          QString httpUrl = obj.value("http_url_to_repo").toString();
          QString sshUrl = obj.value("ssh_url_to_repo").toString();

          Repository *repo = addRepository(name, fullName);
          repo->setUrl(Repository::Https, httpUrl);
          repo->setUrl(Repository::Ssh, sshUrl);
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
        request.setHeader(QNetworkRequest::ContentTypeHeader, kContentType);
        mMgr->get(request);
        startProgress();
      });
}

Account::Kind GitLab::kind() const { return Account::GitLab; }

QString GitLab::name() const { return QStringLiteral("GitLab"); }

QString GitLab::host() const { return QStringLiteral("gitlab.com"); }

void GitLab::connect(const QString &defaultPassword) {
  clearRepos();

  QString token = defaultPassword;
  if (token.isEmpty())
    token = password();

  if (token.isEmpty()) {
    mError->setText(tr("Authentication failed"));
    return;
  }

  QNetworkRequest request(url() + kProjectsFmt.arg(token));
  request.setHeader(QNetworkRequest::ContentTypeHeader, kContentType);
  mMgr->get(request);
  startProgress();
}

void GitLab::requestRepositoryNamespaces(
    const RepositoryNamespacesCallback &callback) {
  RepositoryNamespaces namespaces{
      {QString(), username(), username(), true}};
  const QString token = !mAccessToken.isEmpty() ? mAccessToken : password();
  if (token.isEmpty()) {
    callback(false, namespaces,
             tr("GitLab authentication is required to list namespaces."));
    return;
  }

  QUrl endpoint(url() + QStringLiteral("/namespaces"));
  QUrlQuery query;
  query.addQueryItem("per_page", "100");
  endpoint.setQuery(query);
  if (endpoint.scheme() != "https") {
    callback(false, namespaces, tr("GitLab authentication requires HTTPS."));
    return;
  }

  QNetworkRequest request(endpoint);
  request.setRawHeader("PRIVATE-TOKEN", token.toUtf8());
  request.setHeader(QNetworkRequest::ContentTypeHeader, kContentType);
  sendJsonRequest(request, "GET", {},
                  [callback, namespaces](bool success,
                                         const QJsonDocument &document,
                                         const QString &error) mutable {
                    if (!success || !document.isArray()) {
                      callback(false, namespaces,
                               error.isEmpty()
                                   ? tr("GitLab returned an invalid namespace list.")
                                   : error);
                      return;
                    }

                    for (const QJsonValue &value : document.array()) {
                      const QJsonObject object = value.toObject();
                      const QString kind = object.value("kind").toString();
                      const QString fullPath =
                          object.value("full_path").toString();
                      if (kind == "user") {
                        namespaces[0].id = object.value("id").toVariant().toString();
                        if (!fullPath.isEmpty()) {
                          namespaces[0].name = fullPath;
                          namespaces[0].fullName = fullPath;
                        }
                        continue;
                      }
                      if (kind != "group" || fullPath.isEmpty())
                        continue;
                      namespaces.append(
                          {object.value("id").toVariant().toString(),
                           object.value("name").toString(), fullPath, false});
                    }

                    callback(true, namespaces, QString());
                  });
}

void GitLab::createRepository(const QString &namespaceId, const QString &name,
                              const QString &description, bool isPublic,
                              const CreateRepositoryCallback &callback) {
  const QString token = !mAccessToken.isEmpty() ? mAccessToken : password();
  if (token.isEmpty()) {
    callback(false, nullptr,
             tr("GitLab authentication is required to create a project."));
    return;
  }

  const QUrl endpoint(url() + QStringLiteral("/projects"));
  if (endpoint.scheme() != "https") {
    callback(false, nullptr, tr("GitLab authentication requires HTTPS."));
    return;
  }

  QJsonObject object{{"name", name},
                     {"description", description},
                     {"visibility", isPublic ? "public" : "private"},
                     {"initialize_with_readme", false}};
  if (!namespaceId.isEmpty())
    object.insert("namespace_id", namespaceId.toInt());
  QJsonDocument document(object);

  QNetworkRequest request(endpoint);
  request.setRawHeader("PRIVATE-TOKEN", token.toUtf8());
  request.setHeader(QNetworkRequest::ContentTypeHeader, kContentType);
  sendJsonRequest(request, "POST", document,
                  [this, callback, name](bool success,
                                         const QJsonDocument &response,
                                         const QString &error) {
                    const QJsonObject result = response.object();
                    const QString fullName =
                        result.value("path_with_namespace").toString();
                    const QString httpsUrl =
                        result.value("http_url_to_repo").toString();
                    const QString sshUrl =
                        result.value("ssh_url_to_repo").toString();
                    if (!success || fullName.isEmpty() || httpsUrl.isEmpty() ||
                        sshUrl.isEmpty()) {
                      callback(false, nullptr,
                               error.isEmpty()
                                   ? tr("GitLab did not return clone URLs for the created project.")
                                   : error);
                      return;
                    }

                    Repository *repository = addRepository(
                        result.value("path").toString().isEmpty()
                            ? name
                            : result.value("path").toString(),
                        fullName);
                    repository->setUrl(Repository::Https, httpsUrl);
                    repository->setUrl(Repository::Ssh, sshUrl);
                    callback(true, repository, QString());
                  });
}

QString GitLab::defaultUrl() {
  return QStringLiteral("https://gitlab.com/api/v4");
}

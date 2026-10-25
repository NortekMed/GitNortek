//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Bitbucket.h"
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

const QString kSshFmt = "git@bitbucket.org:%1";
const QString kContentType = "application/json";
const char *kPasswordProperty = "password";

QString encodedPathComponent(const QString &value) {
  return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString repositoryCloneUrl(const QJsonObject &object, const QString &protocol) {
  const QJsonArray cloneUrls = object.value("links").toObject()
                                   .value("clone")
                                   .toArray();
  for (const QJsonValue &value : cloneUrls) {
    const QJsonObject clone = value.toObject();
    if (clone.value("name").toString().compare(protocol,
                                                Qt::CaseInsensitive) == 0)
      return clone.value("href").toString();
  }
  return QString();
}

} // namespace

Bitbucket::Bitbucket(const QString &username) : Account(username) {
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

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject jsonObject = doc.object();
        QJsonArray array = jsonObject["values"].toArray();
        for (int i = 0; i < array.size(); ++i) {
          QJsonObject obj = array.at(i).toObject();
          QJsonObject repository = obj.value("repository").toObject();

          QString name = repository.value("name").toString();
          QString fullName = repository.value("full_name").toString();

          QUrl httpsUrl;
          httpsUrl.setHost(host());
          httpsUrl.setScheme("https");
          httpsUrl.setUserName(this->username());
          httpsUrl.setPath(QString("/%1").arg(fullName));

          Repository *repo = addRepository(name, fullName);
          repo->setUrl(Repository::Https, httpsUrl.toString());
          repo->setUrl(Repository::Ssh, kSshFmt.arg(fullName));
        }

        QString next = jsonObject["next"].toString();
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

Account::Kind Bitbucket::kind() const { return Account::Bitbucket; }

QString Bitbucket::name() const { return QStringLiteral("Bitbucket"); }

QString Bitbucket::host() const { return QStringLiteral("bitbucket.org"); }

void Bitbucket::connect(const QString &password) {
  clearRepos();

  QNetworkRequest request(
      url() +
      "/user/permissions/repositories?sort=repository.name&pagelen=100");
  request.setHeader(QNetworkRequest::ContentTypeHeader, kContentType);

  if (setHeaders(request, password)) {
    QNetworkReply *reply = mMgr->get(request);
    reply->setProperty(kPasswordProperty,
                       !password.isEmpty() ? password : this->password());
    startProgress();
  }
}

void Bitbucket::requestRepositoryNamespaces(
    const RepositoryNamespacesCallback &callback) {
  const QString password = this->password();
  if (password.isEmpty()) {
    callback(false, {}, tr("Bitbucket authentication is required to list workspaces."));
    return;
  }

  QUrl endpoint(url() + QStringLiteral("/user/workspaces"));
  QUrlQuery query;
  query.addQueryItem("pagelen", "100");
  endpoint.setQuery(query);
  if (endpoint.scheme() != "https") {
    callback(false, {}, tr("Bitbucket authentication requires HTTPS."));
    return;
  }

  QNetworkRequest request(endpoint);
  if (!setHeaders(request, password)) {
    callback(false, {}, error()->text());
    return;
  }
  sendJsonRequest(request, "GET", {},
                  [callback](bool success, const QJsonDocument &document,
                             const QString &error) {
                    RepositoryNamespaces namespaces;
                    const QJsonArray values =
                        document.object().value("values").toArray();
                    if (!success || !document.isObject()) {
                      callback(false, namespaces,
                               error.isEmpty()
                                   ? tr("Bitbucket returned an invalid workspace list.")
                                   : error);
                      return;
                    }

                    for (const QJsonValue &value : values) {
                      const QJsonObject workspace =
                          value.toObject().value("workspace").toObject();
                      const QString slug =
                          workspace.value("slug").toString();
                      if (slug.isEmpty())
                        continue;
                      QString name = workspace.value("name").toString();
                      if (name.isEmpty())
                        name = slug;
                      namespaces.append({slug, name, slug, false});
                    }
                    callback(true, namespaces, QString());
                  });
}

void Bitbucket::createRepository(
    const QString &namespaceId, const QString &name,
    const QString &description, bool isPublic,
    const CreateRepositoryCallback &callback) {
  if (namespaceId.isEmpty()) {
    callback(false, nullptr, tr("Select a Bitbucket workspace."));
    return;
  }
  const QString password = this->password();
  if (password.isEmpty()) {
    callback(false, nullptr,
             tr("Bitbucket authentication is required to create a repository."));
    return;
  }

  QString slug = name.trimmed().toLower();
  slug.replace(QRegularExpression(QStringLiteral("\\s+")), "_");
  slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]")), "_");
  const QUrl endpoint(
      url() + QStringLiteral("/repositories/%1/%2")
                  .arg(encodedPathComponent(namespaceId),
                       encodedPathComponent(slug)));
  if (endpoint.scheme() != "https") {
    callback(false, nullptr, tr("Bitbucket authentication requires HTTPS."));
    return;
  }

  QJsonDocument document;
  document.setObject({{"scm", "git"},
                      {"name", name},
                      {"description", description},
                      {"is_private", !isPublic}});
  QNetworkRequest request(endpoint);
  if (!setHeaders(request, password)) {
    callback(false, nullptr, error()->text());
    return;
  }
  sendJsonRequest(request, "POST", document,
                  [this, callback, name, namespaceId](
                      bool success, const QJsonDocument &response,
                      const QString &error) {
                    const QJsonObject object = response.object();
                    QString fullName = object.value("full_name").toString();
                    if (fullName.isEmpty())
                      fullName = QString("%1/%2").arg(namespaceId, name);
                    const QString sshUrl = repositoryCloneUrl(object, "ssh");
                    const QString httpsUrl = repositoryCloneUrl(object, "https");
                    if (!success || sshUrl.isEmpty() || httpsUrl.isEmpty()) {
                      callback(false, nullptr,
                               error.isEmpty()
                                   ? tr("Bitbucket did not return clone URLs for the created repository.")
                                   : error);
                      return;
                    }

                    Repository *repository = addRepository(
                        object.value("name").toString().isEmpty()
                            ? name
                            : object.value("name").toString(),
                        fullName);
                    repository->setUrl(Repository::Https, httpsUrl);
                    repository->setUrl(Repository::Ssh, sshUrl);
                    callback(true, repository, QString());
                  });
}

QString Bitbucket::defaultUrl() {
  return QStringLiteral("https://api.bitbucket.org/2.0");
}

//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Beanstalk.h"
#include "Repository.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

Beanstalk::Beanstalk(const QString &username) : Account(username) {
  QObject::connect(
      mMgr, &QNetworkAccessManager::finished, this,
      [this](QNetworkReply *reply) {
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
          QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
          QJsonArray array = doc.array();
          for (int i = 0; i < array.size(); ++i) {
            QJsonObject obj = array.at(i).toObject();
            obj = obj.value("repository").toObject();
            if (obj.value("vcs").toString() != "git")
              continue;

            QString name = obj.value("name").toString();
            QString httpsUrl = obj.value("repository_url_https").toString();
            QString sshUrl = obj.value("repository_url").toString();

            Repository *repo = addRepository(name, name);
            repo->setUrl(Repository::Https, httpsUrl);
            repo->setUrl(Repository::Ssh, sshUrl);
          }
        } else {
          setErrorReply(*reply);
        }

        mProgress->finish();
      });
}

Account::Kind Beanstalk::kind() const { return Account::Beanstalk; }

QString Beanstalk::name() const { return QStringLiteral("Beanstalk"); }

QString Beanstalk::host() const {
  return QStringLiteral("%1.git.beanstalkapp.com").arg(username());
}

void Beanstalk::connect(const QString &password) {
  clearRepos();

  QNetworkRequest request(url() + "/api/repositories.json");
  if (setHeaders(request, password)) {
    mMgr->get(request);
    startProgress();
  }
}

void Beanstalk::requestRepositoryNamespaces(
    const RepositoryNamespacesCallback &callback) {
  if (callback)
    callback(true, {{QString(), username(), username(), true}}, QString());
}

void Beanstalk::createRepository(const QString &, const QString &name,
                                 const QString &description, bool,
                                 const CreateRepositoryCallback &callback) {
  const QString password = this->password();
  if (password.isEmpty()) {
    callback(false, nullptr,
             tr("Beanstalk authentication is required to create a repository."));
    return;
  }

  const QUrl endpoint(url() + QStringLiteral("/api/repositories.json"));
  if (endpoint.scheme() != "https") {
    callback(false, nullptr, tr("Beanstalk authentication requires HTTPS."));
    return;
  }

  QJsonDocument document;
  document.setObject(
      {{"repository", QJsonObject{{"type_id", "git"},
                                  {"name", name},
                                  {"title", name},
                                  {"description", description}}}});
  QNetworkRequest request(endpoint);
  if (!setHeaders(request, password)) {
    callback(false, nullptr, error()->text());
    return;
  }
  sendJsonRequest(request, "POST", document,
                  [this, callback, name](bool success,
                                         const QJsonDocument &response,
                                         const QString &error) {
                    const QJsonObject object =
                        response.object().value("repository").toObject();
                    const QString repositoryName =
                        object.value("name").toString().isEmpty()
                            ? name
                            : object.value("name").toString();
                    const QString sshUrl =
                        object.value("repository_url").toString();
                    QString httpsUrl =
                        object.value("repository_url_https").toString();
                    if (httpsUrl.isEmpty()) {
                      httpsUrl = QString("https://%1.git.beanstalkapp.com/%2.git")
                                     .arg(username(), repositoryName);
                    }
                    if (!success || sshUrl.isEmpty()) {
                      callback(false, nullptr,
                               error.isEmpty()
                                   ? tr("Beanstalk did not return a clone URL for the created repository.")
                                   : error);
                      return;
                    }

                    Repository *repository =
                        addRepository(repositoryName, repositoryName);
                    repository->setUrl(Repository::Https, httpsUrl);
                    repository->setUrl(Repository::Ssh, sshUrl);
                    callback(true, repository, QString());
                  });
}

bool Beanstalk::supportsPublicRepositories() const { return false; }

QString Beanstalk::defaultUrl() {
  return QStringLiteral("https://<username>.beanstalkapp.com");
}

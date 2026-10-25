#include "Test.h"
#include "git/Remote.h"
#include "host/Beanstalk.h"
#include "host/Bitbucket.h"
#include "host/GitHub.h"
#include "host/Gitea.h"
#include "host/GitLab.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {

template <typename Host> class MockHost : public Host {
public:
  using Host::Host;

  QString password() const override { return QStringLiteral("test-token"); }

  QNetworkRequest request;
  QByteArray method;
  QJsonDocument sentDocument;
  QJsonDocument responseDocument;
  QString responseError;
  bool responseSuccess = true;

protected:
  void sendJsonRequest(const QNetworkRequest &request,
                       const QByteArray &method,
                       const QJsonDocument &document,
                       const Account::JsonRequestCallback &callback) override {
    this->request = request;
    this->method = method;
    sentDocument = document;
    callback(responseSuccess, responseDocument, responseError);
  }
};

} // namespace

class TestHostRepositoryCreation : public QObject {
  Q_OBJECT

private slots:
  void github();
  void gitea();
  void gitlab();
  void bitbucket();
  void beanstalk();
};

void TestHostRepositoryCreation::github() {
  MockHost<GitHub> account("alice");
  account.responseDocument = QJsonDocument(QJsonArray{
      QJsonObject{{"login", "acme"}, {"name", "Acme"}}});
  Account::RepositoryNamespaces namespaces;
  bool namespacesLoaded = false;
  account.requestRepositoryNamespaces(
      [&](bool success, const Account::RepositoryNamespaces &result,
          const QString &error) {
        QVERIFY2(success, qPrintable(error));
        namespaces = result;
        namespacesLoaded = true;
      });
  QVERIFY(namespacesLoaded);
  QCOMPARE(account.method, QByteArray("GET"));
  QCOMPARE(account.request.url().path(), QStringLiteral("/user/orgs"));
  QCOMPARE(namespaces.size(), 2);
  QVERIFY(namespaces.first().personal);
  QCOMPARE(namespaces.last().id, QStringLiteral("acme"));

  account.responseDocument = QJsonDocument(QJsonObject{
      {"name", "widget"},
      {"full_name", "acme/widget"},
      {"clone_url", "https://github.com/acme/widget.git"},
      {"ssh_url", "git@github.com:acme/widget.git"}});
  Repository *created = nullptr;
  account.createRepository(
      "acme", "widget", "Description", false,
      [&](bool success, Repository *repository, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        created = repository;
      });
  QVERIFY(created);
  QCOMPARE(account.method, QByteArray("POST"));
  QCOMPARE(account.request.url().path(), QStringLiteral("/orgs/acme/repos"));
  QCOMPARE(account.sentDocument.object().value("private").toBool(), true);
  QCOMPARE(account.sentDocument.object().value("auto_init").toBool(), false);
  QCOMPARE(created->url(Repository::Https),
           QStringLiteral("https://github.com/acme/widget.git"));
  QCOMPARE(created->url(Repository::Ssh),
           QStringLiteral("git@github.com:acme/widget.git"));
}

void TestHostRepositoryCreation::gitea() {
  MockHost<Gitea> account("alice");
  account.setUrl("https://gitea.example");
  account.responseDocument = QJsonDocument(QJsonArray{
      QJsonObject{{"username", "acme"}, {"full_name", "Acme"}}});
  Account::RepositoryNamespaces namespaces;
  account.requestRepositoryNamespaces(
      [&](bool success, const Account::RepositoryNamespaces &result,
          const QString &error) {
        QVERIFY2(success, qPrintable(error));
        namespaces = result;
      });
  QCOMPARE(account.request.url().path(), QStringLiteral("/api/v1/user/orgs"));
  QCOMPARE(namespaces.size(), 2);
  QCOMPARE(namespaces.last().id, QStringLiteral("acme"));

  account.responseDocument = QJsonDocument(QJsonObject{
      {"name", "widget"},
      {"full_name", "acme/widget"},
      {"clone_url", "https://gitea.example/acme/widget.git"},
      {"ssh_url", "git@gitea.example:acme/widget.git"}});
  Repository *created = nullptr;
  account.createRepository(
      "acme", "widget", "Description", true,
      [&](bool success, Repository *repository, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        created = repository;
      });
  QVERIFY(created);
  QCOMPARE(account.request.url().path(),
           QStringLiteral("/api/v1/org/acme/repos"));
  QCOMPARE(account.sentDocument.object().value("private").toBool(), false);
  QCOMPARE(account.sentDocument.object().value("auto_init").toBool(), false);
  QCOMPARE(created->url(Repository::Https),
           QStringLiteral("https://gitea.example/acme/widget.git"));
}

void TestHostRepositoryCreation::gitlab() {
  MockHost<GitLab> account("alice");
  account.setUrl("https://gitlab.example/api/v4");
  account.responseDocument = QJsonDocument(QJsonArray{
      QJsonObject{{"id", 1}, {"kind", "user"}, {"full_path", "alice"}},
      QJsonObject{{"id", 42},
                  {"kind", "group"},
                  {"name", "Acme"},
                  {"full_path", "acme"}}});
  Account::RepositoryNamespaces namespaces;
  account.requestRepositoryNamespaces(
      [&](bool success, const Account::RepositoryNamespaces &result,
          const QString &error) {
        QVERIFY2(success, qPrintable(error));
        namespaces = result;
      });
  QCOMPARE(namespaces.size(), 2);
  QCOMPARE(namespaces.last().id, QStringLiteral("42"));
  QUrlQuery query(account.request.url());
  QCOMPARE(query.queryItemValue("per_page"), QStringLiteral("100"));

  account.responseDocument = QJsonDocument(QJsonObject{
      {"path", "widget"},
      {"path_with_namespace", "acme/widget"},
      {"http_url_to_repo", "https://gitlab.example/acme/widget.git"},
      {"ssh_url_to_repo", "git@gitlab.example:acme/widget.git"}});
  Repository *created = nullptr;
  account.createRepository(
      "42", "widget", "Description", true,
      [&](bool success, Repository *repository, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        created = repository;
      });
  QVERIFY(created);
  QCOMPARE(account.request.url().path(), QStringLiteral("/api/v4/projects"));
  QCOMPARE(account.sentDocument.object().value("namespace_id").toInt(), 42);
  QCOMPARE(account.sentDocument.object().value("visibility").toString(),
           QStringLiteral("public"));
  QCOMPARE(account.sentDocument.object()
               .value("initialize_with_readme")
               .toBool(),
           false);
}

void TestHostRepositoryCreation::bitbucket() {
  MockHost<Bitbucket> account("alice");
  account.responseDocument = QJsonDocument(QJsonObject{
      {"values", QJsonArray{QJsonObject{
                   {"workspace", QJsonObject{{"slug", "acme"},
                                              {"name", "Acme"}}}}}}});
  Account::RepositoryNamespaces namespaces;
  account.requestRepositoryNamespaces(
      [&](bool success, const Account::RepositoryNamespaces &result,
          const QString &error) {
        QVERIFY2(success, qPrintable(error));
        namespaces = result;
      });
  QCOMPARE(account.request.url().path(),
           QStringLiteral("/2.0/user/workspaces"));
  QCOMPARE(namespaces.size(), 1);
  QCOMPARE(namespaces.first().id, QStringLiteral("acme"));

  const QJsonArray cloneLinks{
      QJsonObject{{"name", "https"},
                  {"href", "https://bitbucket.org/acme/widget.git"}},
      QJsonObject{{"name", "ssh"},
                  {"href", "git@bitbucket.org:acme/widget.git"}}};
  account.responseDocument = QJsonDocument(QJsonObject{
      {"name", "widget"},
      {"full_name", "acme/widget"},
      {"links", QJsonObject{{"clone", cloneLinks}}}});
  Repository *created = nullptr;
  account.createRepository(
      "acme", "widget", "Description", false,
      [&](bool success, Repository *repository, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        created = repository;
      });
  QVERIFY(created);
  QCOMPARE(account.request.url().path(),
           QStringLiteral("/2.0/repositories/acme/widget"));
  QCOMPARE(account.sentDocument.object().value("is_private").toBool(), true);
  QCOMPARE(account.sentDocument.object().value("scm").toString(),
           QStringLiteral("git"));
  QCOMPARE(created->url(Repository::Https),
           QStringLiteral("https://bitbucket.org/acme/widget.git"));
  QCOMPARE(created->url(Repository::Ssh),
           QStringLiteral("git@bitbucket.org:acme/widget.git"));
}

void TestHostRepositoryCreation::beanstalk() {
  MockHost<Beanstalk> account("alice");
  Account::RepositoryNamespaces namespaces;
  account.requestRepositoryNamespaces(
      [&](bool success, const Account::RepositoryNamespaces &result,
          const QString &error) {
        QVERIFY2(success, qPrintable(error));
        namespaces = result;
      });
  QCOMPARE(namespaces.size(), 1);
  QVERIFY(namespaces.first().personal);
  QVERIFY(!account.supportsPublicRepositories());

  account.responseDocument = QJsonDocument(QJsonObject{
      {"repository", QJsonObject{
                          {"name", "widget"},
                          {"repository_url",
                           "git@alice.beanstalkapp.com:/widget.git"},
                          {"repository_url_https",
                           "https://alice.git.beanstalkapp.com/widget.git"}}}});
  Repository *created = nullptr;
  account.createRepository(
      QString(), "widget", "Description", false,
      [&](bool success, Repository *repository, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        created = repository;
      });
  QVERIFY(created);
  QCOMPARE(account.request.url().path(),
           QStringLiteral("/api/repositories.json"));
  const QJsonObject requestRepository =
      account.sentDocument.object().value("repository").toObject();
  QCOMPARE(requestRepository.value("type_id").toString(),
           QStringLiteral("git"));
  QCOMPARE(requestRepository.value("description").toString(),
           QStringLiteral("Description"));
  QCOMPARE(created->url(Repository::Https),
           QStringLiteral("https://alice.git.beanstalkapp.com/widget.git"));
}

int main(int argc, char *argv[]) {
  QTEST_SET_MAIN_SOURCE_PATH
  return Test::runTest<TestHostRepositoryCreation>(argc, argv);
}

#include "HostRepositoryCreation.moc"

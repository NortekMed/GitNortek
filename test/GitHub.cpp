//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "host/GitHub.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

namespace {

class TestGitHub : public GitHub {
public:
  using GitHub::GitHub;

  void setAccessToken(const QString &token) { mAccessToken = token; }
};

class MockGitHub : public TestGitHub {
public:
  using TestGitHub::TestGitHub;

  QUrl endpoint;
  QByteArray method;
  QJsonDocument document;
  bool authentication = false;
  bool responseSuccess = true;
  QJsonObject responseObject;
  QString responseError;

protected:
  void jsonRequest(const QUrl &requestEndpoint, const QByteArray &requestMethod,
                   const QJsonDocument &requestDocument,
                   bool requestAuthentication,
                   const JsonRequestCallback &callback) override {
    endpoint = requestEndpoint;
    method = requestMethod;
    document = requestDocument;
    authentication = requestAuthentication;
    callback(responseSuccess, responseObject, responseError);
  }
};

class HttpServer : public QTcpServer {
public:
  HttpServer(int status, const QByteArray &body)
      : mStatus(status), mBody(body) {
    connect(this, &QTcpServer::newConnection, this, [this] {
      QTcpSocket *socket = nextPendingConnection();
      connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        mRequest += socket->readAll();
        if (!mRequest.contains("\r\n\r\n"))
          return;

        QByteArray reason = mStatus >= 200 && mStatus < 300 ? "OK" : "Error";
        QByteArray response =
            "HTTP/1.1 " + QByteArray::number(mStatus) + " " + reason +
            "\r\nContent-Type: application/json\r\n" +
            "Content-Length: " + QByteArray::number(mBody.size()) +
            "\r\nConnection: close\r\n\r\n" + mBody;
        socket->write(response);
        socket->disconnectFromHost();
      });
    });
    bool listening = listen(QHostAddress::LocalHost);
    Q_ASSERT(listening);
    Q_UNUSED(listening);
  }

  QString url() const {
    return QString("http://127.0.0.1:%1").arg(serverPort());
  }
  QByteArray request() const { return mRequest; }

private:
  int mStatus;
  QByteArray mBody;
  QByteArray mRequest;
};

QByteArray response(const QJsonArray &items = {}, int total = 0) {
  return QJsonDocument(QJsonObject{{"total_count", total}, {"items", items}})
      .toJson(QJsonDocument::Compact);
}

QJsonObject issue(int number) {
  return {{"number", number},
          {"title", QString("Issue %1").arg(number)},
          {"html_url",
           QString("https://github.com/acme/repo/issues/%1").arg(number)},
          {"user", QJsonObject{{"login", QString("user%1").arg(number)}}}};
}

} // namespace

class TestGitHubIssues : public QObject {
  Q_OBJECT

private slots:
  void parseRemoteUrl();
  void anonymousRequest();
  void rejectsBearerTokenOverHttp();
  void parsesAndCapsResults();
  void acceptsDeletedAuthor();
  void malformedJson();
  void httpError();
  void membershipResponse();
  void createIssueRequestAndResponse();
  void validatesAuthenticatedRequests();
  void rejectsUnsafeAuthentication();
};

void TestGitHubIssues::parseRemoteUrl() {
  auto verify = [](const QString &url, const QString &host) {
    GitHub::RemoteRepository repo = GitHub::parseRemoteUrl(url);
    QCOMPARE(repo.host, host);
    QCOMPARE(repo.owner, QString("Owner"));
    QCOMPARE(repo.name, QString("Repo"));
    QVERIFY(repo.isValid());
  };
  verify("https://GitHub.COM/Owner/Repo.git/", "github.com");
  verify("http://example.com/Owner/Repo", "example.com");
  verify("ssh://git@Example.COM/Owner/Repo.git", "example.com");
  verify("git://Example.COM/Owner/Repo/", "example.com");
  verify("git@Example.COM:Owner/Repo.git", "example.com");

  const QStringList invalid = {"/tmp/Owner/Repo",
                               "Owner/Repo",
                               "file:///Owner/Repo",
                               "https://example.com/x",
                               "https://example.com/a/b/c",
                               "https://example.com/a//b",
                               "https://example.com//Owner/Repo",
                               "git@example.com:Owner"};
  for (const QString &url : invalid)
    QVERIFY2(!GitHub::parseRemoteUrl(url).isValid(), qPrintable(url));
}

void TestGitHubIssues::anonymousRequest() {
  HttpServer server(200, response());
  TestGitHub github("");
  github.setUrl(server.url());
  int calls = 0;
  github.requestOpenIssues(
      "acme", "repo",
      [&](bool success, const GitHub::Issues &, int, const QString &) {
        QVERIFY(success);
        ++calls;
      });
  QTRY_COMPARE(calls, 1);

  QList<QByteArray> lines = server.request().split('\n');
  QVERIFY(lines.first().startsWith("GET /api/v3/search/issues?"));
  QUrl requestUrl(QString::fromLatin1(lines.first().split(' ').at(1)));
  QUrlQuery query(requestUrl);
  QCOMPARE(query.queryItemValue("q"), "repo:acme/repo is:issue is:open");
  QCOMPARE(query.queryItemValue("sort"), "updated");
  QCOMPARE(query.queryItemValue("order"), "desc");
  QCOMPARE(query.queryItemValue("per_page"), "50");
  QCOMPARE(query.queryItemValue("page"), "1");
  QVERIFY(!server.request().contains("Authorization:"));
  QByteArray lowerRequest = server.request().toLower();
  QVERIFY(lowerRequest.contains("accept: application/vnd.github+json"));
  QVERIFY(lowerRequest.contains("x-github-api-version: 2022-11-28"));
  QVERIFY(lowerRequest.contains("user-agent: gitnortek"));
}

void TestGitHubIssues::rejectsBearerTokenOverHttp() {
  HttpServer server(200, response());
  TestGitHub github("");
  github.setUrl(server.url());
  github.setAccessToken("secret-token");
  int calls = 0;
  QString error;
  github.requestOpenIssues(
      "acme", "repo",
      [&](bool success, const GitHub::Issues &, int, const QString &text) {
        QVERIFY(!success);
        error = text;
        ++calls;
      });
  QTRY_COMPARE(calls, 1);
  QVERIFY(error.contains("HTTPS"));
  QVERIFY(server.request().isEmpty());
}

void TestGitHubIssues::parsesAndCapsResults() {
  QJsonArray items;
  for (int i = 60; i >= 1; --i)
    items.append(issue(i));
  HttpServer server(200, response(items, 123));
  TestGitHub github("");
  github.setUrl(server.url());

  int calls = 0;
  GitHub::Issues result;
  int total = 0;
  github.requestOpenIssues("acme", "repo",
                           [&](bool success, const GitHub::Issues &issues,
                               int totalCount, const QString &error) {
                             QVERIFY2(success, qPrintable(error));
                             result = issues;
                             total = totalCount;
                             ++calls;
                           });
  QTRY_COMPARE(calls, 1);
  QCOMPARE(total, 123);
  QCOMPARE(result.size(), 50);
  QCOMPARE(result.first().number, 60);
  QCOMPARE(result.first().title, "Issue 60");
  QCOMPARE(result.first().author, "user60");
  QCOMPARE(result.first().url, QUrl("https://github.com/acme/repo/issues/60"));
  QCOMPARE(result.last().number, 11);
}

void TestGitHubIssues::acceptsDeletedAuthor() {
  QJsonObject item = issue(7);
  item.insert("user", QJsonValue::Null);
  HttpServer server(200, response({item}, 1));
  TestGitHub github("");
  github.setUrl(server.url());

  int calls = 0;
  GitHub::Issues result;
  github.requestOpenIssues("acme", "repo",
                           [&](bool success, const GitHub::Issues &issues, int,
                               const QString &error) {
                             QVERIFY2(success, qPrintable(error));
                             result = issues;
                             ++calls;
                           });
  QTRY_COMPARE(calls, 1);
  QCOMPARE(result.size(), 1);
  QVERIFY(result.constFirst().author.isEmpty());
}

void TestGitHubIssues::malformedJson() {
  HttpServer server(200, "{not json");
  TestGitHub github("");
  github.setUrl(server.url());
  int calls = 0;
  QString error;
  github.requestOpenIssues("acme", "repo",
                           [&](bool success, const GitHub::Issues &issues, int,
                               const QString &text) {
                             QVERIFY(!success);
                             QVERIFY(issues.isEmpty());
                             error = text;
                             ++calls;
                           });
  QTRY_COMPARE(calls, 1);
  QVERIFY(error.contains("JSON", Qt::CaseInsensitive));
}

void TestGitHubIssues::httpError() {
  HttpServer server(422,
                    QJsonDocument(QJsonObject{{"message", "Validation Failed"}})
                        .toJson(QJsonDocument::Compact));
  TestGitHub github("");
  github.setUrl(server.url());
  int calls = 0;
  QString error;
  github.requestOpenIssues(
      "acme", "repo",
      [&](bool success, const GitHub::Issues &, int, const QString &text) {
        QVERIFY(!success);
        error = text;
        ++calls;
      });
  QTRY_COMPARE(calls, 1);
  QCOMPARE(error, "Validation Failed");
}

void TestGitHubIssues::membershipResponse() {
  MockGitHub github("");
  github.setUrl("https://github.example.com/");
  github.setAccessToken("secret-token");
  github.responseObject = {{"state", "active"}};

  int calls = 0;
  github.requestOrganizationMembership(
      "acme", [&](bool success, bool active, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        QVERIFY(active);
        ++calls;
      });
  QCOMPARE(calls, 1);
  QCOMPARE(
      github.endpoint,
      QUrl("https://github.example.com/api/v3/user/memberships/orgs/acme"));
  QCOMPARE(github.method, QByteArray("GET"));
  QVERIFY(github.document.isEmpty());
  QVERIFY(github.authentication);

  github.responseObject = {{"state", "pending"}};
  github.requestOrganizationMembership(
      "acme", [&](bool success, bool active, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        QVERIFY(!active);
        ++calls;
      });
  QCOMPARE(calls, 2);

  github.responseObject = {{"state", 1}};
  github.requestOrganizationMembership(
      "acme", [&](bool success, bool, const QString &error) {
        QVERIFY(!success);
        QVERIFY(error.contains("membership", Qt::CaseInsensitive));
        ++calls;
      });
  QCOMPARE(calls, 3);
}

void TestGitHubIssues::createIssueRequestAndResponse() {
  MockGitHub github("");
  github.setAccessToken("secret-token");
  github.responseObject = {
      {"number", 42}, {"html_url", "https://github.com/acme/repo/issues/42"}};

  int calls = 0;
  github.createIssue(
      "acme", "repo", "A title", "Issue body",
      [&](bool success, int number, const QUrl &url, const QString &error) {
        QVERIFY2(success, qPrintable(error));
        QCOMPARE(number, 42);
        QCOMPARE(url, QUrl("https://github.com/acme/repo/issues/42"));
        ++calls;
      });
  QCOMPARE(calls, 1);
  QCOMPARE(github.endpoint,
           QUrl("https://api.github.com/repos/acme/repo/issues"));
  QCOMPARE(github.method, QByteArray("POST"));
  QVERIFY(github.authentication);
  QCOMPARE(github.document.object().value("title").toString(), "A title");
  QCOMPARE(github.document.object().value("body").toString(), "Issue body");

  github.responseObject = {{"number", 0}, {"html_url", "not a URL"}};
  github.createIssue(
      "acme", "repo", "title", {},
      [&](bool success, int number, const QUrl &, const QString &error) {
        QVERIFY(!success);
        QCOMPARE(number, 0);
        QVERIFY(error.contains("issue", Qt::CaseInsensitive));
        ++calls;
      });
  QCOMPARE(calls, 2);
}

void TestGitHubIssues::validatesAuthenticatedRequests() {
  MockGitHub github("");
  github.setAccessToken("secret-token");
  int calls = 0;

  github.requestOrganizationMembership(
      "bad/org", [&](bool success, bool, const QString &) {
        QVERIFY(!success);
        ++calls;
      });
  github.createIssue(" acme", "repo", "title", {},
                     [&](bool success, int, const QUrl &, const QString &) {
                       QVERIFY(!success);
                       ++calls;
                     });
  github.createIssue("acme", "repo", "  ", {},
                     [&](bool success, int, const QUrl &, const QString &) {
                       QVERIFY(!success);
                       ++calls;
                     });
  QCOMPARE(calls, 3);
  QVERIFY(github.endpoint.isEmpty());
}

void TestGitHubIssues::rejectsUnsafeAuthentication() {
  TestGitHub github("");
  github.setUrl("http://127.0.0.1");
  int calls = 0;
  QString error;
  github.createIssue("acme", "repo", "title", {},
                     [&](bool success, int, const QUrl &, const QString &text) {
                       QVERIFY(!success);
                       error = text;
                       ++calls;
                      });
  QCOMPARE(calls, 1);
  QVERIFY(error.contains("authentication", Qt::CaseInsensitive));
}

TEST_MAIN(TestGitHubIssues)

#include "GitHub.moc"

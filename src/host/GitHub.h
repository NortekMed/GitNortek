//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef GITHUB_H
#define GITHUB_H

#include "Account.h"
#include <QJsonDocument>
#include <QList>
#include <QMap>
#include <QUrl>
#include <functional>

class GitHub : public Account {
  Q_OBJECT

public:
  struct RemoteRepository {
    QString host;
    QString owner;
    QString name;

    bool isValid() const;
  };

  struct Issue {
    int number = 0;
    QString title;
    QString author;
    QUrl url;
  };

  using Issues = QList<Issue>;
  using IssuesCallback = std::function<void(
      bool success, const Issues &issues, int totalCount, const QString &error)>;
  using AvatarCallback =
      std::function<void(bool success, const QMap<QString, QUrl> &avatars)>;

  GitHub(const QString &username);

  Kind kind() const override;
  QString name() const override;
  QString host() const override;
  void connect(const QString &password = QString()) override;

  void requestForkParents(Repository *repo) override;
  virtual void createPullRequest(Repository *repo, const QString &ownerRepo,
                                 const QString &title, const QString &body,
                                 const QString &head, const QString &base,
                                 bool canModify) override;

  void requestComments(Repository *repo, const QString &oid) override;
  void requestCommitAvatars(const QString &owner, const QString &name,
                            const QStringList &oids, int size,
                            const AvatarCallback &callback);
  virtual void requestOpenIssues(const QString &owner,
                                 const QString &repository,
                                 const IssuesCallback &callback);

  void authorize() override;
  bool isAuthorizeSupported() override;

  static QString defaultUrl();
  static RemoteRepository parseRemoteUrl(const QString &url);

private:
  using Callback = std::function<void(const QJsonObject &)>;
  using ResultCallback = std::function<void(bool, const QJsonObject &)>;

  void graphql(const QString &query, const Callback &callback);
  void graphqlResult(const QString &query, const ResultCallback &callback);

  void rest(const QUrl &url, const QJsonDocument &doc = QJsonDocument(),
            const Callback &callback = Callback());

  QString mState;
};

#endif

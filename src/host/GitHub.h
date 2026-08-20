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
#include <QMap>
#include <QUrl>
#include <functional>

class GitHub : public Account {
  Q_OBJECT

public:
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

  void authorize() override;
  bool isAuthorizeSupported() override;

  static QString defaultUrl();

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

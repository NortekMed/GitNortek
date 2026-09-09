//
//          Copyright (c) 2018, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef GITCREDENTIAL_H
#define GITCREDENTIAL_H

#include "CredentialHelper.h"

class GitCredential : public CredentialHelper {
public:
  bool get(const QString &url, QString &username, QString &password) override;

  bool store(const QString &url, const QString &username,
             const QString &password) override;

private:
  bool run(const QString &action, const QString &url, QString &username,
           QString *password) const;
};

#endif

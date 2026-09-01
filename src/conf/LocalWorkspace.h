//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef LOCALWORKSPACE_H
#define LOCALWORKSPACE_H

#include <QColor>
#include <QString>
#include <QStringList>

class LocalWorkspace {
public:
  LocalWorkspace();

  QString id;
  QString name;
  QString description;
  QString iconName;
  QColor color;
  QString syncDirectory;
  bool syncEnabled = false;
  QStringList repositories;
  QStringList manualRepositories;
  QStringList synchronizedRepositories;
};

#endif

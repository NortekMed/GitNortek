//
//          Copyright (c) 2022, Gittyup authors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Kas
//

#ifndef UTIL_PATH_H
#define UTIL_PATH_H

#include <QString>
#include <QStringList>

namespace util {
QString canonicalizePath(QString path);
QString pathCompareKey(const QString &path);
bool pathsEqual(const QString &lhs, const QString &rhs);
bool containsPath(const QStringList &paths, const QString &path);
}

#endif

#include "Path.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <memory>
#include <windows.h>
#endif

namespace util {
QString canonicalizePath(QString path) {
#ifdef Q_OS_WIN
  // Convert from potential 8.3 paths to full paths on Windows
  {
    auto len = GetLongPathNameW((LPCWSTR)path.utf16(), nullptr, 0);

    // GetLongPathNameW() returns 0 if the given path doesn't exist (yet)
    if (len != 0) {
      std::unique_ptr<wchar_t[]> buf{new wchar_t[len]};
      len = GetLongPathNameW((LPCWSTR)path.utf16(), buf.get(), len);
      path = QString::fromWCharArray(buf.get(), len);
    }
  }
#endif
  return path;
}

QString pathCompareKey(const QString &path) {
  QString key = path;
#ifdef Q_OS_WIN
  key = QDir::fromNativeSeparators(key).toCaseFolded();
#endif
  return key;
}

bool pathsEqual(const QString &lhs, const QString &rhs) {
  return pathCompareKey(lhs) == pathCompareKey(rhs);
}

bool containsPath(const QStringList &paths, const QString &path) {
  for (const QString &candidate : paths) {
    if (pathsEqual(candidate, path))
      return true;
  }
  return false;
}
} // namespace util

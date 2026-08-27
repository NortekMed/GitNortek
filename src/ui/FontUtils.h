//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef FONTUTILS_H
#define FONTUTILS_H

#include <QFont>
#include <QFontInfo>

namespace FontUtils {

inline QFont copySize(QFont font, const QFont &source) {
  if (source.pointSizeF() > 0)
    font.setPointSizeF(source.pointSizeF());
  else if (source.pixelSize() > 0)
    font.setPixelSize(source.pixelSize());
  return font;
}

inline int pointSize(const QFont &font) {
  return font.pointSizeF() > 0 ? qRound(font.pointSizeF())
                               : QFontInfo(font).pointSize();
}

} // namespace FontUtils

#endif

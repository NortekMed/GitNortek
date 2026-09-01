//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "LocalWorkspace.h"
#include <QUuid>

LocalWorkspace::LocalWorkspace()
    : id(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

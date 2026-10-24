//
// GitNortek is licensed under the MIT License. See LICENSE.md.
//

#ifndef GLOBALGITCONFIGCHANGE_H
#define GLOBALGITCONFIGCHANGE_H

#include <QString>
#include <functional>

class QWidget;

namespace git {
class Config;
}

// Ask before applying a change to Git's global configuration. The mutation is
// invoked only after confirmation and should return false if the write fails.
bool applyGlobalGitConfigChange(
    QWidget *parent, const QString &key, const QString &oldValue,
    const QString &newValue,
    const std::function<bool(git::Config &)> &mutation);

#endif

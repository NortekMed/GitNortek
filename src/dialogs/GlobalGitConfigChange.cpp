//
// GitNortek is licensed under the MIT License. See LICENSE.md.
//

#include "GlobalGitConfigChange.h"
#include "git/Config.h"
#include <QCoreApplication>
#include <QMessageBox>

bool applyGlobalGitConfigChange(
    QWidget *parent, const QString &key, const QString &oldValue,
    const QString &newValue,
    const std::function<bool(git::Config &)> &mutation) {
  if (oldValue == newValue)
    return true;

  QMessageBox message(
      QMessageBox::Question,
      QCoreApplication::translate("GlobalGitConfigChange",
                                  "Confirm Git setting change"),
      QCoreApplication::translate("GlobalGitConfigChange",
                                  "Change this global Git setting?"),
      QMessageBox::Yes | QMessageBox::No, parent);
  message.setTextFormat(Qt::PlainText);
  const QString previous =
      oldValue.isEmpty()
          ? QCoreApplication::translate("GlobalGitConfigChange", "(not set)")
          : oldValue;
  const QString next =
      newValue.isEmpty()
          ? QCoreApplication::translate("GlobalGitConfigChange", "(not set)")
          : newValue;
  message.setInformativeText(
      QStringLiteral("%1\n\n%2: %3 → %4")
          .arg(QCoreApplication::translate(
                   "GlobalGitConfigChange",
                   "This affects GitNortek and other Git clients. "
                   "Repository-specific settings may override it."),
               key, previous, next));
  message.setDefaultButton(QMessageBox::No);
  if (message.exec() != QMessageBox::Yes)
    return false;

  git::Config config = git::Config::global();
  if (config.isValid() && mutation(config))
    return true;

  QMessageBox::critical(
      parent,
      QCoreApplication::translate("GlobalGitConfigChange",
                                  "Unable to update Git configuration"),
      QCoreApplication::translate(
          "GlobalGitConfigChange",
          "GitNortek could not write the global Git configuration. The "
          "setting was not applied."));
  return false;
}

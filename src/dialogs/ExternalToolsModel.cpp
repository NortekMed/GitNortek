//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Shane Gramlich
//

#include "ExternalToolsModel.h"
#include "conf/Settings.h"
#include "dialogs/GlobalGitConfigChange.h"
#include "git/Config.h"
#include <QDirIterator>
#include <QSet>
#include <QWidget>

ExternalToolsModel::ExternalToolsModel(const QString &type, bool detected,
                                       QObject *parent)
    : QAbstractTableModel(parent), mType(type), mDetected(detected) {
  refresh();
}

int ExternalToolsModel::rowCount(const QModelIndex &parent) const {
  return mEntries.size();
}

int ExternalToolsModel::columnCount(const QModelIndex &parent) const {
  return 3;
}

QVariant ExternalToolsModel::data(const QModelIndex &index, int role) const {
  switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
      switch (index.column()) {
        case Name:
          return mEntries.at(index.row()).name;
        case Command: {
          return mEntries.at(index.row()).command;
        }
        case Arguments:
          return mEntries.at(index.row()).arguments;
      }
  }

  return QVariant();
}

QVariant ExternalToolsModel::headerData(int section,
                                        Qt::Orientation orientation,
                                        int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return QVariant();

  switch (section) {
    case Name:
      return tr("Name");
    case Command:
      return tr("Command");
    case Arguments:
      return tr("Arguments");
  }

  return QVariant();
}

Qt::ItemFlags ExternalToolsModel::flags(const QModelIndex &index) const {
  if (!mDetected)
    return Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsSelectable;

  if (mEntries.at(index.row()).found)
    return Qt::ItemIsEnabled;

  return Qt::ItemFlags();
}

bool ExternalToolsModel::setData(const QModelIndex &index,
                                 const QVariant &value, int role) {
  if (!index.isValid())
    return false;

  const ExternalTool::Info &tool = mEntries[index.row()];
  const QString oldName = tool.name;
  QString name = tool.name;
  QString cmd = tool.command;
  QString args = tool.arguments;

  switch (index.column()) {
    case Name:
      name = value.toString();
      break;

    case Command:
      cmd = value.toString();
      break;

    case Arguments:
      args = value.toString();
      break;
  }

  const QString oldKey = QString("%1tool.%2.cmd").arg(mType, oldName);
  const QString newKey = QString("%1tool.%2.cmd").arg(mType, name);
  const QString oldValue = git::Config::global().value<QString>(oldKey);
  const QString newValue = QString("%1 %2").arg(cmd, args);
  const QString displayKey =
      oldKey == newKey ? oldKey : QString("%1 → %2").arg(oldKey, newKey);
  const QString displayOld =
      oldKey == newKey ? oldValue
                       : QString("%1 = %2\n%3 = %4")
                             .arg(oldKey, oldValue, newKey,
                                  git::Config::global().value<QString>(newKey));
  bool applied = applyGlobalGitConfigChange(
      qobject_cast<QWidget *>(parent()), displayKey, displayOld, newValue,
      [oldKey, newKey, oldValue, newValue](git::Config &global) {
        if (oldKey != newKey && !global.remove(oldKey))
          return false;
        if (global.setValue(newKey, newValue))
          return true;
        if (oldKey != newKey)
          global.setValue(oldKey, oldValue);
        return false;
      });
  if (!applied)
    return false;

  refresh();
  return true;
}

void ExternalToolsModel::refresh() {
  beginResetModel();

  mEntries = mDetected ? ExternalTool::readBuiltInTools(mType)
                       : ExternalTool::readGlobalTools(mType);

  // Sort enabled items first.
  std::sort(mEntries.begin(), mEntries.end());

  endResetModel();
}

void ExternalToolsModel::add(const QString &path) {
  if (path.isEmpty())
    return;

  QString name = QFileInfo(path).baseName();
  QString key = QString("%1tool.%2.cmd").arg(mType, name);
  QString cmd = QString("\"%1\" \"$LOCAL\" \"$REMOTE\"").arg(path);
  QString oldValue = git::Config::global().value<QString>(key);
  if (!applyGlobalGitConfigChange(qobject_cast<QWidget *>(parent()), key,
                                  oldValue, cmd,
                                  [key, cmd](git::Config &global) {
                                    return global.setValue(key, cmd);
                                  }))
    return;
}

void ExternalToolsModel::remove(const QString &name) {
  QString regex = QString("%1tool\\.%2\\.cmd").arg(mType, name);
  git::Config config = git::Config::global();
  git::Config::Iterator it = config.glob(regex);
  while (git::Config::Entry entry = it.next()) {
    const QString key = entry.name();
    const QString oldValue = entry.value<QString>();
    if (!applyGlobalGitConfigChange(
            qobject_cast<QWidget *>(parent()), key, oldValue, QString(),
            [key](git::Config &global) { return global.remove(key); }))
      return;
  }
}

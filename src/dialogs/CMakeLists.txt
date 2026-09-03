add_library(
  dialogs
  AboutDialog.cpp
  AccountDialog.cpp
  AddRemoteDialog.cpp
  AddSubmoduleDialog.cpp
  AmendDialog.cpp
  BranchDelegate.cpp
  BranchTableModel.cpp
  CheckoutDialog.cpp
  CloneDialog.cpp
  CommitDialog.cpp
  ConfigDialog.cpp
  DeleteBranchDialog.cpp
  DeleteTagDialog.cpp
  DirectorySelectionDialog.cpp
  DiffPanel.cpp
  ExternalToolsDialog.cpp
  ExternalToolsModel.cpp
  FastIssueDialog.cpp
  HotkeysPanel.cpp
  IconLabel.cpp
  LocalWorkspaceDialog.cpp
  MergeDialog.cpp
  ModifySubmoduleDialog.cpp
  NewBranchDialog.cpp
  PluginsPanel.cpp
  PullRequestDialog.cpp
  RebaseConflictDialog.cpp
  RemoteDialog.cpp
  RemoteTableModel.cpp
  RenameBranchDialog.cpp
  SettingsDialog.cpp
  StartDialog.cpp
  SubmoduleDelegate.cpp
  SubmoduleTableModel.cpp
  TagDialog.cpp
  ThemeDialog.cpp
  UpdateSubmodulesDialog.cpp
  WorktreeDialog.cpp)

target_compile_definitions(
  dialogs PRIVATE GITNORTEK_BUILD_DATE="${GITNORTEK_BUILD_DATE}"
                  GITNORTEK_BUILD_REVISION="${GITNORTEK_BUILD_REVISION}")

target_include_directories(dialogs PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(
  dialogs
  cli
  conf
  cred
  host
  index
  gitnortekUtil
  Qt6::Widgets)

set_target_properties(dialogs PROPERTIES AUTOMOC ON)

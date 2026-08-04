<!--- Github page main file --->

GitNortek is a NortekMed-maintained fork of [Gittyup](https://github.com/Murmele/Gittyup), a graphical Git client designed to help you understand and manage your source code history.

This fork is maintained at [NortekMed/Gittyup](https://github.com/NortekMed/Gittyup) and carries NortekMed-specific naming, packaging, and integration changes. GitNortek is based on Gittyup, which is a continuation of the [GitAhead](https://github.com/gitahead/gitahead) client.

GitNortek packages are produced by the NortekMed fork release workflow when available. Upstream Gittyup packages remain available from the upstream project but are not GitNortek builds.

GitNortek can be built from source by following the directions in the [GitNortek repository](https://github.com/NortekMed/Gittyup#how-to-build).

To see the changes of the current version please have a look at the <A href="#changelog">changelog</A> section

![GitNortek](/rsrc/screenshots/main_dark_orig.png)

Project Lineage
===============

GitNortek is a fork of [Gittyup](https://github.com/Murmele/Gittyup). Upstream Gittyup project resources, issue history, and release notes remain useful for understanding the base application.

For GitNortek-specific changes, packaging, or bugs, use the [NortekMed fork repository](https://github.com/NortekMed/Gittyup).

Multi language support
======================

GitNortek supports the following languages:
- English (en)
- German (de)
- Spanisch (es)
- Japanese (ja)
- Portuguese (pt)
- Portuguese Brazil (pt_BR)
- Chinese (zh_CN)
- Russian (ru)

By default the system language is used. To switch to another language execute the application with the following command
```
LANG=<lang> <executable>
```


Features
========

### Single branch view to focus on your work
Select "Show Selected Branch" in the drop down menu above the commit list
![Single branch](/rsrc/screenshots/main_show_selected_branch.png)

### Fullscreen
of the history or the change dialog by pressing Ctrl+M

### Tabs
to be able to switch fast between repositories

### Diff View
Staging and unstaging changes, viewing Blame
![Diff View](/rsrc/screenshots/DiffView.png)

### Tree View
To visit the blame with its history for unchanged files

![Tree View](/rsrc/screenshots/treeview.png)

### Blame View
See blame of the current version with an integrated timeline to see who changed which line

![Blame View](/rsrc/screenshots/BlameView.png)

### Dynamic Line Wrapping
Courtesy of Scintilla.

![Line Wrapping](/rsrc/screenshots/line-wrap-demo-2.gif)

### Single line staging 
by eighter clicking on the checkboxes next to each line or by selecting the relevant code and pressing "S". For unstaging you can uncheck the checkboxes or press "U". To revert changes, select the text and press "R".

![Single line staging](/rsrc/screenshots/double_treeview_single_line_staging.png)

### Amending commits
Editing properties of a commit
![Amend Dialog](/rsrc/screenshots/AmendDialog.png)

### Solving rebase conflicts
Solving rebase conflicts and continuing after conflicts are solved

![Rebase Conflicts](/rsrc/screenshots/RebaseConflicts.png)

### Starring commits
to find specific commits much faster
![Starring commits](/rsrc/screenshots/starring_commits.png)

### Tag selection
Use an existing tag as template for your next tag. So you never have to look which is your latest tag

![Tag selection](/rsrc/screenshots/tag_selection.png)

### Commit message template
Create you commit messages according a defined template. The first template is automatically applied to the commit message editor.

![Commit message template selection](/rsrc/screenshots/CommitMessageTemplateSelection.png)

![Commit message template editor](/rsrc/screenshots/CommitMessageTemplateEditor.png)

### And a lot more ...

Changelog
=========

{% include_relative changelog.md %}

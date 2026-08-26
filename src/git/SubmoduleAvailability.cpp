//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "SubmoduleAvailability.h"
#include "Commit.h"
#include "Reference.h"
#include "Remote.h"
#include "Repository.h"
#include "Submodule.h"
#include "Tree.h"
#include "git2/buffer.h"
#include "git2/graph.h"
#include "git2/remote.h"
#include "git2/sys/errors.h"
#include <QObject>
#include <QSharedPointer>

namespace git {

namespace {

QString lastError(const QString &fallback) {
  const git_error *error = git_error_last();
  return error && error->message ? QString::fromUtf8(error->message) : fallback;
}

SubmoduleAvailability::Issue issue(const Submodule &submodule, const Id &pinned,
                                   const QString &url,
                                   SubmoduleAvailability::Issue::Reason reason,
                                   const QString &message) {
  SubmoduleAvailability::Issue result;
  result.reason = reason;
  result.name = submodule.name();
  result.path = submodule.path();
  result.url = url;
  result.pinnedId = pinned;
  result.message = message;
  return result;
}

void configureCallbacks(git_remote_callbacks &opts,
                        Remote::Callbacks *callbacks) {
  if (!callbacks)
    return;

#ifndef USE_SYSTEM_LIBGIT2
  opts.connected = &Remote::Callbacks::connected;
  opts.about_to_disconnect = &Remote::Callbacks::about_to_disconnect;
#endif
  opts.sideband_progress = &Remote::Callbacks::sideband;
  opts.credentials = &Remote::Callbacks::credentials;
  opts.certificate_check = &Remote::Callbacks::certificate;
  opts.transfer_progress = &Remote::Callbacks::transfer;
  opts.update_tips = &Remote::Callbacks::update;
  opts.remote_ready = &Remote::Callbacks::remoteReady;
  opts.payload = callbacks;
}

} // namespace

QList<SubmoduleAvailability::Issue>
SubmoduleAvailability::check(const Repository &repo, const Commit &parent,
                             Remote::Callbacks *callbacks) {
  if (!repo.isValid() || !parent.isValid())
    return {};

  return check(repo, parent, repo.submodules(), callbacks);
}

QList<SubmoduleAvailability::Issue>
SubmoduleAvailability::check(const Repository &repo, const Commit &parent,
                             const QList<Submodule> &submodules,
                             Remote::Callbacks *callbacks) {
  QList<Issue> issues;
  if (!repo.isValid() || !parent.isValid())
    return issues;

  const Tree tree = parent.tree();
  const Tree headTree = repo.head().target().tree();
  const Id modulesId = tree.id(".gitmodules");
  if (!modulesId.isNull() && modulesId != headTree.id(".gitmodules")) {
    Issue result;
    result.reason = Issue::LocalError;
    result.name = QObject::tr("Submodule configuration");
    result.path = ".gitmodules";
    result.message = QObject::tr(
        "The pushed commit uses a different submodule configuration from the "
        "checked-out branch and cannot be checked safely.");
    issues.append(result);
    return issues;
  }

  foreach (const Submodule &submodule, submodules) {
    const Id pinned = tree.id(submodule.path());
    QString url = submodule.url();
    git_buf resolved = GIT_BUF_INIT;
    if (!git_submodule_resolve_url(&resolved, repo, url.toUtf8()))
      url = QString::fromUtf8(resolved.ptr);
    git_buf_dispose(&resolved);

    if (pinned.isNull())
      continue;

    if (!submodule.isInitialized()) {
      issues.append(issue(
          submodule, pinned, url, Issue::LocalError,
          QObject::tr(
              "The submodule is not initialized and cannot be checked.")));
      continue;
    }

    Repository child = submodule.open();
    if (!child.isValid()) {
      issues.append(issue(
          submodule, pinned, url, Issue::LocalError,
          QObject::tr(
              "The initialized submodule repository could not be opened.")));
      continue;
    }

    git_remote *rawRemote = nullptr;
    int error = git_remote_create_anonymous(&rawRemote, child, url.toUtf8());
    QSharedPointer<git_remote> remote(rawRemote, git_remote_free);
    if (error) {
      issues.append(issue(submodule, pinned, url, Issue::RemoteError,
                          lastError(QObject::tr("Unable to create remote."))));
      continue;
    }

    git_remote_callbacks remoteCallbacks = GIT_REMOTE_CALLBACKS_INIT;
    if (callbacks)
      callbacks->setUrl(url);
    configureCallbacks(remoteCallbacks, callbacks);
    git_proxy_options proxyOptions = GIT_PROXY_OPTIONS_INIT;
    QByteArray proxy = Remote::proxyUrl(url, proxyOptions.type);
    proxyOptions.url = proxy;

    git_error_clear();
    error = git_remote_connect(remote.data(), GIT_DIRECTION_FETCH,
                               &remoteCallbacks, &proxyOptions, nullptr);
    if (error) {
      issues.append(
          issue(submodule, pinned, url, Issue::RemoteError,
                lastError(QObject::tr(
                    "Unable to read advertised remote references."))));
      continue;
    }

    const git_remote_head **heads = nullptr;
    size_t count = 0;
    error = git_remote_ls(&heads, &count, remote.data());
    bool available = false;
    QString comparisonError;
    if (!error) {
      for (size_t i = 0; i < count && !available; ++i) {
        const Id advertised(heads[i]->oid);
        if (advertised == pinned) {
          available = true;
          break;
        }

        git_error_clear();
        int descendant = git_graph_descendant_of(child, advertised, pinned);
        if (descendant == 1)
          available = true;
        else if (descendant < 0)
          comparisonError = lastError(QObject::tr(
              "Advertised commits are not available locally for comparison."));
      }
    }
    if (error) {
      issues.append(
          issue(submodule, pinned, url, Issue::RemoteError,
                lastError(QObject::tr(
                    "Unable to read advertised remote references."))));
    } else if (!available) {
      issues.append(
          issue(submodule, pinned, url,
                comparisonError.isEmpty() ? Issue::NotAdvertised
                                          : Issue::ComparisonUnavailable,
                comparisonError.isEmpty()
                    ? QObject::tr("The pinned commit is not reachable from an "
                                  "advertised remote reference.")
                    : comparisonError));
    }
  }

  return issues;
}

} // namespace git

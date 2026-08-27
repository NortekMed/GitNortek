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
#include "git2/object.h"
#include "git2/refs.h"
#include "git2/remote.h"
#include "git2/sys/errors.h"
#include <QObject>
#include <QSharedPointer>

namespace git {

namespace {

QString lastError(const QString &fallback) {
  const git_error *error = git_error_last();
  if (!error || error->klass == GIT_ERROR_NONE || !error->message)
    return fallback;

  QString message = QString::fromUtf8(error->message);
  return message.compare("no error", Qt::CaseInsensitive) == 0 ? fallback
                                                               : message;
}

const QByteArray kCheckRefPrefix = "refs/gitnortek/submodule-push-check/";

void removeCheckRefs(git_repository *repo) {
  git_strarray refs = {};
  if (git_reference_list(&refs, repo))
    return;

  for (size_t i = 0; i < refs.count; ++i) {
    QByteArray name(refs.strings[i]);
    if (name.startsWith(kCheckRefPrefix))
      git_reference_remove(repo, name);
  }
  git_strarray_dispose(&refs);
}

bool isReachableFromCheckRefs(git_repository *repo, const git_oid *pinned) {
  git_strarray refs = {};
  if (git_reference_list(&refs, repo))
    return false;

  bool available = false;
  for (size_t i = 0; i < refs.count && !available; ++i) {
    QByteArray name(refs.strings[i]);
    if (!name.startsWith(kCheckRefPrefix))
      continue;

    git_reference *ref = nullptr;
    git_object *commit = nullptr;
    if (!git_reference_lookup(&ref, repo, name) &&
        !git_reference_peel(&commit, ref, GIT_OBJECT_COMMIT)) {
      const git_oid *advertised = git_object_id(commit);
      available = git_oid_equal(advertised, pinned) ||
                  git_graph_descendant_of(repo, advertised, pinned) == 1;
    }
    git_object_free(commit);
    git_reference_free(ref);
    git_error_clear();
  }
  git_strarray_dispose(&refs);
  return available;
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
    git_repository *childRepo = child;
    const git_oid *pinnedOid = pinned;

    git_remote *rawRemote = nullptr;
    int error = git_remote_create_anonymous(&rawRemote, child, url.toUtf8());
    QSharedPointer<git_remote> remote(rawRemote, git_remote_free);
    if (error) {
      issues.append(issue(submodule, pinned, url, Issue::RemoteError,
                          lastError(QObject::tr("Unable to create remote."))));
      continue;
    }

    removeCheckRefs(childRepo);

    git_fetch_options fetchOptions = GIT_FETCH_OPTIONS_INIT;
    if (callbacks)
      callbacks->setUrl(url);
    configureCallbacks(fetchOptions.callbacks, callbacks);
    fetchOptions.callbacks.update_tips = nullptr;
    fetchOptions.follow_redirects = GIT_REMOTE_REDIRECT_INITIAL;
    fetchOptions.update_fetchhead = 0;
    QByteArray proxy = Remote::proxyUrl(url, fetchOptions.proxy_opts.type);
    fetchOptions.proxy_opts.url = proxy;

    QByteArray heads =
        "+refs/heads/*:refs/gitnortek/submodule-push-check/heads/*";
    QByteArray tags = "+refs/tags/*:refs/gitnortek/submodule-push-check/tags/*";
    char *rawRefspecs[] = {heads.data(), tags.data()};
    git_strarray refspecs = {rawRefspecs, 2};

    git_error_clear();
    error = git_remote_fetch(remote.data(), &refspecs, &fetchOptions,
                             "submodule push check");
    if (error) {
      issues.append(
          issue(submodule, pinned, url, Issue::RemoteError,
                lastError(QObject::tr("Unable to fetch advertised remote "
                                      "branches and tags."))));
      removeCheckRefs(childRepo);
      continue;
    }

    bool available = isReachableFromCheckRefs(childRepo, pinnedOid);
    removeCheckRefs(childRepo);
    if (!available)
      issues.append(issue(
          submodule, pinned, url, Issue::NotAdvertised,
          QObject::tr("The pinned commit is not reachable from any advertised "
                      "branch or tag. Push the submodule commit first.")));
  }

  return issues;
}

} // namespace git

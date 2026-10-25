//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Remote.h"
#include "Branch.h"
#include "Commit.h"
#include "Config.h"
#include "Id.h"
#include "qtsupport.h"
#include "TagRef.h"
#include "Tag.h"
#include "git2/buffer.h"
#include "git2/clone.h"
#include "git2/graph.h"
#include "git2/remote.h"
#include "git2/signature.h"
#include "git2/sys/errors.h"
#include <libssh2.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QNetworkProxyFactory>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QTime>
#include <QUrl>
#include <QVector>
#include <functional>
#include <cstdint>

namespace git {

namespace {

const QString kLogKey = "remote/log";
const QStringList kKeyKinds = {"ed25519", "rsa", "dsa"};

QString keyFile(const QString &path = QString()) {
  QDir dir = QDir::home();
  if (!path.isEmpty()) {
    QFileInfo file(path);

    if (!file.isAbsolute())
      file.setFile(dir.absolutePath() + '/' + file.filePath());

    if (file.exists())
      return file.absoluteFilePath();
  }

  if (!dir.cd(".ssh"))
    return QString();

  foreach (const QString &kind, kKeyKinds) {
    QString name = QString("id_%1").arg(kind);
    if (dir.exists(name))
      return dir.absoluteFilePath(name);
  }

  return QString();
}

static QRegularExpression urlRegex{"^[0-9a-zA-Z+-]+://"};
static QRegularExpression sshRegex{
    "^(([^:@/\\\\]+)@)?([0-9a-zA-Z+.-]{2,}):(.*)$"};
QUrl parseUrl(const QString &url) {
  QUrl res;

  if (urlRegex.match(url).hasMatch()) {
    res = QUrl(url);
  } else {
    QRegularExpressionMatch matches = sshRegex.match(url);
    if (matches.hasMatch()) {
      res.setScheme("ssh");

      res.setUserName(matches.captured(2));
      res.setHost(matches.captured(3));

      QString path = matches.captured(4);
      if (!path.startsWith('/')) {
        path.push_front('/');
      }

      res.setPath(path);
    } else {
      res = QUrl::fromLocalFile(url);
    }
  }

  return res;
}

int pack_progress(int stage, unsigned int current, unsigned int total,
                  void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  if (stage == GIT_PACKBUILDER_ADDING_OBJECTS) {
    cbs->add(total, current);
  } else if (stage == GIT_PACKBUILDER_DELTAFICATION) {
    cbs->delta(total, current);
  }

  return 0;
}

int push_transfer_progress(unsigned int current, unsigned int total,
                           size_t bytes, void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  if (cbs->state() == Remote::Callbacks::Transfer) {
    if (!cbs->transfer(total, current, bytes))
      return -1;
  }

  return 0;
}

int push_update_reference(const char *name, const char *status, void *payload) {
  if (status)
    reinterpret_cast<Remote::Callbacks *>(payload)->rejected(name, status);
  return 0;
}

int push_negotiation(const git_push_update **updates, size_t len,
                     void *payload) {
  QList<Remote::PushUpdate> list;
  for (size_t i = 0; i < len; ++i) {
    const git_push_update *update = updates[i];
    list.append(
        {update->src_refname, update->dst_refname, update->src, update->dst});
  }

  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  return cbs->negotiation(list) ? 0 : -1;
}

class ConfigFile {
public:
  struct Host {
    QString file;
    QString hostName;
    QString user;
    QStringList patterns;
    uint16_t port = 0;
  };

  ConfigFile(const QString &path) {
    QFileInfo file(path);
    QDir dir = QDir::home();
    if (path.isEmpty()) {
      if (!dir.cd(".ssh"))
        return;

      file.setFile(dir.absoluteFilePath("config"));
    }

    if (!file.isAbsolute())
      file.setFile(dir.absolutePath() + '/' + file.filePath());

    mFile.reset(new QFile(file.absoluteFilePath()));
    if (!mFile->open(QFile::ReadOnly))
      return;

    mValid = true;
  }

  bool isValid() const { return mValid; }

  void apply(const QString &hostname,
             std::function<void(const Host &)> handler) const {
    for (Host host : parse()) {
      if (host.patterns.empty()) {
        handler(host);
      } else {
        bool matched = false;

        for (const QString &pattern : host.patterns) {
          QRegularExpression re{
              QRegularExpression::wildcardToRegularExpression(pattern)};
          if (re.match(hostname).hasMatch()) {
            handler(host);
            matched = true;
            break;
          }
        }

        if (matched) {
          break;
        }
      }
    }
  }

private:
  bool mValid = false;
  QScopedPointer<QFile> mFile;

  QList<Host> parse() const {
    Q_ASSERT(isValid());

    QList<Host> hosts;
    QTextStream in(mFile.data());
    hosts.append(Host{});
    while (!in.atEnd()) {
      // Skip comments and empty lines.
      QString line = in.readLine().trimmed();
      if (line.isEmpty() || line.startsWith('#'))
        continue;

      // Split into keyword and arguments.
      // FIXME: Doesn't handle quoted arguments.
      QRegularExpression re("(\\s+|\\s*=\\s*)");
      QStringList words = line.split(re);
      QString keyword = words.takeFirst().toLower();
      if (keyword == "host") {
        Host host;
        host.patterns = words;
        hosts.append(host);
      } else if (keyword == "hostname") {
        if (!hosts.isEmpty() && !words.isEmpty())
          hosts.last().hostName = words.first();
      } else if (keyword == "user") {
        if (!hosts.isEmpty() && !words.isEmpty())
          hosts.last().user = words.first();
      } else if (keyword == "identityfile") {
        if (!hosts.isEmpty() && !words.isEmpty()) {
          QString file = words.first();
          hosts.last().file = file.replace('~', QDir::homePath());
        }
      } else if (keyword == "port") {
        if (!hosts.isEmpty() && !words.isEmpty() && hosts.last().port == 0) {
          bool ok;
          uint16_t port = words.first().toUShort(&ok);
          if (ok) {
            hosts.last().port = port;
          }
        }
      }
    }

    return hosts;
  }
};

} // namespace

#ifndef USE_SYSTEM_LIBGIT2
void Remote::Callbacks::connected(git_remote *remote, void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  QMutexLocker locker(&cbs->mRemoteMutex);
  cbs->mRemote = remote;
}

void Remote::Callbacks::about_to_disconnect(git_remote *remote, void *payload) {
  Q_UNUSED(remote)

  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  QMutexLocker locker(&cbs->mRemoteMutex);
  cbs->mRemote = nullptr;
}
#endif

int Remote::Callbacks::sideband(const char *str, int len, void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  cbs->sideband(QString::fromUtf8(str, len));
  return 0;
}

static void interactiveCallback(const char *name, int name_len,
                                const char *instruction, int instruction_len,
                                int num_prompts,
                                const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                                LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                                void **abstract) {
  if (num_prompts == 0)
    return;

  Remote::Callbacks *cbs = (Remote::Callbacks *)*abstract;

  QVector<Remote::SshInteractivePrompt> promptsVector(num_prompts);
  QVector<QString> responsesVector(num_prompts);

  for (int i = 0; i < num_prompts; ++i) {
    promptsVector[i] = {
        QString::fromUtf8((const char *)prompts[i].text, prompts[i].length),
        (bool)prompts[i].echo};
  }

  cbs->interactiveAuth(QString::fromUtf8(name, name_len),
                       QString::fromUtf8(instruction, instruction_len),
                       promptsVector, responsesVector);

  if (responsesVector.length() < num_prompts)
    responsesVector.resize(num_prompts);

  for (int i = 0; i < num_prompts; ++i) {
    if (responsesVector[i].isEmpty()) {
      responses[i].text = nullptr;
      responses[i].length = 0;

    } else {
      // The newline is also sent by OpenSSH and needed for some
      // keyboard-interactive prompts
      QByteArray bytes = (responsesVector[i] + "\n").toUtf8();

      // Use malloc and copy the response data to it
      // libssh2 free()s this memory by itself
      responses[i].length = bytes.size() - 1;
      responses[i].text = (char *)malloc(responses[i].length);

      memcpy(responses[i].text, bytes.data(), responses[i].length);
    }
  }
}

int Remote::Callbacks::credentials(git_credential **out, const char *url,
                                   const char *name, unsigned int types,
                                   void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  if (types & GIT_CREDENTIAL_SSH_KEY) {
    const git_error *error = git_error_last();
    const QString remoteUrl = cbs->url();
    const QString agentKey = remoteUrl + "\n" + QString::fromUtf8(name);

    // First try to get key from agent.
    if (!cbs->mAgentNames.contains(agentKey)) {
      log(QString("agent: %1").arg(name));
      cbs->mAgentNames.insert(agentKey);
      if (cbs->connectToAgent())
        return git_credential_ssh_key_from_agent(out, name);

    } else if (error) {
      log(error->message);
    }

    // Read SSH config file.
    QString key;
    ConfigFile configFile(cbs->configFilePath());
    if (configFile.isValid()) {
      // Extract hostname from the unresolved URL.
      configFile.apply(parseUrl(cbs->url()).host(),
                       [&key, remoteUrl, cbs](const ConfigFile::Host &host) {
                         QString attempt = remoteUrl + "\n" + host.file;
                         if (!host.file.isEmpty() &&
                             !cbs->mKeyFiles.contains(attempt)) {
                           key = host.file;
                         }
                       });
    }

    if (key.isEmpty()) {
      key = keyFile(cbs->keyFilePath());
      if (cbs->mKeyFiles.contains(remoteUrl + "\n" + key))
        key = "";
    }

    // Search for default keys.
    if (!key.isEmpty()) {
      cbs->mKeyFiles.insert(remoteUrl + "\n" + key);

      if (!QFile::exists(key)) {
        QString err = QString("identity file not found: %1").arg(key);
        git_error_set_str(GIT_ERROR_NET, err.toUtf8());
        return -GIT_ERROR_NET;
      }

      QString pub = QString("%1.pub").arg(key);
      if (!QFile::exists(pub))
        pub = "";

      // Check if the private key is encrypted.
      QFile file(key);
      if (!file.open(QFile::ReadOnly)) {
        git_error_set_str(GIT_ERROR_NET, "failed to open SSH identity file");
        return -GIT_ERROR_NET;
      }

      QTextStream in(&file);
      in.readLine(); // -----BEGIN PRIVATE KEY-----
      QString line = in.readLine();
      if (!line.startsWith("Proc-Type:") || !line.endsWith("ENCRYPTED")) {
        QByteArray base64 = QByteArray::fromBase64(line.toLocal8Bit());
        if (!base64.contains("aes256-ctr") || !base64.contains("bcrypt"))
          return git_credential_ssh_key_new(
              out, name,
              !pub.isEmpty() ? pub.toLocal8Bit().constData() : nullptr,
              key.toLocal8Bit(), nullptr);
      }

      // Prompt for passphrase to decrypt key.
      QString passphrase;
      QString username = name;
      if (!cbs->credentials(url, username, passphrase))
        return -1;

      return git_credential_ssh_key_new(
          out, username.toUtf8(),
          !pub.isEmpty() ? pub.toLocal8Bit().constData() : nullptr,
          key.toLocal8Bit(), passphrase.toUtf8());
    }
  }

  if (types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
    QString password;
    QString username = QUrl::fromPercentEncoding(name);
    if (cbs->credentials(url, username, password)) {
      return git_credential_userpass_plaintext_new(out, username.toUtf8(),
                                                   password.toUtf8());
    }
  }

  if (types & GIT_CREDENTIAL_SSH_INTERACTIVE) {
    return git_credential_ssh_interactive_new(out, name, interactiveCallback,
                                              cbs);
  }

  return -1;
}

int Remote::Callbacks::certificate(git_cert *cert, int valid, const char *host,
                                   void *payload) {
  Q_UNUSED(host)

  if (valid || cert->cert_type == GIT_CERT_HOSTKEY_LIBSSH2)
    return 0;

  int noVerify = 0;
  QByteArray env = qgetenv("GIT_SSL_NO_VERIFY");
  if (!env.isNull() && !git_config_parse_bool(&noVerify, env) && noVerify)
    return 0;

  Repository repo = reinterpret_cast<Remote::Callbacks *>(payload)->repo();
  Config config = repo.isValid() ? repo.gitConfig() : Config::global();
  if (!config.value<bool>("http.sslVerify", true))
    return 0;

  git_error_set_str(GIT_ERROR_SSL, "invalid certificate");
  return -GIT_ERROR_SSL;
}

int Remote::Callbacks::transfer(const git_indexer_progress *stats,
                                void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  switch (cbs->state()) {
    case Transfer:
      return cbs->transfer(stats->total_objects, stats->received_objects,
                           stats->received_bytes)
                 ? 0
                 : -1;

    case Resolve:
      return cbs->resolve(stats->total_deltas, stats->indexed_deltas) ? 0 : -1;

    case Update:
      return 0;
  }
  return 0;
}

int Remote::Callbacks::update(const char *name, const git_oid *a,
                              const git_oid *b, void *payload) {
  reinterpret_cast<Remote::Callbacks *>(payload)->update(name, a, b);
  return 0;
}

int Remote::Callbacks::remoteReady(git_remote *remote, int direction,
                                   void *payload) {
  Remote::Callbacks *cbs = reinterpret_cast<Remote::Callbacks *>(payload);
  const char *remoteUrl =
      direction == GIT_DIRECTION_PUSH && git_remote_pushurl(remote)
          ? git_remote_pushurl(remote)
          : git_remote_url(remote);
  QString resolved(remoteUrl);
  if (!cbs->url(resolved))
    return -1;

  QUrl resolvedUrl = parseUrl(resolved);

  // Find matching config entry.
  if (resolvedUrl.scheme().compare("ssh", Qt::CaseInsensitive) == 0) {
    ConfigFile configFile(cbs->configFilePath());
    if (configFile.isValid()) {
      bool explicitPort = resolvedUrl.port() >= 0;
      bool explicitUser = !resolvedUrl.userName().isEmpty();

      configFile.apply(resolvedUrl.host(),
                       [&resolvedUrl, explicitPort,
                        explicitUser](const ConfigFile::Host &host) {
                         if (!host.hostName.isEmpty()) {
                           resolvedUrl.setHost(host.hostName);
                         }

                         if (host.port != 0 && !explicitPort) {
                           resolvedUrl.setPort(host.port);
                         }

                         if (!host.user.isEmpty() && !explicitUser) {
                           resolvedUrl.setUserName(host.user);
                         }
                       });
    }
  }

  resolved = resolvedUrl.toString();
  if (direction == GIT_DIRECTION_PUSH && git_remote_pushurl(remote)) {
    // The local transport reads the instance URL when opening its target.
    // Keep both URLs aligned on the push-only remote copy.
    git_remote_set_instance_url(remote, resolved.toUtf8());
    git_remote_set_instance_pushurl(remote, resolved.toUtf8());
  } else {
    git_remote_set_instance_url(remote, resolved.toUtf8());
  }
  return 0;
}

void Remote::Callbacks::stop() {
#ifndef USE_SYSTEM_LIBGIT2
  QMutexLocker locker(&mRemoteMutex);
  if (mRemote)
    git_remote_stop(mRemote);
#endif
}

Remote::Remote() {}

Remote::Remote(git_remote *remote) : d(remote, git_remote_free) {}

QString Remote::name() const {
  QString name = git_remote_name(d.data());
  return !name.isEmpty() ? name : url();
}

void Remote::setName(const QString &name) {
  if (!isValid() || name == this->name())
    return;
  git_strarray problems;
  const char *current = git_remote_name(d.data());
  git_repository *repo = git_remote_owner(d.data());
  if (git_remote_rename(&problems, repo, current, name.toUtf8()))
    return;

  // FIXME: Report problems?
  git_strarray_dispose(&problems);
  Repository repository(repo);
  emit repository.notifier()->remoteUpdated(repository.lookupRemote(name));
}

QString Remote::url() const { return git_remote_url(d.data()); }

void Remote::setUrl(const QString &url) {
  if (!isValid() || url == this->url())
    return;
  git_repository *repo = git_remote_owner(d.data());
  QString name = git_remote_name(d.data());
  if (!git_remote_set_url(repo, name.toUtf8(), url.toUtf8())) {
    Repository repository(repo);
    emit repository.notifier()->remoteUpdated(repository.lookupRemote(name));
  }
}

QString Remote::pushUrl() const {
  const char *url = isValid() ? git_remote_pushurl(d.data()) : nullptr;
  return url ? QString::fromUtf8(url) : QString();
}

QString Remote::effectivePushUrl() const {
  QString configuredUrl = pushUrl();
  return configuredUrl.isEmpty() ? url() : configuredUrl;
}

namespace {

QStringList remoteRefspecs(const git_remote *remote, bool fetch) {
  if (!remote)
    return QStringList();

  git_strarray array = {nullptr, 0};
  int error = fetch ? git_remote_get_fetch_refspecs(&array, remote)
                    : git_remote_get_push_refspecs(&array, remote);
  if (error)
    return QStringList();

  QStringList result;
  for (size_t i = 0; i < array.count; ++i)
    result.append(QString::fromUtf8(array.strings[i]));
  git_strarray_dispose(&array);
  return result;
}

Result validateRefspecs(const QStringList &refspecs, bool fetch) {
  foreach (const QString &refspec, refspecs) {
    QByteArray value = refspec.toUtf8();
    git_refspec *parsed = nullptr;
    int error = git_refspec_parse(&parsed, value.constData(), fetch);
    if (error)
      return Result(error);
    git_refspec_free(parsed);
  }

  return Result(0);
}

} // namespace

QStringList Remote::fetchRefspecs() const {
  return remoteRefspecs(d.data(), true);
}

QStringList Remote::pushRefspecs() const {
  return remoteRefspecs(d.data(), false);
}

Result Remote::configure(const QString &newName, const QString &newUrl,
                         const QString &newPushUrl,
                         const QStringList &newFetchRefspecs,
                         const QStringList &newPushRefspecs) {
  if (!isValid())
    return Result(GIT_EINVALID);

  if (newUrl.trimmed().isEmpty()) {
    git_error_set_str(GIT_ERROR_INVALID, "The fetch URL cannot be empty");
    return Result(GIT_EINVALIDSPEC);
  }

  int validName = 0;
  QByteArray nameBytes = newName.toUtf8();
  if (int error = git_remote_name_is_valid(&validName, nameBytes.constData()))
    return Result(error);
  if (!validName) {
    git_error_set_str(GIT_ERROR_INVALID, "Invalid remote name");
    return Result(GIT_EINVALIDSPEC);
  }

  Repository repository(git_remote_owner(d.data()));
  const QString oldName = name();
  const QString oldUrl = url();
  const QString oldPushUrl = pushUrl();
  QStringList oldFetchRefspecs = fetchRefspecs();
  const QStringList oldPushRefspecs = pushRefspecs();

  if (newName != oldName && repository.lookupRemote(newName)) {
    git_error_set_str(GIT_ERROR_INVALID,
                      "A remote with that name already exists");
    return Result(GIT_EEXISTS);
  }

  Result validation = validateRefspecs(newFetchRefspecs, true);
  if (validation.error())
    return validation;
  validation = validateRefspecs(newPushRefspecs, false);
  if (validation.error())
    return validation;

  QStringList fetchRefspecsToSave = newFetchRefspecs;
  if (newName != oldName) {
    const QString oldDefault =
        QStringLiteral("+refs/heads/*:refs/remotes/%1/*").arg(oldName);
    const QString newDefault =
        QStringLiteral("+refs/heads/*:refs/remotes/%1/*").arg(newName);
    for (QString &refspec : fetchRefspecsToSave) {
      if (refspec == oldDefault)
        refspec = newDefault;
    }

    git_strarray problems = {nullptr, 0};
    QByteArray oldNameBytes = oldName.toUtf8();
    int error =
        git_remote_rename(&problems, git_remote_owner(d.data()),
                          oldNameBytes.constData(), nameBytes.constData());
    if (error)
      return Result(error);
    git_strarray_dispose(&problems);
  }

  QByteArray urlBytes = newUrl.toUtf8();
  if (newUrl != oldUrl) {
    if (int error =
            git_remote_set_url(git_remote_owner(d.data()),
                               nameBytes.constData(), urlBytes.constData()))
      return Result(error);
  }

  if (newPushUrl != oldPushUrl) {
    QByteArray pushUrlBytes = newPushUrl.toUtf8();
    const char *pushUrl =
        newPushUrl.isEmpty() ? nullptr : pushUrlBytes.constData();
    if (int error = git_remote_set_pushurl(git_remote_owner(d.data()),
                                           nameBytes.constData(), pushUrl))
      return Result(error);
  }

  git_repository *repo = git_remote_owner(d.data());
  auto replaceRefspecs = [repo, &nameBytes](const QStringList &oldRefspecs,
                                            const QStringList &newRefspecs,
                                            bool fetch) {
    if (oldRefspecs == newRefspecs)
      return 0;

    QString key =
        QStringLiteral("remote.%1.%2")
            .arg(QString::fromUtf8(nameBytes),
                 fetch ? QStringLiteral("fetch") : QStringLiteral("push"));
    Config config = Repository(repo).gitConfig();
    if (!oldRefspecs.isEmpty() && !config.remove(key, QStringLiteral(".*")))
      return -1;

    foreach (const QString &refspec, newRefspecs) {
      QByteArray refspecBytes = refspec.toUtf8();
      int error = fetch ? git_remote_add_fetch(repo, nameBytes.constData(),
                                               refspecBytes.constData())
                        : git_remote_add_push(repo, nameBytes.constData(),
                                              refspecBytes.constData());
      if (error)
        return error;
    }
    return 0;
  };

  if (int error = replaceRefspecs(oldFetchRefspecs, fetchRefspecsToSave, true))
    return Result(error);
  if (int error = replaceRefspecs(oldPushRefspecs, newPushRefspecs, false))
    return Result(error);

  Remote updated = repository.lookupRemote(newName);
  if (!updated)
    return Result(GIT_ENOTFOUND);

  d = updated.d;
  emit repository.notifier()->remoteUpdated(updated);
  return Result(0);
}

Result Remote::fetch(Callbacks *callbacks, bool tags, bool prune) {
  git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
#ifndef USE_SYSTEM_LIBGIT2
  opts.callbacks.connected = &Remote::Callbacks::connected;
  opts.callbacks.about_to_disconnect = &Remote::Callbacks::about_to_disconnect;
#endif
  opts.callbacks.sideband_progress = &Remote::Callbacks::sideband;
  opts.callbacks.credentials = &Remote::Callbacks::credentials;
  opts.callbacks.certificate_check = &Remote::Callbacks::certificate;
  opts.callbacks.transfer_progress = &Remote::Callbacks::transfer;
  opts.callbacks.update_tips = &Remote::Callbacks::update;
  opts.callbacks.remote_ready = &Remote::Callbacks::remoteReady;
  opts.callbacks.payload = callbacks;

  QByteArray proxy = proxyUrl(url(), opts.proxy_opts.type);
  opts.proxy_opts.url = proxy;

  if (tags)
    opts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_ALL;

  if (prune)
    opts.prune = GIT_FETCH_PRUNE;

  // Write reflog message.
  QString msg = QString("fetch: %1").arg(name());

  return git_remote_fetch(d.data(), nullptr, &opts, msg.toUtf8());
}

Result Remote::push(Callbacks *callbacks, const QStringList &refspecs) {
  git_remote *rawRemote = nullptr;
  if (int error = git_remote_dup(&rawRemote, d.data()))
    return Result(error);
  QSharedPointer<git_remote> remote(rawRemote, git_remote_free);

  git_push_options opts = GIT_PUSH_OPTIONS_INIT;
#ifndef USE_SYSTEM_LIBGIT2
  opts.callbacks.connected = &Remote::Callbacks::connected;
  opts.callbacks.about_to_disconnect = &Remote::Callbacks::about_to_disconnect;
#endif
  opts.callbacks.sideband_progress = &Remote::Callbacks::sideband;
  opts.callbacks.credentials = &Remote::Callbacks::credentials;
  opts.callbacks.certificate_check = &Remote::Callbacks::certificate;
  opts.callbacks.transfer_progress = &Remote::Callbacks::transfer;
  opts.callbacks.update_tips = &Remote::Callbacks::update;
  opts.callbacks.remote_ready = &Remote::Callbacks::remoteReady;
  opts.callbacks.pack_progress = &pack_progress;
  opts.callbacks.push_transfer_progress = &push_transfer_progress;
  opts.callbacks.push_update_reference = &push_update_reference;
  opts.callbacks.push_negotiation = &push_negotiation;
  opts.callbacks.payload = callbacks;

  QByteArray proxy = proxyUrl(effectivePushUrl(), opts.proxy_opts.type);
  opts.proxy_opts.url = proxy;

  QVector<char *> raw;
  QVector<QByteArray> storage;
  foreach (const QString &refspec, refspecs) {
    storage.append(refspec.toUtf8());
    raw.append(storage.last().data());
  }

  git_strarray array;
  array.strings = raw.data();
  array.count = raw.size();
  return git_remote_push(remote.data(), &array, &opts);
}

Result Remote::push(Callbacks *callbacks, const Reference &src,
                    const QString &dst, bool force, bool tags,
                    bool useConfiguredPushRefspecs) {
  if (useConfiguredPushRefspecs && dst.isEmpty()) {
    QStringList configuredRefspecs = pushRefspecs();
    if (!configuredRefspecs.isEmpty()) {
      if (force) {
        for (QString &refspec : configuredRefspecs) {
          if (!refspec.startsWith(QLatin1Char('+')))
            refspec.prepend(QLatin1Char('+'));
        }
      }

      if (tags) {
        Repository repo(git_remote_owner(d.data()));
        foreach (const TagRef &tag, repo.tags()) {
          QByteArray tagName = tag.qualifiedName().toUtf8();
          bool included = false;
          foreach (const QString &refspec, configuredRefspecs) {
            QByteArray specBytes = refspec.toUtf8();
            git_refspec *parsed = nullptr;
            if (!git_refspec_parse(&parsed, specBytes.constData(), false)) {
              included |=
                  git_refspec_src_matches(parsed, tagName.constData()) != 0;
              git_refspec_free(parsed);
            }
            if (included)
              break;
          }

          if (!included) {
            QString refspec = tag.qualifiedName();
            if (force)
              refspec.prepend(QLatin1Char('+'));
            configuredRefspecs.append(refspec);
          }
        }
      }

      return push(callbacks, configuredRefspecs);
    }
  }

  Repository repo(git_remote_owner(d.data()));
  QString prefix = force ? "+" : QString();
  QString refspec = prefix + src.qualifiedName();
  if (!dst.isEmpty()) {
    refspec += ":" + dst;
  } else if (src.isTag()) {
    refspec += ":" + src.qualifiedName();
  } else {
    QString key = QString("branch.%1.merge").arg(src.name());
    QString upstream = repo.gitConfig().value<QString>(key);
    if (!upstream.isEmpty())
      refspec += ":" + upstream;
  }

  QStringList refspecs(refspec);
  if (tags) {
    // Add tags individually. Wildcard push refspecs aren't supported yet.
    foreach (const TagRef &tag, repo.tags())
      refspecs.append(prefix + tag.qualifiedName());
  }

  return push(callbacks, refspecs);
}

Remote::TagStatusResult Remote::tagStatus(Callbacks *callbacks,
                                          const Reference &tag) const {
  if (!isValid() || !callbacks || !tag.isTag())
    return {TagStatus::Unknown, Result(GIT_EINVALID)};

  git_repository *repo = git_remote_owner(d.data());
  git_remote *remote = nullptr;
  if (int error = git_remote_create_anonymous(&remote, repo, url().toUtf8()))
    return {TagStatus::Unknown, Result(error)};

  git_remote_callbacks opts = GIT_REMOTE_CALLBACKS_INIT;
  opts.sideband_progress = &Callbacks::sideband;
  opts.credentials = &Callbacks::credentials;
  opts.certificate_check = &Callbacks::certificate;
  opts.remote_ready = &Callbacks::remoteReady;
  opts.payload = callbacks;
  git_proxy_options proxyOpts = GIT_PROXY_OPTIONS_INIT;
  QByteArray proxy = proxyUrl(url(), proxyOpts.type);
  proxyOpts.url = proxy;

  const git_remote_head **heads = nullptr;
  size_t count = 0;
  int error = git_remote_connect(remote, GIT_DIRECTION_FETCH, &opts, &proxyOpts,
                                 nullptr);
  if (!error)
    error = git_remote_ls(&heads, &count, remote);
  if (error) {
    Result result(error);
    git_remote_disconnect(remote, &opts);
    git_remote_free(remote);
    return {TagStatus::Unknown, result};
  }

  const QByteArray tagName = tag.qualifiedName().toUtf8();
  const TagRef tagRef(tag);
  Id localTagId;
  if (Tag annotated = tagRef.tag(); annotated.isValid())
    localTagId = annotated.id();
  else
    localTagId = tag.target().id();
  const QByteArray targetName = tag.target().id().toString().toUtf8();
  git_oid target;
  if (git_oid_fromstrn(&target, targetName, targetName.size())) {
    git_remote_disconnect(remote, &opts);
    git_remote_free(remote);
    return {TagStatus::Unknown, Result(GIT_EINVALID)};
  }
  bool reachable = false;
  for (size_t i = 0; i < count; ++i) {
    const git_remote_head *head = heads[i];
    if (!qstrcmp(head->name, tagName.constData())) {
      git_remote_disconnect(remote, &opts);
      git_remote_free(remote);
      return {localTagId == Id(&head->oid) ? TagStatus::Present
                                           : TagStatus::Conflict,
              Result(0)};
    }
    if (!qstrncmp(head->name, "refs/heads/", 11) &&
        (!git_oid_cmp(&target, &head->oid) ||
         git_graph_descendant_of(repo, &head->oid, &target) == 1))
      reachable = true;
  }

  git_remote_disconnect(remote, &opts);
  git_remote_free(remote);
  return {reachable ? TagStatus::Pushable : TagStatus::TargetLocalOnly,
          Result(0)};
}

Result Remote::clone(Callbacks *callbacks, const QString &url,
                     const QString &path, bool bare) {
  git_repository *repo = nullptr;
  git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
#ifndef USE_SYSTEM_LIBGIT2
  opts.fetch_opts.callbacks.connected = &Remote::Callbacks::connected;
  opts.fetch_opts.callbacks.about_to_disconnect =
      &Remote::Callbacks::about_to_disconnect;
#endif
  opts.fetch_opts.callbacks.sideband_progress = &Remote::Callbacks::sideband;
  opts.fetch_opts.callbacks.credentials = &Remote::Callbacks::credentials;
  opts.fetch_opts.callbacks.certificate_check = &Remote::Callbacks::certificate;
  opts.fetch_opts.callbacks.transfer_progress = &Remote::Callbacks::transfer;
  opts.fetch_opts.callbacks.update_tips = &Remote::Callbacks::update;
  opts.fetch_opts.callbacks.remote_ready = &Remote::Callbacks::remoteReady;
  opts.fetch_opts.callbacks.payload = callbacks;
  opts.bare = bare;

  QByteArray proxy = proxyUrl(url, opts.fetch_opts.proxy_opts.type);
  opts.fetch_opts.proxy_opts.url = proxy;

  return git_clone(&repo, url.toUtf8(), path.toUtf8(), &opts);
}

QByteArray Remote::proxyUrl(const QString &url, git_proxy_t &type) {
  type = GIT_PROXY_AUTO;

  QUrl tmp(url);
  QNetworkProxyQuery query(tmp);
  QList<QNetworkProxy> proxies =
      QNetworkProxyFactory::systemProxyForQuery(query);
  if (proxies.isEmpty())
    return QByteArray();

  QNetworkProxy proxy = proxies.first();
  if (proxy.type() == QNetworkProxy::NoProxy)
    type = GIT_PROXY_NONE;

  QString host = proxy.hostName();
  if (host.isEmpty())
    return QByteArray();

  type = GIT_PROXY_SPECIFIED;
  return QString("http://%1:%2").arg(host).arg(proxy.port()).toUtf8();
}

bool Remote::isLoggingEnabled() { return QSettings().value(kLogKey).toBool(); }

void Remote::setLoggingEnabled(bool enable) {
  QSettings().setValue(kLogKey, enable);
}

void Remote::log(const QString &text) {
  if (!isLoggingEnabled())
    return;

  QString name = QCoreApplication::applicationName();
  QDir tempDir = QDir::temp();
  tempDir.mkpath(name);
  tempDir.cd(name);

  QFile file(tempDir.filePath("remote.log"));
  if (!file.open(QFile::WriteOnly | QIODevice::Append))
    return;

  QString time = QTime::currentTime().toString(Qt::ISODateWithMs);
  QTextStream(&file) << time << " - " << text << Qt::endl;
}

} // namespace git

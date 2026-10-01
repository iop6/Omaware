// SPDX-License-Identifier: GPL-3.0-or-later
#include "agenttransfer.h"
#include "paths.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QUuid>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
struct Fd { int value; ~Fd() { if (value >= 0) ::close(value); } };
int directory() {
    const auto root = QFile::encodeName(Paths::root());
    QDir().mkpath(Paths::root());
    Fd parent{::open(root.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (parent.value < 0) return -1;
    ::mkdirat(parent.value, "transfers", 0700);
    int fd = ::openat(parent.value, "transfers", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat s{};
    if (fd >= 0 && (::fstat(fd, &s) || s.st_uid != ::getuid() || (s.st_mode & 0077))) { ::close(fd); return -1; }
    return fd;
}
bool regular(int fd, bool bounded) {
    struct stat s{};
    return fd >= 0 && !::fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_uid == ::getuid() && s.st_nlink == 1 && (!bounded || s.st_size <= AgentTransfer::limit);
}
bool secret(const QString &name) {
    return QRegularExpression("(^\\.|password|passwd|shadow|secret|credential|token|id_rsa|id_ed25519|\\.(pem|key|p12|pfx)$)", QRegularExpression::CaseInsensitiveOption).match(name).hasMatch();
}
}
bool AgentTransfer::validName(const QString &name) {
    return QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$").match(name).hasMatch() && !secret(name);
}
bool AgentTransfer::validGuestPath(const QString &path) {
    if (!path.startsWith('/') || path.size() > 1024 || path.contains(QChar::Null) || path.contains('\n') || path.contains('\r') || QDir::cleanPath(path) != path) return false;
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) if (part == ".." || secret(part)) return false;
    return path != "/" && !path.startsWith("/proc/") && !path.startsWith("/sys/") && !path.startsWith("/dev/");
}
bool AgentTransfer::read(const QString &name, QByteArray &bytes, QString &error) {
    error = "Upload requires a regular, single-link, user-owned file of at most 32 KiB in OmaWare's private transfers directory.";
    if (!validName(name)) return false;
    Fd dir{directory()}; if (dir.value < 0) return false;
    Fd fd{::openat(dir.value, QFile::encodeName(name).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
    if (!regular(fd.value, true)) return false;
    QFile file; if (!file.open(fd.value, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return false;
    bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit) return false;
    error.clear(); return true;
}
bool AgentTransfer::write(const QString &name, const QByteArray &bytes, bool overwrite, QString &error) {
    error = "Download destination exists or is unsafe; use a plain file name in OmaWare's private transfers directory.";
    if (!validName(name) || bytes.size() > limit) return false;
    Fd dir{directory()}; if (dir.value < 0) return false;
    const auto target = QFile::encodeName(name);
    struct stat s{};
    if (::fstatat(dir.value, target.constData(), &s, AT_SYMLINK_NOFOLLOW) == 0 && (!overwrite || !S_ISREG(s.st_mode) || s.st_nlink != 1 || s.st_uid != ::getuid())) return false;
    const auto temporary = (".transfer-" + QUuid::createUuid().toString(QUuid::Id128)).toLatin1();
    Fd fd{::openat(dir.value, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (fd.value < 0) return false;
    QFile file; bool ok = file.open(fd.value, QIODevice::WriteOnly, QFileDevice::DontCloseHandle) && file.write(bytes) == bytes.size() && file.flush() && ::fsync(fd.value) == 0;
    if (ok) {
        if (overwrite) ok = ::renameat(dir.value, temporary.constData(), dir.value, target.constData()) == 0;
        else ok = ::linkat(dir.value, temporary.constData(), dir.value, target.constData(), 0) == 0;
    }
    ::unlinkat(dir.value, temporary.constData(), 0);
    if (ok) error.clear();
    return ok;
}
QString AgentTransfer::command(const QString &direction, const QString &guest, const QByteArray &bytes, bool overwrite) {
    // All interpolated strings are base64. Walk directory components with no-follow descriptors,
    // so neither a guest symlink nor a shell metacharacter can redirect the transfer.
    QString script = "import os,stat,base64\np=base64.b64decode('" + QString::fromLatin1(guest.toUtf8().toBase64()) + "').decode()\nparts=p.split('/')[1:]\nd=os.open('/',os.O_RDONLY|os.O_DIRECTORY)\nfor part in parts[:-1]:\n n=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=d)\n os.close(d)\n d=n\n";
    if (direction == "upload") {
        script += "b=base64.b64decode('" + QString::fromLatin1(bytes.toBase64()) + "')\n";
        // Existing files are checked before truncation; no-follow excludes symlinks.
        script += "f=os.open(parts[-1],os.O_WRONLY|os.O_CREAT|os.O_NONBLOCK|os.O_NOFOLLOW" + QString(overwrite ? "" : "|os.O_EXCL") + ",0o600,dir_fd=d)\ns=os.fstat(f)\nassert stat.S_ISREG(s.st_mode) and s.st_nlink==1\nos.ftruncate(f,0)\nwith os.fdopen(f,'wb') as out: out.write(b)\n";
    } else {
        script += "f=os.open(parts[-1],os.O_RDONLY|os.O_NONBLOCK|os.O_NOFOLLOW,dir_fd=d)\ns=os.fstat(f)\nassert stat.S_ISREG(s.st_mode) and s.st_nlink==1 and s.st_size<=32768\nwith os.fdopen(f,'rb') as inp: b=inp.read(32769)\nassert len(b)<=32768\nprint(base64.b64encode(b).decode())\n";
    }
    return "python3 -I -c \"import base64;exec(base64.b64decode('" + QString::fromLatin1(script.toUtf8().toBase64()) + "'))\"";
}

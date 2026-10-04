// SPDX-License-Identifier: GPL-3.0-or-later
// Safe host-only transfer regression tests. All files live in a temporary directory; no VMs are contacted.
#include "agenttransfer.h"
#include "paths.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstdio>

void check(bool ok, const char *name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) std::exit(1);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir("/tmp/omaware-transfer-check-XXXXXX");
    check(dir.isValid(), "temporary directory");
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    QByteArray bytes;
    for (int i = 0; i < 32768; ++i)
        bytes.append(char(i % 256));
    QString error;
    QByteArray read;
    check(AgentTransfer::write("data.bin", bytes, false, error), "maximum-size host write");
    check(AgentTransfer::read("data.bin", read, error) && read == bytes, "maximum-size host roundtrip");
    check(!AgentTransfer::write("data.bin", "replacement", false, error), "host overwrite denied by default");
    check(AgentTransfer::read("data.bin", read, error) && read == bytes, "existing host content preserved");
    check(!AgentTransfer::write("too-big.bin", bytes + 'x', false, error), "oversized host write refused");
    check(!AgentTransfer::validName("../outside") && !AgentTransfer::validName("/etc/passwd") &&
                    !AgentTransfer::validName("id_ed25519"),
            "unsafe host names rejected");
    auto transfers = Paths::root() + "/transfers/";
    check(::symlink("data.bin", QFile::encodeName(transfers + "sym.bin")) == 0, "symlink fixture");
    check(!AgentTransfer::read("sym.bin", read, error) && !AgentTransfer::write("sym.bin", "x", true, error),
            "host symlink refused");
    check(::link(QFile::encodeName(transfers + "data.bin"), QFile::encodeName(transfers + "hard.bin")) == 0,
            "hardlink fixture");
    check(!AgentTransfer::read("hard.bin", read, error), "host hardlink refused");
    check(::mkfifo(QFile::encodeName(transfers + "fifo.bin"), 0600) == 0, "fifo fixture");
    check(!AgentTransfer::read("fifo.bin", read, error), "host FIFO refused without blocking");
    auto guest = dir.path() + "/guest.bin";
    auto run = [&](QString command, QByteArray &out) {
        QProcess p;
        p.start("/bin/sh", {"-c", command});
        if (!p.waitForFinished(10000)) {
            p.kill();
            p.waitForFinished();
            return -99;
        }
        out = p.readAllStandardOutput();
        return p.exitCode();
    };
    QByteArray out;
    check(run(AgentTransfer::command("upload", guest, bytes, false), out) == 0, "maximum-size guest upload");
    check(run(AgentTransfer::command("download", guest, {}, false), out) == 0 &&
                    QByteArray::fromBase64(out.trimmed()) == bytes,
            "binary guest roundtrip");
    check(run(AgentTransfer::command("upload", guest, "replacement", false), out) != 0,
            "guest overwrite denied by default");
    check(run(AgentTransfer::command("upload", guest, "replacement", true), out) == 0, "explicit guest overwrite");
    check(::symlink("guest.bin", QFile::encodeName(dir.path() + "/guest-sym.bin")) == 0, "guest symlink fixture");
    check(run(AgentTransfer::command("upload", dir.path() + "/guest-sym.bin", "x", true), out) != 0,
            "guest symlink upload refused");
    check(run(AgentTransfer::command("download", dir.path() + "/guest-sym.bin", {}, false), out) != 0,
            "guest symlink download refused");
    auto quoted = dir.path() + "/quote'$(touch INJECTED).bin";
    check(AgentTransfer::validGuestPath(quoted), "quoted guest path supported");
    check(run(AgentTransfer::command("upload", quoted, "quoted", false), out) == 0 && QFile::exists(quoted),
            "shell metacharacters remain literal");
    check(!AgentTransfer::validGuestPath("/etc/shadow") && !AgentTransfer::validGuestPath("/proc/self/mem") &&
                    !AgentTransfer::validGuestPath("/tmp/../etc/passwd"),
            "restricted guest paths rejected");
}

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a disposable QGA test guest on a disposable test machine; no packages changed."""
import os, pathlib, re, shutil, socket, subprocess, tempfile
assert os.environ.get('OMAWARE_VM_TEST_HOST') == socket.gethostname(), 'Set OMAWARE_VM_TEST_HOST to this disposable machine\'s hostname.'
base = pathlib.Path(__file__).resolve().parents[2] / 'artifacts/agent-fixture'
base.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='omaware-agent-') as tmp:
    root = pathlib.Path(tmp)
    for directory in ('bin', 'dev', 'proc', 'sys', 'tmp', 'run', 'run/qga', 'var/run', 'data', 'etc'):
        (root / directory).mkdir(parents=True, exist_ok=True)
    shutil.copy('/usr/bin/busybox', root / 'bin/busybox')
    for name in ('sh', 'mount', 'sleep', 'sync', 'mkdir', 'cat', 'dmesg', 'poweroff'):
        (root / 'bin' / name).symlink_to('busybox')
    binaries = ['/usr/sbin/qemu-ga']
    deps = subprocess.check_output(['ldd', binaries[0]], text=True)
    binaries += re.findall(r'(/[^\s()]+)', deps)
    for name in set(binaries):
        dest = root / name.lstrip('/'); dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(name, dest)
    # This guest kernel has the required virtio and ext4 drivers built in.
    for module in ('virtio_pci', 'virtio_blk', 'virtio_console', 'ext4'):
        assert subprocess.check_output(['modprobe', '--show-depends', module], text=True).startswith('builtin ')
    (root / 'init').write_text('''#!/bin/sh
export PATH=/bin:/usr/sbin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mkdir -p /dev/virtio-ports
for i in /sys/class/virtio-ports/*; do
    [ -e "$i/name" ] && ln -s /dev/$(basename "$i") /dev/virtio-ports/$(cat "$i/name")
done
mount -t ext4 /dev/vda /data
( i=0; while true; do i=$((i+1)); echo "$i" > /data/counter; sync; sleep 0.1; done ) &
/usr/sbin/qemu-ga -p /dev/virtio-ports/org.qemu.guest_agent.0 -t /run/qga -f /run/qga.pid &
while true; do sleep 1; done
''')
    # busybox applets used by the init script.
    for name in ('ln', 'basename'):
        (root / 'bin' / name).symlink_to('busybox')
    (root / 'init').chmod(0o755)
    with open(base / 'initrd.cpio.gz', 'wb') as output:
        subprocess.run('find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1', shell=True, cwd=root, stdout=output, check=True)
print(base)

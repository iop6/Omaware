#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicitly invoked with pkexec to authorize OmaWare-owned active bridges (one or more network UUIDs,
so building a lab asks for the password once).

No polkit policy is installed. Every invocation uses the system's existing
administrator authentication for pkexec. No physical interface is reconfigured.
OmaWare only runs a root-owned installed copy (install -o root -m 0755, or
cmake --install --component helper), and -I keeps Python from importing modules
from the script's directory or environment.

Refusals exit with a code OmaWare turns into a stable reason (src/bridgehelper.cpp):
3 network not OmaWare's, 4 bridge not active, 5 deny rule, 6 unsafe bridge policy
files, 7 no usable qemu-bridge-helper. Other failures exit with 1 or 2.
"""
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET

NOT_OWNED, INACTIVE, DENIED, UNSAFE_POLICY, NO_BRIDGE_HELPER = 3, 4, 5, 6, 7


def fail(code, message):
    print(message, file=sys.stderr)
    raise SystemExit(code)


if os.geteuid() != 0 or not 2 <= len(sys.argv) <= 17:
    raise SystemExit('Administrator authorization and one to sixteen network UUIDs are required.')
grants = []
for argument in sys.argv[1:]:
    identity = str(uuid.UUID(argument))
    raw = subprocess.check_output(['/usr/bin/virsh', '-c', 'qemu:///system', 'net-dumpxml', identity])
    root = ET.fromstring(raw)
    if not root.findtext('name', '').startswith('omaware-') or root.find('metadata/{https://omaware.org/xmlns/network/1}managed') is None:
        fail(NOT_OWNED, 'The selected network is not managed by OmaWare.')
    bridge = root.find('bridge').get('name', '') if root.find('bridge') is not None else ''
    if not bridge.startswith('oma') or len(bridge) > 15 or not bridge.isalnum() or not Path('/sys/class/net', bridge, 'bridge').is_dir():
        fail(INACTIVE, 'The managed bridge is not active. Start the network first.')
    grants.append((identity, bridge))
helper = next((p for p in (Path('/usr/lib/qemu/qemu-bridge-helper'), Path('/usr/libexec/qemu-bridge-helper')) if p.exists()), None)
if helper is None or helper.is_symlink() or helper.stat().st_uid != 0 or helper.stat().st_mode & 0o022:
    fail(NO_BRIDGE_HELPER, 'A root-owned, non-writable QEMU bridge helper is required.')
directory = Path('/etc/qemu')
if directory.is_symlink() or (directory.exists() and (directory.stat().st_uid != 0 or directory.stat().st_mode & 0o022)):
    fail(UNSAFE_POLICY, 'Unexpected bridge configuration directory ownership or permissions.')
directory.mkdir(mode=0o755, exist_ok=True)
acl = directory / 'bridge.conf'
if acl.is_symlink() or (acl.exists() and (acl.stat().st_uid != 0 or acl.stat().st_mode & 0o022)):
    fail(UNSAFE_POLICY, 'Unexpected bridge ACL ownership or permissions.')
old = acl.read_bytes() if acl.exists() else b''
if len(old) > 65536:
    fail(UNSAFE_POLICY, 'The bridge ACL is too large to update.')
lines = old.decode().splitlines()
def check_rules(lines, visited):
    if len(visited) > 16:
        fail(UNSAFE_POLICY, 'Too many included bridge ACL files. Ask the host administrator to review its policy.')
    for line in lines:
        words = line.split('#', 1)[0].split()
        if len(words) != 2:
            continue
        rule, value = words
        if rule == 'deny' and (value == 'all' or value in (bridge for _, bridge in grants)):
            fail(DENIED, 'An existing deny rule blocks this bridge. Ask the host administrator to review its policy.')
        if rule == 'include':
            included = Path(value)
            if not included.is_absolute() or included in visited or included.is_symlink() or not included.is_file():
                fail(UNSAFE_POLICY, 'An included bridge ACL could not be safely checked. Ask the host administrator to review its policy.')
            if included.stat().st_uid != 0 or included.stat().st_mode & 0o022 or included.stat().st_size > 65536:
                fail(UNSAFE_POLICY, 'Unexpected included bridge ACL ownership, permissions or size.')
            check_rules(included.read_text().splitlines(), visited | {included})
check_rules(lines, {acl})
existing = {line.strip() for line in lines}
added = b''.join(b'\n# OmaWare network ' + identity.encode() + b'\nallow ' + bridge.encode() + b'\n'
                 for identity, bridge in grants if 'allow ' + bridge not in existing)
if added:
    with tempfile.NamedTemporaryFile(dir=directory, prefix='.omaware-', delete=False) as file:
        temp = Path(file.name)
        file.write(old + added)
        file.flush()
        os.fsync(file.fileno())
    try:
        temp.chmod(stat.S_IMODE(acl.stat().st_mode) if acl.exists() else 0o644)
        os.replace(temp, acl)
    finally:
        temp.unlink(missing_ok=True)
helper.chmod(stat.S_IMODE(helper.stat().st_mode) | stat.S_ISUID)
print('Authorized ' + ', '.join(bridge for _, bridge in grants) + ' for user-session VMs.')

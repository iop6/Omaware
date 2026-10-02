# SPDX-License-Identifier: GPL-3.0-or-later
# Protocol-only subprocess test; isolated runtime prevents access to any running app.
import json
import os
import subprocess
import sys
import tempfile

binary = sys.argv[1]
expected = {
    'omaware_overview', 'propose_lab', 'lab_status', 'delete_lab', 'vm_power',
    'screenshot', 'vm_input', 'type_login', 'run_command', 'list_snapshots',
    'snapshot_vm', 'restore_snapshot', 'set_cable', 'vm_details', 'transfer_file',
    'wait_for_vm', 'update_vm_resources', 'diagnose_vm', 'clone_vm', 'manage_iso',
    'manage_network_adapter',
    'list_installation_media', 'list_owned_networks', 'create_vm', 'create_network',
    'authorize_network', 'provision_status',
}
requests = [
    {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
        'protocolVersion': '2025-06-18', 'capabilities': {},
        'clientInfo': {'name': 'hermes-smoke', 'version': '1'}}},
    {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
    {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
    {'jsonrpc': '2.0', 'id': 3, 'method': 'ping'},
    {'jsonrpc': '2.0', 'id': 4, 'method': 'not_a_method'},
    {'jsonrpc': '2.0', 'id': 5, 'method': 'tools/call', 'params': {
        'name': 'not_a_tool', 'arguments': {}}},
    {'jsonrpc': '2.0', 'id': 6, 'method': 'tools/call', 'params': {
        'name': 'omaware_overview', 'arguments': {}}},
    {'jsonrpc': '2.0', 'id': 7, 'method': 'tools/call', 'params': {
        'name': 'create_vm', 'arguments': {'source': '/etc/passwd'}}},
    {'jsonrpc': '2.0', 'id': 8, 'method': 'tools/call', 'params': {
        'name': 'provision_status', 'arguments': {
            'request_id': '10000000-0000-4000-8000-000000000001'}}},
]
# A fresh runtime directory ensures no tool call can reach the user's app or VMs.
with tempfile.TemporaryDirectory(prefix='omaware-mcp-smoke-') as runtime:
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime)
    p = subprocess.run([binary, 'mcp'], input=''.join(json.dumps(r) + '\n' for r in requests),
                       text=True, capture_output=True, env=env, timeout=20)
assert p.returncode == 0, (p.returncode, p.stderr)
replies = [json.loads(line) for line in p.stdout.splitlines()]
assert len(replies) == 8, replies
by_id = {r['id']: r for r in replies}
assert by_id[1]['result']['serverInfo']['name'] == 'omaware'
assert by_id[1]['result']['protocolVersion'] == '2025-06-18'
tools = by_id[2]['result']['tools']
names = {t['name'] for t in tools}
assert expected <= names, 'Missing tools: ' + str(expected - names)
assert len(names) == len(tools), 'Duplicate tool names'
for t in tools:
    schema = t['inputSchema']
    assert schema['type'] == 'object', t['name']
    assert set(schema.get('required', [])) <= set(schema.get('properties', {})), t['name']
assert by_id[3]['result'] == {}
assert by_id[4]['error']['code'] == -32601
assert by_id[5]['error']['code'] == -32602
assert by_id[6]['result']['isError'] is True
assert by_id[6]['result']['structuredContent']['code'] == 'app_unavailable'
assert by_id[7]['error']['code'] == -32602  # invalid provisioning never forwarded
assert by_id[8]['result']['structuredContent']['code'] == 'app_unavailable'
print('PASS: initialization, notification handling, ping, tool schemas, unknown method/tool, unavailable-app error')
print('Server:', by_id[1]['result']['serverInfo'])
print('Tools:', len(tools), ', '.join(sorted(names)))
print('Unavailable-app response:', json.dumps(by_id[6]['result']))

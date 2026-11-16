#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare private Milvus standalone configuration for the small RPC example."""
import argparse
import json
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream-config', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--rpc-port', type=int, default=19536)
    parser.add_argument('--etcd-port', type=int, default=23796)
    parser.add_argument('--etcd-peer-port', type=int, default=23896)
    args = parser.parse_args()
    ports = [args.rpc_port, args.etcd_port, args.etcd_peer_port]
    if len(set(ports)) != 3 or not all(1024 < port < 65536 for port in ports):
        parser.error('choose three distinct unprivileged TCP ports')
    if not (args.upstream_config/'milvus.yaml').is_file():
        parser.error('upstream-config must contain the pinned distribution milvus.yaml')
    root = args.directory.resolve()
    root.mkdir(parents=True, mode=0o700, exist_ok=False)
    config = root/'configs'
    config.mkdir()
    for name in ('milvus.yaml', 'hook.yaml', 'glog.conf'):
        source = args.upstream_config/name
        if source.is_file():
            shutil.copyfile(source, config/name)
    data = root/'data'
    data.mkdir()
    etcd = {
        'name': 'default',
        'listen-client-urls': 'http://127.0.0.1:'+str(args.etcd_port),
        'advertise-client-urls': 'http://127.0.0.1:'+str(args.etcd_port),
        'listen-peer-urls': 'http://127.0.0.1:'+str(args.etcd_peer_port),
        'initial-advertise-peer-urls': 'http://127.0.0.1:'+str(args.etcd_peer_port),
        'initial-cluster': 'default=http://127.0.0.1:'+str(args.etcd_peer_port),
        'data-dir': str(data/'etcd'),
    }
    (config/'etcd.yaml').write_text(json.dumps(etcd, indent=2)+'\n')
    override = {
        'etcd': {'endpoints': ['127.0.0.1:'+str(args.etcd_port)],
                 'rootPath': 'nvfuse-release-demo', 'use': {'embed': False}},
        'mq': {'type': 'rocksmq'}, 'rocksmq': {'path': str(data/'rocksmq')},
        'common': {'storageType': 'local'},
        'localStorage': {'path': str(data/'local')},
        'queryNode': {'enableDisk': True}, 'dataNode': {'enableDisk': True},
        'proxy': {'ip': '127.0.0.1', 'port': args.rpc_port},
        'log': {'level': 'warn', 'rootPath': str(root/'logs')},
    }
    # JSON objects are valid YAML and avoid a configuration-time PyYAML dependency.
    (config/'user.yaml').write_text(json.dumps(override, indent=2)+'\n')
    print(json.dumps({'MILVUSCONF': str(config), 'DEPLOY_MODE': 'STANDALONE',
                      'uri': 'http://127.0.0.1:'+str(args.rpc_port),
                      'etcd_command': ['etcd', '--config-file', str(config/'etcd.yaml')]}, indent=2))


if __name__ == '__main__':
    main()

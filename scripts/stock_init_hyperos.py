"""校验固定 HyperOS 原 init 清单，准备原盘挂载与元数据检查输入。"""
import json
from pathlib import Path
import shutil

from inspect_hyperos_input import digest, require

MANIFEST_SHA256 = '1ece2c7f87a9064a28a823309476d10044a795bad25b887e5e07a8859137ad06'
SOURCE_COMMIT = '9d7ddd3f046e89d33cb205b5fbbf6dc5b728155c'


def prepare(manifest, output):
    require(digest(manifest) == MANIFEST_SHA256, '固定 HyperOS init 清单摘要不匹配')
    source = json.loads(manifest.read_text())
    require(source['prepared'] and source['source_identity']['source_commit'] == SOURCE_COMMIT,
            '固定 HyperOS init 来源不同')
    for record in (*source['files'].values(), *source['apex_files'].values(), *source['images'].values()):
        path = Path(record.get('source', record.get('path')))
        require(path.is_file() and path.stat().st_size == record['bytes'] and digest(path) == record['sha256'],
                '原 HyperOS 文件或分区摘要不匹配：' + str(path))
    for apex in source['apex']:
        require(digest(Path(apex['source'])) == apex['sha256'], '原 HyperOS APEX 容器摘要不匹配')
        if 'original_apex' in apex:
            require(digest(Path(apex['original_apex'])) == apex['original_apex_sha256'],
                    '原 HyperOS 内层 APEX 摘要不匹配')
        for record in apex['members'].values():
            require(digest(Path(record['path'])) == record['sha256'], '原 HyperOS APEX 成员摘要不匹配')
    original_tsv = manifest.with_name('files.tsv')
    require(digest(original_tsv) == source['files_tsv_sha256'], '原 HyperOS 部署清单摘要不匹配')
    lines = [line for line in original_tsv.read_text().splitlines() if not line.startswith('L\t')]
    expected = {path: dict(source['files'][path]['original_metadata'], inode_type=0o100000)
                for path in ('/system/bin/init', '/product/etc/selinux/product_sepolicy.cil',
                             '/system_ext/etc/selinux/system_ext_sepolicy.cil',
                             '/vendor/etc/selinux/vendor_sepolicy.cil', '/init.environ.rc')}
    expected['/system_dlkm'] = source['system_dlkm_root_metadata']
    for path, metadata in sorted(source['root_links'].items()):
        label = metadata['selinux']
        require(bytes.fromhex(metadata['selinux_xattr_hex']) == label.encode() + b'\0',
                '原根链接标签编码不同：' + path)
        lines.append('L\t%d\t%d\t%s\t%s\t%s' %
                     (metadata['uid'], metadata['gid'], path, metadata['link_target'], label))
        expected[path] = dict(metadata, mode=0o777, inode_type=0o120000)
    (output / 'files.tsv').write_text('\n'.join(lines) + '\n')
    metadata_lines = []
    for path, metadata in sorted(expected.items()):
        require(metadata['selinux_xattr_hex'], '原元数据缺少 SELinux 标签：' + path)
        metadata_lines.append('%o\t%o\t%d\t%d\t%s\t%s' %
                              (metadata['inode_type'], metadata['mode'], metadata['uid'], metadata['gid'],
                               path, metadata['selinux_xattr_hex']))
    (output / 'original-metadata.tsv').write_text('\n'.join(metadata_lines) + '\n')
    target = output / 'root/init.environ.rc'
    target.parent.mkdir(parents=True, exist_ok=True)
    record = source['files']['/init.environ.rc']
    shutil.copyfile(record['source'], target)
    target.chmod(record['mode'])
    return {'hyperos_manifest': str(manifest), 'hyperos_manifest_sha256': MANIFEST_SHA256,
            'source_identity': source['source_identity'], 'files': source['files'],
            'closure': {'files': len(source['bootstrap_closure']['files']),
                        'bytes': sum(source['files'][path]['bytes'] for path in source['bootstrap_closure']['files'])},
            'deployed_system_partition': source['images']['system'],
            'original_vendor': source['images']['vendor'], 'original_system_dlkm': source['images']['system_dlkm'],
            'original_root_links': source['root_links'], 'expected_runtime_metadata': expected,
            'files_tsv_sha256': digest(output / 'files.tsv'),
            'original_metadata_tsv_sha256': digest(output / 'original-metadata.tsv'),
            'missing_service_binaries': source['missing_service_binaries'],
            'limitations': ['Mounted original partitions precede selinux_setup; first-stage ramdisk remains unexecuted.',
                            'Root symlink creation modes are 0777; original inode modes remain in the fixed source manifest.',
                            'Original userdata, encryption metadata and boot-property setup remain unprepared.']}

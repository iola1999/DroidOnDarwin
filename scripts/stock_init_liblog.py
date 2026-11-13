#!/usr/bin/env python3
"""校验可选的 SDK36 四向量 FATAL 记录及结束汇总。"""

PAYLOAD_MAX = 4068
TAG_MAX = 128
CAPTURE_MAX = 2048
RECORD_MAX = 512
FATAL_FIELDS = {
    'stock_init_liblog_fatal', 'log_index', 'pid', 'tid', 'header_tid', 'log_id',
    'sec', 'nsec', 'fd', 'write_result', 'tag_bytes', 'message_bytes',
    'captured_bytes', 'tag_hex', 'message_hex',
}
SUMMARY_FIELDS = {
    'stock_init_liblog_summary', 'seen', 'recorded', 'omitted',
    'capture_limit_bytes', 'record_limit',
}


def _integer(value, minimum=0, maximum=(1 << 64) - 1):
    return type(value) is int and minimum <= value <= maximum


def _hex_bytes(value, length):
    if type(value) is not str or len(value) != 2 * length:
        return None
    try:
        data = bytes.fromhex(value)
    except ValueError:
        return None
    return data if data.hex() == value else None


def validate_liblog(rows, required=False):
    """required=True 要求一份结束汇总；允许其匹配数为零。"""
    fatal = [(index, row) for index, row in enumerate(rows) if 'stock_init_liblog_fatal' in row]
    summaries = [(index, row) for index, row in enumerate(rows) if 'stock_init_liblog_summary' in row]
    if not fatal and not summaries:
        return not required
    if len(summaries) != 1:
        return False
    position, summary = summaries[0]
    if (set(summary) != SUMMARY_FIELDS or summary['stock_init_liblog_summary'] is not True or
            any(not _integer(summary[key]) for key in ('seen', 'recorded', 'omitted', 'capture_limit_bytes', 'record_limit')) or
            summary['capture_limit_bytes'] != CAPTURE_MAX or summary['record_limit'] != RECORD_MAX or
            summary['recorded'] != len(fatal) or summary['recorded'] != min(summary['seen'], RECORD_MAX) or
            summary['omitted'] != summary['seen'] - summary['recorded']):
        return False
    for expected, (index, row) in enumerate(fatal, 1):
        if (set(row) != FATAL_FIELDS or row['stock_init_liblog_fatal'] is not True or index >= position or
                not _integer(row['log_index'], 1, RECORD_MAX) or row['log_index'] != expected or
                any(not _integer(row[key], 1, (1 << 63) - 1) for key in ('pid', 'tid')) or
                not _integer(row['header_tid'], 0, (1 << 16) - 1) or row['header_tid'] != row['tid'] & 0xffff or
                not _integer(row['log_id'], 0, 7) or row['log_id'] not in (0, 1, 3, 4, 7) or
                not _integer(row['sec'], 0, (1 << 32) - 1) or not _integer(row['nsec'], 0, 999999999) or
                not _integer(row['fd'], 0, (1 << 63) - 1) or
                not _integer(row['tag_bytes'], 1, TAG_MAX) or not _integer(row['message_bytes'], 1, PAYLOAD_MAX) or
                not _integer(row['captured_bytes'], 1, CAPTURE_MAX) or
                row['captured_bytes'] != min(row['message_bytes'], CAPTURE_MAX)):
            return False
        payload = 1 + row['tag_bytes'] + row['message_bytes']
        if payload > PAYLOAD_MAX or not _integer(row['write_result'], -4095, 11 + payload):
            return False
        tag = _hex_bytes(row['tag_hex'], row['tag_bytes'])
        message = _hex_bytes(row['message_hex'], row['captured_bytes'])
        if tag is None or message is None or tag[-1] != 0 or b'\0' in tag[:-1]:
            return False
        if row['message_bytes'] == row['captured_bytes']:
            if message[-1] != 0 or b'\0' in message[:-1]:
                return False
        elif b'\0' in message:
            return False
    return True

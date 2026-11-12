#!/usr/bin/env python3
"""核对原 init 的用户停止、受限宿主停止与回收结果。"""


def validate(rows, observation, mode, required=False):
    pid = observation['ancestor_pid']
    stopped = observation['deadline_stop']
    killed = observation['kill_result']
    requests = [row for row in rows if 'stock_init_stop_request' in row]
    host = [row for row in rows if 'stock_init_host_stop' in row]
    reaped = [row for row in rows if 'stock_init_host_stop_reaped' in row]
    boundaries = [row for row in rows if 'stock_init_host_stop_boundaries' in row]
    owned = [row for row in rows if 'stock_init_owned_ordinary' in row]
    ready = [row for row in rows if 'stock_init_ordinary_ready' in row]
    current = required or bool(host or reaped or boundaries or owned or ready)
    if current:
        if (reaped != [{'stock_init_host_stop_reaped': True, 'ancestor_pid': pid, 'result': -3}] or
                reaped[0]['stock_init_host_stop_reaped'] is not True or
                any(type(reaped[0][key]) is not int for key in ('ancestor_pid', 'result'))):
            return False
        if mode == 'lifecycle':
            expected = {'stock_init_host_stop_boundaries', 'ancestor_pid', 'invalid_pid_results',
                        'extra_argument_result', 'missing_pid_result', 'owner_tid', 'host_task_result',
                        'peer_tid', 'peer_result'}
            if len(boundaries) != 1 or set(boundaries[0]) != expected:
                return False
            row = boundaries[0]
            if (row['stock_init_host_stop_boundaries'] is not True or row['ancestor_pid'] != pid or
                    any(type(row[key]) is not int for key in expected - {'stock_init_host_stop_boundaries', 'invalid_pid_results'}) or
                    type(row['invalid_pid_results']) is not list or row['invalid_pid_results'] != [-22, -22, -22] or
                    any(type(value) is not int for value in row['invalid_pid_results']) or
                    row['extra_argument_result'] != -22 or row['missing_pid_result'] != -3 or
                    row['host_task_result'] != -1 or row['peer_result'] != -1 or row['owner_tid'] <= 0 or
                    row['peer_tid'] <= 0 or row['owner_tid'] == row['peer_tid']):
                return False
            if len(owned) != 1:
                return False
            row = owned[0]
            fields = {'stock_init_owned_ordinary', 'pid', 'ready_pid', 'host_stop_result',
                      'alive_result', 'kill_result', 'waited', 'status'}
            if (set(row) != fields or row['stock_init_owned_ordinary'] is not True or
                    any(type(row[key]) is not int for key in fields - {'stock_init_owned_ordinary'}) or
                    row['pid'] <= 1 or row['pid'] == pid or row['ready_pid'] != row['pid'] or
                    row['host_stop_result'] != -1 or row['alive_result'] != 0 or
                    row['kill_result'] != 0 or row['waited'] != row['pid'] or row['status'] != 9 or
                    ready != [{'stock_init_ordinary_ready': True, 'pid': row['pid'], 'guest_stop_errno': 38}] or
                    ready[0]['stock_init_ordinary_ready'] is not True or
                    any(type(ready[0][key]) is not int for key in ('pid', 'guest_stop_errno'))):
                return False
        elif boundaries or owned or ready:
            return False
    if requests:
        if (requests != [{'stock_init_stop_request': True, 'ancestor_pid': pid, 'signal': 9, 'result': killed}] or
                requests[0]['stock_init_stop_request'] is not True or
                any(type(requests[0][key]) is not int for key in ('ancestor_pid', 'signal', 'result'))):
            return False
        if killed in (-13, -1):
            if len(host) != 1:
                return False
            row = host[0]
            if (set(row) != {'stock_init_host_stop', 'ancestor_pid', 'signal', 'result'} or
                    row['stock_init_host_stop'] is not True or row['ancestor_pid'] != pid or row['signal'] != 9 or
                    any(type(row[key]) is not int for key in ('ancestor_pid', 'signal', 'result')) or
                    row['result'] not in (0, -3) or stopped != (row['result'] == 0)):
                return False
        elif killed not in (0, -3) or host or stopped != (killed == 0):
            return False
    elif host or killed != 0 or (current and stopped):
        return False
    observed_at = rows.index(observation)
    if requests and rows.index(requests[0]) >= observed_at:
        return False
    if host and not rows.index(requests[0]) < rows.index(host[0]) < observed_at:
        return False
    if current:
        reaped_at = rows.index(reaped[0])
        if reaped_at <= observed_at:
            return False
        if mode == 'lifecycle' and (not requests or not
                rows.index(boundaries[0]) < rows.index(owned[0]) < rows.index(requests[0])):
            return False
        for index, row in enumerate(rows):
            if 'stock_init_data_cleanup' in row and index <= reaped_at:
                return False
    return True

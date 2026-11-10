#!/usr/bin/env python3
"""Запустить четыре реализации и проверить их по журналу событий."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'build'
MODES = ('fork-named', 'fork-unnamed', 'independent-shm', 'independent-mq')
CASES = ('ordinary', 'conflict', 'drought', 'overwater', 'interrupt')


def fields(line):
    return dict(part.split('=', 1) for part in line.split(' detail=', 1)[0].split() if '=' in part)


def check_log(log, case):
    lines = log.read_text().splitlines()
    assert sum(line.startswith('SUMMARY ') for line in lines) == 1 and lines[-1].startswith('SUMMARY ')
    totals = fields(lines[-1])
    assert totals['invariants'] == 'ok'
    pending = set()
    seen = set()
    flowerToRequest = {}
    requestToGardener = {}
    gardenerToChoice = {}
    flowerToDeathTick = {}
    events = []
    max_busy = 0
    last_tick = 0
    for line in lines[:-1]:
        event = fields(line)
        kind = event['event']
        f, g, r = (int(event[key]) for key in ('flower', 'gardener', 'request'))
        tick = int(event['t'])
        assert tick >= last_tick, 'время в журнале идёт назад'
        last_tick = tick
        if f in flowerToDeathTick:
            assert tick == flowerToDeathTick[f] and kind in ('CANCEL', 'DRIED'), 'погибший цветок снова активен'
        if kind == 'STATE' and ('detail=DRIED;' in line or 'detail=DEAD_OVERWATER;' in line):
            flowerToDeathTick[f] = tick
        events.append(kind)
        if kind == 'REQUEST':
            assert r not in seen and f not in flowerToRequest, 'повторная заявка'
            seen.add(r)
            pending.add(r)
            flowerToRequest[f] = r
        elif kind == 'CHOICE':
            assert r == min(pending) and g not in requestToGardener.values(), 'нарушен FIFO или садовник занят'
            gardenerToChoice[g] = r
        elif kind == 'ACCEPT':
            assert flowerToRequest[f] == r and gardenerToChoice[g] == r and g not in requestToGardener.values()
            pending.remove(r)
            requestToGardener[r] = g
        elif kind == 'WATER_START':
            assert requestToGardener[r] == g
            max_busy = max(max_busy, len(requestToGardener))
        elif kind == 'CONFLICT':
            assert gardenerToChoice[g] == r and requestToGardener[r] != g, 'конфликт без второго садовника'
        elif kind == 'COMPLETE' or kind == 'CANCEL':
            assert flowerToRequest.pop(f) == r
            if r in requestToGardener:
                assert requestToGardener.pop(r) == g
            else:
                assert kind == 'CANCEL'
                pending.remove(r)
    for key, value in [('created', len(seen)), ('completed', events.count('COMPLETE')),
                       ('cancelled', events.count('CANCEL')), ('pending', len(pending)),
                       ('assigned', len(requestToGardener)), ('conflicts', events.count('CONFLICT'))]:
        assert int(totals[key]) == value, key
    if case == 'interrupt':
        assert totals['reason'] == 'interrupted'
    elif case == 'overwater':
        assert totals['reason'] == 'all_inactive' and 'OVERWATER' in events and int(totals['alive']) == 0
    elif case == 'drought':
        assert 'DRIED' in events and 'CANCEL' in events
    else:
        assert totals['reason'] == 'time_limit' and 'RECOVERY_END' in events
        if case == 'conflict':
            assert max_busy >= 2, 'не было одновременного полива двух цветов'
    return int(totals['conflicts'])


def wait_for(path, marker, process):
    until = time.monotonic() + 10
    while time.monotonic() < until and process.poll() is None:
        if path.exists() and marker.encode() in path.read_bytes():
            return
        time.sleep(0.01)
    raise AssertionError('не дождались ' + marker)


def run(mode, case, folder):
    config = ROOT / 'configs' / (('ordinary' if case == 'interrupt' else case) + '.conf')
    keyToValue = dict(line.split('=', 1) for line in config.read_text().splitlines())
    garden = folder.name
    env = dict(os.environ, HSE_GARDEN=garden)
    independent = mode.startswith('independent-')
    args = [BUILD / mode / 'server' if independent else BUILD / mode]
    log, stdout_path = folder / 'events.log', folder / 'stdout'
    args += ['--config', config, '--delay-ms', '30', '--log', log]
    if case == 'interrupt':
        args += ['--unlimited', 'true', '--water-duration', '1000000']
    processes = []
    with stdout_path.open('w') as out:
        try:
            server = subprocess.Popen(args, env=env, stdout=out, start_new_session=True)
            processes.append(server)
            if independent:
                wait_for(stdout_path, 'READY', server)
                for role in ('flower', 'gardener'):
                    for i in range(1, int(keyToValue[role + 's']) + 1):
                        processes.append(subprocess.Popen([BUILD / mode / role, str(i)], env=env,
                                                          stdout=out, start_new_session=True))
            if case == 'interrupt':
                wait_for(log, 'event=WATER_START', server)
                server.send_signal(signal.SIGINT)
            assert server.wait(timeout=15) == (130 if case == 'interrupt' else 0), 'код координатора'
            for worker in processes[1:]:
                assert worker.wait(timeout=5) in (0, 130), 'код участника'
            try:
                os.killpg(server.pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError('после координатора остались дочерние процессы')
        finally:
            for process in processes:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            for process in processes:
                process.wait()
    for root in (Path('/dev/shm'), Path('/dev/mqueue')):
        assert not list(root.glob('*' + garden + '*')), 'не удалён IPC в ' + str(root)
    return check_log(log, case)


if __name__ == '__main__':
    conflict_count = 0
    for mode in MODES:
        for case in CASES:
            with tempfile.TemporaryDirectory(prefix='f3') as folder:
                conflicts = run(mode, case, Path(folder))
                if case == 'conflict':
                    conflict_count += conflicts
                print(f'OK {mode} {case} (conflicts={conflicts})', flush=True)
    assert conflict_count > 0, 'ни один запуск conflict.conf не показал конкуренцию'
    print('20/20 пройдено')

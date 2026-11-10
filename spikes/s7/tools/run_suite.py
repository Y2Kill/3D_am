"""Isolate every experiment in its own process; candidate failures are observations."""
import argparse
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import sys
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode', choices=['matrix', 'guard', 'reject'])
parser.add_argument('--runner', type=pathlib.Path, required=True)
parser.add_argument('--build', type=pathlib.Path, required=True)
parser.add_argument('--hypothesis-timeout', type=float, default=60.0)
args = parser.parse_args()
args.build = args.build.resolve()
args.runner = args.runner.resolve()
FIXTURES = args.build / 'fixtures'
RUNS = args.build / 'runs'
RESULTS = args.build / 'results.json'


def run(candidate, fixture, name, probe=False):
    out = RUNS / name
    out.mkdir(parents=True, exist_ok=True)
    command = [str(args.runner), '--candidate', candidate, '--input', str(fixture), '--out', str(out)]
    if probe:
        command.append('--probe-schema')
    start = time.perf_counter()
    try:
        process = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                 timeout=args.hypothesis_timeout)
    except subprocess.TimeoutExpired:
        return {'status': 'timeout_hypothesis', 'wall_ms': 1000 * (time.perf_counter() - start)}
    wall = 1000 * (time.perf_counter() - start)
    try:
        result = json.loads(process.stdout)
    except (ValueError, TypeError):
        result = {}
    if process.returncode not in (0, 10):
        return {'status': 'crash', 'exit_code': process.returncode,
                'exit_hex': hex(process.returncode & 0xffffffff), 'partial': result,
                'stderr': process.stderr[-2000:], 'wall_ms': wall}
    if not result:
        raise RuntimeError(f'Runner produced no result: {command}: {process.stdout} {process.stderr}')
    if not probe and 'original_unchanged' not in result:
        raise RuntimeError('Runner setup failed: ' + str(result))
    result['wall_ms'] = wall
    result['exit_code'] = process.returncode
    return result


def summarize(trials):
    keys = ('parse_ms', 'v2_ms', 'serialize_ms', 'cycle_ms', 'wall_ms', 'peak_working_set', 'peak_pagefile')
    result = {'outcomes': sorted(set(t.get('status', 'probe') for t in trials))}
    result['medians'] = {key: statistics.median(t[key] for t in trials if isinstance(t.get(key), (int, float)))
                         if any(isinstance(t.get(key), (int, float)) for t in trials) else None for key in keys}
    result['parse_accepted'] = sum(bool(t.get('parse_accepted')) for t in trials)
    result['locations'] = trials[0].get('locations')
    return result


def matrix():
    config = json.loads((args.build / 'configuration.json').read_text(encoding='utf-8'))
    result = {'configuration': config, 'platform': platform.platform(), 'linux': 'not_run',
              'hypothesis_timeout_seconds': args.hypothesis_timeout, 'repetitions': 5, 'cases': {}, 'validators': {}}
    result['example_validators'] = run('E', ROOT / 'schemas/examples/manifest-1.0.example.json', 'probes/example', probe=True)
    fixtures = sorted(FIXTURES.glob('*.json'))
    for fixture in fixtures:
        entry = {'bytes': fixture.stat().st_size, 'sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(), 'candidates': {}}
        for candidate in 'ABCDE':
            trials = [run(candidate, fixture, f'{fixture.stem}/{candidate}/{i}') for i in range(5)]
            entry['candidates'][candidate] = {'trials': trials, 'summary': summarize(trials)}
            for trial in trials:
                if trial.get('status') == 'unsafe_write' and not trial['original_unchanged']:
                    raise RuntimeError('FAIL-CLOSED VIOLATION: ' + fixture.name + candidate)
        result['cases'][fixture.name] = entry
        result['validators'][fixture.name] = run('E', fixture, f'probes/{fixture.stem}', probe=True)
        RESULTS.write_text(json.dumps(result, indent=2, ensure_ascii=True), encoding='utf-8')
        print('Measured ' + fixture.name, flush=True)
    result['strict_parsers'] = {c: all(not case['candidates'][c]['summary']['parse_accepted']
                                    for name, case in result['cases'].items() if name.startswith('invalid_')) for c in 'ABCDE'}
    RESULTS.write_text(json.dumps(result, indent=2, ensure_ascii=True), encoding='utf-8')
    print(f'Matrix: PASS ({len(fixtures)} fixtures, 5 candidates, 5 processes per candidate/fixture)')


def seed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('manifest.json', text)
        archive.writestr('content/s7.bin', b'S7 guard content')


def guard():
    for candidate in 'ABCDE':
        blocked = 0
        for fixture_name in ('valid_negative_zero.json', 'valid_huge_positive.json', 'invalid_00.json'):
            path = RUNS / 'guard' / candidate / fixture_name.replace('.json', '.zip')
            seed(path, (FIXTURES / fixture_name).read_bytes())
            before = hashlib.sha256(path.read_bytes()).hexdigest()
            result = run(candidate, path, 'guard/' + candidate + '/output')
            after = hashlib.sha256(path.read_bytes()).hexdigest()
            if result.get('status') != 'success':
                assert before == after, (candidate, fixture_name, 'source ZIP changed')
                blocked += 1
            with zipfile.ZipFile(path) as archive:
                assert archive.read('content/s7.bin') == b'S7 guard content'
        assert blocked, candidate
        if candidate == 'A':
            assert blocked == 3, 'A must reject -0 and/or 1e400 safely'
    print('Guard: PASS (SHA-256 unchanged for every unsafe write; content preserved)')


def reject():
    matrix_data = json.loads(RESULTS.read_text(encoding='utf-8'))
    for candidate in 'ABCDE':
        accepted = []
        for fixture in sorted(FIXTURES.glob('invalid_*.json')):
            result = run(candidate, fixture, 'reject/' + candidate + '/' + fixture.stem)
            if result.get('parse_accepted'):
                accepted.append(fixture.name)
        assert matrix_data['strict_parsers'][candidate] == (not accepted), (candidate, accepted)
    print('Reject: PASS (all 12 malformed numbers checked; strictness matches measured matrix)')


if args.mode == 'matrix':
    matrix()
elif args.mode == 'guard':
    guard()
else:
    reject()

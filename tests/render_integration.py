#!/usr/bin/env python3
"""Two-peer native cosmetic framebuffer acceptance with optional explicit mapping; requires a caller-owned DISPLAY."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import time

from isolated_services import Services


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def evidence_json(evidence, known_secrets):
    # Logs/errors already use Services.redact; its broad token pattern would erase artifact hashes.
    text = json.dumps(evidence, indent=2) + '\n'
    for value in sorted(known_secrets, key=len, reverse=True):
        if value:
            text = text.replace(json.dumps(value)[1:-1], '[redacted]')
    return text


def evidence_serialization_check():
    services = Services.__new__(Services)
    session = 'e' * 64
    password = 'fixture-"password\\with\nunicode-\u00e7'
    services.secrets = {session, password, ''}
    sha256 = hashlib.sha256(b'render-evidence-fixture').hexdigest()
    evidence = {
        'source_sha256': sha256, 'client_sha256': sha256,
        'clients': [{'argv': ['ikv-client', '--session-stdin'],
                     'stdout': {'sha256': sha256}, 'stderr': {'sha256': sha256},
                     'framebuffer': {'sha256': sha256}}],
        'error': services.redact(session + ' ' + password),
    }
    text = evidence_json(evidence, services.secrets)
    assert json.loads(text) == evidence, 'evidence serialization must preserve all SHA-256 fields'
    assert session not in text and password not in text, 'credentials leaked into evidence'
    assert services.redact(sha256) == '[redacted-token]', 'general log redaction must remain strict'
    # Synthetic leakage also exercises JSON-escaped known-secret masking at the final boundary.
    leaked = evidence_json({'diagnostic': session + ' ' + password}, services.secrets)
    assert json.loads(leaked)['diagnostic'] == '[redacted] [redacted]'


def output_directory(value):
    path = Path(value).absolute()
    for parent in (path, *path.parents):
        if parent.is_symlink():
            raise ValueError('output directory must not traverse symlinks')
    if path.exists():
        if not path.is_dir() or path.stat().st_uid != os.getuid() or any(path.iterdir()):
            raise ValueError('output directory must be new or empty and owned by this user')
        if path.stat().st_mode & 0o077:
            raise ValueError('existing output directory must have mode 0700')
    else:
        path.mkdir(mode=0o700)
    return path


def save(path, text):
    with open(path, 'x', encoding='utf-8', opener=lambda p, flags: os.open(p, flags, 0o600)) as stream:
        stream.write(text)


def ppm(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('renderer did not produce a regular screenshot')
    if path.stat().st_size > 3840 * 2160 * 3 + 4096:
        raise ValueError('screenshot exceeds bounded P6 size')
    data = path.read_bytes()
    # Native writers emit a plain P6 header. Do not skip raster whitespace bytes.
    match = re.match(rb'P6\s+([0-9]+)\s+([0-9]+)\s+255(?:\r\n|\n| |\t)', data)
    if not match:
        raise ValueError('screenshot is not an 8-bit P6 framebuffer')
    width, height = map(int, match.groups())
    if not (640 <= width <= 3840 and 480 <= height <= 2160):
        raise ValueError('screenshot dimensions outside native client bounds')
    body = data[match.end():]
    if len(body) != width * height * 3:
        raise ValueError('P6 raster length does not match dimensions')
    first = body[:3]
    if all(body[index:index + 3] == first for index in range(0, len(body), 3)):
        raise ValueError('native framebuffer is uniform')
    return {'path': path.name, 'width': width, 'height': height,
            'bytes': len(data), 'sha256': digest(path), 'nonuniform': True}


def create_identity(services, class_id=1, sex=1, *, appearance=None):
    if type(class_id) is not int or not 1 <= class_id <= 3:
        raise ValueError('native character class_id must be an integer from 1 through 3')
    if type(sex) is not int or sex not in (0, 1):
        raise ValueError('native character sex must be integer 0 or 1')
    if appearance is None:
        appearance = {'sex': sex, 'face': 0, 'hair': 0, 'hair_color': 0, 'beard': 0, 'beard_color': 0}
    if (not isinstance(appearance, dict) or
            set(appearance) != {'sex', 'face', 'hair', 'hair_color', 'beard', 'beard_color'} or
            any(type(value) is not int for value in appearance.values()) or
            appearance['sex'] not in (0, 1)):
        raise ValueError('appearance must contain exactly six native integer cosmetic fields')
    limits = {'face': 10, 'hair': (21, 18)[appearance['sex']],
              'hair_color': (10, 5)[appearance['sex']], 'beard': (25, 15)[appearance['sex']],
              'beard_color': (2, 5)[appearance['sex']]}
    if any(not 0 <= appearance[field] < count for field, count in limits.items()):
        raise ValueError('native character cosmetics exceed source sex-specific bounds')
    username = 'render_' + secrets.token_hex(6)
    password = 'Render!' + secrets.token_hex(24)
    services.secrets.add(password)
    account = services.api('POST', '/v1/register', {'username': username, 'password': password})
    session = services.api('POST', '/v1/login', {'username': username, 'password': password})['session']
    character = services.api('POST', '/v1/characters', {
        'name': 'Render' + secrets.token_hex(5), 'class_id': class_id, 'appearance': appearance,
    }, session=session)
    if type(character['id']) is not int or character['id'] <= 0:
        raise ValueError('created character has no real positive id')
    listing = services.api('GET', '/v1/characters', session=session)['characters']
    if [item['id'] for item in listing] != [character['id']]:
        raise ValueError('HTTPS character ownership listing mismatch')
    for item in (character, listing[0]):
        if item['account_id'] != account['account_id']:
            raise ValueError('created or listed character is not owned by registered account')
        if type(item['class_id']) is not int or item['class_id'] != class_id:
            raise ValueError('created or listed native character class mismatch')
        if item['appearance'] != appearance or any(type(value) is not int for value in item['appearance'].values()):
            raise ValueError('created or listed native character cosmetics mismatch')
    return {'id': character['id'], 'session': session, 'class_id': class_id, 'appearance': appearance}


def script_coordinate_rejection(services, args, character, evidence):
    with tempfile.TemporaryDirectory(prefix='ikv-script-check-') as temporary:
        commands = Path(temporary) / 'commands.json'
        save(commands, json.dumps([{'tick': 1, 'destination': {'x': 18446744073709551615, 'y': 0}}],
                                  separators=(',', ':')))
        child = services.start(args.client, '--host', 'localhost', '--port', str(services.world_port),
                               '--ca', services.ca, '--session-stdin', '--character', str(character),
                               '--asset-root', args.asset_root, '--commands', commands,
                               stdin='0' * 64 + '\n')
        try:
            services.until(lambda: child['process'].poll() is not None and child['eof'],
                           'unsigned Script coordinate rejection', timeout=5)
            diagnostic = 'destination exceeds custom protocol coordinate limit'
            if child['process'].returncode != 1 or not any(diagnostic in line for line in child['lines']):
                raise ValueError('native Script did not reject the unsigned coordinate before startup: ' +
                                 services.redact('\n'.join(child['lines'])[-12000:]))
            evidence['script_coordinate_rejection'] = {'exit_code': 1, 'diagnostic': diagnostic}
        finally:
            services.stop(child)


def render(services, args, output, identities, backends, evidence):
    if (not isinstance(identities, (list, tuple)) or not isinstance(backends, (list, tuple)) or
            not 1 <= len(identities) <= 6 or len(identities) != len(backends)):
        raise ValueError('render requires one through six explicitly matched identities and backends')
    if any(type(backend) is not str or backend not in ('opengl', 'vulkan') for backend in backends):
        raise ValueError('native render backends must be opengl or vulkan')
    if any(not isinstance(identity, dict) or type(identity.get('id')) is not int or
           identity['id'] <= 0 for identity in identities):
        raise ValueError('native render identities must have real positive integer character ids')
    if len({identity['id'] for identity in identities}) != len(identities):
        raise ValueError('native render character ids must be unique')
    running = []
    with tempfile.TemporaryDirectory(prefix='ikv-render-logs-') as temporary:
        try:
            for identity, backend in zip(identities, backends):
                rt = 'required' if args.require_rt and len(backends) == 1 and backend == 'vulkan' else 'off'
                label = backend + ('-rt-required' if rt == 'required' else '')
                if backends.count(backend) > 1:
                    label += '-' + str(identity['id'])
                screenshot = output / (label + '.ppm')
                argv = [args.client, '--host', 'localhost', '--port', str(services.world_port),
                        '--ca', str(services.ca), '--session-stdin', '--character', str(identity['id']),
                        '--asset-root', args.asset_root, '--renderer', backend,
                        '--appearance', 'enhanced' if rt == 'required' else 'original', '--rt', rt, '--width', '960', '--height', '640',
                        '--ticks', '400', '--screenshot', str(screenshot)]
                if rt == 'required':
                    argv += ['--rt-style', getattr(args, 'rt_style', 'faithful')]
                if getattr(args, 'model_map', ''):
                    argv += ['--model-map', args.model_map]
                stdout = open(Path(temporary) / (label + '.stdout'), 'w+b')
                stderr = open(Path(temporary) / (label + '.stderr'), 'w+b')
                record = {'backend': backend, 'rt_policy': rt, 'display': os.environ['DISPLAY'],
                          'character_id': identity['id'], 'argv': argv}
                record.update({key: identity[key] for key in ('class_id', 'appearance') if key in identity})
                evidence['clients'].append(record)
                process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=stdout, stderr=stderr,
                                           start_new_session=True)
                running.append((process, stdout, stderr, record, screenshot))
                process.stdin.write((identity['session'] + '\n').encode('ascii'))
                process.stdin.close()
            deadline = time.monotonic() + 120
            ready = False
            while any(process.poll() is None for process, *_ in running):
                services.pump(0.03)
                for process, stdout, stderr, *_ in running:
                    if max(os.fstat(stdout.fileno()).st_size, os.fstat(stderr.fileno()).st_size) > 32 * 1024 * 1024:
                        raise ValueError('native client log exceeded 32 MiB')
                if not ready:
                    messages = []
                    for _, stdout, _, _, _ in running:
                        messages.append(os.pread(stdout.fileno(), 32 * 1024 * 1024, 0).decode('utf-8', errors='replace'))
                    if all(re.search(r'READY client_self=[1-9][0-9]*', text) for text in messages):
                        ids = ','.join(str(identity['id']) for identity in identities)
                        leases = int(services.sql('SELECT count(*) FROM characters WHERE id IN (' + ids + ') AND lease_instance IS NOT NULL'))
                        if leases == len(backends):
                            ready = True
                            evidence.setdefault('authoritative_connections', []).append({'backends': backends, 'simultaneous_owned_leases': leases})
                if time.monotonic() >= deadline:
                    raise TimeoutError('native renderer acceptance exceeded 120 seconds')
            if not ready:
                raise ValueError('did not observe simultaneous authoritative owned client leases')
        finally:
            for process, stdout, stderr, record, screenshot in running:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                record['exit_code'] = process.returncode
                for name, stream in (('stdout', stdout), ('stderr', stderr)):
                    stream.seek(0)
                    text = services.redact(stream.read(32 * 1024 * 1024).decode('utf-8', errors='replace'))
                    stream.close()
                    path = output / (screenshot.stem + '.' + name + '.log')
                    save(path, text)
                    record[name] = {'path': path.name, 'sha256': digest(path)}
                    record.setdefault('runtime_diagnostics', []).extend(
                        line for line in text.splitlines()
                        if line.startswith(('Vulkan device=', 'Vulkan submitted frame=', 'Vulkan completed frames=')))
            # Never turn a cleanup result into success: validation follows only on normal return.
        for process, _, _, record, screenshot in running:
            if process.returncode != 0:
                raise ValueError('actual ' + record['backend'] + ' client failed; see redacted logs')
            record['framebuffer'] = ppm(screenshot)
            if (record['framebuffer']['width'], record['framebuffer']['height']) != (960, 640):
                raise ValueError('native screenshot dimensions differ from requested window')
            if record['rt_policy'] == 'required':
                diagnostics = record.get('runtime_diagnostics', [])
                device = any(line.startswith('Vulkan device=') and 'hardwareRT=1' in line for line in diagnostics)
                # Faithful RT (default) traces only Sun-ray actor shadows; physical adds reflections and diffuse GI.
                style = getattr(args, 'rt_style', 'faithful')
                effects = '1' if style == 'physical' else '0'
                submitted = any(line.startswith('Vulkan submitted frame=') and
                                all(flag in line for flag in ('provider=vulkan', 'appearance=enhanced', 'rtStyle=' + style,
                                    'hardwareRayQueries=1', 'accelerationStructure=1', 'shadows=1',
                                    'reflections=' + effects, 'diffuseGI=' + effects)) for line in diagnostics)
                completed = any(re.search(r'^Vulkan completed frames=[1-9][0-9]* provider=vulkan hardwareRayQueries=1(?: |$)', line)
                                for line in diagnostics)
                if not (device and submitted and completed):
                    raise ValueError('required RT lacks actual GPU capability, ' + style + ' RT submission, or completed RT frame diagnostics')
        ids = ','.join(str(identity['id']) for identity in identities)
        services.until(lambda: services.sql('SELECT count(*) FROM characters WHERE id IN (' + ids + ') AND lease_instance IS NOT NULL') == '0', 'native render client lease release')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('auth', 'server', 'client', 'source', 'asset-root', 'output-dir'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--model-map', default='')
    parser.add_argument('--require-rt', action='store_true')
    parser.add_argument('--rt-style', choices=('faithful', 'physical'), default='faithful',
                        help='RT lighting style of the --require-rt case')
    parser.add_argument('--six-pc', action='store_true',
                        help='classes 1..3 x sexes 0/1: six real identities; default two individualized cosmetic peers')
    args = parser.parse_args()
    if not os.environ.get('DISPLAY'):
        parser.error('a user-provided DISPLAY is required; this script never starts a virtual display')
    if os.environ.get('SDL_VIDEO_DRIVER', os.environ.get('SDL_VIDEODRIVER', '')).lower() in ('dummy', 'offscreen'):
        parser.error('dummy/offscreen SDL video drivers are not native window acceptance')
    if not Path(args.asset_root).is_dir():
        parser.error('--asset-root must contain legitimate original assets')
    output = output_directory(args.output_dir)
    source = Path(args.source) / 'src/client_main.cpp'
    evidence = {'status': 'failed', 'display': os.environ['DISPLAY'],
                'asset_root': str(Path(args.asset_root).resolve()), 'clients': [],
                'source_sha256': digest(source), 'client_sha256': digest(Path(args.client)),
                'scope': 'Native artifact capture and owned TLS/PostgreSQL connection status only. Bounded nonuniform P6 processing does not prove authentic assets, visual parity, or RT effects; parent must inspect actual scenes. Caller DISPLAY may itself be virtual or software-rendered; no physical-GPU claim.'}
    services = Services(args)
    try:
        with services:
            if args.six_pc:
                identities = [create_identity(services, class_id, sex)
                              for class_id in range(1, 4) for sex in range(2)]
                backends = ['opengl', 'vulkan'] * 3
            else:
                identities = [create_identity(services), create_identity(services, appearance={
                    'sex': 1, 'face': 2, 'hair': 3, 'hair_color': 1, 'beard': 4, 'beard_color': 2,
                })]
                backends = ['opengl', 'vulkan']
            services.start_world(Path(args.source) / 'config/gameplay.json', asset_root=args.asset_root)
            script_coordinate_rejection(services, args, identities[0]['id'], evidence)
            if args.six_pc:
                for identity, backend in zip(identities, backends):
                    case_label = f"case-class{identity['class_id']}-sex{identity['appearance']['sex']}"
                    case_output = output_directory(output / case_label)
                    first_record = len(evidence['clients'])
                    try:
                        render(services, args, case_output, [identity], [backend], evidence)
                    finally:
                        for record in evidence['clients'][first_record:]:
                            record.update({'case_label': case_label, 'output_dir': case_label})
            else:
                render(services, args, output, identities, backends, evidence)
            if args.require_rt:
                render(services, args, output, identities[:1], ['vulkan'], evidence)
        evidence['status'] = 'passed'
    except Exception as error:
        evidence['error'] = services.redact(str(error))
        print(evidence['error'], file=sys.stderr)
    finally:
        save(output / 'evidence.json', evidence_json(evidence, services.secrets))
    return 0 if evidence['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())

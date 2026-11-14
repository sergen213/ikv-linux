#!/usr/bin/env python3
"""Wave 29 TopkapiPerf3 parity captures: one owned isolated stack (tests/isolated_services.py), clock frozen at noon, no
bots, harmless NPCs; the user's Vulkan settings (FSR3 balanced, view 7) with --style physical|faithful and --fg on|off.
Still shots at --shots ticks; --walk DX,DZ walks from --at to --at+(DX,DZ) starting at tick 112 with a shot every 2 ticks
(motion ghosting). --shaders: experiment SPIR-V bind-mounted over the build's IKV_SHADER_DIR (private namespace).
--env K=V repeats. Run under a private kwin_wayland --virtual WAYLAND_DISPLAY; holds flock -x /tmp/ikv-perf-bench.lock.
usage: parity.py --build ABS --out ABS_DIR --tag T --zone topkapi --at X,Z --camera YAW,PITCH,DIST [--style physical]
                 [--fg off] [--shots 200,240] [--walk 10,0] [--shaders DIR] [--res 2743x1543] [--env K=V]"""
import argparse, json, os, signal, subprocess, sys, time
from pathlib import Path
from types import SimpleNamespace
import numpy as np
from PIL import Image

MAIN = Path('/mnt/2nd4TB/IKV')
TREE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TREE / 'tests'))


def main():
    a = argparse.ArgumentParser()
    a.add_argument('--build', required=True); a.add_argument('--out', required=True); a.add_argument('--tag', required=True)
    a.add_argument('--zone', default='topkapi'); a.add_argument('--at', required=True); a.add_argument('--camera', required=True)
    a.add_argument('--style', default='physical'); a.add_argument('--fg', default='off'); a.add_argument('--shots', default='200,240')
    a.add_argument('--walk'); a.add_argument('--shaders', default=''); a.add_argument('--res', default='2743x1543')
    a.add_argument('--view', type=int, default=7); a.add_argument('--env', action='append', default=[]); a.add_argument('--child', action='store_true')
    a = a.parse_args()
    if not a.child:
        assert os.environ.get('WAYLAND_DISPLAY'), 'private kwin_wayland --virtual WAYLAND_DISPLAY required'
        return subprocess.run(['flock', '-x', '/tmp/ikv-perf-bench.lock', sys.executable, __file__, '--child', *sys.argv[1:]]).returncode
    from isolated_services import Services  # noqa: E402
    from render_integration import create_identity  # noqa: E402
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    build = Path(a.build)
    config = json.loads((TREE / 'config/gameplay.json').read_text())
    config['bots']['spawn'] = []; config['clock']['start_hour'] = 12; config['clock']['rate'] = 0
    for n in config['npcs']:
        if 'aggro' in n:
            n.update(aggro=1, damage=1); n.pop('damage_range', None); n.pop('skills', None)
        if 'trap' in n:
            n['trap']['damage'] = 1
    x, z = (float(v) for v in a.at.split(','))
    commands, shots = [], []
    if a.walk:
        dx, dz = (float(v) for v in a.walk.split(','))
        commands.append({'tick': 112, 'destination': {'x': round((x + dx) * 1000), 'y': round((z + dz) * 1000)}})
        ticks = list(range(110, 172, 2))
    else:
        ticks = [int(t) for t in a.shots.split(',')]
    for t in ticks:
        shots.append(out / f'{a.tag}-t{t}.ppm'); commands.append({'tick': t, 'screenshot': str(shots[-1])})
    commands.sort(key=lambda c: c['tick'])
    args = SimpleNamespace(auth=str(build / 'ikv-auth'), server=str(build / 'ikv-server'), client=str(build / 'ikv-client'), source=str(TREE), build=str(build))
    env = dict(os.environ, SDL_VIDEODRIVER='wayland'); env.pop('DISPLAY', None); env.pop('IKV_RENDER_WEATHER', None)
    for kv in a.env:
        k, v = kv.split('=', 1); env[k] = v
    width, height = a.res.split('x')
    with Services(args) as services:
        gameplay = services.root / 'gameplay.json'; gameplay.write_text(json.dumps(config))
        services.start_world(gameplay, asset_root=str(MAIN / 'original/app/base'))
        me = create_identity(services)
        state = dict(zone=a.zone, x=round(x * 1000), y=round(z * 1000), level=30, hp=2000, max_hp=2000)
        services.sql(f"UPDATE characters SET state='{json.dumps(state)}'::jsonb WHERE id={me['id']}")
        cam = a.camera.split(',')
        settings = out / f'{a.tag}.settings.json'
        settings.write_text(json.dumps({'camera': {'yaw': float(cam[0]), 'pitch': float(cam[1]), 'distance': float(cam[2])}, 'view_distance': a.view}))
        cmdfile = out / f'{a.tag}.commands.json'; cmdfile.write_text(json.dumps(commands))
        client = build / 'ikv-client'
        argv = [str(client), '--host', 'localhost', '--port', str(services.world_port), '--ca', str(services.ca),
                '--session-stdin', '--character', str(me['id']), '--asset-root', str(MAIN / 'original/app/base'), '--settings', str(settings),
                '--renderer', 'vulkan', '--appearance', 'enhanced', '--rt', 'required', '--rt-style', a.style, '--upscaler', 'fsr3',
                '--fsr-mode', 'balanced', '--texture-detail', '2', '--lightmap-detail', '2', '--frame-generation', a.fg,
                '--width', width, '--height', height, '--ticks', str(max(ticks) + 10), '--commands', str(cmdfile)]
        if a.shaders:
            argv = ['unshare', '-rm', 'sh', '-c', 'mount --bind "$0" "$1" && shift && exec "$@"', a.shaders, str(client.parent / 'shaders/vulkan'), *argv]
        log = open(out / f'{a.tag}.log', 'w+b')
        p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=env)
        p.stdin.write((me['session'] + '\n').encode()); p.stdin.close()
        t0 = time.monotonic()
        while p.poll() is None and time.monotonic() - t0 < 300:
            services.pump(0.05)
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGKILL); p.wait()
        log.close()
    pngs = []
    for s in shots:
        if s.exists():
            png = s.with_suffix('.png'); Image.fromarray(np.asarray(Image.open(s).convert('RGB'))).save(png); s.unlink(); pngs.append(str(png))
    print(json.dumps({'tag': a.tag, 'exit': p.returncode, 'pngs': len(pngs)}))
    return 0


if __name__ == '__main__':
    sys.exit(main())

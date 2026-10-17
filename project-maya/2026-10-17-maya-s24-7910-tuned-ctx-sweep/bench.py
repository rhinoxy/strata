#!/usr/bin/env python3
# Maya S24 (GLM-5.3-Flash) context-length sweep benchmark - 7910, 2026-10-15
import json, time, urllib.request, uuid, os, re, sys, subprocess

CFG = '/home/k-takeda/project-maya/maya-maya-s24-hip.json'
BASE = 'http://127.0.0.1:8082'
LOG = '/home/k-takeda/project-maya/maya-maya-s24-hip.log'
OUT = os.environ.get('MAYA_OUT') or '/home/k-takeda/project-maya/bench/results/2026-10-15-maya-s24-7910-ctx-sweep'
os.makedirs(OUT, exist_ok=True)
KEY = json.load(open(CFG))['api_key']

PARA = ("It was late in the evening when the tide finally turned at the old harbor, and the lamps along "
        "the pier came on one by one. A fisherman repaired his nets while gulls circled above the market "
        "roof, and the smell of salt and tar drifted over the stone quay. Somewhere beyond the breakwater "
        "a ship's horn sounded twice, low and patient, as if the sea itself were clearing its throat. ")
STORY_INSTR = ("\n\n[Task] Continue this story in the same third-person style for a long stretch, keeping the "
               "same tone, characters and setting. Do not summarize, do not explain, do not stop early, and do "
               "not add headings. Write continuously until you run out of the output budget. [Story]\n")

def api(path, body=None, timeout=3600):
    req = urllib.request.Request(BASE + path,
        data=json.dumps(body).encode() if body is not None else None,
        headers={'Authorization': 'Bearer ' + KEY, 'Content-Type': 'application/json'})
    return json.load(urllib.request.urlopen(req, timeout=timeout))

def metrics():
    return api('/metrics')

def log_pos():
    return os.path.getsize(LOG)

def log_spec(start):
    total_a = total_b = 0
    nlines = 0
    with open(LOG, 'rb') as f:
        f.seek(start)
        data = f.read().decode('utf-8', 'replace')
    for m in re.finditer(r'glm spec: (\d+) of (\d+) drafts accepted \(([\d.]+)%\)', data):
        total_a += int(m.group(1)); total_b += int(m.group(2)); nlines += 1
    return total_a, total_b, nlines

def chat(content, max_tokens):
    body = {'model': 'glm-5.3-flash',
            'messages': [{'role': 'user', 'content': content}],
            'temperature': 0, 'top_p': 1, 'max_tokens': max_tokens, 'stream': False}
    t0 = time.time()
    r = api('/v1/chat/completions', body)
    return time.time() - t0, r

results = []

def record(tag, target, wall, r, spec):
    prompt_tokens = r.get('usage', {}).get('prompt_tokens')
    completion_tokens = r.get('usage', {}).get('completion_tokens')
    # find matching server record
    rec = None
    for q in reversed(metrics().get('requests', [])):
        pt = q.get('prompt_tokens') or 0
        if abs((pt or 0) - (prompt_tokens or 0)) <= max(3, (prompt_tokens or 0) * 0.01):
            rec = q
            break
    row = {'tag': tag, 'target': target, 'wall_s': round(wall, 2),
           'prompt_tokens': prompt_tokens, 'completion_tokens': completion_tokens,
           'spec_accepted': spec[0], 'spec_total': spec[1], 'spec_req_lines': spec[2],
           'spec_pct': round(100.0 * spec[0] / spec[1], 1) if spec[1] else None,
           'finish': (r.get('choices') or [{}])[0].get('finish_reason'),
           'server': {k: rec.get(k) for k in ('prompt_tokens', 'reused', 'prompt_ms', 'prefill_tok_s',
                       'decode_ms', 'decode_tok_s', 'output_tokens', 'finish', 'hit_rate')} if rec else None}
    results.append(row)
    print(json.dumps(row, ensure_ascii=False), flush=True)
    return row

# ---- idle power sampling (2 min) ----
def idle_sample(name, iters=None, interval=2.0):
    iters = int(os.environ.get('MAYA_IDLE_ITERS') or iters or 30)
    samples = {1: [], 2: []}
    vrams = {0: [], 2: []}
    for i in range(iters):
        out = subprocess.run(['amd-smi', 'metric', '-p', '-g', '1', '2', '--json'],
                             capture_output=True, text=True)
        try:
            d = json.loads(out.stdout)
            for g in d.get('gpu_data', []):
                gid = g.get('gpu')
                if gid in samples:
                    samples[gid].append(g.get('power', {}).get('socket_power', {}).get('value'))
        except Exception:
            pass
        for c in (0, 2):
            try:
                with open('/sys/class/drm/card%d/device/mem_info_vram_used' % c) as f:
                    vrams[c].append(int(f.read()))
            except Exception:
                pass
        time.sleep(interval)
    res = {}
    for g, s in samples.items():
        s = [x for x in s if x is not None]
        res['gpu%d_power_w' % g] = round(sum(s) / len(s), 2) if s else None
        res['gpu%d_n' % g] = len(s)
    for c, s in vrams.items():
        res['card%d_vram_used_mb' % c] = round(sum(s) / len(s) / 1048576, 1) if s else None
    json.dump(res, open(os.path.join(OUT, name), 'w'), indent=1)
    print(name, json.dumps(res), flush=True)
    return res

print('== idle before ==', flush=True)
idle_before = idle_sample('idle_before.json') if int(os.environ.get('MAYA_IDLE_ITERS') or 30) > 0 else {}

# ---- calibration: tokens per paragraph (max_tokens=1, tiny prefill) ----
nonce0 = uuid.uuid4().hex[:16]
cal_text = nonce0 + ' ' + PARA * 3
wall, r = chat(cal_text, 1)
cal_pt = r['usage']['prompt_tokens']
per_para = (cal_pt - 20) / 3.0
print('calibration: %d prompt tokens, %.1f tokens/paragraph' % (cal_pt, per_para), flush=True)
record('calibration', 0, wall, r, log_spec(0) and (0, 0, 0))

# ---- sweep ----
TARGETS = [int(x) for x in (os.environ.get('MAYA_TARGETS') or '2048,8192,32768,131072').split(',')]
# adaptive: estimate prefill rate from a 2k fresh run first
budget_s = 1500.0
spent = 0.0
rate_est = None

for target in TARGETS:
    reps = max(1, int(round((target - 60) / per_para)))
    nonce = uuid.uuid4().hex[:16]
    text = nonce + ' ' + (PARA * reps) + STORY_INSTR + PARA * 2
    if rate_est:
        eta = (reps * per_para) / rate_est
        if eta > budget_s - spent:
            print('SKIP ctx %d: projected prefill %.0fs exceeds remaining budget %.0fs'
                  % (target, eta, budget_s - spent), flush=True)
            results.append({'tag': 'skipped', 'target': target, 'projected_s': round(eta, 1)})
            continue
    pos = log_pos()
    wall, r = chat(text, 512)
    spec = log_spec(pos)
    row = record('ctx%d' % target, target, wall, r, spec)
    if row.get('server'):
        pr = row['server'].get('prefill_tok_s')
        if pr and row['server'].get('reused', 0) < 100:
            rate_est = pr
    spent += wall
    time.sleep(3)

# ---- idle after ----
print('== idle after ==', flush=True)
idle_after = idle_sample('idle_after.json') if int(os.environ.get('MAYA_IDLE_ITERS') or 30) > 0 else {}

summary = {'idle_before': idle_before, 'idle_after': idle_after, 'sweep': results,
           'para_tokens': per_para, 'started': None}
json.dump(summary, open(os.path.join(OUT, 'results.json'), 'w'), indent=1, ensure_ascii=False)
print('== DONE ==', flush=True)
print(json.dumps(summary, ensure_ascii=False, indent=1))


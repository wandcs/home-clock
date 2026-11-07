"""Two actual state-machine builds; Linux loopback, no SDK/device build."""
import hashlib, importlib.util, json, os, shlex, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
BASE=Path(__file__).resolve().parent
OUT=ROOT/'.local/s25/tls'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def function(text, signature):
    start=text.index(signature);a=text.index('{',start);depth=0
    for pos in range(a,len(text)):
        depth+=(text[pos]=='{')-(text[pos]=='}')
        if depth==0:return text[start:pos+1]+'\n'
    raise ValueError(signature)
def main():
    OUT.mkdir(parents=True,exist_ok=True);source=ROOT/'.local/esp-idf-v6.1/components/esp-tls/esp_tls.c'
    spec=importlib.util.spec_from_file_location('tls_patch',BASE/'prepare.py');patch=importlib.util.module_from_spec(spec);spec.loader.exec_module(patch)
    original=source.read_bytes();modified=patch.generate(original);(OUT/'esp_tls.c').write_bytes(modified)
    header=source.with_suffix('.h');types=header.read_text();start=types.index('typedef enum esp_tls_conn_state {');end=types.index('} esp_tls_conn_state_t;',start)+len('} esp_tls_conn_state_t;')
    (OUT/'tls-state.inc').write_text(types[start:end]+'\n',newline='\n')
    sigs=['static void ms_to_timeval(','static int esp_tls_low_level_conn(','int esp_tls_conn_new_async(']
    for name,data in [('old',original),('fixed',modified)]:
        text=data.decode('utf-8');(OUT/f'{name}.inc').write_text('\n'.join(function(text,sig) for sig in sigs),newline='\n')
    # A changed SDK must fail closed before touching output or returning a patch.
    try:patch.generate(original+b'\n')
    except ValueError:pass
    else:raise AssertionError('Modified SDK was accepted')
    posix_root=subprocess.run(['wsl.exe','--exec','wslpath','-a',str(ROOT)],capture_output=True,text=True,check=True,timeout=10).stdout.strip() if os.name=='nt' else str(ROOT)
    command='#!/bin/sh\nset -eu\ncd '+shlex.quote(posix_root)+'\n'
    for name in ['old','fixed']:
        command+=f'cc -std=c17 -O1 -g -Wall -Wextra -Werror -Wno-implicit-fallthrough -D_POSIX_C_SOURCE=200809L -DTLS_FUNCTIONS=\\\"{name}.inc\\\" tools/network-hardening/tls/tls-select-test.c -I.local/s25/tls -o .local/s25/tls/{name}-test\n'
    # Keep the old red exit separate; both executables run all three checks.
    command+='set +e\n.local/s25/tls/old-test > .local/s25/tls/old-output.jsonl\nold_status=$?\n.local/s25/tls/fixed-test > .local/s25/tls/fixed-output.jsonl\nfixed_status=$?\nprintf "%s %s\\n" "$old_status" "$fixed_status"\n'
    (OUT/'run.sh').write_text(command,newline='\n')
    script=posix_root+'/.local/s25/tls/run.sh';r=subprocess.run((['wsl.exe','--exec','sh',script] if os.name=='nt' else ['sh',script]),capture_output=True,text=True,timeout=20)
    print(r.stdout,end='');print(r.stderr,end='',file=sys.stderr)
    if r.returncode:raise SystemExit(r.returncode)
    old_status,fixed_status=map(int,r.stdout.split())
    old=[json.loads(line) for line in (OUT/'old-output.jsonl').read_text().splitlines()]
    fixed=[json.loads(line) for line in (OUT/'fixed-output.jsonl').read_text().splitlines()]
    result={'scope':'Locked IDF low_level_conn + async functions; real pending loopback TCP, select, getsockopt; TLS crypto is a seam, not a P4/TLS integration test','old_exit':old_status,'fixed_exit':fixed_status,'old':old,'fixed':fixed,'modified_source_sha256':hashlib.sha256(modified).hexdigest(),'inputs_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [source,header,BASE/'prepare.py',BASE/'run.py',BASE/'tls-select-test.c']}}
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n',newline='\n')
    print(json.dumps(result,indent=2))
    assert old_status==2 and len(old)==3 and all(not case['pass'] for case in old)
    assert fixed_status==0 and len(fixed)==3 and all(case['pass'] for case in fixed)
    assert source.read_bytes()==original
if __name__=='__main__':main()

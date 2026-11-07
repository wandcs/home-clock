"""Optional producer scheduling regression; host shim, no device/timing claim.

Requires Linux GCC, pthreads and ASan/UBSan (or Windows current-user WSL), plus
esp_codec_dev 1.6.2 source resolved by a normal public build at
firmware/managed_components/espressif__esp_codec_dev/esp_codec_dev_vol.c.
This check is optional; normal firmware builds do not require running it.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'firmware/features/alarm-audio'
FIXTURE=ROOT/'tools/alarm-output-readiness'


def linux(path):
    return '/mnt/'+path.drive[0].lower()+path.as_posix()[2:] if os.name=='nt' else str(path)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'.local/personal-alarm/streaming-host')
    out=parser.parse_args().output.resolve();out.relative_to((ROOT/'.local').resolve());out.mkdir(parents=True,exist_ok=True)
    prefix=['wsl.exe','--exec'] if os.name=='nt' else []
    def run(args,success=True):
        p=subprocess.run(prefix+args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        with (out/'commands.log').open('a',encoding='utf8') as f:f.write(json.dumps(args)+'\n'+p.stdout+'\n')
        assert (p.returncode==0)==success,p.stdout
        return p.stdout.strip()
    common=['gcc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I'+linux(FIXTURE/'stubs'),'-I'+linux(SRC),linux(SRC/'alarm_source.c'),linux(SRC/'alarm_player.c'),linux(SRC/'alarm_reference.c'),linux(FIXTURE/'test_bsp.c'),linux(ROOT/'firmware/managed_components/espressif__esp_codec_dev/esp_codec_dev_vol.c')]
    source=(SRC/'alarm_bsp.c').read_text(encoding='utf8')
    # Replay the original unconditional per-pump sleep on the same reviewed C.
    old=source.replace('        if (state != AA_PLAYING) {\n            /* Stack scanning',
                       '        if (true) {\n            /* Stack scanning')
    assert old!=source
    (out/'old-delay.c').write_text(old,encoding='utf8')
    run(common+[linux(out/'old-delay.c'),'-lm','-o',linux(out/'old-delay')])
    rejected=run([linux(out/'old-delay'),'0'],success=False)
    assert 'AA_PLAYING' in rejected
    run(common+[linux(SRC/'alarm_bsp.c'),'-lm','-o',linux(out/'current')])
    results={str(n):run([linux(out/'current'),str(n)]) for n in (0,7,10,15)}
    report={'ok':True,'red_old_active_delay_rejected':True,'sanitizers':'ASan + UBSan',
            'scenarios':results,'checks':['no extra delay or stack scan while PLAYING','priority7 above locked LVGL6','normal stop','write error stops without replay','immediate PA cutoff while sink blocked','zero volume stays muted'],
            'limitations':['I2S/RTOS are host shims','does not prove FreeRTOS priority-inheritance timing or actual DMA underruns','no sound or device access'],
            'source_sha256_lf':{p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes().replace(b'\r\n',b'\n')).hexdigest() for p in [SRC/'alarm_bsp.c',SRC/'alarm_bsp.h',FIXTURE/'test_bsp.c',FIXTURE/'stubs/aa_host.h',FIXTURE/'stubs/esp_timer.h']}}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8');print(json.dumps(report,indent=2))


if __name__=='__main__':main()

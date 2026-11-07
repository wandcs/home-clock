"""Focused host build selection and playback checks; WSL uses desktop-user identity.

This does not access a device or play sound. Fixtures are synthetic, not private
music. Generated files and reports remain under the ignored .local directory.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import wave

ROOT=Path(__file__).resolve().parents[2]
TOOLS=ROOT/'tools/alarm-audio'
SRC=ROOT/'firmware/features/alarm-audio'


def linux(path):
    return '/mnt/'+path.drive[0].lower()+path.as_posix()[2:] if os.name=='nt' else str(path)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'.local/personal-alarm/host')
    args=parser.parse_args()
    out=args.output.resolve(); out.relative_to((ROOT/'.local').resolve())
    out.mkdir(parents=True,exist_ok=True)
    prefix=['wsl.exe','--exec'] if os.name=='nt' else []
    def run(command,success=True):
        proc=subprocess.run(prefix+command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        with (out/'commands.log').open('a',encoding='utf8') as log: log.write(json.dumps(command)+'\n'+proc.stdout+'\n')
        assert (proc.returncode==0)==success, proc.stdout
        return proc.stdout
    spec=importlib.util.spec_from_file_location('private_alarm',TOOLS/'prepare-private.py')
    helper=importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    wav=out/'fixture.wav'
    frames=31*22050
    payload=struct.pack('<'+str(frames)+'h',*((i*73)%60001-30000 for i in range(frames)))
    with wave.open(str(wav),'wb') as handle:
        handle.setnchannels(1);handle.setsampwidth(2);handle.setframerate(22050);handle.writeframes(payload)
    pcm,metadata=helper.read_wav(wav); assert pcm==payload
    for name in ('missing.wav','invalid.wav'):
        if name=='invalid.wav': (out/name).write_bytes(b'invalid')
        try: helper.read_wav(out/name)
        except ValueError as error: assert str(out) not in str(error)
        else: raise AssertionError('invalid input was accepted')
    invalid=out/'wrong-rate.wav'
    with wave.open(str(invalid),'wb') as handle:
        handle.setnchannels(1);handle.setsampwidth(2);handle.setframerate(48000);handle.writeframes(b'\0\0'*64)
    try: helper.read_wav(invalid)
    except ValueError: pass
    else: raise AssertionError('wrong format accepted')
    try: helper.prepare(wav,ROOT/'must-not-write.c')
    except ValueError: pass
    else: raise AssertionError('public output was accepted')
    project=out/'project';project.mkdir(exist_ok=True)
    cmake='cmake_minimum_required(VERSION 3.16)\nproject(reference_check C)\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n'
    cmake+='add_executable(reference_check "'+linux(TOOLS/'test_reference.c')+'" "'+linux(SRC/'alarm_source.c')+'" "'+linux(SRC/'alarm_player.c')+'")\n'
    cmake+='target_include_directories(reference_check PRIVATE "'+linux(SRC)+'")\n'
    cmake+='target_compile_options(reference_check PRIVATE -std=c11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer)\n'
    cmake+='target_link_options(reference_check PRIVATE -fsanitize=address,undefined)\ntarget_link_libraries(reference_check PRIVATE m)\n'
    cmake+='include("'+linux(TOOLS/'select.cmake')+'")\nclock_add_alarm_reference(reference_check python3)\n'
    (project/'CMakeLists.txt').write_text(cmake,encoding='utf8')
    build=out/'build'; reports=[]
    for mode,selected,expected in [('default','',88200),('private',linux(wav),frames),('default-again','',88200)]:
        text=run(['env','HOME_CLOCK_ALARM_AUDIO_WAV='+selected,'cmake','-S',linux(project),'-B',linux(build)])
        run(['cmake','--build',linux(build)])
        result=run([linux(build/'reference_check'),str(expected)]).strip()
        commands=json.loads((build/'compile_commands.json').read_text(encoding='utf8'))
        references=[entry['file'] for entry in commands if entry['file'].endswith('/alarm_reference.c') or entry['file'].endswith('/private-alarm/reference.c')]
        assert len(references)==1
        assert references[0].endswith('/private-alarm/reference.c')==(mode=='private')
        reports.append({'mode':mode,'frames':expected,'test':result,'unique_reference':True})
    bad=run(['env','HOME_CLOCK_ALARM_AUDIO_WAV='+linux(out/'missing.wav'),'cmake','-S',linux(project),'-B',linux(build)],success=False)
    assert 'Explicit private alarm audio is missing or invalid' in bad and 'missing.wav' not in bad
    # End with the public selection in the reusable cache.
    run(['env','HOME_CLOCK_ALARM_AUDIO_WAV=','cmake','-S',linux(project),'-B',linux(build)])
    report={'ok':True,'sanitizers':'ASan + UBSan','selection':reports,'invalid_input_rejected':True,
            'fixture_is_synthetic':True,'processing_preserves_pcm':True,'output_scope_enforced':True,
            'device_access':False,'audible_verification':False,'sdk_build':False}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()

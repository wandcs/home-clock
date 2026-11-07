"""Focused registered-callback/first-failure/concurrent snapshot host checks."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
ROOT=Path(__file__).resolve().parents[3]
SDK=ROOT/'.local/esp-idf-v6.1'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def linux(path):
    s=path.resolve().as_posix()
    return '/mnt/'+s[0].lower()+s[2:]
def function(text,name):
    match=re.search(r'(?m)^[^\n]*\b'+re.escape(name)+r'\([^\n]*\)\s*\{',text)
    if not match:raise ValueError('Pinned SDK function not found: '+name)
    pos=text.index('{',match.start());depth=0
    for end in range(pos,len(text)):
        depth+=(text[end]=='{')-(text[end]=='}')
        if not depth:return text[match.start():end+1]
    raise ValueError('Unbalanced pinned SDK function: '+name)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();out=(ROOT/args.out).resolve()
    out.relative_to(ROOT/'.local/s25/diagnostics');out.mkdir(parents=True,exist_ok=False)
    generated=out/'generated';generated.mkdir()
    base=Path(__file__).parent
    sdk_c=SDK/'components/heap/heap_caps.c';sdk_h=SDK/'components/heap/include/esp_heap_caps.h'
    sdk_text=sdk_c.read_text(encoding='utf-8-sig');sdk_header=sdk_h.read_text(encoding='utf-8-sig')
    names=['MALLOC_CAP_DMA','MALLOC_CAP_INTERNAL','MALLOC_CAP_8BIT','MALLOC_CAP_DEFAULT','MALLOC_CAP_SPIRAM']
    caps='\n'.join(re.search(r'(?m)^#define\s+'+name+r'\s+[^\n]+',sdk_header)[0] for name in names)
    hook_type=re.search(r'typedef void \(\*esp_alloc_failed_hook_t\)[^;]+;',sdk_header)[0]
    (generated/'esp_heap_caps.h').write_text('#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#include "esp_err.h"\n'+caps+'\n'+hook_type+'\nesp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);\n',encoding='utf-8')
    source='\n'.join(sdk_text.splitlines()[:5])+'\n#include "esp_heap_caps.h"\n#include "platform.h"\n#define HEAP_IRAM_ATTR\n#define NOINLINE_ATTR\nstatic esp_alloc_failed_hook_t alloc_failed_callback;\n'
    source+=function(sdk_text,'heap_caps_alloc_failed')+'\n'+function(sdk_text,'heap_caps_register_failed_alloc_callback')
    source+='\nvoid test_emit_failed_alloc(size_t size,uint32_t caps,const char *name){test_callback_begin();heap_caps_alloc_failed(size,caps,name);test_callback_end();}\nbool test_hook_registered(void){return alloc_failed_callback!=NULL;}\n'
    (generated/'sdk-hook.c').write_text(source,encoding='utf-8')
    files=['firmware/main/network_memory.c','firmware/main/network_memory.h','firmware/main/app_main.c','firmware/main/app_ui.c']
    files+=[p.relative_to(ROOT).as_posix() for p in base.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    before={n:sha(ROOT/n) for n in files}
    commands=[]
    def run(argv,tag,allow_failure=False):
        command=['wsl.exe','--cd',linux(ROOT),'--exec']+argv
        result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45)
        text=result.stdout.decode('utf-8',errors='replace');(out/(tag+'.log')).write_text(text,encoding='utf-8')
        commands.append({'tag':tag,'argv':command,'exit':result.returncode})
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n',encoding='utf-8')
        if result.returncode and not allow_failure:raise RuntimeError(text[-6000:])
        return result.returncode,text
    include=['-I'+linux(generated),'-I'+linux(base/'stubs'),'-I'+linux(ROOT/'firmware/main'),'-I'+linux(base)]
    inputs=[linux(ROOT/'firmware/main/network_memory.c'),linux(generated/'sdk-hook.c'),linux(base/'platform.c'),linux(base/'memory-test.c')]
    wraps=['-Wl,--wrap='+n for n in ['malloc','calloc','realloc','free','printf','puts']]
    common=['gcc','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-pthread']+include
    binary=out/'memory-test'
    run(common+['-fsanitize=address,undefined','-fno-omit-frame-pointer']+inputs+wraps+['-o',linux(binary)],'build-asan-ubsan')
    results=[]
    for case in ['filter','concurrent']:
        _,text=run([linux(binary),case],case)
        result=json.loads(text.splitlines()[-1]);assert result['passed'];results.append(result)
    tsan=out/'memory-test-tsan'
    run(common+['-fsanitize=thread','-fPIE','-pie']+inputs+wraps+['-o',linux(tsan)],'build-tsan')
    rc,text=run([linux(tsan),'concurrent'],'tsan',True)
    if rc:
        unsupported=('ThreadSanitizer: unexpected memory mapping' in text or
                     ('tsan_platform_linux.cpp' in text and 'personality(' in text))
        if not unsupported:raise RuntimeError(text[-6000:])
        tsan_result={'status':'runtime-unavailable','reason':'WSL sanitizer mapping/personality restriction','exit':rc}
    else:
        tsan_result=json.loads(text.splitlines()[-1]);assert tsan_result['passed']
    main=(ROOT/'firmware/main/app_main.c').read_text(encoding='utf-8-sig')
    assert main.count('network_memory_start()')==1
    start=main.index('network_memory_start()')
    assert all(start<main.index(name) for name in ['app_state_init()','weather_start()','app_ui_start()','network_time_start()'])
    ui=(ROOT/'firmware/main/app_ui.c').read_text(encoding='utf-8-sig')
    assert all('\\"'+key+'\\"' in ui for key in ['dma_largest','alloc_failures','alloc_fail_first_size','alloc_fail_first_caps','alloc_fail_first_ms'])
    after={n:sha(ROOT/n) for n in files};assert before==after,'Diagnostics source changed during host check'
    result={'passed':True,'cases':results,'thread_sanitizer':tsan_result,'inputs':before,'inputs_stable':True,
            'pinned_sdk_heap_sha256':sha(sdk_c),'pinned_sdk_header_sha256':sha(sdk_h),
            'sdk_registration_and_dispatcher_original_bodies':True,'startup_single_registration_before_workers':True,
            'callback_allocator_and_stdio_wrappers':True,'SDK_build_invoked':False,'device_access':False}
    (out/'checks.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='inputs'},indent=2))
if __name__=='__main__':main()

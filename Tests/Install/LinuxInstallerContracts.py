#!/usr/bin/env python3
# Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
# Isolated fake-HOME fault injection; never uses the caller's install paths.
import pathlib,tempfile,tarfile,hashlib,os,subprocess,json,sys,shutil
root=pathlib.Path(tempfile.mkdtemp(prefix='swara-installer-contracts-'))
script=pathlib.Path(sys.argv[1]);rows=[]
shim='''#!/usr/bin/python3
import os,sys,pathlib,subprocess,signal
name=pathlib.Path(sys.argv[0]).name;a=sys.argv[1:];fault=os.environ.get('FAULT','')
if name=='curl':
    if a[-1].endswith('/latest'):print('https://github.com/montronedsp/SWARAXT/releases/tag/v1.2.3',end='');sys.exit(0)
    target=a[a.index('-o')+1];p=pathlib.Path(target)
    if a[-1].endswith('.tar.gz'):p.write_bytes(pathlib.Path(os.environ['ASSET']).read_bytes())
    else:p.write_text(('0'*64 if fault=='checksum' else os.environ['SHA'])+'  SwaraXT-Linux-x86_64-v1.2.3.tar.gz\\n')
    sys.exit(0)
src,dest=a[-2:]
if name=='chmod' and fault=='chmod-stage':sys.exit(1)
if name=='rm' and fault=='cleanup-backup' and any(v.endswith('.backup') for v in a):sys.exit(1)
if name=='cp' and ((fault=='stage-copy' and dest.endswith('.vst3.stage')) or (fault=='standalone-copy' and 'swara-xt.stage' in dest)):sys.exit(1)
if name=='mv':
    if fault=='backup' and dest.endswith('.vst3.backup'):sys.exit(1)
    if fault=='standalone-backup' and dest.endswith('swara-xt.backup'):sys.exit(1)
    if fault in ['publish','rollback'] and src.endswith('.vst3.stage'):sys.exit(1)
    if fault=='rollback' and src.endswith('.vst3.backup'):sys.exit(1)
    if fault=='standalone-publish' and src.endswith('swara-xt.stage'):sys.exit(1)
    if fault=='interrupt' and dest.endswith('.vst3.backup'):
        r=subprocess.call(['/bin/mv',*a]);os.kill(os.getppid(),signal.SIGKILL);sys.exit(r)
sys.exit(subprocess.call(['/bin/'+name,*a]))
'''
for case in ['fresh','upgrade','checksum','missing-vst','missing-standalone','stage-copy','standalone-copy','backup','publish','rollback','standalone-publish','interrupt','repeat','chmod-stage','standalone-backup','cleanup-backup','stale-backup']:
    test=pathlib.Path(tempfile.mkdtemp(prefix=case+'-',dir=root));home=test/'home';home.mkdir();release=test/'payload/Swara XT';vst=release/'VST3/Swara XT.vst3';st=release/'Standalone/Swara XT'
    if case!='missing-vst':vst.mkdir(parents=True);(vst/'plugin.so').write_text('new-vst')
    if case!='missing-standalone':st.parent.mkdir(parents=True);st.write_text('new-standalone')
    asset=test/'asset.tar.gz'
    with tarfile.open(asset,'w:gz') as t:t.add(release,arcname='Swara XT')
    oldv=home/'.vst3/Swara XT.vst3';olds=home/'.local/bin/swara-xt'
    if case!='fresh':oldv.mkdir(parents=True);(oldv/'plugin.so').write_text('old-vst');olds.parent.mkdir(parents=True);olds.write_text('old-standalone')
    if case=='stale-backup':
        backup=pathlib.Path(str(oldv)+'.backup');backup.mkdir();(backup/'plugin.so').write_text('old-vst')
    bin=test/'bin';bin.mkdir()
    for name in ['curl','cp','mv','chmod','rm']:(bin/name).write_text(shim);(bin/name).chmod(0o755)
    downloads=test/'downloads';downloads.mkdir()
    hashes=lambda:{str(p.relative_to(home)):hashlib.sha256(p.read_bytes()).hexdigest() for p in home.rglob('*') if p.is_file()}
    before_hashes=hashes()
    env=dict(os.environ,TMPDIR=str(downloads),HOME=str(home),PATH=str(bin)+':/usr/bin:/bin',ASSET=str(asset),SHA=hashlib.sha256(asset.read_bytes()).hexdigest(),FAULT=case)
    result=subprocess.run(['bash',str(script)],env=env,capture_output=True,text=True);log=result.stdout+result.stderr
    oldcopies=lambda:[str(p.relative_to(home)) for p in home.rglob('plugin.so') if p.read_text()=='old-vst']
    before=oldcopies();again=None
    if case in ['interrupt','rollback','repeat']:
        env['FAULT']='';r=subprocess.run(['bash',str(script)],env=env,capture_output=True,text=True);again=r.returncode;log+='\nSECOND INVOCATION\n'+r.stdout+r.stderr
    preserved=oldcopies()
    ok=True
    if case in ['fresh','upgrade','repeat']:ok=result.returncode==0 and (oldv/'plugin.so').read_text()=='new-vst'
    elif case in ['interrupt','rollback']:ok=bool(before) and bool(preserved)
    elif case in ['checksum','missing-vst','missing-standalone','stage-copy','standalone-copy','backup','publish','standalone-publish','chmod-stage','standalone-backup','cleanup-backup','stale-backup']:ok=result.returncode!=0 and bool(preserved)
    # Every failure must retain both components, not only the VST bundle.
    if case not in ['fresh','upgrade','repeat']:
        ok=ok and any(p.is_file() and p.read_text()=='old-standalone' for p in home.rglob('swara-xt*'))
    if case!='interrupt':
        ok=ok and not any(downloads.iterdir()) and not (home/'.swara-xt-install.lock').exists()
        ok=ok and not any(p.name.endswith('.stage') for p in home.rglob('*'))
    (test/'execution.log').write_text(log)
    rows.append(dict(beforeHashes=before_hashes,afterHashes=hashes(),case=case,exit=result.returncode,secondExit=again,oldCopiesBeforeRepeat=before,oldCopiesFinal=preserved,pass_=ok,fixture=str(test)))
print(json.dumps(rows,indent=2));sys.exit(any(not r['pass_'] for r in rows))

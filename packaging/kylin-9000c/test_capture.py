import os,pathlib,tempfile,subprocess,time,struct,shutil
root=pathlib.Path(tempfile.mkdtemp(prefix='sunshine-release-test-'))
env=os.environ.copy();env['XDG_RUNTIME_DIR']=str(root);env['DBUS_SESSION_BUS_ADDRESS']='unix:path=/run/user/'+str(os.getuid())+'/bus'
p=subprocess.Popen(['/usr/bin/python3',str(pathlib.Path(__file__).with_name('capture.py'))],env=env)
try:
 frame=root/'sunshine-capture/frame.bgra'
 for _ in range(60):
  if frame.exists():break
  time.sleep(.1)
 data=frame.read_bytes(); w,h,a=struct.unpack('<IIQ',data[:16])
 assert (w,h)==(1920,1080) and len(data)==16+w*h*4
 assert sum(any(data[i:i+3]) for i in range(16,len(data)-3,4096))>100
 time.sleep(1)
 b=struct.unpack('<IIQ',frame.open('rb').read(16))[2]
 assert b>a
 print('PASS: public capture helper produces nonblack complete frames and advances sequence')
finally:
 p.terminate();p.wait(timeout=8)
 assert not (root/'sunshine-capture/frame.bgra').exists()
 shutil.rmtree(root)
 print('PASS: termination removes transient frame')

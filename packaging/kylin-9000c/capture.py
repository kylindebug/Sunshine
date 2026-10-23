#!/usr/bin/python3
"""UKUI native shared-memory screen capture for Sunshine, without PNG."""
import os,pathlib,signal,struct,time,mmap
import dbus
os.umask(0o077)
directory=pathlib.Path(os.environ.get('XDG_RUNTIME_DIR', '/run/user/'+str(os.getuid())))/'sunshine-capture'
directory.mkdir(mode=0o700,exist_ok=True)
frame=directory/'frame.bgra'
temporary=directory/'frame.tmp'
width=int(os.environ.get('SUNSHINE_CAPTURE_WIDTH','1920'))
height=int(os.environ.get('SUNSHINE_CAPTURE_HEIGHT','1080'))
size=width*height*4
fd=os.memfd_create('sunshine-ukui-screencopy',0)
os.ftruncate(fd,size)
pixels=mmap.mmap(fd,size)
bus=dbus.SessionBus()
running=True
def stop(*_):
 global running
 running=False
signal.signal(signal.SIGTERM,stop)
signal.signal(signal.SIGINT,stop)
sequence=time.monotonic_ns()
errors=0
try:
 while running:
  start=time.monotonic()
  try:
   result=bus.call_blocking('org.ukui.KWin','/Screencopy','org.ukui.kwin.Screencopy','screencopyArea','hiiiiib',(dbus.types.UnixFd(fd),0,0,width,height,0,True),timeout=5)
   if not result: raise RuntimeError('Screencopy rejected')
   sequence+=1
   with temporary.open('wb') as output:
    output.write(struct.pack('<IIQ',width,height,sequence))
    output.write(pixels)
   os.replace(temporary,frame)
   errors=0
  except Exception as exc:
   errors+=1
   if errors==1 or errors%30==0: print('Screencopy failed:',type(exc).__name__,flush=True)
   frame.unlink(missing_ok=True)
  time.sleep(max(.001,1/30-(time.monotonic()-start)))
finally:
 frame.unlink(missing_ok=True)
 temporary.unlink(missing_ok=True)
 pixels.close()
 os.close(fd)

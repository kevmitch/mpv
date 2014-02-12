#!/usr/bin/env python
import os,sys,re,time,math
unix_unfriendly      = re.compile('[^.\w-]+',re.IGNORECASE)

def unix_friendly(s):
    """Make a given string suitable for a Unix friendly filename.
    Transfer to lowercase and replace each unix unfriendly group of characters with '_'"""
    return unix_unfriendly.sub('_',s.encode('ascii','replace').lower())

def time_read_chunk(url_of_large_file,chunk_size,target_seconds=2.0):
    seconds=0.0
    while True:
        start=time.time()
        rc=os.system('../build/mpv --vo=null --no-resume-playback --cache=%d --cache-min=99 --length=0 --read-chunk=%d %s'%(time_read_chunk.cache_size,chunk_size,url_of_large_file))
        seconds=time.time()-start
        if rc!=0: break
        if seconds<target_seconds:
            time_read_chunk.cache_size*=2
            continue
        else:
            break
    
    return chunk_size,time_read_chunk.cache_size*0.99/seconds
time_read_chunk.cache_size=32

def time_read_chunks(url_of_large_file,minpow,npow,cycles):
    outfile='%s_%02d_%02d'%(unix_friendly(url_of_large_file),minpow,npow)
    f=open(outfile,'w')
    f.write('read-chunk kbps\n')
    for i in xrange(npow):
        for j in xrange(cycles):
            f.write('%d %g\n'%time_read_chunk(url_of_large_file,chunk_size=2**(minpow+i)))
    f.close()


if __name__ == "__main__":
    time_read_chunks(url_of_large_file=sys.argv[1],minpow=11,npow=14,cycles=10)
    

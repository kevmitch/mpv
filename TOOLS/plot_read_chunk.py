#!/usr/bin/env python
import sys
from matplotlib import pyplot as pp
import numpy as np

def plot_file(fnames_labels):

    fig=pp.figure()
    ax=fig.add_subplot(111)
    labels=[]
    for fname_label in fnames_labels:
        fname,label=fname_label.split('=')
        data=np.genfromtxt(fname,skip_header=1)
        data[:,0]/=1024.
        ax.loglog(data[:,0],data[:,1],'.',label=label)
        labels.append(label)    
    ax.set_xscale('log',basex=2)
    ax.set_yscale('log',basey=2)
    ax.set_xlabel('read_chunk (KiB)')
    ax.set_ylabel('throughput (KiB/s)')
    ax.legend(loc='best',frameon=False)
    ax.set_title('smb:// performance')
    fig.savefig('.'.join(labels)+'.png',dpi=150)
    

if __name__ == "__main__":
    plot_file(sys.argv[1:])

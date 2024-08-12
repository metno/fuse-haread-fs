# fuse-haread-fs


## Project status

Pre-alfa. Work in progress 

## Name
fuse-haread-fs

## Scenario 
You have "parallel production", which means we are producing more or less the same files in two separate data centers. Downstream users mount the two file systems read-only with CIFS or NFS. They now need to be bothered with which of the file systems their files should be read from.

## Description
fuse-haread-fs is a union mount filesystem implementation for Linux. It combines multiple (two at this point) different underlying mount points into one, resulting in single directory structure that contains underlying files and sub-directories from all sources .

It behaves like  [OverlayFS](https://github.com/containers/fuse-overlayfs) or [UnionFS](https://github.com/rpodgorny/unionfs-fuse), except it supports NFS and CIFS as underlying filesystems, which OverlayFS does not. It will not block, unlike UnionFS, if one underlying file system blocks (for example, if the NFS server is down). If a file system blocks, users still have access to files on the remaining system, if it is online. Another distinction is that haread-fs is a read-only file system.

## Installation
`make`

## Usage example
`./haread-fs /lustre/storeA,/lustre/storeB mountpoint -f `

The `-f`is important . It tells fuse not to fork. Important to keep the file system monitoring threads running


## License
GPL v3


## Bugs 
Probably plenty
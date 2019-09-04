# usb_rt_driver
Linux driver for usb devices that supports high frequency communication. Based 
on usb-skeleton from the linux source with slight modifications for 
asynchronous i/o.

Uses cmake to build. Dependencies are cmake, linux-headers, and dkms. The 
recommended install is to create a deb package and install it. 

## build
```console
$ mkdir build
$ cd build
$ cmake ..
$ make package
$ sudo dpkg -i usb_rt_driver*.deb
```

## install notes
The package will install source to `/usr/src/usb_rt-*` and registers it 
with dkms. The source is then built automatically using dkms when new kernel 
versions are installed. Additionally `99-usb_rt_driver.rules` is installed to 
`/etc/udev/rules.d` to assist with permissions.
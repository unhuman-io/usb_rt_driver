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

## driver
This driver will create character devices to communicate with usb hardware. The 
default dev name is `/dev/usbrt*`. Communication with the device is possible 
using standard system calls for files, and command line utilities. For example, 
to read 32 bytes of floats from usbrt0
```console
$ od -w32 -f /dev/usbrt0 
```
and a c code example for communication is:
```c
#include <unistd.h>

int main() {
    uint8_t data[32];
    int fd = open("/dev/usbrt0", O_RDWR);
    read(fd, data, 32);
    write(fd, data, 32);
    return 0;
}
```

## other notes
Only up to 64 byte packets can be properly processed through this driver.
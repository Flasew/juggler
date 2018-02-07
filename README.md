Juggler targets on making the Linux kernel capable of handling extensively reordered TCP traffic by both lowering cpu utilization and avoiding Dupacks and retransmissions.

This project is initiated by Yilong Geng, Mohammad Alizadeh, Vimalkumar
Jeyakumar and Abdul Kabbani.

Installation instructions
------------------------
*These instructions work for a Debian 9 system. The patch will be applied to Linux kernel 4.1.8.*

To use juggler you need to compile you own patched kernel.

To start let's create a working directory.
```bash
mkdir juggler && cd juggler
```

From now on, most commands require root privileges, please become root.

To compile the kernel we will need a couple of dependencies:
```bash
apt install git fakeroot build-essential ncurses-dev xz-utils libssl-dev bc
```

Add stretch backports to your sources.list
```bash
echo "deb http://ftp.debian.org/debian stretch-backports main" >> /etc/apt/sources.list
apt update
```
and install kernel-package:
```bash
apt install kernel-package
```

Download the Linux kernel 4.1.8:
```bash
wget https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.1.8.tar.xz
tar -xf linux-4.1.8.tar.xz
```

Clone the juggler repository:
```bash
git clone https://github.com/gengyl08/juggler.git
```

Apply the patch:
```bash
cd linux-4.1.8
cp -r juggler/linux-4.1/* .
```

Compile your patched kernel:
```bash
fakeroot make-kpkg --initrd --revision=1.0.juggler kernel_image kernel_headers -j $(nproc --all)
```

If the compilation completes successfully, you should have 2 brand new deb's in the upper dir. We will use dpkg to install them
```bash
cd ..
dpkg -i linux-headers-4.1.8_1.0.juggler_amd64.deb
dpkg -i linux-image-4.1.8_1.0.juggler_amd64.deb
```
Now you may reboot the system to use the new kernel. By default, the new kernel will be added to grub as an entry under advanced options. Make sure to select it when booting the system.

```bash
shutdown -r now
```

Check if you are using the new kernel:
```bash
uname -r
```
It should display `4.1.8`.

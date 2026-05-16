# Отчёт по тестированию SimpleFS (Кузнецов Н.А. 1304)

Реализован модуль ядра Linux `simplefs` и userspace-утилита
[user/simplefs_cli](user/simplefs_cli.c).

Модуль:

* регистрирует ФС `simplefs` в VFS, монтируется через `mount -t simplefs`;
* при первом монтировании пишет две копии суперблока по смещениям
  `sb_first_offset` / `sb_second_offset` и создаёт фиксированное число
  файлов размера `max_file_sectors` секторов каждый;
* считает CRC32 суперблока, при монтировании проверяет обе копии и
  восстанавливает битую из валидной;
* файловые операции: `read`, `write`, `llseek`, `lookup`, `iterate_shared`;
* IOCTL: `ZERO_ALL`, `ERASE_FS`, `GET_META` (имя, offset, размер,
  CRC32 содержимого по каждому файлу), `GET_MAPPING` (список секторов файла).

Утилита `simplefs_cli`:

* `demo` — в каждый файл пишет случайное 64-битное число и читает обратно
  со сверкой;
* `zero`, `erase`, `meta`, `mapping` — вызовы соответствующих IOCTL.

## 2. Окружение

* Гость: Ubuntu, ядро `6.14.0-37-generic`
* Loop-устройство на обычном файле `disk.img` (1 МБ = 2048 секторов)
* Параметры модуля при тестах: `sb_first_offset=0 sb_second_offset=64
  max_file_sectors=8`

## 3. Сборка

```
nick@nick:~/projects/simplefs$ make clean
make -C /lib/modules/6.14.0-37-generic/build M=/home/nick/projects/simplefs clean
make[1]: Entering directory '/usr/src/linux-headers-6.14.0-37-generic'
make[2]: Entering directory '/home/nick/projects/simplefs'
  CLEAN   Module.symvers
make[2]: Leaving directory '/home/nick/projects/simplefs'
make[1]: Leaving directory '/usr/src/linux-headers-6.14.0-37-generic'
make -C user clean
make[1]: Entering directory '/home/nick/projects/simplefs/user'
rm -f simplefs_cli
make[1]: Leaving directory '/home/nick/projects/simplefs/user'
nick@nick:~/projects/simplefs$ make
make -C /lib/modules/6.14.0-37-generic/build M=/home/nick/projects/simplefs modules
make[1]: Entering directory '/usr/src/linux-headers-6.14.0-37-generic'
make[2]: Entering directory '/home/nick/projects/simplefs'
warning: the compiler differs from the one used to build the kernel
  The kernel was built by: x86_64-linux-gnu-gcc-14 (Ubuntu 14.2.0-19ubuntu2) 14.2.0
  You are using:           gcc-14 (Ubuntu 14.2.0-19ubuntu2) 14.2.0
  CC [M]  simplefs_main.o
  LD [M]  simplefs.o
  MODPOST Module.symvers
  CC [M]  simplefs.mod.o
  CC [M]  .module-common.o
  LD [M]  simplefs.ko
  BTF [M] simplefs.ko
Skipping BTF generation for simplefs.ko due to unavailability of vmlinux
make[2]: Leaving directory '/home/nick/projects/simplefs'
make[1]: Leaving directory '/usr/src/linux-headers-6.14.0-37-generic'
nick@nick:~/projects/simplefs$ make user
make -C user
make[1]: Entering directory '/home/nick/projects/simplefs/user'
gcc -Wall -Wextra -O2 -o simplefs_cli simplefs_cli.c
make[1]: Leaving directory '/home/nick/projects/simplefs/user'
nick@nick:~/projects/simplefs$ ls -la simplefs.ko user/simplefs_cli
-rw-rw-r-- 1 nick nick 570456 мая 14 20:42 simplefs.ko
-rwxrwxr-x 1 nick nick  17568 мая 14 20:42 user/simplefs_cli
```

## 4. Подготовка диска и монтирование

```
nick@nick:~/projects/simplefs$ cd ~/projects/simplefs
nick@nick:~/projects/simplefs$ dd if=/dev/zero of=disk.img bs=512 count=2048 status=none
nick@nick:~/projects/simplefs$ sudo losetup -fP --show disk.img
[sudo] password for nick:
/dev/loop0
nick@nick:~/projects/simplefs$ sudo insmod simplefs.ko sb_first_offset=0 sb_second_offset=64 max_file_sectors=8
nick@nick:~/projects/simplefs$ sudo mkdir -p /mnt/simplefs
nick@nick:~/projects/simplefs$ sudo mount -t simplefs /dev/loop0 /mnt/simplefs
nick@nick:~/projects/simplefs$ mount | grep simplefs
/dev/loop0 on /mnt/simplefs type simplefs (rw,relatime)
nick@nick:~/projects/simplefs$ ls /mnt/simplefs | head
file_0
file_1
file_10
file_100
file_101
file_102
file_103
file_104
file_105
file_106
nick@nick:~/projects/simplefs$ ls /mnt/simplefs | wc -l
254
nick@nick:~/projects/simplefs$ sudo dmesg | tail -10
[   14.616062] NET: Registered PF_QIPCRTR protocol family
[   14.691664] loop0: detected capacity change from 0 to 8
[   22.054151] systemd-journald[347]: File /var/log/journal/8bd3f2cec74f4583b13801d1dac87a7f/user-1000.journal corrupted or uncleanly shut down, renaming and replacing.
[  199.484854] loop0: detected capacity change from 0 to 2048
[  206.560455] simplefs: loading out-of-tree module taints kernel.
[  206.560461] simplefs: module verification failed: signature and/or required key missing - tainting kernel
[  206.561529] simplefs: registered
[  214.289672] simplefs: no valid SB found, initializing new FS
[  214.290123] simplefs: initialized 254 files (M=8)
[  214.290138] simplefs: mounted; files=254 M=8 sb1=0 sb2=64
```

При монтировании на чистом диске модуль создал 254 файла по M=8 секторов
и записал обе копии суперблока.

## 5. Запись и чтение случайных значений

В каждый файл пишется случайное 64-битное число и сразу читается обратно.

```
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli demo /mnt/simplefs | head -5
[sudo] password for nick:
[OK]   file_0  0x3e32403074d6a152
[OK]   file_1  0x3d2e139217ab8b4a
[OK]   file_2  0x7457d4812c58b80c
[OK]   file_3  0x2624231204434e48
[OK]   file_4  0x527166190cf21e0e
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli demo /mnt/simplefs | tail -5
[OK]   file_251  0x0197855352aa159b
[OK]   file_252  0x796e66ca4ecfc83e
[OK]   file_253  0x375f5cc140a8cb3d

Total: ok=254 fail=0
```

## 6. IOCTL: GET_META (метаинформация и хеши)

```
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli meta /mnt/simplefs | head -10
NAME                   OFFSET_SEC         SIZE  CRC32
file_0                          1         4096  0xacc71247
file_1                          9         4096  0x2119a578
file_2                         17         4096  0x9b361f72
file_3                         25         4096  0xe652eb0a
file_4                         33         4096  0xd13b74ea
file_5                         41         4096  0x6e98586e
file_6                         49         4096  0x979a667b
file_7                         65         4096  0x36e16537
file_8                         73         4096  0xc068967a
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli meta /mnt/simplefs | tail -3
file_251                     2017         4096  0xdf9ff88a
file_252                     2025         4096  0x76fccc92
file_253                     2033         4096  0x673221c0
```

Между `file_6` (сектора 49..56) и `file_7` (offset 65) пропущен
сектор 64 — он зарезервирован под backup-копию суперблока.

## 7. IOCTL: GET_MAPPING

```
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli mapping /mnt/simplefs file_3
name:          file_3
start_sector:  25
sector_count:  8
size_bytes:    4096
sectors:       [25 .. 32]
```

## 8. IOCTL: ZERO_ALL

```
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli zero /mnt/simplefs
All files zeroed.
nick@nick:~/projects/simplefs$ sudo hexdump -C -n 32 -s $((25*512)) /dev/loop0
00003200  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
*
00003220
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli meta /mnt/simplefs | head -5
NAME                   OFFSET_SEC         SIZE  CRC32
file_0                          1         4096  0x00000000
file_1                          9         4096  0x00000000
file_2                         17         4096  0x00000000
file_3                         25         4096  0x00000000
```

Сектора файлов занулены, CRC32 содержимого = 0.

## 9. Восстановление primary SB из backup

Размонтируем, забиваем первый сектор случайными данными, монтируем снова.

```
nick@nick:~/projects/simplefs$ sudo umount /mnt/simplefs
nick@nick:~/projects/simplefs$ sudo dd if=/dev/urandom of=/dev/loop0 bs=512 count=1 conv=notrunc status=none
nick@nick:~/projects/simplefs$ sudo mount -t simplefs /dev/loop0 /mnt/simplefs
nick@nick:~/projects/simplefs$ sudo dmesg | tail -5
[  214.289672] simplefs: no valid SB found, initializing new FS
[  214.290123] simplefs: initialized 254 files (M=8)
[  214.290138] simplefs: mounted; files=254 M=8 sb1=0 sb2=64
[  341.734117] simplefs: primary SB broken, using backup and restoring primary
[  341.734336] simplefs: mounted; files=254 M=8 sb1=0 sb2=64
nick@nick:~/projects/simplefs$ ls /mnt/simplefs | wc -l
254
```

## 10. IOCTL: ERASE_FS и повторная инициализация

```
nick@nick:~/projects/simplefs$ sudo ./user/simplefs_cli erase /mnt/simplefs
FS erased (unmount to complete).
nick@nick:~/projects/simplefs$ sudo umount /mnt/simplefs
nick@nick:~/projects/simplefs$ sudo mount -t simplefs /dev/loop0 /mnt/simplefs
nick@nick:~/projects/simplefs$ sudo dmesg | tail -5
[  341.734117] simplefs: primary SB broken, using backup and restoring primary
[  341.734336] simplefs: mounted; files=254 M=8 sb1=0 sb2=64
[  358.791965] simplefs: no valid SB found, initializing new FS
[  358.791992] simplefs: initialized 254 files (M=8)
[  358.791998] simplefs: mounted; files=254 M=8 sb1=0 sb2=64
nick@nick:~/projects/simplefs$ ls /mnt/simplefs | wc -l
254
```

После `erase` обе копии SB занулены, при следующем монтировании ФС
инициализируется заново.

## 11. Завершение

```
nick@nick:~/projects/simplefs$ sudo umount /mnt/simplefs
nick@nick:~/projects/simplefs$ sudo rmmod simplefs
nick@nick:~/projects/simplefs$ sudo losetup -d /dev/loop0
```

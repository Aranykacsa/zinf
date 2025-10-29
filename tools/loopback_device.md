```bash
# 1. Create a 5 MB test file (10,240 sectors of 512 bytes)
dd if=/dev/zero of=testdisk.img bs=512 count=10240

# 2. Attach it as a loopback device
sudo losetup --find --show testdisk.img
```

```bash
# 3. Remove loopback device
sudo losetup -d /dev/loop0
```

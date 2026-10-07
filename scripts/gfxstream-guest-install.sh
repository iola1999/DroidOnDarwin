#!/system/bin/sh
# Install from an extracted, hash-checked package as root. The original Mesa
# libraries remain available when QEMU starts with a different GPU device.
set -eu
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo 'root shell required' >&2; exit 1; }
[ "$(getenforce)" = Permissive ] || { echo 'this profile requires the HyperOS permissive image' >&2; exit 1; }
sha256sum -c SHA256SUMS
rc=/vendor/etc/init/hw/init.virtio.rc
hook='    exec u:r:vendor_virtgpu_detect:s0 root graphics -- /vendor/bin/husk-gfxstream-detect'
original='    exec u:r:vendor_virtgpu_detect:s0 system graphics -- /vendor/bin/virtgpu_detect'
grep -F -x "$original" "$rc" >/dev/null || { echo 'unsupported vendor init file' >&2; exit 1; }
mount -o remount,rw /vendor
for lib in lib64/*.so lib64/egl/*.so; do
    destination="/vendor/$lib"
    # Processes may still map the previous library until the next cold boot.
    cp "$lib" "$destination.husk-new"
    chmod 0644 "$destination.husk-new"
    mv -f "$destination.husk-new" "$destination"
    restorecon "$destination"
done
cp husk-gfxstream-detect /vendor/bin/
chmod 0755 /vendor/bin/husk-gfxstream-detect
if ! grep -F -x "$hook" "$rc" >/dev/null; then
    # Keep the init file's inode, mode and SELinux context.
    sed '/-- \/vendor\/bin\/virtgpu_detect/i\    exec u:r:vendor_virtgpu_detect:s0 root graphics -- /vendor/bin/husk-gfxstream-detect' "$rc" > init.virtio.rc
    cat init.virtio.rc > "$rc"
    rm init.virtio.rc
fi
restorecon /vendor/bin/husk-gfxstream-detect
sync
echo 'HUSK-GFXSTREAM INSTALLED; reboot the guest to select its GPU driver'

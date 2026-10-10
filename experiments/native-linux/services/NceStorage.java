/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.os.IBinder;
import java.io.File;

public final class NceStorage {
    public static void main(String[] args) throws Exception {
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, "vold");
        Object service = Class.forName("android.os.IVold$Stub")
                .getMethod("asInterface", IBinder.class).invoke(null, binder);
        // Exercise the original CE/DE preparation before system_server stores credentials.
        Class.forName("android.os.IVold")
                .getMethod("prepareUserStorage", String.class, int.class, int.class)
                .invoke(service, null, 0, 3);
        for (String path : new String[]{"/data/media/0", "/data/system_ce/0", "/data/system_de/0",
                "/data/vendor_ce/0/facedata", "/data/vendor_de/0/fpdata",
                "/data/misc_ce/0/vold", "/data/misc_de/0/vold", "/data/misc/profiles/cur/0"}) {
            if (!new File(path).isDirectory()) throw new AssertionError("vold directory missing: " + path);
        }
        System.out.println("{\"test\":\"android_vold_user_storage\",\"passed\":true,\"user\":0,\"flags\":3}");
    }
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
public final class NceShutdown {
    public static void main(String[] args) throws Exception {
        Class<?> manager = Class.forName("android.app.ActivityManager");
        Object service = manager.getMethod("getService").invoke(null);
        Class<?> contract = Class.forName("android.app.IActivityManager");
        boolean timedOut = (Boolean) contract.getMethod("shutdown", int.class).invoke(service, 10000);
        if (timedOut) throw new AssertionError("Android activity shutdown timed out");
        System.out.println("{\"test\":\"android_framework_shutdown\",\"passed\":true}");
    }
}

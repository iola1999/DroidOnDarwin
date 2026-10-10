/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.os.Parcel;
import android.os.Process;
import android.system.Os;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class NceZygoteChild {
    private static int counter;

    public static void main(String[] args) throws Exception {
        if (Process.myUid() != 1000 || ++counter != 1 || args.length != 1) {
            throw new AssertionError("child identity or static state");
        }
        if (System.getProperty("nce.child.marker") != null) throw new AssertionError("inherited child write");
        System.setProperty("nce.child.marker", args[0]);
        String status = new String(Files.readAllBytes(Paths.get("/proc/self/status")), StandardCharsets.US_ASCII);
        String cgroup = new String(Files.readAllBytes(Paths.get("/proc/self/cgroup")), StandardCharsets.US_ASCII);
        if (!status.contains("Seccomp:\t2\n") || !cgroup.contains("/uid_1000/pid_" + Process.myPid())) {
            throw new AssertionError("child seccomp or cgroup");
        }
        Process.class.getDeclaredMethod("setThreadGroup", int.class, int.class)
                     .invoke(null, Process.myTid(), 5);
        cgroup = new String(Files.readAllBytes(Paths.get("/proc/self/task/" + Process.myTid() + "/cgroup")),
                            StandardCharsets.US_ASCII);
        if (!cgroup.contains(":/top-app\n")) throw new AssertionError("CPU scheduling group");
        String[] values = new String[4096];
        for (int i = 0; i < values.length; ++i) values[i] = "zygote-" + i;
        System.gc();
        for (int i = 0; i < values.length; ++i) {
            if (!values[i].equals("zygote-" + i)) throw new AssertionError("child heap");
        }
        CountDownLatch done = new CountDownLatch(1);
        Thread thread = new Thread(new Runnable() {
            @Override public void run() { done.countDown(); }
        }, "zygote-child-worker");
        thread.start();
        if (!done.await(2, TimeUnit.SECONDS)) throw new AssertionError("child thread");
        thread.join();
        Parcel parcel = Parcel.obtain();
        parcel.writeString("forked Android");
        parcel.setDataPosition(0);
        if (!"forked Android".equals(parcel.readString())) throw new AssertionError("child parcel");
        parcel.recycle();
        String result = Process.myPid() + ":" + Os.getppid() + ":" + Process.myUid() + ":4096\n";
        try (FileOutputStream output = new FileOutputStream("/data/nce/zygote/temporary-" + args[0])) {
            output.write(result.getBytes(StandardCharsets.US_ASCII));
            output.getFD().sync();
        }
        Files.move(Paths.get("/data/nce/zygote/temporary-" + args[0]),
                   Paths.get("/data/nce/zygote/child-" + args[0]), StandardCopyOption.ATOMIC_MOVE);
    }
}

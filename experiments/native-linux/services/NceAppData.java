/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.os.SystemClock;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.concurrent.TimeUnit;

public final class NceAppData {
    private static final String PACKAGE = "com.google.android.deskclock";
    private static final String LABEL = "NCE storage check";
    private static final File RECORD = new File("/data/system/nce-clock-record-v1");

    private static long findAlarm(String path) {
        if (!new File(path).isFile()) return -1;
        try (SQLiteDatabase database = SQLiteDatabase.openDatabase(path, null,
                SQLiteDatabase.OPEN_READONLY | SQLiteDatabase.NO_LOCALIZED_COLLATORS);
             Cursor rows = database.rawQuery(
                "SELECT _id,hour,minutes,daysofweek FROM alarm_templates WHERE label=?",
                new String[]{LABEL})) {
            if (!rows.moveToFirst()) return -1;
            long id = rows.getLong(0);
            if (rows.getInt(1) != 23 || rows.getInt(2) != 59 || rows.getInt(3) == 0
                    || rows.moveToNext()) {
                throw new AssertionError("clock alarm contents changed");
            }
            return id;
        }
    }

    public static void main(String[] args) throws Exception {
        boolean restored = RECORD.exists();
        String path;
        long id;
        if (restored) {
            try (DataInputStream input = new DataInputStream(new FileInputStream(RECORD))) {
                path = input.readUTF();
                id = input.readLong();
                if (input.read() != -1 || id < 0) throw new AssertionError("clock record format");
            }
            if (findAlarm(path) != id) throw new AssertionError("clock data did not survive restart");
        } else {
            // Let the actual application handle its public SET_ALARM intent.
            // A repeating alarm retains its template after an occurrence.
            Process command = new ProcessBuilder("/system/bin/cmd", "activity", "start-activity", "-W",
                    "-a", "android.intent.action.SET_ALARM", "-p", PACKAGE,
                    "--ei", "android.intent.extra.alarm.HOUR", "23",
                    "--ei", "android.intent.extra.alarm.MINUTES", "59",
                    "--eial", "android.intent.extra.alarm.DAYS", "1",
                    "--es", "android.intent.extra.alarm.MESSAGE", LABEL,
                    "--ez", "android.intent.extra.alarm.SKIP_UI", "true").redirectErrorStream(true).start();
            if (!command.waitFor(20, TimeUnit.SECONDS)) {
                command.destroyForcibly();
                throw new AssertionError("clock intent timed out");
            }
            System.err.write(command.getInputStream().readAllBytes());
            if (command.exitValue() != 0) throw new AssertionError("clock intent failed");
            String[] paths = {"/data/user_de/0/" + PACKAGE + "/databases/alarms.db",
                              "/data/user/0/" + PACKAGE + "/databases/alarms.db"};
            path = null;
            id = -1;
            for (int attempt = 0; attempt < 40 && id < 0; ++attempt) {
                for (String candidate : paths) {
                    id = findAlarm(candidate);
                    if (id >= 0) {
                        path = candidate;
                        break;
                    }
                }
                if (id < 0) SystemClock.sleep(250);
            }
            if (id < 0) throw new AssertionError("clock did not store the alarm");
            try (FileOutputStream file = new FileOutputStream(RECORD);
                 DataOutputStream output = new DataOutputStream(file)) {
                output.writeUTF(path);
                output.writeLong(id);
                output.flush();
                file.getFD().sync();
            }
        }
        System.out.println("{\"test\":\"android_application_data_persistence\",\"passed\":true,"
                + "\"package\":\"" + PACKAGE + "\",\"row_id\":" + id + ",\"restored\":" + restored + "}");
    }
}

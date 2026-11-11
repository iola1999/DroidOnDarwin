/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.os.SystemClock;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

public final class NceApkInstall {
    private static final File INPUT = new File("/data/nce/apk-fixture");
    private static final Pattern APPLICATION_LOG = Pattern.compile(
            "^\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3} +\\d+ +\\d+ [IWEF] DroidApkFixture *: (\\{.*\\})$");
    private static final Set<String> APPLICATION_KEYS = new HashSet<>(Arrays.asList(
            "status", "package", "mode", "version", "value", "marker", "created_now", "created_version", "launch_count"));
    private static int sequence;
    private static String operation = "input";
    private static String stage = "unknown";

    private static String text(File file, int limit) throws Exception {
        long size = file.length();
        if (!file.isFile() || size > limit) throw new AssertionError("Invalid file size: " + file);
        byte[] bytes = Files.readAllBytes(file.toPath());
        if (bytes.length > limit) throw new AssertionError("File grew beyond limit: " + file);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static String digest(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (java.io.InputStream input = Files.newInputStream(file.toPath())) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        StringBuilder result = new StringBuilder();
        for (byte value : digest.digest()) result.append(String.format("%02x", value & 255));
        return result.toString();
    }

    private static int integer(JSONObject object, String name) throws Exception {
        Object value = object.get(name);
        if (!(value instanceof Integer) && !(value instanceof Long)) throw new AssertionError("Invalid integer: " + name);
        long number = ((Number) value).longValue();
        if (number < Integer.MIN_VALUE || number > Integer.MAX_VALUE) throw new AssertionError("Integer overflow: " + name);
        return (int) number;
    }

    private static String command(boolean requireSuccess, String... arguments) throws Exception {
        ArrayList<String> argv = new ArrayList<>();
        argv.add("/system/bin/cmd");
        argv.addAll(Arrays.asList(arguments));
        File output = new File(INPUT, "command-" + sequence++ + ".log");
        Process child = new ProcessBuilder(argv).redirectErrorStream(true).redirectOutput(output).start();
        boolean completed = child.waitFor(60, TimeUnit.SECONDS);
        if (!completed) {
            child.destroyForcibly();
            if (!child.waitFor(5, TimeUnit.SECONDS)) throw new AssertionError("Android command could not be reaped: " + argv);
        }
        String result = text(output, 65536);
        System.err.println("APK command " + argv + " exit=" + child.exitValue() + "\n" + result);
        if (!completed) throw new AssertionError("Android command timed out: " + argv);
        if (requireSuccess && child.exitValue() != 0) throw new AssertionError("Android command failed: " + argv);
        return result;
    }

    private static String installedPath(String packageName, boolean required) throws Exception {
        String output = command(required, "package", "path", "--user", "0", packageName).trim();
        if (!required) {
            if (!output.isEmpty()) throw new AssertionError("Fresh run found an installed package: " + output);
            return null;
        }
        if (!output.startsWith("package:/") || output.indexOf('\n') >= 0 || output.indexOf('\r') >= 0) {
            throw new AssertionError("Expected one installed APK: " + output);
        }
        return output.substring("package:".length());
    }

    private static JSONArray application(String packageName, String mode, JSONObject artifact, String phase, long offset) throws Exception {
        long deadline = SystemClock.uptimeMillis() + 60_000;
        try (RandomAccessFile log = new RandomAccessFile("/stderr", "r")) {
            log.seek(offset);
            JSONArray observed = new JSONArray();
            long scanEnd = -1;
            while (SystemClock.uptimeMillis() < deadline) {
                if (log.length() < offset) throw new AssertionError("Application log was truncated");
                String line = readCompleteLine(log, observed.length() == 0 ? log.length() : scanEnd);
                if (line == null) {
                    if (observed.length() != 0) return observed;
                    SystemClock.sleep(50);
                    continue;
                }
                Matcher match = APPLICATION_LOG.matcher(line);
                if (!match.matches()) continue;
                JSONObject result = new JSONObject(match.group(1));
                if (!"passed".equals(result.opt("status"))) throw new AssertionError("Application failure: " + result);
                Set<String> keys = new HashSet<>();
                java.util.Iterator<String> names = result.keys();
                while (names.hasNext()) keys.add(names.next());
                if (!keys.equals(APPLICATION_KEYS) || !packageName.equals(result.get("package"))
                        || !mode.equals(result.get("mode")) || integer(result, "version") != integer(artifact, "version")
                        || integer(result, "value") != integer(artifact, "expected_value")
                        || integer(result, "created_version") != 1 || integer(result, "launch_count") < 1
                        || !(result.get("created_now") instanceof Boolean)
                        || !(result.get("marker") instanceof String)
                        || !result.getString("marker").matches("[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}")) {
                    throw new AssertionError("Unexpected application result: " + result);
                }
                boolean firstCreation = phase.equals("fresh-v1") && observed.length() == 0;
                if (result.getBoolean("created_now") != firstCreation
                        || (firstCreation && integer(result, "launch_count") != 1)) {
                    throw new AssertionError("Unexpected private data creation: " + result);
                }
                if (observed.length() != 0) {
                    JSONObject previous = observed.getJSONObject(observed.length() - 1);
                    if (!result.getString("marker").equals(previous.getString("marker"))
                            || integer(result, "launch_count") != (long) integer(previous, "launch_count") + 1) {
                        throw new AssertionError("Activity creation sequence changed: " + result);
                    }
                }
                observed.put(result);
                // Check complete records already present; unrelated later writes must not extend the scan.
                if (scanEnd < 0) scanEnd = log.length();
            }
            if (observed.length() != 0) throw new AssertionError("Application log snapshot validation timed out: " + packageName);
        }
        throw new AssertionError("Application JSON did not arrive: " + packageName);
    }

    // Return only LF-terminated UTF-8 within limit; an incomplete line preserves its starting cursor.
    private static String readCompleteLine(RandomAccessFile file, long limit) throws Exception {
        long start = file.getFilePointer();
        if (start >= limit) return null;
        ByteArrayOutputStream line = new ByteArrayOutputStream(4096);
        byte[] buffer = new byte[4096];
        long position = start;
        while (position < limit) {
            int requested = (int) Math.min(Math.min(limit - position, buffer.length), 65537 - line.size());
            int count = file.read(buffer, 0, requested);
            if (count <= 0) break;
            int end = 0;
            while (end < count && buffer[end] != '\n') ++end;
            if (line.size() + end > 65536) throw new AssertionError("Application log line exceeds limit");
            line.write(buffer, 0, end);
            if (end < count) {
                file.seek(position + end + 1);
                byte[] bytes = line.toByteArray();
                int length = bytes.length;
                if (length > 0 && bytes[length - 1] == '\r') --length;
                return new String(bytes, 0, length, StandardCharsets.UTF_8);
            }
            position += count;
        }
        file.seek(start);
        return null;
    }

    private static void launch(String mode, JSONObject artifact, String phase) throws Exception {
        String packageName = artifact.getString("package");
        operation = phase + ":" + packageName;
        command(true, "activity", "force-stop", "--user", "0", packageName);
        // Zygote shares this append-only log. Only records after this launch are eligible.
        long offset = new File("/stderr").length();
        String started = command(true, "activity", "start-activity", "-W", "--user", "0", "-n",
                packageName + "/app.droidondarwin.fixture.MainActivity");
        if (!Arrays.asList(started.split("\\R")).contains("Status: ok")) throw new AssertionError("Activity did not start: " + started);
        JSONArray results = application(packageName, mode, artifact, phase, offset);
        String path = installedPath(packageName, true);
        String hash = digest(new File(path));
        if (!hash.equals(artifact.getString("sha256"))) throw new AssertionError("Installed APK bytes differ: " + path);
        for (int index = 0; index < results.length(); ++index) {
            System.out.println(new JSONObject().put("test", "android_apk_fixture")
                    .put("stage", phase).put("artifact", artifact.getString("apk"))
                    .put("installed_sha256", hash).put("application", results.getJSONObject(index)).put("passed", true));
        }
    }

    private static void install(JSONObject artifact, boolean update) throws Exception {
        operation = "install:" + artifact.getString("apk");
        String path = new File(INPUT, artifact.getString("apk")).getPath();
        if (!digest(new File(path)).equals(artifact.getString("sha256"))) throw new AssertionError("Staged APK bytes differ");
        String output = update ? command(true, "package", "install", "-r", "--user", "0", path)
                               : command(true, "package", "install", "--user", "0", path);
        if (!Arrays.asList(output.split("\\R")).contains("Success")) throw new AssertionError("PackageManager rejected APK: " + output);
    }

    public static void main(String[] args) throws Exception {
        try {
            stage = text(new File(INPUT, "stage"), 64).trim();
            if (!stage.equals("install-update") && !stage.equals("verify-restart")) throw new AssertionError("Unknown APK stage: " + stage);
            JSONArray inputs = new JSONObject(text(new File(INPUT, "manifest.json"), 65536)).getJSONArray("artifacts");
            Map<String, JSONObject> artifacts = new HashMap<>();
            for (int index = 0; index < inputs.length(); ++index) {
                JSONObject artifact = inputs.getJSONObject(index);
                String name = artifact.getString("apk");
                if (!name.matches("(java|jni)-v[12]\\.apk") || artifacts.put(name, artifact) != null
                        || !artifact.getString("package").matches("[a-zA-Z][a-zA-Z0-9_]*(\\.[a-zA-Z][a-zA-Z0-9_]*)+")
                        || !artifact.getString("sha256").matches("[0-9a-f]{64}")) throw new AssertionError("Invalid fixture manifest");
                int version = name.charAt(name.length() - 5) - '0';
                if (integer(artifact, "version") != version || integer(artifact, "expected_value") != 100 + version) {
                    throw new AssertionError("Invalid fixture version");
                }
            }
            if (artifacts.size() != 4) throw new AssertionError("Missing fixture APKs");
            for (String mode : new String[]{"java", "jni"}) {
                JSONObject first = artifacts.get(mode + "-v1.apk"), second = artifacts.get(mode + "-v2.apk");
                if (!first.getString("package").equals(second.getString("package"))) throw new AssertionError("Update package changed");
                if (stage.equals("install-update")) installedPath(first.getString("package"), false);
            }
            if (stage.equals("install-update")) {
                for (int version : new int[]{1, 2}) {
                    for (String mode : new String[]{"java", "jni"}) {
                        JSONObject artifact = artifacts.get(mode + "-v" + version + ".apk");
                        install(artifact, version == 2);
                        launch(mode, artifact, version == 1 ? "fresh-v1" : "updated-v2");
                    }
                }
            } else {
                for (String mode : new String[]{"java", "jni"}) launch(mode, artifacts.get(mode + "-v2.apk"), "restarted-v2");
            }
            System.out.println(new JSONObject().put("test", "android_apk_fixture_stage").put("stage", stage)
                    .put("installs", stage.equals("install-update") ? 4 : 0).put("passed", true));
        } catch (Throwable error) {
            System.out.println(new JSONObject().put("test", "android_apk_fixture_stage").put("stage", stage)
                    .put("operation", operation).put("error", error.toString()).put("passed", false));
            throw error;
        }
    }
}

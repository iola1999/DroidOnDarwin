/* SPDX-License-Identifier: GPL-2.0-or-later */
import java.io.IOException;
import java.io.RandomAccessFile;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicReference;

public final class NceApkLogTest {
    private static Method helper;

    private static String readLine(RandomAccessFile file, long end) throws Exception {
        return (String) helper.invoke(null, file, end);
    }

    private static void appendFromThread(Path path, String text) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread writer = new Thread(() -> {
            try (RandomAccessFile output = new RandomAccessFile(path.toFile(), "rw")) {
                output.seek(output.length());
                output.write(text.getBytes(StandardCharsets.UTF_8));
            } catch (Throwable error) {
                failure.set(error);
            }
        });
        writer.start();
        writer.join(2000);
        require(!writer.isAlive(), "writer did not finish");
        if (failure.get() != null) throw new AssertionError(failure.get());
    }

    private static void require(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }

    private static final class AppendingFile extends RandomAccessFile {
        private final Path path;
        String afterLength;
        String beforeRead;
        long bytesRead;
        int largestRead;

        AppendingFile(Path path) throws IOException {
            super(path.toFile(), "r");
            this.path = path;
        }

        private void append(String text) throws IOException {
            try {
                appendFromThread(path, text);
            } catch (Exception error) {
                throw new IOException(error);
            }
        }

        @Override
        public long length() throws IOException {
            long captured = super.length();
            if (afterLength != null) {
                String text = afterLength;
                afterLength = null;
                append(text);
            }
            return captured;
        }

        @Override
        public int read(byte[] bytes, int offset, int length) throws IOException {
            if (beforeRead != null) {
                String text = beforeRead;
                beforeRead = null;
                append(text);
            }
            largestRead = Math.max(largestRead, length);
            int count = super.read(bytes, offset, length);
            if (count > 0) bytesRead += count;
            return count;
        }
    }

    public static void main(String[] args) throws Exception {
        Class<?> source = Class.forName("NceApkInstall");
        helper = source.getDeclaredMethod("readCompleteLine", RandomAccessFile.class, long.class);
        helper.setAccessible(true);
        Path path = Path.of(args[0]);

        Files.writeString(path, "");
        try (AppendingFile reader = new AppendingFile(path)) {
            require(readLine(reader, reader.length()) == null, "empty file produced a line");
            require(reader.getFilePointer() == 0 && reader.bytesRead == 0, "empty read moved the cursor");
        }

        // The writer extends the real file after length was captured, before the caller receives it.
        Files.writeString(path, "partial");
        try (AppendingFile reader = new AppendingFile(path)) {
            reader.afterLength = " rest\n";
            long end = reader.length();
            require(readLine(reader, end) == null, "half line accepted beyond the captured end");
            require(reader.getFilePointer() == 0 && reader.bytesRead == end, "partial read lost its boundary");
            require(readLine(reader, reader.length()).equals("partial rest"), "completed append was lost");
            require(reader.getFilePointer() == reader.length(), "completed line cursor is wrong");
        }

        Files.writeString(path, "partial");
        try (AppendingFile reader = new AppendingFile(path)) {
            long end = reader.length();
            reader.beforeRead = " rest\n";
            require(readLine(reader, end) == null, "read consumed an append beyond the captured end");
            require(reader.getFilePointer() == 0 && reader.bytesRead == end, "read exceeded its captured range");
            require(readLine(reader, reader.length()).equals("partial rest"), "retry did not read the completed line");
        }

        Files.writeString(path, "complete\n");
        try (AppendingFile reader = new AppendingFile(path)) {
            reader.afterLength = "background tail";
            long end = reader.length();
            require(readLine(reader, end).equals("complete"), "complete line rejected after partial append");
            require(reader.getFilePointer() == end, "complete line consumed the following tail");
            require(readLine(reader, reader.length()) == null, "unfinished tail accepted");
            require(reader.getFilePointer() == end, "unfinished tail was consumed");
        }

        Files.writeString(path, "first\npartial");
        try (AppendingFile reader = new AppendingFile(path)) {
            long end = reader.length();
            require(readLine(reader, end).equals("first"), "first snapshot line lost");
            long start = reader.getFilePointer();
            reader.beforeRead = " rest\n";
            require(readLine(reader, end) == null, "snapshot accepted a line completed after its end");
            require(reader.getFilePointer() == start, "snapshot tail cursor changed");
            require(readLine(reader, reader.length()).equals("partial rest"), "new snapshot lost completed tail");
        }

        Files.writeString(path, "\ncrlf\r\n");
        try (AppendingFile reader = new AppendingFile(path)) {
            require(readLine(reader, reader.length()).isEmpty(), "empty complete line lost");
            require(reader.getFilePointer() == 1, "prefetched bytes moved the line cursor");
            require(readLine(reader, reader.length()).equals("crlf"), "CRLF bytes wrong");
            require(readLine(reader, reader.length()) == null, "EOF produced a line");
            require(reader.getFilePointer() == reader.length(), "EOF moved the cursor");
        }

        String unicode = "a".repeat(4095) + "中文🙂";
        Files.writeString(path, unicode + "\n");
        try (AppendingFile reader = new AppendingFile(path)) {
            require(readLine(reader, reader.length()).equals(unicode), "UTF-8 split across blocks changed");
            require(reader.getFilePointer() == reader.length(), "UTF-8 line cursor is wrong");
            require(reader.largestRead <= 4096, "read exceeded the block limit");
        }

        String maximum = "a".repeat(65536);
        Files.writeString(path, maximum);
        try (AppendingFile reader = new AppendingFile(path)) {
            require(readLine(reader, reader.length()) == null, "maximum unfinished line accepted");
            require(reader.getFilePointer() == 0, "maximum unfinished line cursor changed");
            appendFromThread(path, "\n");
            require(readLine(reader, reader.length()).equals(maximum), "maximum complete line rejected");
        }

        Files.writeString(path, maximum + "a\n");
        try (AppendingFile reader = new AppendingFile(path)) {
            try {
                readLine(reader, reader.length());
                throw new AssertionError("overlong line accepted");
            } catch (InvocationTargetException error) {
                require(error.getCause() instanceof AssertionError
                        && error.getCause().getMessage().equals("Application log line exceeds limit"),
                        "unexpected overlong-line failure");
            }
            require(reader.bytesRead == 65537 && reader.largestRead <= 4096, "overlong read exceeded its byte limit");
        }
        System.out.println("APK log boundary checks passed");
    }
}

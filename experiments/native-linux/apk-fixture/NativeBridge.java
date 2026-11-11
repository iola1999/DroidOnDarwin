package app.droidondarwin.fixture;

final class NativeBridge {
    static {
        System.loadLibrary("fixture");
    }

    static native int value();
}

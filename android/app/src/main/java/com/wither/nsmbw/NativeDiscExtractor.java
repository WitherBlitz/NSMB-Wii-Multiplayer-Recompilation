package com.wither.nsmbw;

import java.io.IOException;

/** JNI entry point for the nod-backed ISO, WBFS and RVZ Wii disc reader. */
final class NativeDiscExtractor {
    interface Progress {
        void update(int completed, int total, String message);
    }

    static {
        System.loadLibrary("disc_import");
    }

    private NativeDiscExtractor() {}

    private static native String extract(int fd, String destination, Progress callback);

    static int extractDisc(int fd, String destination, Progress progress) throws IOException {
        final String result = extract(fd, destination, progress);
        if (result == null) {
            throw new IOException("Native disc reader returned no result");
        }
        if (!result.startsWith("OK:")) {
            throw new IOException(result.startsWith("ERROR:") ? result.substring(6) : result);
        }
        try {
            return Integer.parseInt(result.substring(3));
        } catch (NumberFormatException e) {
            throw new IOException("Native disc reader returned invalid file count", e);
        }
    }
}

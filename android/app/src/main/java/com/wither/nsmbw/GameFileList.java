package com.wither.nsmbw;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The files an extracted game must hold before it may start: sys/boot.bin, sys/main.dol and
 * sys/fst.bin, and every file the disc's own file table (sys/fst.bin) lists under files/. Only
 * whether each one is there is checked, so any region or revision's disc passes as long as nothing
 * was left out. runtime/src/game_folder.cpp checks the same. Plain Java, so it runs off the phone too.
 */
final class GameFileList {
    private GameFileList() {}

    static final String[] SYSTEM_FILES = {"sys/boot.bin", "sys/main.dol", "sys/fst.bin"};

    /**
     * The required files missing from `root`, as paths relative to it, at most `limit` of them
     * (empty when none is missing). A file table that can't be read counts as sys/fst.bin missing.
     */
    static List<String> missing(File root, int limit) {
        final List<String> missing = new ArrayList<>();
        for (String name : SYSTEM_FILES) {
            if (!new File(root, name).isFile()) missing.add(name);
        }
        if (!missing.isEmpty()) return missing;
        final byte[] fst;
        try {
            fst = read(new File(root, "sys/fst.bin"));
        } catch (IOException e) {
            missing.add("sys/fst.bin");
            return missing;
        }
        if (fst.length < 12) {
            missing.add("sys/fst.bin");
            return missing;
        }
        // Entry 0 is the root; its third word is the number of entries, and a directory's is the
        // index past its last entry. The names follow the entries.
        final int entries = be32(fst, 8);
        if (entries <= 0 || (long) entries * 12 > fst.length) {
            missing.add("sys/fst.bin");
            return missing;
        }
        final int strings = entries * 12;
        final String[] dirs = new String[entries + 1];
        final int[] ends = new int[entries + 1];
        int depth = 0;
        dirs[0] = "files";
        ends[0] = entries;
        for (int i = 1; i < entries && missing.size() < limit; ++i) {
            while (depth > 0 && i >= ends[depth]) --depth;
            final int word = be32(fst, i * 12);
            // null for a name that isn't plain ASCII: extractors spell those differently, so it isn't checked.
            final String name = name(fst, strings + (word & 0xFFFFFF));
            final String path = dirs[depth] == null || name == null ? null : dirs[depth] + "/" + name;
            if ((word >>> 24) != 0) {
                ++depth;
                dirs[depth] = path;
                ends[depth] = Math.min(entries, be32(fst, i * 12 + 8));
            } else if (path != null && !new File(root, path).isFile()) {
                missing.add(path);
            }
        }
        return missing;
    }

    private static String name(byte[] fst, int at) {
        if (at < 0 || at >= fst.length) return null;
        int end = at;
        while (end < fst.length && fst[end] != 0) {
            if (fst[end] < 0x20) return null;  // also every byte from 0x80 up (Java bytes are signed)
            ++end;
        }
        final String name = new String(fst, at, end - at, java.nio.charset.StandardCharsets.US_ASCII);
        if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0
                || name.indexOf('\\') >= 0) {
            return null;
        }
        return name;
    }

    private static byte[] read(File file) throws IOException {
        final long length = file.length();
        if (length > (64 << 20)) throw new IOException("too big");
        final byte[] data = new byte[(int) length];
        try (InputStream in = new FileInputStream(file)) {
            for (int at = 0; at < data.length; ) {
                final int n = in.read(data, at, data.length - at);
                if (n <= 0) throw new IOException("short");
                at += n;
            }
        }
        return data;
    }

    private static int be32(byte[] b, int at) {
        return ((b[at] & 0xFF) << 24) | ((b[at + 1] & 0xFF) << 16) | ((b[at + 2] & 0xFF) << 8) | (b[at + 3] & 0xFF);
    }
}

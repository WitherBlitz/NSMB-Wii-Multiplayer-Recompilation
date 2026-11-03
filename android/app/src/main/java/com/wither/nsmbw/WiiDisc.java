package com.wither.nsmbw;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * A Wii disc image (.iso) extracted the way Dolphin's Extract Entire Disc does it: the game
 * partition's sys/ (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin) and files/. The partition
 * is encrypted with a title key that its ticket holds encrypted with the Wii's common key, as on the
 * console; Dolphin decrypts it the same way.
 */
final class WiiDisc {
    interface Progress { void update(long done, long total); }

    private static final int MAGIC = 0x5D1C9EA3;
    private static final int CLUSTER = 0x8000;      // on the disc: 0x400 of hashes, then the data
    private static final int CLUSTER_DATA = 0x7C00;
    private static final int BATCH = 64;            // clusters read and decrypted at a time (2 MiB)
    private static final byte[][] COMMON_KEYS = {
        hex("ebe42a225e8593e448d9c5457381aaf7"),    // retail
        hex("63b82bb4f4614e2e13f2fefbba4c9b7e"),    // Korean
    };

    private final FileChannel disc;
    private final long dataStart, dataSize;
    private final SecretKeySpec titleKey;
    private final Cipher cipher;
    private final byte[] raw = new byte[CLUSTER * BATCH];
    private final byte[] plain = new byte[CLUSTER_DATA * BATCH];
    private long cachedFirst = -1;
    private int cachedCount;
    // The partition's layout (boot.bin): where main.dol and the file table are, and their sizes.
    private long dolOffset, dolSize, fstOffset, apploaderSize, totalSize;
    private byte[] fst;

    /** The disc header's first 8 bytes (game ID and revision), or null when it isn't a Wii disc. */
    static byte[] header(FileChannel disc) throws IOException {
        final byte[] head = new byte[0x20];
        if (read(disc, 0, head) < head.length || be32(head, 0x18) != MAGIC) return null;
        final byte[] id = new byte[8];
        System.arraycopy(head, 0, id, 0, 8);
        return id;
    }

    WiiDisc(FileChannel disc) throws IOException {
        this.disc = disc;
        final byte[] head = new byte[0x80];
        if (read(disc, 0, head) < head.length || be32(head, 0x18) != MAGIC) {
            throw new IOException(Lang.t("that isn't a Wii disc image"));
        }
        if (head[0x61] != 0) throw new IOException(Lang.t("that disc image isn't encrypted the way a Wii disc is"));
        final long partition = gamePartition();
        final byte[] ticket = new byte[0x2C0];
        if (read(disc, partition, ticket) < ticket.length) throw new IOException(Lang.t("the disc image is cut short"));
        final int keyIndex = ticket[0x1F1] & 0xFF;
        if (keyIndex >= COMMON_KEYS.length) throw new IOException(Lang.t("that disc image isn't from a retail Wii disc"));
        dataStart = partition + ((be32(ticket, 0x2B8) & 0xFFFFFFFFL) << 2);
        dataSize = (be32(ticket, 0x2BC) & 0xFFFFFFFFL) << 2;
        try {
            cipher = Cipher.getInstance("AES/CBC/NoPadding");
            final byte[] iv = new byte[16];
            System.arraycopy(ticket, 0x1DC, iv, 0, 8);                     // the title ID
            cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(COMMON_KEYS[keyIndex], "AES"), new IvParameterSpec(iv));
            titleKey = new SecretKeySpec(cipher.doFinal(ticket, 0x1BF, 16), "AES");
        } catch (GeneralSecurityException e) {
            throw new IOException(e.getMessage(), e);
        }
        readLayout();
    }

    /** How many bytes extract() writes. */
    long extractedSize() { return totalSize; }

    private void readLayout() throws IOException {
        final byte[] boot = readPartition(0, 0x440);
        dolOffset = (be32(boot, 0x420) & 0xFFFFFFFFL) << 2;
        fstOffset = (be32(boot, 0x424) & 0xFFFFFFFFL) << 2;
        final long fstSize = (be32(boot, 0x428) & 0xFFFFFFFFL) << 2;
        if (fstSize < 12 || fstSize > (64 << 20)) throw new IOException(Lang.t("the disc's file table is damaged"));
        final byte[] apploaderHead = readPartition(0x2440, 0x20);
        apploaderSize = 0x20L + (be32(apploaderHead, 0x14) & 0xFFFFFFFFL) + (be32(apploaderHead, 0x18) & 0xFFFFFFFFL);
        final byte[] dolHead = readPartition(dolOffset, 0x100);
        dolSize = 0x100;
        for (int i = 0; i < 18; ++i) {
            dolSize = Math.max(dolSize, (be32(dolHead, i * 4) & 0xFFFFFFFFL) + (be32(dolHead, 0x90 + i * 4) & 0xFFFFFFFFL));
        }
        fst = readPartition(fstOffset, (int) fstSize);
        final int entries = be32(fst, 8);
        if (entries <= 0 || (long) entries * 12 > fstSize) throw new IOException(Lang.t("the disc's file table is damaged"));
        totalSize = 0x440 + 0x2000 + apploaderSize + dolSize + fstSize;
        for (int i = 1; i < entries; ++i) {
            if ((fst[i * 12] & 0xFF) == 0) totalSize += be32(fst, i * 12 + 8) & 0xFFFFFFFFL;
        }
    }

    /** The data partition (type 0) from the partition table at 0x40000. */
    private long gamePartition() throws IOException {
        final byte[] groups = new byte[32];
        read(disc, 0x40000, groups);
        for (int g = 0; g < 4; ++g) {
            final int count = be32(groups, g * 8);
            final long table = (be32(groups, g * 8 + 4) & 0xFFFFFFFFL) << 2;
            if (count <= 0 || count > 64) continue;
            final byte[] entries = new byte[count * 8];
            read(disc, table, entries);
            for (int i = 0; i < count; ++i) {
                if (be32(entries, i * 8 + 4) == 0) return (be32(entries, i * 8) & 0xFFFFFFFFL) << 2;
            }
        }
        throw new IOException(Lang.t("that disc image has no game partition"));
    }

    /** Writes sys/ and files/ into `root`; returns how many files it wrote. */
    int extract(File root, Progress progress) throws IOException {
        final int entries = be32(fst, 8);
        final long total = totalSize;
        final long[] done = {0};
        final File sys = new File(root, "sys");
        write(new File(sys, "boot.bin"), 0, 0x440, done, total, progress);
        write(new File(sys, "bi2.bin"), 0x440, 0x2000, done, total, progress);
        write(new File(sys, "apploader.img"), 0x2440, apploaderSize, done, total, progress);
        write(new File(sys, "main.dol"), dolOffset, dolSize, done, total, progress);
        write(new File(sys, "fst.bin"), fstOffset, fst.length, done, total, progress);

        // The FST: entry 0 is the root; a directory's third word is the index past its last entry.
        final int strings = entries * 12;
        final File[] dirs = new File[entries + 1];
        final int[] ends = new int[entries + 1];
        int depth = 0;
        dirs[0] = new File(root, "files");
        ends[0] = entries;
        int files = 5;
        for (int i = 1; i < entries; ++i) {
            while (depth > 0 && i >= ends[depth]) --depth;
            final int word = be32(fst, i * 12);
            final String name = name(fst, strings + (word & 0xFFFFFF));
            final File path = new File(dirs[depth], name);
            if ((word >>> 24) != 0) {
                ++depth;
                dirs[depth] = path;
                ends[depth] = Math.min(entries, be32(fst, i * 12 + 8));
                if (!path.isDirectory() && !path.mkdirs()) throw new IOException("cannot create " + path);
            } else {
                write(path, (be32(fst, i * 12 + 4) & 0xFFFFFFFFL) << 2, be32(fst, i * 12 + 8) & 0xFFFFFFFFL,
                        done, total, progress);
                ++files;
            }
        }
        return files;
    }

    private void write(File target, long offset, long size, long[] done, long total, Progress progress)
            throws IOException {
        final File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        if (offset + size > dataSize / CLUSTER * CLUSTER_DATA) throw new IOException(Lang.t("the disc image is cut short"));
        try (OutputStream out = new FileOutputStream(target)) {
            final byte[] chunk = new byte[1 << 20];
            for (long at = 0; at < size; ) {
                final int n = (int) Math.min(chunk.length, size - at);
                readPartition(offset + at, chunk, n);
                out.write(chunk, 0, n);
                at += n;
                done[0] += n;
                progress.update(done[0], total);
            }
        }
    }

    private byte[] readPartition(long offset, int size) throws IOException {
        final byte[] out = new byte[size];
        readPartition(offset, out, size);
        return out;
    }

    /** `size` bytes of the decrypted partition at `offset` (an offset in its data, hashes left out). */
    private void readPartition(long offset, byte[] out, int size) throws IOException {
        for (int at = 0; at < size; ) {
            final long cluster = (offset + at) / CLUSTER_DATA;
            final int within = (int) ((offset + at) % CLUSTER_DATA);
            if (cluster < cachedFirst || cluster >= cachedFirst + cachedCount) decrypt(cluster);
            final int start = (int) (cluster - cachedFirst) * CLUSTER_DATA + within;
            final int n = Math.min(size - at, cachedCount * CLUSTER_DATA - start);
            System.arraycopy(plain, start, out, at, n);
            at += n;
        }
    }

    /** Reads and decrypts up to BATCH clusters from `first`: each one's IV is in its hash area. */
    private void decrypt(long first) throws IOException {
        final long clusters = (dataSize + CLUSTER - 1) / CLUSTER;
        if (first >= clusters) throw new IOException(Lang.t("the disc image is cut short"));
        final int count = (int) Math.min(BATCH, clusters - first);
        final int got = read(disc, dataStart + first * CLUSTER, raw, count * CLUSTER);
        if (got < count * CLUSTER) throw new IOException(Lang.t("the disc image is cut short"));
        try {
            for (int c = 0; c < count; ++c) {
                cipher.init(Cipher.DECRYPT_MODE, titleKey, new IvParameterSpec(raw, c * CLUSTER + 0x3D0, 16));
                cipher.doFinal(raw, c * CLUSTER + 0x400, CLUSTER_DATA, plain, c * CLUSTER_DATA);
            }
        } catch (GeneralSecurityException e) {
            throw new IOException(e.getMessage(), e);
        }
        cachedFirst = first;
        cachedCount = count;
    }

    /** An FST name, refused when it could leave its folder. */
    private static String name(byte[] fst, int at) throws IOException {
        int end = at;
        while (end < fst.length && fst[end] != 0) ++end;
        final String name = new String(fst, at, end - at, StandardCharsets.ISO_8859_1);
        if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) {
            throw new IOException(Lang.t("the disc's file table is damaged"));
        }
        return name;
    }

    private static int read(FileChannel channel, long position, byte[] buffer) throws IOException {
        return read(channel, position, buffer, buffer.length);
    }

    private static int read(FileChannel channel, long position, byte[] buffer, int length) throws IOException {
        final ByteBuffer target = ByteBuffer.wrap(buffer, 0, length);
        while (target.hasRemaining()) {
            if (channel.read(target, position + target.position()) <= 0) break;
        }
        return target.position();
    }

    private static int be32(byte[] b, int at) {
        return (b[at] & 0xFF) << 24 | (b[at + 1] & 0xFF) << 16 | (b[at + 2] & 0xFF) << 8 | (b[at + 3] & 0xFF);
    }

    private static byte[] hex(String s) {
        final byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; ++i) out[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
        return out;
    }
}

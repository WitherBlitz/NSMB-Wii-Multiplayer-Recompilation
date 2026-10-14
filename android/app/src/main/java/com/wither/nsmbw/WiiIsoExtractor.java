package com.wither.nsmbw;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.ArrayList;
import java.util.List;
import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/** Extracts a retail Wii ISO directly from a seekable Android document. */
final class WiiIsoExtractor {
    interface Progress { void update(int done, int total, String message); }
    private static final int SECTOR = 0x8000, HASH = 0x400, DATA = 0x7c00;
    private static final byte[] COMMON_KEY = {
        (byte)0xeb, (byte)0xe4, 0x2a, 0x22, 0x5e, (byte)0x85, (byte)0x93, (byte)0xe4,
        0x48, (byte)0xd9, (byte)0xc5, 0x45, 0x73, (byte)0x81, (byte)0xaa, (byte)0xf7
    };
    private final FileChannel channel;
    private final long length;
    private final Cipher aes;
    private long dataStart, dataLength, cachedSector = -1;
    private SecretKeySpec titleKey;
    private final byte[] encrypted = new byte[SECTOR], plain = new byte[DATA];

    private WiiIsoExtractor(FileChannel channel) throws IOException {
        this.channel = channel;
        length = channel.size();
        if (length < 0x50000) throw new IOException("ISO incomplete or not seekable");
        try { aes = Cipher.getInstance("AES/CBC/NoPadding"); }
        catch (GeneralSecurityException e) { throw new IOException("AES is unavailable", e); }
    }

    static int extract(FileChannel channel, File destination, Progress progress) throws IOException {
        return new WiiIsoExtractor(channel).extractInto(destination, progress);
    }

    private int extractInto(File root, Progress progress) throws IOException {
        byte[] disc = raw(0, 0x100);
        String problem = AppFiles.headerProblem(disc);
        if (problem != null) throw new IOException(problem);
        if (u32(disc, 0x18) != 0x5d1c9ea3L)
            throw new IOException("Not an uncompressed Wii ISO. WBFS, RVZ and NKit are unsupported.");
        if (disc[0x61] != 0) throw new IOException("Development disc format is unsupported");

        byte[] group = raw(0x40000, 8);
        int count = (int)u32(group, 0);
        long table = u32(group, 4) * 4;
        if (count < 1 || count > 32 || table < 0 || table + count * 8L > length)
            throw new IOException("Invalid Wii partition table");
        byte[] entries = raw(table, count * 8);
        long partition = -1;
        for (int i = 0; i < count; i++) {
            if (u32(entries, i * 8 + 4) == 0) {
                partition = u32(entries, i * 8) * 4;
                break;
            }
        }
        if (partition < 0 || partition + 0x2c0 > length)
            throw new IOException("Wii game partition not found");

        byte[] ticket = raw(partition, 0x2c0);
        if ((ticket[0x1f1] & 0xff) != 0)
            throw new IOException("Unsupported Wii common key index");
        byte[] titleIv = new byte[16];
        System.arraycopy(ticket, 0x1dc, titleIv, 0, 8);
        try {
            aes.init(Cipher.DECRYPT_MODE, new SecretKeySpec(COMMON_KEY, "AES"), new IvParameterSpec(titleIv));
            titleKey = new SecretKeySpec(aes.doFinal(ticket, 0x1bf, 16), "AES");
        } catch (GeneralSecurityException e) {
            throw new IOException("Cannot decrypt Wii title key", e);
        }
        dataStart = partition + u32(ticket, 0x2b8) * 4;
        dataLength = u32(ticket, 0x2bc) * 4;
        if (dataLength < SECTOR || dataStart < 0 || dataStart + dataLength > length)
            throw new IOException("Truncated Wii partition");

        byte[] boot = part(0, 0x440);
        if (u32(boot, 0x18) != 0x5d1c9ea3L || AppFiles.headerProblem(boot) != null)
            throw new IOException("Game partition failed verification");
        long dolOffset = u32(boot, 0x420) * 4;
        long fstOffset = u32(boot, 0x424) * 4;
        long fstSize = u32(boot, 0x428) * 4;
        if (fstSize < 12 || fstSize > 32L * 1024 * 1024)
            throw new IOException("Invalid Wii file system table size");
        byte[] fst = part(fstOffset, (int)fstSize);
        int entriesCount = (int)u32(fst, 8);
        if (entriesCount < 1 || entriesCount > fst.length / 12 || fst[0] == 0)
            throw new IOException("Invalid Wii file system table");

        List<DiscFile> files = new ArrayList<>();
        List<Directory> parents = new ArrayList<>();
        parents.add(new Directory("", entriesCount));
        int names = entriesCount * 12;
        for (int i = 1; i < entriesCount; i++) {
            while (!parents.isEmpty() && i >= parents.get(parents.size()-1).end)
                parents.remove(parents.size()-1);
            if (parents.isEmpty()) throw new IOException("Broken Wii directory structure");
            int e = i * 12;
            boolean folder = fst[e] != 0;
            int nameOffset = ((fst[e+1] & 255) << 16) | ((fst[e+2] & 255) << 8) | (fst[e+3] & 255);
            String name = name(fst, names + nameOffset);
            String prefix = parents.get(parents.size()-1).path;
            String path = prefix.isEmpty() ? name : prefix + "/" + name;
            if (folder) {
                long end = u32(fst, e+8);
                if (end <= i || end > parents.get(parents.size()-1).end)
                    throw new IOException("Broken Wii directory bounds");
                parents.add(new Directory(path, (int)end));
            } else {
                files.add(new DiscFile("files/" + path, u32(fst, e+4) * 4, u32(fst, e+8)));
            }
        }

        List<DiscFile> all = new ArrayList<>();
        all.add(new DiscFile("sys/boot.bin", 0, 0x440));
        all.add(new DiscFile("sys/bi2.bin", 0x440, 0x2000));
        byte[] apploader = part(0x2440, 0x20);
        all.add(new DiscFile("sys/apploader.img", 0x2440,
                0x20 + u32(apploader, 0x14) + u32(apploader, 0x18)));
        byte[] dol = part(dolOffset, 0x100);
        long dolSize = 0x100;
        for (int i = 0; i < 18; i++) {
            long off = u32(dol, i*4);
            long size = u32(dol, i < 7 ? 0x90 + i*4 : 0xac + (i-7)*4);
            if (size > 0) dolSize = Math.max(dolSize, off + size);
        }
        if (dolSize > 128L*1024*1024) throw new IOException("Invalid main.dol");
        all.add(new DiscFile("sys/main.dol", dolOffset, dolSize));
        all.add(new DiscFile("sys/fst.bin", fstOffset, fstSize));
        all.addAll(files);

        for (DiscFile f : all) validateRange(f.offset, f.size);
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create import folder");
        byte[] buffer = new byte[128*1024];
        for (int i = 0; i < all.size(); i++) {
            DiscFile f = all.get(i);
            File dest = new File(root, f.path);
            File parent = dest.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create " + parent);
            progress.update(i, all.size(), "Extracting " + (i+1) + "/" + all.size() + ": " + f.path);
            try (FileOutputStream output = new FileOutputStream(dest)) {
                long pos = f.offset, remaining = f.size;
                while (remaining > 0) {
                    int chunk = (int)Math.min(buffer.length, remaining);
                    partInto(pos, buffer, chunk);
                    output.write(buffer, 0, chunk);
                    remaining -= chunk;
                    pos += chunk;
                }
            }
        }
        progress.update(all.size(), all.size(), "Extracted and verified " + all.size() + " files");
        return all.size();
    }

    private static String name(byte[] fst, int offset) throws IOException {
        if (offset < 0 || offset >= fst.length) throw new IOException("Invalid Wii file name offset");
        int end = offset;
        while (end < fst.length && fst[end] != 0 && end-offset < 255) end++;
        if (end == fst.length || end-offset >= 255) throw new IOException("Invalid Wii file name");
        String name = new String(fst, offset, end-offset, StandardCharsets.UTF_8);
        if (name.isEmpty() || name.equals(".") || name.equals("..") ||
            name.contains("/") || name.contains("\\") || name.contains(":"))
            throw new IOException("Unsafe Wii file path");
        return name;
    }

    private static final class Directory {
        final String path;
        final int end;
        Directory(String path, int end) { this.path = path; this.end = end; }
    }
    private static final class DiscFile {
        final String path;
        final long offset, size;
        DiscFile(String path, long offset, long size) {
            this.path = path; this.offset = offset; this.size = size;
        }
    }

    private void validateRange(long off, long size) throws IOException {
        long limit = (dataLength / SECTOR) * DATA;
        if (off < 0 || size < 0 || off > limit || size > limit-off)
            throw new IOException("File is outside Wii game partition");
    }
    private byte[] part(long offset, int size) throws IOException {
        byte[] dst = new byte[size];
        partInto(offset, dst, size);
        return dst;
    }
    private void partInto(long offset, byte[] dst, int size) throws IOException {
        validateRange(offset, size);
        int pos = 0;
        while (size > 0) {
            long sec = offset / DATA;
            if (cachedSector != sec) decrypt(sec);
            int index = (int)(offset % DATA);
            int len = Math.min(size, DATA-index);
            System.arraycopy(plain, index, dst, pos, len);
            offset += len; pos += len; size -= len;
        }
    }
    private void decrypt(long sector) throws IOException {
        rawInto(dataStart + sector*SECTOR, encrypted);
        byte[] iv = new byte[16];
        System.arraycopy(encrypted, 0x3d0, iv, 0, 16);
        try {
            aes.init(Cipher.DECRYPT_MODE, titleKey, new IvParameterSpec(iv));
            if (aes.doFinal(encrypted, HASH, DATA, plain, 0) != DATA)
                throw new IOException("Incomplete decrypted sector");
        } catch (GeneralSecurityException e) {
            throw new IOException("Cannot decrypt Wii sector", e);
        }
        cachedSector = sector;
    }
    private byte[] raw(long offset, int size) throws IOException {
        byte[] dst = new byte[size]; rawInto(offset, dst); return dst;
    }
    private void rawInto(long offset, byte[] dst) throws IOException {
        if (offset < 0 || dst.length > length-offset) throw new IOException("Truncated Wii ISO");
        ByteBuffer data = ByteBuffer.wrap(dst);
        while (data.hasRemaining()) {
            int n = channel.read(data, offset);
            if (n <= 0) throw new IOException("ISO provider does not support random access");
            offset += n;
        }
    }
    private static long u32(byte[] d, int i) {
        return ((long)(d[i]&255)<<24)|((long)(d[i+1]&255)<<16)|
                ((long)(d[i+2]&255)<<8)|(d[i+3]&255);
    }
}

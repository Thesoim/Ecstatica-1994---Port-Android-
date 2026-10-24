package thesoim.ecstatica;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Unpacks the bundled game.zip into the app's files folder. Plain Java. */
final class GameInstaller {
    interface Progress { void percent(int pct); }

    private GameInstaller() { }

    static void install(InputStream raw, final long totalBytes, File root, Progress progress)
            throws IOException {
        final long[] read = { 0 };
        InputStream counting = new FilterInputStream(raw) {
            @Override public int read() throws IOException {
                int b = super.read();
                if (b >= 0) read[0]++;
                return b;
            }
            @Override public int read(byte[] b, int off, int len) throws IOException {
                int n = super.read(b, off, len);
                if (n > 0) read[0] += n;
                return n;
            }
        };
        String rootPath = root.getCanonicalPath() + File.separator;
        int last = -1;
        try (ZipInputStream zin = new ZipInputStream(counting)) {
            byte[] buf = new byte[1 << 16];
            for (ZipEntry e; (e = zin.getNextEntry()) != null; ) {
                File out = new File(root, e.getName());
                if (!out.getCanonicalPath().startsWith(rootPath)) throw new IOException("bad entry " + e.getName());
                if (e.isDirectory()) { out.mkdirs(); continue; }
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    for (int n; (n = zin.read(buf)) > 0; ) os.write(buf, 0, n);
                }
                if (progress != null && totalBytes > 0) {
                    int pct = (int) Math.min(99, read[0] * 100 / totalBytes);
                    if (pct != last) { last = pct; progress.percent(pct); }
                }
            }
        }
        if (progress != null) progress.percent(100);
    }
}

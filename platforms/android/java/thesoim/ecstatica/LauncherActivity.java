package thesoim.ecstatica;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;

/**
 * Starts the game. The game data ships inside the APK (assets/game); on the
 * first run it is unpacked into the app's files folder, then GameActivity
 * (the engine) is started and this screen closes. No file picking.
 */
public class LauncherActivity extends Activity {
    private static final String ASSET = "game.zip";
    private static final String MARKER = ".installed_v1.0.0";

    private TextView status;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        final File root = getExternalFilesDir(null) != null ? getExternalFilesDir(null) : getFilesDir();
        final File marker = new File(root, MARKER);
        if (marker.exists() && new File(root, "e1/CODE/ECSTATIC.FAN").exists()) {
            launch();           /* already unpacked: straight into the game, no screen of ours */
            return;
        }
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER);
        col.setBackgroundColor(Color.BLACK);
        col.setPadding(48, 48, 48, 48);

        TextView title = new TextView(this);
        title.setText("Ečstatica");
        title.setTextColor(Color.rgb(0xC8, 0x1E, 0x14));
        title.setTextSize(34);
        title.setTypeface(Typeface.SERIF, Typeface.BOLD);
        title.setGravity(Gravity.CENTER);
        col.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(0xE2, 0xD4, 0xB4));
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER);
        col.addView(status);
        setContentView(col);

        new Thread(new Runnable() {
            @Override public void run() {
                String err = null;
                try {
                    long total = 0;
                    InputStream in;
                    try {
                        android.content.res.AssetFileDescriptor fd = getAssets().openFd(ASSET);
                        total = fd.getLength();
                        in = fd.createInputStream();
                    } catch (IOException notStored) {
                        in = getAssets().open(ASSET);
                        total = in.available();
                    }
                    GameInstaller.install(in, total, root, null);
                    if (!new File(root, "e1/CODE/ECSTATIC.FAN").exists())
                        throw new IOException("game data is incomplete");
                    if (!marker.createNewFile() && !marker.exists()) throw new IOException("cannot write marker");
                } catch (Exception e) {
                    err = e.getMessage() != null ? e.getMessage() : e.toString();
                }
                final String error = err;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        if (error == null) launch();
                        else status.setText("Setup failed: " + error + "\nFree some storage space and reopen the app.");
                    }
                });
            }
        }).start();
    }

    private void launch() {
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra("game", "e1");
        startActivity(i);
        finish();
        overridePendingTransition(0, 0);
    }
}

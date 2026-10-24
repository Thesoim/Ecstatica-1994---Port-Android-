package thesoim.ecstatica;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

/* The engine runs in src/platforms/android.c. This only makes the window
 * full screen, keeps it awake and lets the picture use the display cutout. */
public class GameActivity extends NativeActivity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        hideSystemBars();
    }

    /* The engine keeps its state in C globals, which Android would keep alive
     * with this process after the game is closed and then reuse on the next
     * start, replaying half-finished state (a skipped intro that no longer
     * skips, stuck keys). The game process exists for the game alone, so end
     * it with the activity: every start is a clean one. */
    @Override
    protected void onDestroy() {
        super.onDestroy();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideSystemBars();
    }

    /* Called from the engine (JNI) when the Telegram badge is tapped. The
     * tg:// link opens the channel directly inside the Telegram app; if no
     * Telegram is installed the same page opens in the browser. */
    public void openLink(final String url) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                String handle = url.substring(url.lastIndexOf('/') + 1);
                if (!handle.isEmpty()) {
                    try {
                        Intent tg = new Intent(Intent.ACTION_VIEW, Uri.parse("tg://resolve?domain=" + handle));
                        tg.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        startActivity(tg);
                        return;
                    } catch (Exception notInstalled) { /* fall through to the web page */ }
                }
                try {
                    Intent web = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
                    web.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    startActivity(web);
                } catch (Exception ignored) { }
            }
        });
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }
}

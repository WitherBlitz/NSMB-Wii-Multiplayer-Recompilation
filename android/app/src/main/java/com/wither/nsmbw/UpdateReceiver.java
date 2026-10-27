package com.wither.nsmbw;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.widget.Toast;

/**
 * Hears what the system did with an update Updater committed. It runs in the app's main process; a
 * game that started the update (its Update button) hears a failure through ACTION_GAME_UPDATE_RESULT.
 */
public final class UpdateReceiver extends BroadcastReceiver {
    /** To GameActivity, inside the app only: the install failed (EXTRA_MESSAGE) or was cancelled (none). */
    static final String ACTION_GAME_UPDATE_RESULT = "com.wither.nsmbw.GAME_UPDATE_RESULT";
    static final String EXTRA_MESSAGE = "message";

    @Override
    public void onReceive(Context context, Intent intent) {
        final int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        final AppSettings settings = new AppSettings(context);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            // The system's install confirmation (and, the first time, "allow from this source").
            final Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(confirm);
            }
        } else if (status == PackageInstaller.STATUS_SUCCESS) {
            // Heard by the new version, in a new process. The game asked for the update: open it again.
            // (Android may not let an app open itself from here; the update is in place either way.)
            if (settings.reopenGameAfterUpdate()) {
                settings.setReopenGameAfterUpdate(false);
                context.startActivity(new Intent(context, LauncherActivity.class)
                        .putExtra(LauncherActivity.EXTRA_RESTART_GAME, true)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
            }
        } else {
            final String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            final boolean cancelled = status == PackageInstaller.STATUS_FAILURE_ABORTED;
            final String text = "The update wasn't installed" + (cancelled
                    ? "." : ": " + (message != null ? message : "status " + status));
            if (settings.reopenGameAfterUpdate()) {
                settings.setReopenGameAfterUpdate(false);
                final Intent result = new Intent(ACTION_GAME_UPDATE_RESULT).setPackage(context.getPackageName());
                if (!cancelled) {
                    result.putExtra(EXTRA_MESSAGE, text);
                }
                context.sendBroadcast(result);
            } else {
                Toast.makeText(context, text, Toast.LENGTH_LONG).show();
            }
        }
    }
}

package com.wither.nsmbw;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/**
 * Starts the app's main process afresh: for a game that ran in it (MainProcessGameActivity), whose
 * native runtime starts once per process. It runs in a process of its own (":restart"), ends the
 * old one, opens `next` in a new one and goes. Invisible; a moment on screen.
 */
public final class RestartActivity extends Activity {
    private static final String EXTRA_PID = "pid";
    private static final String EXTRA_NEXT = "next";

    /** From the main process: replace it with a fresh one showing `next`. The caller finishes itself. */
    static void restart(Context context, Intent next) {
        context.startActivity(new Intent(context, RestartActivity.class)
                .putExtra(EXTRA_PID, Process.myPid())
                .putExtra(EXTRA_NEXT, next)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0 && pid != Process.myPid()) {
            Process.killProcess(pid);
        }
        final Intent next = getIntent().getParcelableExtra(EXTRA_NEXT);
        if (next != null) {
            startActivity(next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
        }
        finish();
        overridePendingTransition(0, 0);
        Process.killProcess(Process.myPid());
    }
}

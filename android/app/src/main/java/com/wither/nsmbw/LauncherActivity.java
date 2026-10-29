package com.wither.nsmbw;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.GradientDrawable;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Start screen: play, or import the player's own game the way Dolphin extracts it (the folder, or
 * a zip of it). Until a game is imported the Dolphin steps are shown here; Help repeats them.
 */
public final class LauncherActivity extends Activity {
    private static final int REQUEST_IMPORT = 1;
    private static final int REQUEST_ZIP = 2;
    private static final int REQUEST_LOGS = 3;
    /** Set by GameActivity.restartForSession: open the game again once its old process is gone. */
    static final String EXTRA_RESTART_GAME = "restartGame";
    /** Set by the game's "Start Menu": show this screen even with Skip Start Menu on. */
    static final String EXTRA_SHOW_START_MENU = "showStartMenu";

    static final String PROJECT_URL = "https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation";

    private static final String DOLPHIN_STEPS =
            "Use your own USA New Super Mario Bros. Wii (SMNE01, revision 1). Easiest: in Dolphin on a "
            + "computer, right-click the game, choose Properties, Filesystem, then right-click the disc and "
            + "choose Extract Entire Disc. Copy that folder (or a zip of it) to this phone and tap Import. "
            + "Dolphin shows the revision under Properties > Info.";

    private AppSettings settings;
    /** The On/Off pills of the Skip Start Menu and Auto Update rows. */
    private TextView skip;
    private Button updateCheck;
    private TextView autoUpdate;
    /** Skip Start Menu with Auto Update on: open the game once the update check finds nothing. */
    private boolean skipAfterCheck;
    private TextView mainProcess;
    private boolean updating;
    /** An update waiting for the player to allow installs from this app. */
    private Updater.Release waitingUpdate;

    private TextView status;
    private Button play;
    private View steps;
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        settings = new AppSettings(this);
        // Skip Start Menu: straight into the game, unless the game sent the player here or is
        // restarting itself into a LAN session.
        final Intent launch = getIntent();
        final boolean asked = launch != null && (launch.getBooleanExtra(EXTRA_SHOW_START_MENU, false)
                || launch.getBooleanExtra(EXTRA_RESTART_GAME, false));
        final boolean opened = savedInstanceState == null && !asked;
        if (opened && settings.skipStartMenu() && AppFiles.gameReady(this)) {
            if (!settings.autoUpdate()) {
                startActivity(GameActivity.intent(this));
                overridePendingTransition(0, 0);
                finish();
                return;
            }
            skipAfterCheck = true;  // Auto Update first; the game opens once there's nothing to install
        }
        Lang.load(this);
        final int pad = dp(20);
        final int screenWidth = getResources().getDisplayMetrics().widthPixels;
        // Landscape (or a tablet): title, status and Play on the left, the rest beside them.
        final boolean twoPanes = screenWidth >= dp(720);

        final LinearLayout intro = new LinearLayout(this);
        intro.setOrientation(LinearLayout.VERTICAL);
        final LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        final TextView title = text("NSMBW", 34);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.START);
        header.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        // The game's language, with a globe on its left; the start menu follows it too.
        final TextView language = text(Lang.NAMES[Lang.index()], 15);
        language.setTypeface(MEDIUM);
        language.setPadding(dp(14), dp(8), dp(18), dp(8));
        language.setBackground(rounded(Color.TRANSPARENT, Color.TRANSPARENT, dp(18), STROKE));
        language.setCompoundDrawablesRelativeWithIntrinsicBounds(R.drawable.ic_language, 0, 0, 0);
        language.setCompoundDrawablePadding(dp(6));
        language.setContentDescription(Lang.t("Language"));
        language.setOnClickListener(v -> chooseLanguage());
        final LinearLayout.LayoutParams languageParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        languageParams.rightMargin = dp(8);
        header.addView(language, languageParams);
        final TextView help = text(Lang.t("Help"), 15);
        help.setTypeface(MEDIUM);
        help.setPadding(dp(18), dp(8), dp(18), dp(8));
        help.setBackground(rounded(Color.TRANSPARENT, Color.TRANSPARENT, dp(18), STROKE));
        help.setOnClickListener(v -> showHelp());
        header.addView(help);
        intro.addView(header, matchWidth(0));
        final TextView subtitle = text(Lang.t("New Super Mario Bros. Wii, recompiled to run natively"), 15);
        subtitle.setGravity(Gravity.START);
        subtitle.setTextColor(MUTED);
        intro.addView(subtitle, matchWidth(dp(2)));

        status = text("", 15);
        status.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        status.setCompoundDrawablePadding(dp(10));
        status.setPadding(dp(16), dp(12), dp(16), dp(12));
        status.setBackground(rounded(SURFACE, SURFACE, dp(16), 0));
        intro.addView(status, matchWidth(dp(20)));

        play = button(Lang.t("Play"), true);
        play.setOnClickListener(v -> startActivity(GameActivity.intent(this)));
        intro.addView(play, matchWidth(dp(14)));

        final LinearLayout updates = card(Lang.t("Updates"));
        updateCheck = button(Lang.t("Check for Updates"), false);
        updateCheck.setOnClickListener(v -> checkForUpdates(false));
        updates.addView(updateCheck, matchWidth(dp(10)));
        autoUpdate = toggle(updates, Lang.t("Auto Update"), Lang.t("On: opening the app installs the latest release from "
                + "GitHub when there is one. Your saves, settings and game files are kept."), v -> {
                    settings.setAutoUpdate(!settings.autoUpdate());
                    updateAutoUpdateButton();
                });
        updateAutoUpdateButton();

        final LinearLayout options = card(Lang.t("Options"));
        final Button settingsButton = button(Lang.t("Settings"), false);
        settingsButton.setOnClickListener(v -> SettingsMenu.show(this));
        options.addView(settingsButton, matchWidth(dp(10)));
        final Button devices = button(Lang.t("Tailscale & LAN Devices"), false);
        devices.setOnClickListener(v -> DevicesDialog.show(this));
        options.addView(devices, matchWidth(dp(10)));
        skip = toggle(options, Lang.t("Skip Start Menu"), Lang.t("On: opening the app goes straight into the game. "
                + "In the game, the phone's Back gesture and Start Menu bring you back here."), v -> {
                    settings.setSkipStartMenu(!settings.skipStartMenu());
                    updateSkipButton();
                });
        updateSkipButton();
        mainProcess = toggle(options, Lang.t("Xiaomi Mode"), Lang.t("On: the game runs in the app's main process. For "
                + "Xiaomi, Redmi and POCO phones (on by default there), whose system closes the game's own "
                + "process and leaves a black screen."), v -> {
                    settings.setGameInMainProcess(!settings.gameInMainProcess());
                    showToggle(mainProcess, settings.gameInMainProcess());
                });
        showToggle(mainProcess, settings.gameInMainProcess());

        final LinearLayout gameData = card(Lang.t("Game Data"));
        // How to get the game files, the same steps KartPad gives, until a game is imported.
        final LinearLayout stepsBox = new LinearLayout(this);
        stepsBox.setOrientation(LinearLayout.VERTICAL);
        final TextView stepsTitle = text(Lang.t("Get the game files (once)"), 18);
        stepsTitle.setTypeface(Typeface.DEFAULT_BOLD);
        stepsTitle.setGravity(Gravity.START);
        stepsBox.addView(stepsTitle);
        final TextView stepsText = text(Lang.t(
                "1. In Dolphin on a computer, right-click New Super Mario Bros. Wii and choose Properties.\n"
                + "2. Open Filesystem, right-click the disc at the top and choose Extract Entire Disc.\n"
                + "3. Copy that folder (or a zip of it) to this phone.\n"
                + "4. Tap Import from Extracted Game Data Folder\u2026 or Import Game Data Zip\u2026 below and choose it.\n\n"
                + "Use the USA disc, revision 1 (SMNE01): Dolphin shows the revision under Properties > Info."), 15);
        stepsText.setGravity(Gravity.START);
        stepsText.setPadding(0, dp(6), 0, dp(14));
        stepsText.setLineSpacing(dp(3), 1f);
        stepsBox.addView(stepsText);
        steps = stepsBox;
        gameData.addView(stepsBox, matchWidth(dp(10)));
        final TextView importHelp = text(Lang.t("Import your own New Super Mario Bros. Wii (USA, SMNE01) once. "
                + "Easiest: the folder Dolphin's Extract Entire Disc makes (DATA, or the folder that holds it), "
                + "or a zip of that folder. Your saves are kept."), 13);
        importHelp.setGravity(Gravity.START);
        importHelp.setTextColor(MUTED);
        gameData.addView(importHelp, matchWidth(dp(8)));
        final Button importFolder = button(Lang.t("Import from Extracted Game Data Folder\u2026"), false);
        importFolder.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_IMPORT));
        gameData.addView(importFolder, matchWidth(dp(12)));
        final Button importZip = button(Lang.t("Import Game Data Zip\u2026"), false);
        importZip.setOnClickListener(v -> pickZip());
        gameData.addView(importZip, matchWidth(dp(10)));

        final Button downloadLogs = button(Lang.t("Download Logs (For Github Reports)"), false);
        downloadLogs.setOnClickListener(v -> LogExport.pickDestination(this, REQUEST_LOGS));

        final TextView version = text("NSMBW " + versionText(), 13);
        version.setTextColor(FAINT);

        final LinearLayout page = new LinearLayout(this);
        page.setPadding(pad, dp(16), pad, pad);
        if (twoPanes) {
            page.setOrientation(LinearLayout.HORIZONTAL);
            page.setGravity(Gravity.CENTER_HORIZONTAL);
            final int left = dp(340);
            final int right = Math.min(dp(500), screenWidth - 2 * pad - left - dp(24));
            version.setGravity(Gravity.START);
            intro.addView(version, matchWidth(dp(16)));
            page.addView(intro, new LinearLayout.LayoutParams(left, LinearLayout.LayoutParams.WRAP_CONTENT));
            final LinearLayout cards = new LinearLayout(this);
            cards.setOrientation(LinearLayout.VERTICAL);
            cards.addView(updates, matchWidth(0));
            cards.addView(options, matchWidth(dp(14)));
            cards.addView(gameData, matchWidth(dp(14)));
            cards.addView(downloadLogs, matchWidth(dp(14)));
            final LinearLayout.LayoutParams cardsParams =
                    new LinearLayout.LayoutParams(right, LinearLayout.LayoutParams.WRAP_CONTENT);
            cardsParams.leftMargin = dp(24);
            page.addView(cards, cardsParams);
        } else {
            page.setOrientation(LinearLayout.VERTICAL);
            page.setGravity(Gravity.CENTER_HORIZONTAL);
            final int width = Math.min(dp(520), screenWidth - 2 * pad);
            final LinearLayout column = new LinearLayout(this);
            column.setOrientation(LinearLayout.VERTICAL);
            column.addView(intro, matchWidth(0));
            column.addView(updates, matchWidth(dp(20)));
            column.addView(options, matchWidth(dp(14)));
            column.addView(gameData, matchWidth(dp(14)));
            column.addView(downloadLogs, matchWidth(dp(14)));
            column.addView(version, matchWidth(dp(18)));
            page.addView(column, new LinearLayout.LayoutParams(width, LinearLayout.LayoutParams.WRAP_CONTENT));
        }

        final ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(page);
        setContentView(scroll);
        restartGameIfAsked(getIntent());
        if (opened && settings.autoUpdate()) {
            checkForUpdates(true);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        restartGameIfAsked(intent);
    }

    /** Network play: the game asked to be started again into a LAN session. */
    private void restartGameIfAsked(Intent intent) {
        if (intent == null || !intent.getBooleanExtra(EXTRA_RESTART_GAME, false)) {
            return;
        }
        intent.removeExtra(EXTRA_RESTART_GAME);
        status.setText(Lang.t("Starting the LAN game..."));
        main.postDelayed(() -> startActivity(GameActivity.intent(this)), 700);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // The game's Language tab may have changed the language since this screen was drawn.
        final int shown = Lang.index();
        Lang.load(this);
        if (Lang.index() != shown) {
            recreate();
            return;
        }
        refresh();
        // Back from "Allow from this source": carry on with the update that was waiting for it.
        if (waitingUpdate != null && Updater.canInstall(this)) {
            final Updater.Release release = waitingUpdate;
            waitingUpdate = null;
            startUpdate(release);
        }
    }

    // --- updates -------------------------------------------------------------------------------

    private void updateAutoUpdateButton() {
        showToggle(autoUpdate, settings.autoUpdate());
    }

    /** The latest GitHub release: installs it when newer. `automatic`: Auto Update, at app start. */
    private void checkForUpdates(boolean automatic) {
        if (updating) {
            return;
        }
        updating = true;
        updateCheck.setEnabled(false);
        updateCheck.setText(Lang.t("Checking for updates\u2026"));
        new Thread(() -> {
            Updater.Release found = null;
            String failure = null;
            try {
                found = Updater.latest();
            } catch (Exception e) {
                failure = e.getMessage();
            }
            final Updater.Release release = found;
            final String error = failure;
            main.post(() -> {
                updating = false;
                updateCheck.setEnabled(true);
                updateCheck.setText(Lang.t("Check for Updates"));
                final String installed = Updater.installedVersion(this);
                if (release != null && Updater.isNewer(release.version, installed)) {
                    startUpdate(release);
                } else if (!automatic) {
                    final String message = error != null ? Lang.f("Couldn't check for updates: %s", error)
                            : release == null ? Lang.t("The latest release has no Android download.")
                            : Lang.f("You have the latest version (%s).", installed);
                    new AlertDialog.Builder(this).setMessage(message).setPositiveButton(Lang.t("OK"), null).show();
                } else if (skipAfterCheck) {
                    openGame();
                }
            });
        }, "update-check").start();
    }

    private void startUpdate(Updater.Release release) {
        if (!Updater.canInstall(this)) {
            new AlertDialog.Builder(this)
                    .setTitle(Lang.f("NSMBW %s is available", release.version))
                    .setMessage(Lang.t("To install updates, let NSMBW install apps: turn on Allow from this source "
                            + "on the next screen, then come back here."))
                    .setPositiveButton(Lang.t("Open Settings"), (d, w) -> {
                        waitingUpdate = release;
                        startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                Uri.parse("package:" + getPackageName())));
                    })
                    .setNegativeButton(Lang.t("Not Now"), (d, w) -> {
                        if (skipAfterCheck) {
                            openGame();
                        }
                    })
                    .show();
            return;
        }
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        final TextView label = text(Lang.t("Downloading\u2026"), 14);
        final LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(16), dp(24), dp(8));
        box.addView(bar);
        box.addView(label);
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(Lang.f("Updating to NSMBW %s", release.version)).setView(box).setCancelable(false).show();
        new Thread(() -> {
            String failure = null;
            try {
                Updater.downloadAndInstall(getApplicationContext(), release, (done, total) -> main.post(() -> {
                    bar.setIndeterminate(total <= 0);
                    if (total > 0) {
                        bar.setMax(1000);
                        bar.setProgress((int) (done * 1000 / total));
                        label.setText(Lang.f("Downloading\u2026 %d of %d MB", done >> 20, total >> 20));
                    }
                }));
            } catch (Exception e) {
                failure = e.getMessage();
            }
            final String error = failure;
            main.post(() -> {
                dialog.dismiss();
                if (error != null) {
                    new AlertDialog.Builder(this).setMessage(Lang.f("The update failed: %s", error))
                            .setPositiveButton(Lang.t("OK"), (d, w) -> {
                                if (skipAfterCheck) {
                                    openGame();
                                }
                            }).show();
                } else {
                    status.setText(Lang.f("Installing NSMBW %s\u2026", release.version));
                }
            });
        }, "update-download").start();
    }

    private void openGame() {
        skipAfterCheck = false;
        if (!AppFiles.gameReady(this)) {
            return;
        }
        startActivity(GameActivity.intent(this));
        overridePendingTransition(0, 0);
        finish();
    }

    private void updateSkipButton() {
        showToggle(skip, settings.skipStartMenu());
    }

    private void refresh() {
        final boolean present = AppFiles.hasGameData(this);
        final String problem = present ? AppFiles.gameProblem(AppFiles.gameDir(this)) : null;
        if (!present) {
            showStatus(Lang.t("No game data yet. Import the folder Dolphin made, or a zip of it."), AMBER);
        } else if (problem != null) {
            showStatus(Lang.f("The imported game can't be played. %s", problem), ACCENT);
        } else {
            showStatus(Lang.t("Game data is installed and checked."), GREEN);
        }
        play.setEnabled(present && problem == null);
        steps.setVisibility(present && problem == null ? View.GONE : View.VISIBLE);
    }

    /** Getting Started, laid out like KartPad's. */
    private void showHelp() {
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(24), dp(16), dp(24), dp(16));
        section(column, Lang.t("1. Import New Super Mario Bros. Wii"), Lang.t(DOLPHIN_STEPS));
        section(column, Lang.t("2. Play"), Lang.t("Tap Play. For LAN play, move up to the Couch button at the top right of the save file screen and switch it to LAN; "
                + "every player imports their own game."));
        section(column, Lang.t("Stuck on a step?"), Lang.t("The GitHub page covers the supported game, the steps above "
                + "and common problems."));
        column.addView(link(Lang.t("Setup Guide on GitHub"), v -> openWeb(PROJECT_URL + "#getting-the-game-files")));
        final TextView version = text("NSMBW " + versionText(), 13);
        version.setGravity(Gravity.START);
        version.setTextColor(Color.rgb(170, 178, 186));
        version.setPadding(0, dp(16), 0, 0);
        column.addView(version);
        final ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        new AlertDialog.Builder(this).setTitle(Lang.t("Getting Started")).setView(scroll)
                .setPositiveButton(Lang.t("Done"), null).show();
    }

    /** The Language button: sets the game's language and redraws this screen in it. */
    private void chooseLanguage() {
        new AlertDialog.Builder(this)
                .setTitle(Lang.t("Language"))
                .setSingleChoiceItems(Lang.NAMES, Lang.index(), (dialog, which) -> {
                    dialog.dismiss();
                    if (which != Lang.index()) {
                        Lang.set(this, which);
                        recreate();
                    }
                })
                .setNegativeButton(Lang.t("Cancel"), null)
                .show();
    }

    private void section(LinearLayout column, String title, String body) {
        final TextView heading = text(title, 20);
        heading.setGravity(Gravity.START);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(heading);
        final TextView text = text(body, 15);
        text.setGravity(Gravity.START);
        text.setTextColor(Color.rgb(200, 206, 212));
        text.setPadding(0, dp(6), 0, dp(18));
        column.addView(text);
    }

    private void openWeb(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            new AlertDialog.Builder(this).setMessage(url).setPositiveButton(Lang.t("OK"), null).show();
        }
    }

    private String versionText() {
        try {
            final PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName + " · Build " + AppFiles.versionCode(info);
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    private void pickZip() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] {"application/zip", "application/x-zip-compressed", "application/x-zip"});
        startActivityForResult(intent, REQUEST_ZIP);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        if (requestCode == REQUEST_IMPORT) {
            runImport(progress -> copyGame(uri, progress));
        } else if (requestCode == REQUEST_ZIP) {
            runImport(progress -> copyZip(uri, progress));
        } else if (requestCode == REQUEST_LOGS) {
            saveLogs(uri);
        }
    }

    private void saveLogs(Uri destination) {
        final String appVersion = versionText();
        new Thread(() -> {
            String message;
            try {
                final int sessions = LogExport.write(this, destination, appVersion);
                message = sessions == 0
                        ? Lang.t("Saved, but there are no game logs yet: play once, then download them again.")
                        : sessions == 1
                        ? Lang.t("Saved the logs of the last game session. Attach the zip to your GitHub issue.")
                        : Lang.f("Saved the logs of the last %d game sessions. Attach the zip to your GitHub issue.",
                                sessions);
            } catch (IOException | RuntimeException e) {
                message = Lang.f("Could not save the logs: %s", e.getMessage());
            }
            final String text = message;
            runOnUiThread(() -> new AlertDialog.Builder(this).setMessage(text)
                    .setPositiveButton(Lang.t("OK"), null).show());
        }, "LogExport").start();
    }

    // --- import --------------------------------------------------------------------------------

    private interface Progress { void update(int done, int total, String text); }

    private interface ImportJob { String run(Progress progress) throws IOException; }

    private static final class Entry {
        final String id, name;
        final boolean directory;
        Entry(String id, String name, boolean directory) { this.id = id; this.name = name; this.directory = directory; }
    }

    private void runImport(ImportJob job) {
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        final TextView label = text(Lang.t("Looking at the game files…"), 14);
        final LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(16), dp(24), dp(8));
        box.addView(bar);
        box.addView(label);
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(Lang.t("Importing game data")).setView(box).setCancelable(false).show();

        new Thread(() -> {
            String result;
            try {
                result = job.run((done, total, text) -> main.post(() -> {
                    bar.setIndeterminate(total <= 0);
                    if (total > 0) {
                        bar.setMax(total);
                        bar.setProgress(done);
                    }
                    label.setText(text);
                }));
            } catch (Exception e) {
                AppFiles.deleteRecursively(stagingDir());
                result = Lang.f("%s\n\nNothing was changed.", Lang.f("Import failed: %s", e.getMessage()));
            }
            final String message = result;
            main.post(() -> {
                dialog.dismiss();
                refresh();
                new AlertDialog.Builder(this).setMessage(message).setPositiveButton(Lang.t("OK"), null).show();
            });
        }, "game-import").start();
    }

    private File stagingDir() { return new File(AppFiles.dataDir(this), "game.importing"); }

    /** The checked game at `root` (inside the staging folder) becomes the game; saves stay. */
    private String install(File root, int fileCount) throws IOException {
        final File staging = stagingDir();
        final String problem = AppFiles.gameProblem(root);
        if (problem != null) {
            AppFiles.deleteRecursively(staging);
            return Lang.f("%s\n\nNothing was changed.", problem);
        }
        final File game = AppFiles.gameDir(this);
        AppFiles.deleteRecursively(game);
        if (!root.renameTo(game)) throw new IOException("cannot move the imported data into place");
        AppFiles.deleteRecursively(staging);
        return Lang.f("Imported %d files. Tap Play to start.", fileCount);
    }

    /** A folder from the document picker: copied into the staging folder, checked, then swapped in. */
    private String copyGame(Uri tree, Progress progress) throws IOException {
        final ContentResolver resolver = getContentResolver();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        if (!looksLikeGame(resolver, tree, rootId)) {
            String nested = null;
            for (Entry e : list(resolver, tree, rootId)) {
                if (e.directory && e.name.equalsIgnoreCase("DATA") && looksLikeGame(resolver, tree, e.id)) nested = e.id;
            }
            if (nested == null) {
                return Lang.t("That folder is not an extracted game: it needs files/ and sys/ (or a DATA folder holding "
                        + "them), as Dolphin's Extract Entire Disc makes.");
            }
            rootId = nested;
        }
        // Which game and revision, before copying anything.
        final String problem = treeProblem(resolver, tree, rootId);
        if (problem != null) {
            return Lang.f("%s\n\nNothing was changed.", problem);
        }
        final List<String[]> files = new ArrayList<>();  // {documentId, relative path}
        collect(resolver, tree, rootId, "", files);

        final File staging = stagingDir();
        AppFiles.deleteRecursively(staging);
        for (int i = 0; i < files.size(); ++i) {
            final File target = new File(staging, files.get(i)[1]);
            final File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            final Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, files.get(i)[0]);
            try (InputStream in = resolver.openInputStream(document); OutputStream out = new FileOutputStream(target)) {
                if (in == null) throw new IOException("cannot read " + files.get(i)[1]);
                AppFiles.copy(in, out);
            }
            progress.update(i + 1, files.size(), Lang.f("%d of %d files", i + 1, files.size()));
        }
        return install(staging, files.size());
    }

    /** sys/boot.bin of the picked folder names the game and its revision. */
    private String treeProblem(ContentResolver resolver, Uri tree, String rootId) throws IOException {
        String sysId = null;
        for (Entry e : list(resolver, tree, rootId)) {
            if (e.directory && e.name.equals("sys")) sysId = e.id;
        }
        String bootId = null;
        if (sysId != null) {
            for (Entry e : list(resolver, tree, sysId)) {
                if (!e.directory && e.name.equals("boot.bin")) bootId = e.id;
            }
        }
        if (bootId == null) {
            return Lang.t("That folder has no sys/boot.bin, so the game can't be identified.");
        }
        final byte[] header = new byte[8];
        try (InputStream in = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, bootId))) {
            if (in == null || AppFiles.readFully(in, header) < header.length) {
                return Lang.t("That folder's sys/boot.bin is too short, so the game can't be identified.");
            }
        }
        return AppFiles.headerProblem(header);
    }

    /** A zip of the folder Dolphin extracted: unpacked into the staging folder, checked, swapped in. */
    private String copyZip(Uri zip, Progress progress) throws IOException {
        final long size = documentSize(zip);
        final File staging = stagingDir();
        AppFiles.deleteRecursively(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        final String stagingPath = staging.getCanonicalPath() + File.separator;
        int count = 0;
        try (InputStream raw = getContentResolver().openInputStream(zip)) {
            if (raw == null) throw new IOException("cannot read the zip");
            final CountingInputStream counted = new CountingInputStream(new BufferedInputStream(raw, 1 << 16));
            try (ZipInputStream in = new ZipInputStream(counted)) {
                ZipEntry entry;
                while ((entry = in.getNextEntry()) != null) {
                    // Windows PowerShell's zips separate (and end folder entries) with backslashes.
                    final String name = entry.getName().replace('\\', '/');
                    final File target = new File(staging, name);
                    if (!target.getCanonicalPath().startsWith(stagingPath)) {
                        throw new IOException("the zip holds a path outside its folder: " + name);
                    }
                    if (entry.isDirectory() || name.endsWith("/")) {
                        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
                        continue;
                    }
                    final File parent = target.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
                    try (OutputStream out = new FileOutputStream(target)) {
                        AppFiles.copy(in, out);
                    }
                    ++count;
                    if (size > 0) {
                        final int permille = (int) Math.min(1000, counted.count * 1000 / size);
                        progress.update(permille, 1000, Lang.f("%d of %d MB", counted.count >> 20, size >> 20));
                    } else {
                        progress.update(0, 0, Lang.f("%d files", count));
                    }
                }
            }
        }
        final File root = findGameRoot(staging, 3);
        if (root == null) {
            AppFiles.deleteRecursively(staging);
            return Lang.f("%s\n\nNothing was changed.", Lang.t("That zip doesn't hold an extracted game: zip the "
                    + "folder Dolphin's Extract Entire Disc made (the one with files and sys in it)."));
        }
        return install(root, count);
    }

    /** The extracted game inside an unpacked zip: its top, DATA, or a folder holding either. */
    private static File findGameRoot(File dir, int depth) {
        if (AppFiles.isGameRoot(dir)) return dir;
        if (depth == 0) return null;
        final File[] children = dir.listFiles(File::isDirectory);
        if (children == null) return null;
        for (File child : children) {
            if (child.getName().equals("__MACOSX")) continue;
            final File found = findGameRoot(child, depth - 1);
            if (found != null) return found;
        }
        return null;
    }

    private long documentSize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (RuntimeException ignored) {
            // no size: progress counts files instead
        }
        return -1;
    }

    private static final class CountingInputStream extends FilterInputStream {
        long count;
        CountingInputStream(InputStream in) { super(in); }
        @Override public int read() throws IOException {
            final int b = super.read();
            if (b >= 0) ++count;
            return b;
        }
        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            final int read = super.read(buffer, offset, length);
            if (read > 0) count += read;
            return read;
        }
        @Override public long skip(long n) throws IOException {
            final long skipped = super.skip(n);
            count += skipped;
            return skipped;
        }
    }

    private boolean looksLikeGame(ContentResolver resolver, Uri tree, String id) {
        boolean sys = false, filesDir = false;
        for (Entry e : list(resolver, tree, id)) {
            if (e.directory && e.name.equals("sys")) sys = true;
            if (e.directory && e.name.equals("files")) filesDir = true;
        }
        return sys && filesDir;
    }

    private void collect(ContentResolver resolver, Uri tree, String id, String prefix, List<String[]> out) {
        for (Entry e : list(resolver, tree, id)) {
            final String path = prefix.isEmpty() ? e.name : prefix + "/" + e.name;
            if (e.directory) collect(resolver, tree, e.id, path, out);
            else out.add(new String[] {e.id, path});
        }
    }

    private List<Entry> list(ContentResolver resolver, Uri tree, String id) {
        final List<Entry> entries = new ArrayList<>();
        final Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        final String[] columns = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        };
        try (Cursor c = resolver.query(children, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                entries.add(new Entry(c.getString(0), c.getString(1),
                        DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))));
            }
        }
        return entries;
    }

    // --- views ---------------------------------------------------------------------------------

    private TextView text(String value, int sp) {
        final TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setTextColor(Color.WHITE);
        view.setGravity(Gravity.CENTER_HORIZONTAL);
        return view;
    }

    private TextView link(String label, View.OnClickListener onClick) {
        final TextView view = text(label, 16);
        view.setTextColor(Color.rgb(255, 138, 128));
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setPadding(dp(12), dp(10), dp(12), dp(10));
        view.setOnClickListener(onClick);
        view.setBackgroundResource(android.R.drawable.list_selector_background);
        return view;
    }

    // The start menu's colors, on the theme's #101418 background.
    private static final int ACCENT = Color.rgb(229, 57, 53);
    private static final int SURFACE = Color.rgb(24, 31, 38);
    private static final int RAISED = Color.rgb(38, 48, 58);
    private static final int STROKE = Color.rgb(48, 60, 72);
    private static final int MUTED = Color.rgb(170, 178, 186);
    private static final int FAINT = Color.rgb(120, 128, 136);
    private static final int GREEN = Color.rgb(102, 187, 106);
    private static final int AMBER = Color.rgb(255, 183, 77);
    private static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    /** A rounded shape that ripples when touched; `disabled` is its color while disabled. */
    private RippleDrawable rounded(int color, int disabled, int radius, int stroke) {
        final GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(radius);
        shape.setColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                new int[] {disabled, color}));
        if (stroke != 0) {
            shape.setStroke(dp(1), stroke);
        }
        final GradientDrawable mask = new GradientDrawable();
        mask.setCornerRadius(radius);
        mask.setColor(Color.WHITE);
        return new RippleDrawable(ColorStateList.valueOf(Color.argb(56, 255, 255, 255)), shape, mask);
    }

    /** A pill button: `primary` is the big red Play. */
    private Button button(String label, boolean primary) {
        final Button view = new Button(this);
        view.setText(label);
        view.setAllCaps(false);
        view.setStateListAnimator(null);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, primary ? 20 : 16);
        view.setTypeface(primary ? Typeface.DEFAULT_BOLD : MEDIUM);
        view.setTextColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
                new int[] {FAINT, Color.WHITE}));
        final int height = dp(primary ? 60 : 50);
        view.setMinHeight(height);
        view.setMinimumHeight(height);
        view.setPadding(dp(20), 0, dp(20), 0);
        view.setBackground(primary ? rounded(ACCENT, RAISED, height / 2, 0)
                : rounded(RAISED, SURFACE, height / 2, 0));
        return view;
    }

    /** A rounded section of the start menu, with a small heading. */
    private LinearLayout card(String heading) {
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(14), dp(16), dp(16));
        final GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(22));
        shape.setColor(SURFACE);
        shape.setStroke(dp(1), Color.rgb(34, 44, 53));
        card.setBackground(shape);
        final TextView title = text(heading.toUpperCase(Locale.ROOT), 12);
        title.setGravity(Gravity.START);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setLetterSpacing(0.12f);
        title.setTextColor(MUTED);
        card.addView(title, matchWidth(0));
        return card;
    }

    /** A setting row in `card`: its label, an On/Off pill (returned) and what On does underneath. */
    private TextView toggle(LinearLayout card, String label, String help, View.OnClickListener onClick) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setMinimumHeight(dp(50));
        row.setPadding(dp(20), dp(6), dp(8), dp(6));
        row.setBackground(rounded(RAISED, RAISED, dp(25), 0));
        row.setClickable(true);
        row.setOnClickListener(onClick);
        final TextView name = text(label, 16);
        name.setTypeface(MEDIUM);
        name.setGravity(Gravity.START);
        row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        final TextView pill = text("", 14);
        pill.setTypeface(Typeface.DEFAULT_BOLD);
        pill.setGravity(Gravity.CENTER);
        pill.setMinWidth(dp(64));
        pill.setPadding(dp(14), dp(7), dp(14), dp(7));
        row.addView(pill);
        card.addView(row, matchWidth(dp(10)));
        final TextView about = text(help, 13);
        about.setGravity(Gravity.START);
        about.setTextColor(MUTED);
        about.setPadding(dp(4), dp(6), dp(4), 0);
        card.addView(about, matchWidth(0));
        return pill;
    }

    private void showToggle(TextView pill, boolean on) {
        pill.setText(Lang.t(on ? "On" : "Off"));
        pill.setTextColor(on ? Color.WHITE : MUTED);
        final GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(17));
        shape.setColor(on ? ACCENT : SURFACE);
        pill.setBackground(shape);
    }

    private void showStatus(String text, int color) {
        status.setText(text);
        final GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(color);
        dot.setSize(dp(10), dp(10));
        status.setCompoundDrawablesRelativeWithIntrinsicBounds(dot, null, null, null);
    }

    private LinearLayout.LayoutParams matchWidth(int topMargin) {
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = topMargin;
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}

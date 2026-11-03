package com.wither.nsmbw;

import android.content.Context;
import android.util.Log;

import java.io.IOException;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The start menu's language: the game's own Language setting (Config.toml's [game] language), so
 * picking one here or in the game's Language tab changes both. The USA disc's three languages, and
 * Italian, whose game text comes from a language pack (LauncherActivity.languagePack); English is the
 * default. Text is looked up by its English wording.
 */
final class Lang {
    static final String[] CODES = {"en", "es", "fr", "it"};
    /** Each language's name in itself, as the game's Language tab lists them. */
    static final String[] NAMES = {"English", "Español", "Français", "Italiano"};

    private static int current;
    private static final Map<String, String[]> TABLE = new HashMap<>();

    private Lang() {}

    /** Reads the game's language; call before building any text. */
    static void load(Context context) {
        current = Math.max(0, Arrays.asList(CODES).indexOf(AppFiles.gameLanguage(context)));
    }

    static int index() { return current; }

    /** Whether the disc has this language's text (the others need a language pack). */
    static boolean onDisc(int index) { return index < 3; }

    /** Sets the game's language (it applies the next time the game starts) and the start menu's. */
    static void set(Context context, int index) {
        try {
            AppFiles.setGameLanguage(context, CODES[index]);
            current = index;
        } catch (IOException e) {
            Log.e("NSMBW", "could not save the language", e);
        }
    }

    /** `english` in the start menu's language. */
    static String t(String english) {
        if (current == 0) return english;
        final String[] row = TABLE.get(english);
        if (row == null || row[current - 1] == null) {
            Log.w("NSMBW", "untranslated: " + english);
            return english;
        }
        return row[current - 1];
    }

    /** t(english), with `args` put in (String.format). */
    static String f(String english, Object... args) {
        return String.format(Locale.ROOT, t(english), args);
    }

    private static void put(String english, String spanish, String french) {
        final String[] row = row(english);
        row[0] = spanish;
        row[1] = french;
    }

    private static void it(String english, String italian) {
        row(english)[2] = italian;
    }

    private static String[] row(String english) {
        return TABLE.computeIfAbsent(english, k -> new String[CODES.length - 1]);
    }

    static {
        // --- start screen ---
        put("Language", "Idioma", "Langue");
        put("Help", "Ayuda", "Aide");
        put("New Super Mario Bros. Wii, recompiled to run natively",
                "New Super Mario Bros. Wii, recompilado para ejecutarse de forma nativa",
                "New Super Mario Bros. Wii, recompilé pour fonctionner en natif");
        put("Play", "Jugar", "Jouer");
        put("Updates", "Actualizaciones", "Mises à jour");
        put("Check for Updates", "Buscar actualizaciones", "Rechercher des mises à jour");
        put("Auto Update", "Actualización automática", "Mise à jour automatique");
        put("On: opening the app installs the latest release from GitHub when there is one. Your saves, "
                + "settings and game files are kept.",
                "Activado: al abrir la app se instala la última versión de GitHub, si hay una. Se conservan "
                + "tus partidas, ajustes y archivos del juego.",
                "Activé : à l'ouverture, l'appli installe la dernière version de GitHub s'il y en a une. Vos "
                + "sauvegardes, réglages et fichiers du jeu sont conservés.");
        put("Options", "Opciones", "Options");
        put("Settings", "Ajustes", "Réglages");
        put("Tailscale & LAN Devices", "Dispositivos Tailscale y LAN", "Appareils Tailscale et LAN");
        put("Skip Start Menu", "Omitir menú de inicio", "Passer le menu de démarrage");
        put("On: opening the app goes straight into the game. In the game, the phone's Back gesture and "
                + "Start Menu bring you back here.",
                "Activado: al abrir la app se entra directamente al juego. En el juego, el gesto Atrás del "
                + "teléfono y Start Menu te traen de vuelta aquí.",
                "Activé : l'appli lance directement le jeu. En jeu, le geste Retour du téléphone et Start "
                + "Menu vous ramènent ici.");
        put("Xiaomi Mode", "Modo Xiaomi", "Mode Xiaomi");
        put("On: the game runs in the app's main process. For Xiaomi, Redmi and POCO phones (on by default "
                + "there), whose system closes the game's own process and leaves a black screen.",
                "Activado: el juego se ejecuta en el proceso principal de la app. Para teléfonos Xiaomi, "
                + "Redmi y POCO (activado por defecto en ellos), cuyo sistema cierra el proceso del juego y "
                + "deja la pantalla negra.",
                "Activé : le jeu tourne dans le processus principal de l'appli. Pour les téléphones Xiaomi, "
                + "Redmi et POCO (activé par défaut sur ceux-ci), dont le système ferme le processus du jeu "
                + "et laisse un écran noir.");
        put("On", "Sí", "Oui");
        put("Off", "No", "Non");
        put("Game Data", "Datos del juego", "Données du jeu");
        put("Get the game files (once)", "Consigue los archivos del juego (una vez)",
                "Obtenir les fichiers du jeu (une fois)");
        put("1. In Dolphin on a computer, right-click New Super Mario Bros. Wii and choose Properties.\n"
                + "2. Open Filesystem, right-click the disc at the top and choose Extract Entire Disc.\n"
                + "3. Copy that folder (or a zip of it) to this phone.\n"
                + "4. Tap Import from Extracted Game Data Folder… or Import Game Data Zip… below and "
                + "choose it.\n\n"
                + "Use the USA disc, revision 1 (SMNE01): Dolphin shows the revision under Properties > Info.",
                "1. En Dolphin, en una computadora, haz clic derecho en New Super Mario Bros. Wii y elige "
                + "Propiedades.\n"
                + "2. Abre Sistema de archivos, haz clic derecho en el disco de arriba y elige Extraer disco "
                + "completo.\n"
                + "3. Copia esa carpeta (o un zip de ella) a este teléfono.\n"
                + "4. Toca Importar desde la carpeta de datos extraídos… o Importar zip de datos del "
                + "juego… abajo y elígela.\n\n"
                + "Usa el disco de EE. UU., revisión 1 (SMNE01): Dolphin muestra la revisión en Propiedades > "
                + "Información.",
                "1. Dans Dolphin sur un ordinateur, faites un clic droit sur New Super Mario Bros. Wii et "
                + "choisissez Propriétés.\n"
                + "2. Ouvrez Système de fichiers, faites un clic droit sur le disque en haut et choisissez "
                + "Extraire le disque entier.\n"
                + "3. Copiez ce dossier (ou un zip de celui-ci) sur ce téléphone.\n"
                + "4. Touchez Importer depuis le dossier de données extrait… ou Importer un zip des "
                + "données du jeu… ci-dessous et choisissez-le.\n\n"
                + "Utilisez le disque américain, révision 1 (SMNE01) : Dolphin affiche la révision dans "
                + "Propriétés > Infos.");
        put("Import your own New Super Mario Bros. Wii (USA, SMNE01) once. Easiest: the folder Dolphin's "
                + "Extract Entire Disc makes (DATA, or the folder that holds it), or a zip of that folder. "
                + "Your saves are kept.",
                "Importa tu propio New Super Mario Bros. Wii (EE. UU., SMNE01) una vez. Lo más fácil: la "
                + "carpeta que crea Extraer disco completo de Dolphin (DATA, o la carpeta que la contiene), o "
                + "un zip de esa carpeta. Tus partidas se conservan.",
                "Importez une fois votre propre New Super Mario Bros. Wii (USA, SMNE01). Le plus simple : le "
                + "dossier créé par Extraire le disque entier de Dolphin (DATA, ou le dossier qui le contient), "
                + "ou un zip de ce dossier. Vos sauvegardes sont conservées.");
        put("Import from Extracted Game Data Folder…", "Importar desde la carpeta de datos extraídos…",
                "Importer depuis le dossier de données extrait…");
        put("Import Game Data Zip…", "Importar zip de datos del juego…",
                "Importer un zip des données du jeu…");
        put("Download Logs (For Github Reports)", "Descargar registros (para reportes en GitHub)",
                "Télécharger les journaux (pour les rapports GitHub)");
        put("No game data yet. Import the folder Dolphin made, or a zip of it.",
                "Aún no hay datos del juego. Importa la carpeta que creó Dolphin, o un zip de ella.",
                "Pas encore de données du jeu. Importez le dossier créé par Dolphin, ou un zip de celui-ci.");
        put("The imported game can't be played. %s", "El juego importado no se puede jugar. %s",
                "Le jeu importé ne peut pas être lancé. %s");
        put("Game data is installed and checked.", "Los datos del juego están instalados y verificados.",
                "Les données du jeu sont installées et vérifiées.");
        put("Starting the LAN game...", "Iniciando la partida LAN...", "Lancement de la partie LAN...");
        put("OK", "Aceptar", "OK");
        put("Done", "Listo", "Terminé");
        put("Back", "Atrás", "Retour");
        put("Cancel", "Cancelar", "Annuler");

        // --- updates ---
        put("Checking for updates…", "Buscando actualizaciones…", "Recherche de mises à jour…");
        put("Couldn't check for updates: %s", "No se pudo buscar actualizaciones: %s",
                "Impossible de rechercher des mises à jour : %s");
        put("The latest release has no Android download.", "La última versión no tiene descarga para Android.",
                "La dernière version n'a pas de téléchargement Android.");
        put("You have the latest version (%s).", "Tienes la última versión (%s).",
                "Vous avez la dernière version (%s).");
        put("NSMBW %s is available", "NSMBW %s está disponible", "NSMBW %s est disponible");
        put("To install updates, let NSMBW install apps: turn on Allow from this source on the next screen, "
                + "then come back here.",
                "Para instalar actualizaciones, permite que NSMBW instale apps: activa Permitir desde esta "
                + "fuente en la siguiente pantalla y luego vuelve aquí.",
                "Pour installer les mises à jour, autorisez NSMBW à installer des applis : activez Autoriser "
                + "cette source sur l'écran suivant, puis revenez ici.");
        put("Open Settings", "Abrir ajustes", "Ouvrir les réglages");
        put("Not Now", "Ahora no", "Pas maintenant");
        put("Downloading…", "Descargando…", "Téléchargement…");
        put("Downloading… %d of %d MB", "Descargando… %d de %d MB", "Téléchargement… %d sur %d Mo");
        put("Updating to NSMBW %s", "Actualizando a NSMBW %s", "Mise à jour vers NSMBW %s");
        put("The update failed: %s", "La actualización falló: %s", "La mise à jour a échoué : %s");
        put("Installing NSMBW %s…", "Instalando NSMBW %s…", "Installation de NSMBW %s…");

        // --- help ---
        put("Getting Started", "Primeros pasos", "Premiers pas");
        put("1. Import New Super Mario Bros. Wii", "1. Importa New Super Mario Bros. Wii",
                "1. Importer New Super Mario Bros. Wii");
        put("Use your own USA New Super Mario Bros. Wii (SMNE01, revision 1). Easiest: in Dolphin on a "
                + "computer, right-click the game, choose Properties, Filesystem, then right-click the disc and "
                + "choose Extract Entire Disc. Copy that folder (or a zip of it) to this phone and tap Import. "
                + "Dolphin shows the revision under Properties > Info.",
                "Usa tu propio New Super Mario Bros. Wii de EE. UU. (SMNE01, revisión 1). Lo más fácil: en "
                + "Dolphin, en una computadora, haz clic derecho en el juego, elige Propiedades, Sistema de "
                + "archivos, luego haz clic derecho en el disco y elige Extraer disco completo. Copia esa "
                + "carpeta (o un zip de ella) a este teléfono y toca Importar. Dolphin muestra la revisión en "
                + "Propiedades > Información.",
                "Utilisez votre propre New Super Mario Bros. Wii américain (SMNE01, révision 1). Le plus "
                + "simple : dans Dolphin sur un ordinateur, faites un clic droit sur le jeu, choisissez "
                + "Propriétés, Système de fichiers, puis faites un clic droit sur le disque et choisissez "
                + "Extraire le disque entier. Copiez ce dossier (ou un zip de celui-ci) sur ce téléphone et "
                + "touchez Importer. Dolphin affiche la révision dans Propriétés > Infos.");
        put("2. Play", "2. Juega", "2. Jouer");
        put("Tap Play. For LAN play, move up to the Couch button at the top right of the save file screen and "
                + "switch it to LAN; every player imports their own game.",
                "Toca Jugar. Para jugar en LAN, sube al botón Couch arriba a la derecha de la pantalla de "
                + "archivos y cámbialo a LAN; cada jugador importa su propio juego.",
                "Touchez Jouer. Pour jouer en LAN, montez jusqu'au bouton Couch en haut à droite de l'écran "
                + "des fichiers et passez-le sur LAN ; chaque joueur importe son propre jeu.");
        put("Stuck on a step?", "¿Atascado en un paso?", "Bloqué à une étape ?");
        put("The GitHub page covers the supported game, the steps above and common problems.",
                "La página de GitHub explica el juego compatible, los pasos anteriores y los problemas comunes.",
                "La page GitHub présente le jeu pris en charge, les étapes ci-dessus et les problèmes courants.");
        put("Setup Guide on GitHub", "Guía de instalación en GitHub", "Guide d'installation sur GitHub");

        // --- logs ---
        put("Saved, but there are no game logs yet: play once, then download them again.",
                "Guardado, pero aún no hay registros del juego: juega una vez y luego descárgalos de nuevo.",
                "Enregistré, mais il n'y a pas encore de journaux du jeu : jouez une fois, puis "
                + "téléchargez-les à nouveau.");
        put("Saved the logs of the last game session. Attach the zip to your GitHub issue.",
                "Se guardaron los registros de la última sesión de juego. Adjunta el zip a tu reporte en "
                + "GitHub.",
                "Les journaux de la dernière session de jeu sont enregistrés. Joignez le zip à votre "
                + "rapport GitHub.");
        put("Saved the logs of the last %d game sessions. Attach the zip to your GitHub issue.",
                "Se guardaron los registros de las últimas %d sesiones de juego. Adjunta el zip a tu reporte "
                + "en GitHub.",
                "Les journaux des %d dernières sessions de jeu sont enregistrés. Joignez le zip à votre "
                + "rapport GitHub.");
        put("Could not save the logs: %s", "No se pudieron guardar los registros: %s",
                "Impossible d'enregistrer les journaux : %s");

        // --- import ---
        put("Looking at the game files…", "Revisando los archivos del juego…",
                "Examen des fichiers du jeu…");
        put("Importing game data", "Importando datos del juego", "Importation des données du jeu");
        put("%s\n\nNothing was changed.", "%s\n\nNo se cambió nada.", "%s\n\nRien n'a été modifié.");
        put("Import failed: %s", "La importación falló: %s", "L'importation a échoué : %s");
        put("Imported %d files. Tap Play to start.", "Se importaron %d archivos. Toca Jugar para empezar.",
                "%d fichiers importés. Touchez Jouer pour commencer.");
        put("%d of %d files", "%d de %d archivos", "%d fichiers sur %d");
        put("%d of %d MB", "%d de %d MB", "%d sur %d Mo");
        put("%d files", "%d archivos", "%d fichiers");
        put("That folder is not an extracted game: it needs files/ and sys/ (or a DATA folder holding them), "
                + "as Dolphin's Extract Entire Disc makes.",
                "Esa carpeta no es un juego extraído: necesita files/ y sys/ (o una carpeta DATA que las "
                + "contenga), como las que crea Extraer disco completo de Dolphin.",
                "Ce dossier n'est pas un jeu extrait : il lui faut files/ et sys/ (ou un dossier DATA qui les "
                + "contient), comme ceux que crée Extraire le disque entier de Dolphin.");
        put("That folder has no sys/boot.bin, so the game can't be identified.",
                "Esa carpeta no tiene sys/boot.bin, así que no se puede identificar el juego.",
                "Ce dossier n'a pas de sys/boot.bin, le jeu ne peut donc pas être identifié.");
        put("That folder's sys/boot.bin is too short, so the game can't be identified.",
                "El sys/boot.bin de esa carpeta es demasiado corto, así que no se puede identificar el juego.",
                "Le sys/boot.bin de ce dossier est trop court, le jeu ne peut donc pas être identifié.");
        put("That zip doesn't hold an extracted game: zip the folder Dolphin's Extract Entire Disc made (the "
                + "one with files and sys in it).",
                "Ese zip no contiene un juego extraído: comprime la carpeta que creó Extraer disco completo de "
                + "Dolphin (la que tiene files y sys).",
                "Ce zip ne contient pas de jeu extrait : zippez le dossier créé par Extraire le disque entier "
                + "de Dolphin (celui qui contient files et sys).");
        put("That isn't an extracted game: it needs the files and sys folders Dolphin's Extract Entire Disc "
                + "makes.",
                "Eso no es un juego extraído: necesita las carpetas files y sys que crea Extraer disco "
                + "completo de Dolphin.",
                "Ce n'est pas un jeu extrait : il lui faut les dossiers files et sys que crée Extraire le "
                + "disque entier de Dolphin.");
        put("Its sys/boot.bin is too short, so the game can't be identified.",
                "Su sys/boot.bin es demasiado corto, así que no se puede identificar el juego.",
                "Son sys/boot.bin est trop court, le jeu ne peut donc pas être identifié.");
        put("It has no sys/boot.bin, so the game can't be identified.",
                "No tiene sys/boot.bin, así que no se puede identificar el juego.",
                "Il n'a pas de sys/boot.bin, le jeu ne peut donc pas être identifié.");
        put("That is %s, not New Super Mario Bros. Wii for the USA (SMNE01). This port runs only the USA disc, "
                + "revision 1.",
                "Eso es %s, no New Super Mario Bros. Wii de EE. UU. (SMNE01). Este port solo funciona con el "
                + "disco de EE. UU., revisión 1.",
                "C'est %s, pas New Super Mario Bros. Wii américain (SMNE01). Ce portage ne fonctionne qu'avec "
                + "le disque américain, révision 1.");
        put("That is New Super Mario Bros. Wii revision %d. This port runs only revision 1 (Dolphin shows it "
                + "under Properties > Info).",
                "Eso es New Super Mario Bros. Wii revisión %d. Este port solo funciona con la revisión 1 "
                + "(Dolphin la muestra en Propiedades > Información).",
                "C'est New Super Mario Bros. Wii révision %d. Ce portage ne fonctionne qu'avec la révision 1 "
                + "(Dolphin l'affiche dans Propriétés > Infos).");

        // --- Settings ---
        put("Display", "Pantalla", "Affichage");
        put("Controls", "Controles", "Commandes");
        put("Aspect Ratio", "Relación de aspecto", "Format d'image");
        put("Render Resolution", "Resolución de renderizado", "Résolution de rendu");
        put("Performance Monitor", "Monitor de rendimiento", "Moniteur de performances");
        put("Spinning Coins", "Monedas giratorias", "Pièces qui tournent");
        put("V-Sync", "V-Sync", "V-Sync");
        put("Graphics Compatibility", "Compatibilidad gráfica", "Compatibilité graphique");
        put("Renderer", "Renderizador", "Moteur de rendu");
        put("Automatic", "Automático", "Automatique");
        put("Automatic (recommended): Vulkan, else OpenGL ES", "Automático (recomendado): Vulkan, si no OpenGL ES",
                "Automatique (recommandé) : Vulkan, sinon OpenGL ES");
        put("OpenGL ES 3.1+ (in a later version)", "OpenGL ES 3.1+ (en una versión futura)",
                "OpenGL ES 3.1+ (dans une version future)");
        put("Auto", "Auto", "Auto");
        put("Auto (recommended): on for the GPUs that need it",
                "Auto (recomendado): activado para las GPU que lo necesitan",
                "Auto (recommandé) : activé pour les GPU qui en ont besoin");
        put("On: always use the GPU fixes", "Sí: usar siempre las correcciones de GPU",
                "Oui : toujours utiliser les correctifs GPU");
        put("Off: never use them", "No: no usarlas nunca", "Non : ne jamais les utiliser");
        put("Original 4:3", "4:3 original", "4:3 d'origine");
        put("Fill Screen", "Llenar pantalla", "Plein écran");
        put("Match screen", "Igual a la pantalla", "Comme l'écran");
        put("Touch Controls", "Controles táctiles", "Commandes tactiles");
        put("Touch Control Opacity", "Opacidad de controles táctiles", "Opacité des commandes tactiles");
        put("Tilt (turn the phone)", "Inclinación (gira el teléfono)", "Inclinaison (tournez le téléphone)");
        put("Shake (jolt the phone)", "Sacudida (agita el teléfono)", "Secousse (secouez le téléphone)");
        put("Vibration", "Vibración", "Vibration");

        // --- Tailscale & LAN Devices ---
        put("This phone", "Este teléfono", "Ce téléphone");
        put("No network", "Sin red", "Aucun réseau");
        put("Rooms on your Wi-Fi are found by themselves. Over Tailscale, PCs find every device, and devices "
                + "that have played together find each other. To find another phone the first time, add its "
                + "Tailscale name or address here (or it adds yours).",
                "Las salas de tu Wi-Fi se encuentran solas. Por Tailscale, las PC encuentran todos los "
                + "dispositivos, y los dispositivos que ya jugaron juntos se encuentran entre sí. Para "
                + "encontrar otro teléfono la primera vez, añade aquí su nombre o dirección de Tailscale (o "
                + "que él añada la tuya).",
                "Les salles de votre Wi-Fi sont trouvées toutes seules. Par Tailscale, les PC trouvent tous "
                + "les appareils, et les appareils qui ont déjà joué ensemble se retrouvent. Pour trouver un "
                + "autre téléphone la première fois, ajoutez ici son nom ou son adresse Tailscale (ou il "
                + "ajoute la vôtre).");
        put("Devices asked for rooms", "Dispositivos a los que se piden salas", "Appareils interrogés pour les salles");
        put("None yet", "Ninguno todavía", "Aucun pour l'instant");
        put("Stop asking %s for rooms?", "¿Dejar de pedir salas a %s?", "Ne plus demander de salles à %s ?");
        put("Remove", "Quitar", "Retirer");
        put("Add Device…", "Añadir dispositivo…", "Ajouter un appareil…");
        put("Add a Device", "Añadir un dispositivo", "Ajouter un appareil");
        put("Its Tailscale name (as in the Tailscale app) or address. It is asked for rooms from now on.",
                "Su nombre de Tailscale (como aparece en la app de Tailscale) o su dirección. A partir de ahora "
                + "se le pedirán salas.",
                "Son nom Tailscale (comme dans l'appli Tailscale) ou son adresse. On lui demandera des salles "
                + "à partir de maintenant.");
        put("Add", "Añadir", "Ajouter");
        put("e.g. s26-ultra or 100.84.140.32", "p. ej. s26-ultra o 100.84.140.32", "ex. s26-ultra ou 100.84.140.32");

        // --- in the game (GameActivity) ---
        put("Start Menu", "Menú de inicio", "Menu de démarrage");
        put("Leave LAN Game", "Salir de la partida LAN", "Quitter la partie LAN");
        put("Quit Game", "Salir del juego", "Quitter le jeu");
        put("Back to the Game", "Volver al juego", "Retour au jeu");
        put("Connect", "Conectar", "Se connecter");
        put("To install updates, let NSMBW install apps: turn on Allow from this source on the next screen, "
                + "then come back to the game.",
                "Para instalar actualizaciones, permite que NSMBW instale apps: activa Permitir desde esta "
                + "fuente en la siguiente pantalla y luego vuelve al juego.",
                "Pour installer les mises à jour, autorisez NSMBW à installer des applis : activez Autoriser "
                + "cette source sur l'écran suivant, puis revenez au jeu.");
        // --- Italian, by the same English wording ---
        it("Language", "Lingua");
        it("Help", "Aiuto");
        it("New Super Mario Bros. Wii, recompiled to run natively",
                "New Super Mario Bros. Wii, ricompilato per funzionare in modo nativo");
        it("Play", "Gioca");
        it("Updates", "Aggiornamenti");
        it("Check for Updates", "Cerca aggiornamenti");
        it("Auto Update", "Aggiornamento automatico");
        it("On: opening the app installs the latest release from GitHub when there is one. Your saves, "
                + "settings and game files are kept.",
                "Sì: all'apertura l'app installa l'ultima versione da GitHub, se ce n'è una. Salvataggi, "
                + "impostazioni e file del gioco vengono mantenuti.");
        it("Options", "Opzioni");
        it("Settings", "Impostazioni");
        it("Tailscale & LAN Devices", "Dispositivi Tailscale e LAN");
        it("Skip Start Menu", "Salta il menu iniziale");
        it("On: opening the app goes straight into the game. In the game, the phone's Back gesture and "
                + "Start Menu bring you back here.",
                "Sì: all'apertura l'app entra subito nel gioco. Nel gioco, il gesto Indietro del telefono e "
                + "Start Menu ti riportano qui.");
        it("Xiaomi Mode", "Modalità Xiaomi");
        it("On: the game runs in the app's main process. For Xiaomi, Redmi and POCO phones (on by default "
                + "there), whose system closes the game's own process and leaves a black screen.",
                "Sì: il gioco gira nel processo principale dell'app. Per i telefoni Xiaomi, Redmi e POCO "
                + "(attiva di serie su di essi), il cui sistema chiude il processo del gioco e lascia lo "
                + "schermo nero.");
        it("On", "Sì");
        it("Off", "No");
        it("Game Data", "Dati del gioco");
        it("Get the game files (once)", "Ottieni i file del gioco (una volta)");
        it("1. In Dolphin on a computer, right-click New Super Mario Bros. Wii and choose Properties.\n"
                + "2. Open Filesystem, right-click the disc at the top and choose Extract Entire Disc.\n"
                + "3. Copy that folder (or a zip of it) to this phone.\n"
                + "4. Tap Import from Extracted Game Data Folder… or Import Game Data Zip… below and "
                + "choose it.\n\n"
                + "Use the USA disc, revision 1 (SMNE01): Dolphin shows the revision under Properties > Info.",
                "1. In Dolphin su un computer, fai clic destro su New Super Mario Bros. Wii e scegli "
                + "Proprietà.\n"
                + "2. Apri Filesystem, fai clic destro sul disco in alto e scegli Estrai intero disco.\n"
                + "3. Copia quella cartella (o un suo zip) su questo telefono.\n"
                + "4. Tocca Importa dalla cartella dei dati estratti… o Importa zip dei dati del gioco… qui "
                + "sotto e sceglila.\n\n"
                + "Usa il disco USA, revisione 1 (SMNE01): Dolphin mostra la revisione in Proprietà > Info.");
        it("Import your own New Super Mario Bros. Wii (USA, SMNE01) once. Easiest: the folder Dolphin's "
                + "Extract Entire Disc makes (DATA, or the folder that holds it), or a zip of that folder. "
                + "Your saves are kept.",
                "Importa una volta il tuo New Super Mario Bros. Wii (USA, SMNE01). Il modo più semplice: la "
                + "cartella creata da Estrai intero disco di Dolphin (DATA, o la cartella che la contiene), "
                + "o uno zip di quella cartella. I salvataggi vengono mantenuti.");
        it("Import from Extracted Game Data Folder…", "Importa dalla cartella dei dati estratti…");
        it("Import Game Data Zip…", "Importa zip dei dati del gioco…");
        it("Download Logs (For Github Reports)", "Scarica i log (per le segnalazioni su GitHub)");
        it("No game data yet. Import the folder Dolphin made, or a zip of it.",
                "Ancora nessun dato del gioco. Importa la cartella creata da Dolphin, o un suo zip.");
        it("The imported game can't be played. %s", "Il gioco importato non si può avviare. %s");
        it("Game data is installed and checked.", "I dati del gioco sono installati e verificati.");
        it("Starting the LAN game...", "Avvio della partita in LAN...");
        it("OK", "OK");
        it("Done", "Fatto");
        it("Back", "Indietro");
        it("Cancel", "Annulla");

        it("Checking for updates…", "Ricerca aggiornamenti…");
        it("Couldn't check for updates: %s", "Impossibile cercare aggiornamenti: %s");
        it("The latest release has no Android download.", "L'ultima versione non ha un download per Android.");
        it("You have the latest version (%s).", "Hai l'ultima versione (%s).");
        it("NSMBW %s is available", "NSMBW %s è disponibile");
        it("To install updates, let NSMBW install apps: turn on Allow from this source on the next screen, "
                + "then come back here.",
                "Per installare gli aggiornamenti, consenti a NSMBW di installare app: attiva Consenti da "
                + "questa fonte nella prossima schermata, poi torna qui.");
        it("Open Settings", "Apri impostazioni");
        it("Not Now", "Non ora");
        it("Downloading…", "Download…");
        it("Downloading… %d of %d MB", "Download… %d di %d MB");
        it("Updating to NSMBW %s", "Aggiornamento a NSMBW %s");
        it("The update failed: %s", "L'aggiornamento non è riuscito: %s");
        it("Installing NSMBW %s…", "Installazione di NSMBW %s…");

        it("Getting Started", "Primi passi");
        it("1. Import New Super Mario Bros. Wii", "1. Importa New Super Mario Bros. Wii");
        it("Use your own USA New Super Mario Bros. Wii (SMNE01, revision 1). Easiest: in Dolphin on a "
                + "computer, right-click the game, choose Properties, Filesystem, then right-click the disc and "
                + "choose Extract Entire Disc. Copy that folder (or a zip of it) to this phone and tap Import. "
                + "Dolphin shows the revision under Properties > Info.",
                "Usa il tuo New Super Mario Bros. Wii USA (SMNE01, revisione 1). Il modo più semplice: in "
                + "Dolphin su un computer, fai clic destro sul gioco, scegli Proprietà, Filesystem, poi fai "
                + "clic destro sul disco e scegli Estrai intero disco. Copia quella cartella (o un suo zip) "
                + "su questo telefono e tocca Importa. Dolphin mostra la revisione in Proprietà > Info.");
        it("2. Play", "2. Gioca");
        it("Tap Play. For LAN play, move up to the Couch button at the top right of the save file screen and "
                + "switch it to LAN; every player imports their own game.",
                "Tocca Gioca. Per giocare in LAN, sali fino al pulsante Locale in alto a destra nella "
                + "schermata dei file e passalo a LAN; ogni giocatore importa il proprio gioco.");
        it("Stuck on a step?", "Bloccato a un passaggio?");
        it("The GitHub page covers the supported game, the steps above and common problems.",
                "La pagina GitHub spiega il gioco supportato, i passaggi qui sopra e i problemi comuni.");
        it("Setup Guide on GitHub", "Guida all'installazione su GitHub");

        it("Saved, but there are no game logs yet: play once, then download them again.",
                "Salvato, ma non ci sono ancora log del gioco: gioca una volta, poi scaricali di nuovo.");
        it("Saved the logs of the last game session. Attach the zip to your GitHub issue.",
                "Salvati i log dell'ultima sessione di gioco. Allega lo zip alla tua segnalazione su GitHub.");
        it("Saved the logs of the last %d game sessions. Attach the zip to your GitHub issue.",
                "Salvati i log delle ultime %d sessioni di gioco. Allega lo zip alla tua segnalazione su "
                + "GitHub.");
        it("Could not save the logs: %s", "Impossibile salvare i log: %s");

        it("Looking at the game files…", "Controllo dei file del gioco…");
        it("Importing game data", "Importazione dei dati del gioco");
        it("%s\n\nNothing was changed.", "%s\n\nNon è stato cambiato nulla.");
        it("Import failed: %s", "Importazione non riuscita: %s");
        it("Imported %d files. Tap Play to start.", "Importati %d file. Tocca Gioca per iniziare.");
        it("%d of %d files", "%d di %d file");
        it("%d of %d MB", "%d di %d MB");
        it("%d files", "%d file");
        it("That folder is not an extracted game: it needs files/ and sys/ (or a DATA folder holding them), "
                + "as Dolphin's Extract Entire Disc makes.",
                "Quella cartella non è un gioco estratto: servono files/ e sys/ (o una cartella DATA che li "
                + "contiene), come quelli creati da Estrai intero disco di Dolphin.");
        it("That folder has no sys/boot.bin, so the game can't be identified.",
                "Quella cartella non ha sys/boot.bin, quindi il gioco non può essere identificato.");
        it("That folder's sys/boot.bin is too short, so the game can't be identified.",
                "Il sys/boot.bin di quella cartella è troppo corto, quindi il gioco non può essere "
                + "identificato.");
        it("That zip doesn't hold an extracted game: zip the folder Dolphin's Extract Entire Disc made (the "
                + "one with files and sys in it).",
                "Quello zip non contiene un gioco estratto: comprimi la cartella creata da Estrai intero "
                + "disco di Dolphin (quella con files e sys).");
        it("That isn't an extracted game: it needs the files and sys folders Dolphin's Extract Entire Disc "
                + "makes.",
                "Non è un gioco estratto: servono le cartelle files e sys create da Estrai intero disco di "
                + "Dolphin.");
        it("Its sys/boot.bin is too short, so the game can't be identified.",
                "Il suo sys/boot.bin è troppo corto, quindi il gioco non può essere identificato.");
        it("It has no sys/boot.bin, so the game can't be identified.",
                "Non ha sys/boot.bin, quindi il gioco non può essere identificato.");
        it("That is %s, not New Super Mario Bros. Wii for the USA (SMNE01). This port runs only the USA disc, "
                + "revision 1.",
                "Questo è %s, non New Super Mario Bros. Wii USA (SMNE01). Questo port funziona solo con il "
                + "disco USA, revisione 1.");
        it("That is New Super Mario Bros. Wii revision %d. This port runs only revision 1 (Dolphin shows it "
                + "under Properties > Info).",
                "Questo è New Super Mario Bros. Wii revisione %d. Questo port funziona solo con la revisione 1 "
                + "(Dolphin la mostra in Proprietà > Info).");

        it("Display", "Schermo");
        it("Controls", "Comandi");
        it("Aspect Ratio", "Formato");
        it("Render Resolution", "Risoluzione di rendering");
        it("Performance Monitor", "Monitor prestazioni");
        it("Spinning Coins", "Monete rotanti");
        it("V-Sync", "V-Sync");
        it("Graphics Compatibility", "Compatibilità grafica");
        it("Renderer", "Renderer");
        it("Automatic", "Automatico");
        it("Automatic (recommended): Vulkan, else OpenGL ES", "Automatico (consigliato): Vulkan, altrimenti OpenGL ES");
        it("OpenGL ES 3.1+ (in a later version)", "OpenGL ES 3.1+ (in una versione futura)");
        it("Auto", "Auto");
        it("Auto (recommended): on for the GPUs that need it", "Auto (consigliato): attiva per le GPU che ne hanno bisogno");
        it("On: always use the GPU fixes", "Sì: usa sempre le correzioni per la GPU");
        it("Off: never use them", "No: non usarle mai");
        it("Original 4:3", "4:3 originale");
        it("Fill Screen", "Riempi lo schermo");
        it("Match screen", "Come lo schermo");
        it("Touch Controls", "Comandi touch");
        it("Touch Control Opacity", "Opacità dei comandi touch");
        it("Tilt (turn the phone)", "Inclinazione (ruota il telefono)");
        it("Shake (jolt the phone)", "Scossa (scuoti il telefono)");
        it("Vibration", "Vibrazione");

        it("This phone", "Questo telefono");
        it("No network", "Nessuna rete");
        it("Rooms on your Wi-Fi are found by themselves. Over Tailscale, PCs find every device, and devices "
                + "that have played together find each other. To find another phone the first time, add its "
                + "Tailscale name or address here (or it adds yours).",
                "Le stanze sulla tua rete Wi-Fi si trovano da sole. Con Tailscale, i PC trovano ogni "
                + "dispositivo e i dispositivi che hanno già giocato insieme si ritrovano. Per trovare un "
                + "altro telefono la prima volta, aggiungi qui il suo nome o indirizzo Tailscale (o lui "
                + "aggiunge il tuo).");
        it("Devices asked for rooms", "Dispositivi a cui chiedere le stanze");
        it("None yet", "Ancora nessuno");
        it("Stop asking %s for rooms?", "Smettere di chiedere le stanze a %s?");
        it("Remove", "Rimuovi");
        it("Add Device…", "Aggiungi dispositivo…");
        it("Add a Device", "Aggiungi un dispositivo");
        it("Its Tailscale name (as in the Tailscale app) or address. It is asked for rooms from now on.",
                "Il suo nome Tailscale (come nell'app Tailscale) o il suo indirizzo. D'ora in poi gli verranno "
                + "chieste le stanze.");
        it("Add", "Aggiungi");
        it("e.g. s26-ultra or 100.84.140.32", "es. s26-ultra o 100.84.140.32");

        it("Start Menu", "Menu iniziale");
        it("Leave LAN Game", "Esci dalla partita LAN");
        it("Quit Game", "Esci dal gioco");
        it("Back to the Game", "Torna al gioco");
        it("Connect", "Connetti");
        it("To install updates, let NSMBW install apps: turn on Allow from this source on the next screen, "
                + "then come back to the game.",
                "Per installare gli aggiornamenti, consenti a NSMBW di installare app: attiva Consenti da "
                + "questa fonte nella prossima schermata, poi torna al gioco.");

        // --- the language pack (only a language the disc doesn't have shows these) ---
        it("Language Pack…", "Pacchetto lingua…");
        it("Language Pack", "Pacchetto lingua");
        it("A language pack is installed: the game's own text is in this language.",
                "È installato un pacchetto lingua: anche i testi del gioco sono in questa lingua.");
        it("The start menu and the port's menus are in this language. For the game's own text, import a "
                + "language pack zip made by a translator; until then it stays English.",
                "Il menu iniziale e i menu del port sono in questa lingua. Per i testi del gioco, importa lo "
                + "zip di un pacchetto lingua creato da un traduttore; fino ad allora restano in inglese.");
        it("Replace…", "Sostituisci…");
        it("Import Language Pack…", "Importa pacchetto lingua…");
        it("Importing the language pack", "Importazione del pacchetto lingua");
        it("That zip doesn't hold a language pack: it needs a Message folder, as in the disc's files/US/EngUS.",
                "Quello zip non contiene un pacchetto lingua: serve una cartella Message, come in "
                + "files/US/EngUS del disco.");
        it("The language pack is installed. The game uses it the next time it starts.",
                "Il pacchetto lingua è installato. Il gioco lo userà al prossimo avvio.");
    }
}

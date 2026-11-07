// ui_text.cpp - translations of the port's own menus. See ui_text.h.
#include "ui_text.h"

#include "runtime_config.h"

#include <atomic>
#include <unordered_map>

namespace UiText {
namespace {

// 0 English, 1 Italian. The disc's Spanish and French have no menu translations yet. The letters
// with a grave accent that no disc font has are made at boot (FontGraves).
std::atomic<int> g_language{-1};

int LanguageIndex(const std::string& code) {
    return code == "it" ? 1 : 0;
}

using Table = std::unordered_map<std::string_view, const char*>;

const Table& Italian() {
    static const Table table = {
        // the file select and its menus
        {"Settings", "Impostazioni"},
        {"Update", "Aggiorna"},
        {"Updating...", "Aggiornamento..."},
        {"Updating ", "Aggiornamento "},
        {"Installing...", "Installazione..."},
        {"Restarting...", "Riavvio..."},
        {"Retry Update", "Riprova"},
        {"Play:", "Gioco:"},
        {"Couch", "Locale"},
        {"LAN", "LAN"},
        {"OK", "OK"},
        // settings tabs, and the help line while the cursor is on them
        {"How the game is drawn and shown on your screen.", "Come il gioco viene disegnato e mostrato sullo schermo."},
        {"How loud the game's music and sounds are.", "Il volume della musica e dei suoni del gioco."},
        {"The on-screen controls and how tilting works.", "I controlli su schermo e come funziona l'inclinazione."},
        {"Which controller buttons play the Wii Remote's buttons.",
         "Quali pulsanti del controller fanno da pulsanti del Wii Remote."},
        {"Which keys play the Wii Remote's buttons.", "Quali tasti fanno da pulsanti del Wii Remote."},
        {"Optional helpers; LAN play always keeps the game's own rules.",
         "Aiuti facoltativi; in LAN valgono sempre le regole normali del gioco."},
        {"The language of the game's text and these menus.", "La lingua dei testi del gioco e di questi menu."},
        {"Video", "Video"},
        {"Audio", "Audio"},
        {"Controller", "Controller"},
        {"Keybinds", "Tasti"},
        {"Language", "Lingua"},
        {"Gameplay", "Partita"},
        {"Mods", "Mod"},
        {"Mods that change the game.", "Mod che cambiano il gioco."},
        {"Coming Soon!", "Prossimamente!"},
        // Gameplay
        {"No Time Limit", "Tempo infinito"},
        {"Infinite Lives", "Vite infinite"},
        {"The course timer stands still: take your time.", "Il tempo del livello si ferma: fai con calma."},
        {"A miss costs no life.", "Perdere non costa vite."},
        {"Power-Up", "Potenziamento"},
        {"Small", "Piccolo"},
        {"Super Mushroom", "Superfungo"},
        {"Fire Flower", "Fiore di fuoco"},
        {"Mini Mushroom", "Minifungo"},
        {"Propeller Mushroom", "Fungo elica"},
        {"Penguin Suit", "Tuta pinguino"},
        {"Ice Flower", "Fiore di ghiaccio"},
        {"Hammer Suit", "Tuta martello"},
        {"Star", "Stella"},
        {"For a 16:10 screen: fills it, with a slightly narrower view.", "Per uno schermo 16:10: lo riempie, con una visuale un po' più stretta."},
        {"Controls", "Controlli"},
        {"Edit Touch Layout", "Modifica layout touch"},
        {"Smart Spin", "Giro intelligente"},
        {"Move and resize the on-screen controls.", "Sposta e ridimensiona i controlli su schermo."},
        {"Tilting the phone the other way tilts the remote.", "Inclina il telecomando nel verso opposto al telefono."},
        {"Pressing 2 in the air does a spin.", "Premendo 2 in aria fai una rotazione."},
        {"Cheat Button", "Pulsante trucco"},
        {"An on-screen button that gives you the Power-Up.", "Un pulsante su schermo che ti dà il potenziamento."},
        {"P1 gets it once the game is on.", "G1 lo riceve appena inizia la partita."},
        {"Given to P1.", "Dato a G1."},
        {"Are you sure?", "Sei sicuro?"},
        {"This will mark your save as cheated.", "Il salvataggio verrà segnato come truccato."},
        {"Yes", "Sì"},
        {"No", "No"},
        {"Cheated", "Truccato"},
        {"Left and right pick one; press it to give it to P1.", "Sinistra e destra per scegliere; premi per darlo a G1."},
        // Video
        {"Resolution", "Risoluzione"},
        {"Native", "Nativa"},
        {"Ultrawide", "Ultrawide"},
        {"Fullscreen", "Schermo intero"},
        {"Off", "No"},
        {"On", "Sì"},
        {"Borderless", "Senza bordi"},
        {"Exclusive", "Esclusivo"},
        {"Renderer", "Renderer"},
        {"Automatic", "Automatico"},
        {"V-Sync", "V-Sync"},
        {"Performance Monitor", "Monitor prestazioni"},
        {"Spinning Coins", "Monete rotanti"},
        {"Depth Blur", "Sfocatura"},
        {"The blur and heat haze of desert, beach, water and lava areas.",
         "Sfocatura e foschia di deserto, spiaggia, acqua e lava."},
        {"Battery Saver", "Risparmio batteria"},
        {"Runs at 30FPS instead of 60FPS; pauses during LAN Play.",
         "Gira a 30FPS invece di 60FPS; si sospende nel gioco in LAN."},
        {"The screen's own resolution, filling the whole screen.",
         "La risoluzione dello schermo, a tutto schermo."},
        {"Rendered at this many lines, then scaled to the screen.",
         "Disegnato a queste righe, poi adattato allo schermo."},
        {"Fills the whole screen with a wider view.", "Riempie tutto lo schermo con una visuale più ampia."},
        {"16:9, with black bars at the sides.", "16:9, con bande nere ai lati."},
        {"Borderless: a window covering the whole screen (F11).",
         "Senza bordi: una finestra che copre tutto lo schermo (F11)."},
        {"Exclusive: the game takes over the display.", "Esclusivo: il gioco prende il controllo dello schermo."},
        {"Off: the game plays in a window.", "No: il gioco è in una finestra."},
        {"Picks the best graphics API this device has, from the next start (now %s).",
         "Sceglie la migliore API grafica del dispositivo, dal prossimo avvio (ora %s)."},
        {"Draws with this graphics API from the next start (now %s).",
         "Disegna con questa API grafica dal prossimo avvio (ora %s)."},
        {"Shows the FPS and each frame's time at the top right; left and right set its size.",
         "Mostra FPS e tempo di ogni frame in alto a destra; sinistra e destra ne cambiano la dimensione."},
        {"Animates coins and blocks; off saves a little GPU work.",
         "Anima monete e blocchi; disattivato alleggerisce un po' la GPU."},
        {"No tearing: frames wait for the display, the game keeps its pace.",
         "Niente tearing: i frame aspettano lo schermo, il gioco mantiene il ritmo."},
        // Audio
        {"Master", "Generale"},
        {"Music", "Musica"},
        {"Sound Effects", "Effetti sonori"},
        {"The volume of everything.", "Il volume di tutto."},
        {"The background music.", "La musica di sottofondo."},
        {"Jumps, coins, enemies and the rest.", "Salti, monete, nemici e il resto."},
        {"Menu Sounds", "Suoni dei menu"},
        {"The menus' cursor, select and back sounds.", "Cursore, scelta e indietro dei menu."},
        // Keybinds / Controller
        {"Up", "Su"},
        {"Down", "Giù"},
        {"Left", "Sinistra"},
        {"Right", "Destra"},
        {"Jump (2)", "Salta (2)"},
        {"Run / Fire (1)", "Corri / Fuoco (1)"},
        {"Shake (Spin)", "Scuoti (Giro)"},
        {"Tilt Left", "Inclina a sin."},
        {"Tilt Right", "Inclina a des."},
        {"+ (Pause)", "+ (Pausa)"},
        {"- (Minus)", "- (Meno)"},
        {"None", "Nessuno"},
        {"Right stick left", "Levetta des. a sin."},
        {"Right stick right", "Levetta des. a des."},
        {"Press a button...", "Premi un tasto..."},
        {"Press a key...", "Premi un tasto..."},
        {"Press the new key (Esc cancels).", "Premi il nuovo tasto (Esc annulla)."},
        {"Press the new key or mouse button (Esc cancels).",
         "Premi il nuovo tasto o pulsante del mouse (Esc annulla)."},
        {"Press the new button (Esc cancels).", "Premi il nuovo pulsante (Esc annulla)."},
        {"Hold a button, or 2 or 3 together, then let go. Esc cancels.",
         "Tieni un pulsante, o 2 o 3 insieme, poi lascia. Esc annulla."},
        {"Hold a key, or 2 or 3 together, then let go. Esc cancels, Delete clears.",
         "Tieni un tasto, o 2 o 3 insieme, poi lascia. Esc annulla, Canc cancella."},
        {"Space Smart Spin", "Giro intelligente"},
        {"Reverse Tilt", "Inverti inclinazione"},
        {"Touch Controls", "Comandi touch"},
        {"Mouse as Gyro", "Mouse giroscopio"},
        {"Reset Defaults", "Ripristina"},
        {"The D-pad's up: climb vines, and go through doors and pipes.",
         "Su della croce: arrampicati e passa da porte e tubi."},
        {"The D-pad's down: crouch, and ground-pound in the air.", "Giù della croce: abbassati, e schianto a terra in aria."},
        {"The D-pad's left: walk left, and move a menu's cursor.",
         "Sinistra della croce: cammina a sinistra e muovi il cursore dei menu."},
        {"The D-pad's right: walk right, and move a menu's cursor.",
         "Destra della croce: cammina a destra e muovi il cursore dei menu."},
        {"The 2 button: jump and swim, and choose in menus.", "Il pulsante 2: salta e nuota, e scegli nei menu."},
        {"The 1 button: run, throw fireballs, and go back in menus.",
         "Il pulsante 1: corri, lancia palle di fuoco e torna indietro nei menu."},
        {"The A button: view the whole world map.", "Il pulsante A: guarda tutta la mappa del mondo."},
        {"Shakes the Wii Remote: a spin jump, or picking up and throwing.",
         "Scuote il Wii Remote: salto rotante, o prendere e lanciare."},
        {"Tilts the Wii Remote left, for the platforms that tilt.",
         "Inclina il Wii Remote a sinistra, per le piattaforme inclinabili."},
        {"Tilts the Wii Remote right, for the platforms that tilt.",
         "Inclina il Wii Remote a destra, per le piattaforme inclinabili."},
        {"The + button: pause a course, or open the world map's menu.",
         "Il pulsante +: metti in pausa un livello o apri il menu della mappa."},
        {"The - button: open Select a World on the world map.", "Il pulsante -: apri la scelta del mondo sulla mappa."},
        {"With Space as a Jump key, Space in the air does a spin.",
         "Se Spazio è un tasto Salta, Spazio in aria fa una rotazione."},
        {"Platforms you tilt lean the way the stick or phone goes.",
         "Le piattaforme si inclinano dove va la levetta o il telefono."},
        {"Shows the on-screen Wii Remote, for playing by touch.", "Mostra il Wii Remote su schermo, per giocare col touch."},
        {"Moving the mouse tilts the remote; moving it quickly shakes it.",
         "Muovere il mouse inclina il telecomando; muoverlo veloce lo scuote."},
        {"Puts every controller button back the way it started.", "Riporta ogni pulsante del controller com'era all'inizio."},
        {"Puts every key back the way it started.", "Riporta ogni tasto com'era all'inizio."},
        // Language
        {"Shows the game's text and these menus in %s.", "Mostra i testi del gioco e questi menu in %s."},
        {"The game's own text needs an Italian language pack.",
         "Per i testi del gioco serve un pacchetto della lingua italiana."},
        // LAN
        {"Join a room on another device,", "Entra in una stanza su un altro dispositivo"},
        {"or create one here.", "o creane una qui."},
        {"Tailscale works, but is unstable.", "Tailscale funziona, ma è instabile."},
        {"Join", "Entra"},
        {"Create Room", "Crea stanza"},
        {"(1) Back", "(1) Indietro"},
        {"Pick the save for your room.", "Scegli il salvataggio per la stanza."},
        {"A LAN room needs 2 to 4 players.", "Una stanza LAN richiede da 2 a 4 giocatori."},
        {"How many can join your room?", "Quanti possono entrare nella stanza?"},
        {"Rooms", "Stanze"},
        {"Looking for rooms", "Ricerca stanze"},
        {"Rooms on your Wi-Fi and Tailscale", "Le stanze sulla tua rete Wi-Fi e Tailscale"},
        {"show up here.", "compaiono qui."},
        {"Room", "Stanza"},
        {"  (you)", "  (tu)"},
        {"P  waiting...", "P  in attesa..."},
        {"Starting...", "Avvio..."},
        {"Joining...", "Accesso..."},
        {"Others pick LAN > Join on their device.", "Gli altri scelgono LAN > Entra sul loro dispositivo."},
        {"Start when everyone is here.", "Avvia quando ci sono tutti."},
        {"Waiting for the host to start...", "In attesa che l'host avvii..."},
        {"Start", "Avvia"},
        {"Direct Connection", "Connessione diretta"},
        {"IP:Port", "IP:Porta"},
        {"Asking ", "Richiesta a "},
        {"The host's IP:Port, shown at the top of its room.", "L'IP:Porta dell'host, in cima alla sua stanza."},
        {"Enter connects, Esc cancels.", "Invio connette, Esc annulla."},
        {"No room answered at that address.", "Nessuna stanza ha risposto a quell'indirizzo."},
        {"That isn't an address.", "Questo non è un indirizzo."},
        {"Cancel", "Annulla"},
        {"Leave", "Esci"},
        {"Lost the connection to", "Connessione persa con"},
        {"Waiting for ", "In attesa di "},
        {"player ", "giocatore "},
        {"The game got out of sync between devices.", "Il gioco non è più sincronizzato tra i dispositivi."},
        {"Something went wrong.", "Qualcosa è andato storto."},
        {"Couldn't start the session on this device.", "Impossibile avviare la sessione su questo dispositivo."},
        // LAN messages (netplay_lobby.cpp)
        {"A player didn't answer. Try starting again.", "Un giocatore non ha risposto. Riprova ad avviare."},
        {"Another program is using the LAN play port (UDP 52130).",
         "Un altro programma usa la porta del gioco in LAN (UDP 52130)."},
        {"Couldn't open a network socket.", "Impossibile aprire un socket di rete."},
        {"Getting the host's save...", "Ricezione del salvataggio dell'host..."},
        {"Lost the connection to the room.", "Connessione con la stanza persa."},
        {"Sending your save to everyone...", "Invio del salvataggio a tutti..."},
        {"That room has already started.", "Quella stanza è già partita."},
        {"That room is full.", "Quella stanza è piena."},
        {"The host closed the room.", "L'host ha chiuso la stanza."},
        {"The room didn't answer.", "La stanza non ha risposto."},
        {"The save is too big to send.", "Il salvataggio è troppo grande da inviare."},
        {"Waiting for everyone...", "In attesa di tutti..."},
        {"You are no longer in the room.", "Non sei più nella stanza."},
        // updater
        {"The download was damaged. Try again.", "Il download è danneggiato. Riprova."},
        {"The game's folder can't be written to.", "Impossibile scrivere nella cartella del gioco."},
        {"Update from the app's start screen instead.", "Aggiorna dalla schermata iniziale dell'app."},
    };
    return table;
}

} // namespace

void SetLanguage(const std::string& code) {
    g_language.store(LanguageIndex(code), std::memory_order_relaxed);
}

std::string Tr(std::string_view english) {
    int language = g_language.load(std::memory_order_relaxed);
    if (language < 0) {
        language = LanguageIndex(RuntimeConfigFile::Language());
        g_language.store(language, std::memory_order_relaxed);
    }
    if (language == 1) {
        const Table& table = Italian();
        if (const auto it = table.find(english); it != table.end()) {
            return it->second;
        }
    }
    return std::string(english);
}

} // namespace UiText

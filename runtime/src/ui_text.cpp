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
        // settings tabs
        {"Video", "Video"},
        {"Audio", "Audio"},
        {"Controller", "Controller"},
        {"Keybinds", "Tasti"},
        {"Language", "Lingua"},
        {"Gameplay", "Partita"},
        // Gameplay
        {"No Time Limit", "Tempo infinito"},
        {"Infinite Lives", "Vite infinite"},
        {"The course timer stands still: take your time.", "Il tempo del livello si ferma: fai con calma."},
        {"A miss costs no life.", "Perdere non costa vite."},
        {"These keep the game's own rules in LAN play.", "In LAN valgono le regole normali del gioco."},
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
        {"The best this device has. ", "Il migliore per questo dispositivo. "},
        {"Takes effect at the next start. Now: ", "Si applica al prossimo avvio. Ora: "},
        {"FPS and each frame's time, top right. Left and right: its size.",
         "FPS e tempo di ogni frame, in alto a destra. Sinistra e destra: dimensione."},
        {"Animated coins and blocks. Off saves a little GPU work.",
         "Monete e blocchi animati. Disattivato alleggerisce la GPU."},
        {"No tearing: frames wait for the display, the game keeps its pace.",
         "Niente tearing: i frame aspettano lo schermo, il gioco mantiene il ritmo."},
        // Audio
        {"Master", "Generale"},
        {"Music", "Musica"},
        {"Sound Effects", "Effetti sonori"},
        {"The volume of everything.", "Il volume di tutto."},
        {"The background music.", "La musica di sottofondo."},
        {"Jumps, coins, enemies and the rest.", "Salti, monete, nemici e il resto."},
        {"Left and right change the selected volume.", "Sinistra e destra cambiano il volume scelto."},
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
        {"Space Smart Spin", "Giro intelligente"},
        {"Reverse Tilt", "Inverti inclinazione"},
        {"Touch Controls", "Comandi touch"},
        {"Mouse as Gyro", "Mouse giroscopio"},
        {"Reset Defaults", "Ripristina"},
        // Language
        {"The language of the game's text and menus.", "La lingua dei testi e dei menu del gioco."},
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

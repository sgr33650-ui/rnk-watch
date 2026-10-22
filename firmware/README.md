# RNK Runtime 0.2 (T-Watch 2020 V3)

Firmware qui affiche les cadrans dessinés dans l'Atelier RNK.
Il n'a pas été compilé ni testé sur une vraie montre. Le dessin a été vérifié sur ordinateur
avec un simulateur de l'écran, mais pas le matériel (veille, toucher, bouton).

## Installer
1. Installer PlatformIO (extension VS Code ou `pip install platformio`).
2. Ouvrir ce dossier, brancher la montre en USB, lancer `pio run -t upload`.
3. Si la compilation échoue sur la version de la plateforme, changer `platform` dans
   `platformio.ini` (essayer `espressif32@^6`) et le signaler.

## Utiliser
1. Ouvrir l'Atelier, onglet Montre, Connecter en USB (Chrome ou Edge sur ordinateur).
2. Tester, Régler l'heure, puis Envoyer le projet.
3. Les cadrans sont gardés en mémoire flash (`/layout.json`) et relus au démarrage.

## Commandes
- Balayage gauche vers droite : cadran suivant. Balayage droite vers gauche : applications.
- Bouton de la montre : applications / retour au cadran.
- Appui sur une icône : ouvre la page cible ou lance l'action.
- Veille : extinction après 20 s d'inactivité, sur batterie seulement.
  Réveil par le bouton ou par un toucher. Branchée en USB, la montre reste allumée.

## Protocole série (115200 bauds)
    RNK:PING                       -> RNK:PONG rnk-runtime-0.3
    RNK:INFO                       -> batterie, pages, mémoire
    RNK:TIME a m j h mi s          -> règle l'horloge
    RNK:BEGIN <octets> <crc32>     -> RNK:READY, puis les octets, puis "\nRNK:END\n"
                                   -> RNK:OK <pages> ou RNK:ERR <raison>

## Bluetooth et téléphone (nouveau en 0.2)
La montre s'annonce en Bluetooth sous le nom « RNK Watch ». La page `rnk-compagnon.html`
(Chrome sur Android ou ordinateur, page en https) s'y connecte et envoie la vitesse, la distance,
la durée, les cryptos et l'heure. Le Bluetooth est coupé quand l'écran s'éteint et revient au réveil.
Lignes reçues, terminées par \n, champs séparés par `|` :

    S|vitesse km/h|distance km|durée s|moyenne km/h
    C|SYMBOLE|prix|variation
    M|artiste|titre|position s|durée s|lecture 0/1
    T|année|mois|jour|heure|min|sec
La montre répond `E|action` quand on appuie sur une icône (play, next, prev, voice, bt, vibe).

## Limites de cette version
- Polices : les polices Free de TFT_eSPI, sans accents (JOURS et MOIS sont en capitales sans accent).
  Le rendu diffère un peu de l'aperçu, qui utilise Barlow Condensed.
- Pas de dégradés, de textures ni de vignette. Le cadre est dessiné en aplats.
- Images importées : non affichées (un contour à la place).
- Spotify et assistant vocal : la montre sait afficher un morceau (ligne `M|…`) et envoyer les
  actions play, next, prev, voice, mais la page compagnon ne les relie pas encore à une application.
- Sans connexion au téléphone depuis plus de 15 s, vitesse, distance, durée et morceau affichent « -- ».
- iPhone : le Bluetooth web n'y fonctionne pas.
- Rotation et opacité des éléments : ignorées.

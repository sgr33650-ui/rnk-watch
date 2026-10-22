# RNK Watch

Atelier de cadrans rétro-futuristes pour la **LilyGO T-Watch 2020 V3** (écran carré 240 × 240) : on dessine le cadran dans le navigateur, on l'envoie à la montre en USB, et le téléphone lui fournit ses données en Bluetooth.

| Dossier | Contenu |
| --- | --- |
| `docs/index.html` | **Atelier RNK** : éditeur de cadrans, installable comme une application (PWA) |
| `docs/compagnon.html` | **Page compagnon** : vitesse GPS, cryptos, musique, heure vers la montre (Bluetooth) |
| `firmware/` | **RNK Runtime** : firmware de la montre (PlatformIO) |
| `.github/workflows/` | Publication automatique du site et compilation du firmware |

## Mettre l'application en ligne (une seule fois)

1. Sur GitHub, ouvre **Settings → Pages**.
2. Dans **Build and deployment → Source**, choisis **GitHub Actions**.
3. Va dans l'onglet **Actions** : le workflow « Publier l'application » se lance à chaque modification de `docs/`.
4. L'adresse est `https://<ton-compte>.github.io/<nom-du-depot>/`.

## Installer l'Atelier comme une application

- **Ordinateur (Chrome ou Edge)** : ouvre l'adresse, puis l'icône « Installer » dans la barre d'adresse.
- **Android (Chrome)** : menu ⋮ → *Installer l'application*.

L'Atelier fonctionne ensuite hors ligne. L'envoi à la montre en USB demande Chrome ou Edge sur ordinateur ; la page compagnon demande Chrome sur Android (le Bluetooth web n'existe pas sur iPhone).

## Firmware

```
cd firmware
pio run -t upload
```

Le workflow **Compiler le firmware** produit aussi un fichier `firmware.bin` (onglet Actions → artefact, ou joint à une *release*). Le firmware n'a pas encore été essayé sur une vraie montre par l'auteur de ce dépôt : commence par un essai sur table.

## Images et logos

Dans l'Atelier, une image importée peut être rendue transparente (fond retiré), recolorée (une couleur ou teinte), et sa couleur peut changer automatiquement (dégradé ou alternance de 3 couleurs, avec le rythme en secondes). La montre affiche ces images avec les mêmes réglages.

## Limites connues

- Pas de dégradés ni de textures sur la montre (ils restent dans l'aperçu).
- Polices simples sans accents sur la montre.
- Rotation et opacité d'un élément ignorées sur la montre.
- Spotify et commande vocale ne sont pas branchés.

# Compatibilité Odin 3 : aPS3e (PS3)

Notre propre liste de compatibilité, tenue d'après nos tests sur l'AYN Odin 3 (Snapdragon 8 Elite, Adreno 830) avec notre build de la branche `odin3`. Chaque test met à jour la ligne du jeu : statut, mesure, date et commit testé.

Autres émulateurs : [Eden (Switch)](https://github.com/HamzaDevz/eden/blob/odin3/ODIN3_COMPAT.md) · [Azahar (3DS)](https://github.com/HamzaDevz/azahar/blob/odin3/ODIN3_COMPAT.md) · [Dolphin (GC/Wii)](https://github.com/HamzaDevz/dolphin/blob/odin3/ODIN3_COMPAT.md) · [Cemu (Wii U)](https://github.com/HamzaDevz/Cemu/blob/odin3/ODIN3_COMPAT.md) · [Vita3K (Vita)](https://github.com/HamzaDevz/Vita3K/blob/odin3/ODIN3_COMPAT.md) · [aPS3e (PS3)](https://github.com/HamzaDevz/aps3e/blob/odin3/ODIN3_COMPAT.md) · [Bachata S4 (PS4)](https://github.com/HamzaDevz/bachata-s4/blob/odin3/ODIN3_COMPAT.md) · [xemu (Xbox)](https://github.com/HamzaDevz/xemu/blob/odin3/ODIN3_COMPAT.md)

## Échelle

| Statut | Sens |
|---|---|
| 🟢 Parfait | vu en partie, plein régime, aucun défaut vu |
| 🔵 Jouable | vu en partie, défauts mineurs |
| 🟡 Problèmes | vu en partie, défauts gênants (fps, graphismes, son) |
| 🟠 Menus/intro | démarre, pas encore vu en partie |
| 🔴 Ne marche pas | crash, écran noir ou blocage |
| ⚪ Non testé | présent sur la console, jamais lancé |

## Comment on teste

Paquet `aenu.aps3e.odin3` (version 2.43-odin3), installé à côté de l'officiel `aenu.aps3e`. Banc adb dans `tools/odin3/` : `run.sh <nom> [Section/Clé=valeur]` avec un scénario, sous le verrou partagé de la console ; fps par SurfaceFlinger (présentations, pas images du jeu), puissance et températures par `poll.sh`, captures toutes les 10 s et journal RPCS3 par `probe.sh`.

La page « Game Compatibility Table » de l'app reprend la liste de RPCS3 sur PC (`app/src/main/assets/compatibility_table.json`), pas nos tests : la nôtre est ici.

## Jeux

| Jeu | Statut | Vu jusqu'à | Mesure | Notes | Testé le | Build |
|---|---|---|---|---|---|---|
| God of War: Ascension (USA) `BCUS98232` | 🔴 Ne marche pas | écran noir après l'intro | | correctif (config de la base RPCS3 appliquée au boot + RSX Idle Sleep 100 µs) commité mais jamais lancé sur l'Odin ; sur l'officiel avec la config recommandée : écran de luminosité atteint, 47-56 fps, 14-20 W, CPU 104-105 °C (thread RSX à 97 %) | 2026-10-17 | builds avant 719f52bba |
| God of War: Ascension (EU) `BCES01741` | ⚪ Non testé | | | | | |
| Kingdom Hearts HD 2.5 ReMIX | ⚪ Non testé | | | | | |
| Metal Gear Solid 4 | ⚪ Non testé | | | | | |
| Syberia | ⚪ Non testé | | | | | |

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

Banc VM : odin3-lab `emus/aps3e`, le cœur RPCS3 de l'APK compilé pour linux-aarch64 (mêmes recompilateurs PPU/SPU LLVM, en-têtes Vulkan du NDK) sur lavapipe au profil Vulkan de l'Odin (RPCS3 y voit l'Adreno 830), avec la config, le firmware et les caches de l'Odin ; validé contre l'Odin le 2026-10-22 sur God of War: Ascension (mêmes images de l'intro à 61-70 dB, même blocage).

La page « Game Compatibility Table » de l'app reprend la liste de RPCS3 sur PC (`app/src/main/assets/compatibility_table.json`), pas nos tests : la nôtre est ici.

## Jeux

| Jeu | Statut | Vu jusqu'à | Mesure | Notes | Testé le | Build |
|---|---|---|---|---|---|---|
| God of War: Ascension (USA) `BCUS98232` | 🟠 Menus/intro | écran de luminosité (premier menu) | 60 présentations/s du logo Santa Monica à l'écran de luminosité (2400 images, 3 essais sur 3, odin3-lab run-dev) ; le jeu 1.00 est bridé à 30 fps | SPU MLAA coupé par défaut (patch RPCS3 « Disable SPU MLAA », Whatcookie) : la tâche SPU MLAA ne libérait jamais le label RSX 0x50300510 (`nv406e::semaphore_acquire` expiré, écran noir définitif après le logo Santa Monica, 2 essais sur 2 avant) ; patch.yml enfin installé (l'amont le copiait sur lui-même : aucun patch ne s'appliquait) ; le premier lancement juste après l'installation a encore bloqué une fois ; 60 fps : patch « Unlock FPS » seulement pour la mise à jour 1.04, absente de l'Odin | 2026-10-28 | 6237a4477 |
| God of War: Ascension (EU) `BCES01741` | ⚪ Non testé | | | | | |
| Kingdom Hearts HD 2.5 ReMIX | ⚪ Non testé | | | | | |
| Metal Gear Solid 4 | ⚪ Non testé | | | | | |
| Syberia | ⚪ Non testé | | | | | |

# ArxCoop — Arx Fatalis en coopération (2 à 4 joueurs)

[![Dernière version](https://img.shields.io/github/v/release/Parricidium/ArxCoop?label=T%C3%A9l%C3%A9charger&style=for-the-badge)](https://github.com/Parricidium/ArxCoop/releases/latest)
[![Licence GPLv3](https://img.shields.io/badge/licence-GPLv3-blue?style=for-the-badge)](COPYING)

Mod coopératif pour **Arx Fatalis**, construit sur le moteur libre [Arx Libertatis](https://arx-libertatis.org/).
Un joueur héberge, les autres le rejoignent : tout le monde joue l'aventure ensemble, dans le même monde,
chacun avec son propre personnage. Français et anglais.

*[English summary below.](#english)*

> Vous devez posséder Arx Fatalis (GOG ou Steam). Aucune donnée du jeu n'est fournie ici.

---

## Sommaire

- [Téléchargement et installation](#téléchargement-et-installation)
- [Jouer](#jouer)
- [Fonctionnalités](#fonctionnalités)
- [État des synchronisations](#état-des-synchronisations)
- [Changer son visage (tuto)](#changer-son-visage-tuto)
- [Personnaliser les menus](#personnaliser-les-menus)
- [Menu Administration (F8)](#menu-administration-f8)
- [Touches](#touches)
- [Limites connues](#limites-connues)
- [Compiler](#compiler)
- [Crédits et licence](#crédits-et-licence)

---

## Téléchargement et installation

1. Téléchargez le zip de la [dernière version](https://github.com/Parricidium/ArxCoop/releases/latest).
2. Dézippez-le **dans le dossier d'Arx Fatalis** (celui qui contient `data.pak`).
   Rien n'est écrasé : `arx.exe` est un exécutable en plus, le jeu d'origine reste intact.
3. Au premier lancement, Windows demandera peut-être l'autorisation réseau : acceptez (réseau privé).

Le mod range **tout** (sauvegardes, configuration, visages) dans `userdata\` à côté de `arx.exe`,
jamais dans « Parties enregistrées ». Vos sauvegardes du jeu d'origine ne risquent rien.

**Mise à jour** : dézippez la nouvelle version par-dessus. Tout le monde doit avoir la même version
(le numéro de protocole est vérifié à la connexion).

**Langue** : si le jeu est en anglais (Options > Language), les menus et messages du mod le sont aussi.

## Jouer

| Rôle | Comment |
|---|---|
| **Héberger** | `heberger.cmd`, ou menu *Coopération > Héberger une partie* (pseudo, port), ou `arx.exe --coop-host 27015 --nickname MonPseudo` |
| **Rejoindre** | `rejoindre.cmd`, ou menu *Coopération > Rejoindre une partie* (pseudo, adresse, port, favoris), ou `arx.exe --coop-join 192.168.1.10:27015 --nickname MonPseudo` |

- Même réseau local ou VPN (Radmin VPN, Hamachi, ZeroTier). Par internet, l'hôte ouvre le port **TCP 27015**.
  La page *Héberger* affiche votre IP locale et votre IP internet, à donner à vos amis.
- Tout le monde arrive dans le **salon**. L'hôte y a deux boutons :
  **« Continuer : \<dernière sauvegarde\> »** (chacun recharge automatiquement son personnage « coop: … »,
  ou en crée un s'il est nouveau) et **« Commencer du début »** (chacun crée son personnage).
- On peut **rejoindre une partie en cours** : le nouveau venu crée son personnage (ou recharge le sien)
  et reçoit le niveau, les quêtes, clés, runes, sacs et l'XP du groupe.
- **Sauvegarde** : seul l'hôte sauvegarde (F5 ou menu). Chaque autre joueur sauvegarde alors son
  personnage sous « coop: \<nom\> ». Quand l'hôte charge une sauvegarde, chacun recharge la sienne.
- **Déconnexion** : celui qui plante relance *Rejoindre* : il revient avec son personnage
  (sauvegarde automatique toutes les 3 min) et l'hôte le remet là où il était.

## Fonctionnalités

**Monde partagé**
- Le monde appartient à l'hôte et est vu par tous : PNJ (position, animations, vie, démembrements, sang),
  portes, leviers, pièges, coffres, marchands, objets au sol, quêtes, clés, runes, variables des scripts.
- Ce qu'un joueur déclenche (dialogue, laissez-passer, passage dégagé, levier…) vaut pour tout le monde.
- Les PNJ ciblent le joueur le plus proche ; les dégâts que vous prenez sont bien les vôtres.
- Objets : ce qu'un joueur ramasse disparaît pour les autres ; ce qu'il pose ou jette réapparaît chez eux.
  Coffres et marchands sont communs (dépôt, vente, achat, piles, pièces d'or).
- Chaque joueur garde **son** personnage : stats, points, inventaire, équipement, bourse. L'or et l'XP
  donnés par les quêtes vont à tout le monde ; l'XP des combats est partagée.
- Un sac à dos utilisé par n'importe qui agrandit l'inventaire de tout le monde.
- Les sons du monde de l'hôte (coups, impacts, PNJ) sont entendus par tous.

**Les autres joueurs**
- Vus avec leur tête (ou leur **visage personnalisé**), leur armure, leur arme, leur torche, leurs
  animations (course, saut, accroupi, combat, sorts, penchés, tête qui parle).
- Sorts des coéquipiers visibles (dégâts calculés chez le lanceur).
- **Donner un objet** : clic droit sur l'objet dans l'inventaire, puis clic sur le coéquipier.
- Pseudo, vie et faim des coéquipiers à gauche de l'écran ; ping à côté des noms ; temps de jeu en haut.
- Coéquipiers hors champ : flèche au bord de l'écran avec la distance, présence sur la mini-carte et la carte du livre.
- **Clic molette** : marqueur « par ici » (double cercle rouge avec la distance, 12 s, visible par tous).

**Mort et réanimation**
- À 0 PV on reste **à terre** (« X est à terre ! » + signal sonore) avec un compte à rebours de 2 minutes.
- Un coéquipier maintient **clic gauche** en vous regardant de près (~2 m) pendant 4 s pour vous relever
  (jauge affichée), ou utilise une **potion de vie** (touche H) en vous regardant : relevé instantané.
- Tout le monde à terre = fin de partie.

**Cinématiques et dialogues**
- La caméra d'un dialogue n'est donnée qu'à celui qui l'a déclenché ; les autres entendent la réplique.
  Un Échap de n'importe qui passe la réplique pour tous.
- Option « me figer quand un coéquipier dialogue ».
- À la fin d'une scène qui a déplacé l'hôte (intro, Polsius…) ou d'un changement de niveau, les
  coéquipiers restés loin sont téléportés à côté de lui.
- Le menu de l'hôte ne met pas le monde en pause.

**Vue à la 3e personne**
- **V** : vue subjective / caméra par-dessus l'épaule. **N** : changer d'épaule. **O** : caméra libre
  (la souris tourne autour du personnage). **Molette** : rapprocher / éloigner. La caméra se rapproche
  toute seule devant un mur.

**Confort**
- Le jeu démarre directement sur le menu (intro passée ; `skip_intro=false` dans `userdata\cfg.ini` pour la retrouver).
- Habillage des menus fourni (fond « Arx Fatalis COOP », cadre, panneau) et remplaçable par le vôtre.
- Traduction anglaise complète du mod.

## État des synchronisations

| Élément | État | Remarque |
|---|:---:|---|
| Position, angle, animations des joueurs (4 couches) | ✅ | 20 Hz, lissage |
| Équipement visible (armure, arme, bouclier, torche), tête, visage | ✅ | |
| PNJ : position, animations, vie, mort, visibilité, démembrements | ✅ | IA et physique chez l'hôte, 10 Hz |
| Dégâts PNJ → joueurs, joueurs → PNJ, joueur ↔ joueur | ✅ | via l'hôte |
| Sang, sons du monde | ✅ | |
| Scripts du monde (portes, leviers, pièges, passages, `objecthide/destroy/…`) | ✅ | rejoués chez les clients |
| Variables des scripts (flags de quête) | ✅ | dans les deux sens |
| Journal de quêtes, clés, runes | ✅ | + envoyés à qui rejoint |
| XP, or des quêtes, sacs à dos | ✅ | XP partagée ; rattrapage à l'arrivée |
| Or ramassé / vente | ✅ | reste à celui qui le fait (bourse individuelle) |
| Objets au sol (prise, dépôt, lancer, transport) | ✅ | |
| Coffres, marchands, cadavres (dépôt, vente, achat, piles, pièces) | ✅ | |
| Objets créés par script dans un conteneur | ✅ | même identifiant partout |
| Don d'objet entre joueurs | ✅ | durabilité et poison conservés |
| Sorts | ✅ | visuels partout, dégâts chez le lanceur |
| Champs magiques persistants (« murs bleus ») | ✅ | recréés à l'arrivée |
| Changement de niveau | ✅ | groupé, mené par l'hôte |
| Sauvegarde / chargement | ✅ | hôte + perso de chacun |
| Rejoindre en cours de partie, reconnexion | ✅ | |
| Cinématiques scriptées | ✅ | caméra pour l'initiateur seulement |
| Marqueurs, chat, ping | ✅ | |
| Latence internet réelle | ⚠️ | conçu pour le LAN / VPN, peu testé au-delà |
| Caméra des cinématiques pour tous | ❌ | choix de conception |

## Changer son visage (tuto)

Votre visage remplace celui du héros : les autres joueurs le voient sur votre personnage, et vous le
voyez dans le livre.

1. Lancez le mod, allez dans **Options > Coopération**.
2. Cliquez **« Ouvrir le dossier des visages »** : le dossier `userdata\coop\faces\` est créé et s'ouvre.
3. Déposez-y votre photo (`jpg` ou `png`). Cadrage **serré sur le visage, de face** : du haut du front
   au menton, sans les cheveux ni les épaules (voir `visages\cadrage_photo.png` : tout ce qui est dans
   l'ovale vert est collé sur le visage, le reste est fondu avec la tête du jeu). Le jeu recadre en 5:7
   et réduit la photo tout seul ; une photo trop large donnera un visage minuscule.
4. Revenez sur la page *Coopération* et choisissez votre fichier dans **« Visage »** : la tête du
   personnage s'affiche en 3D dessous, avec votre visage.
5. C'est tout. La photo est envoyée une fois aux autres joueurs (20 à 40 Ko), et nulle part ailleurs.

Pour les bricoleurs : une **tête complète**. Éditez `visages\tete_hero_N_skin.png` (N = 1 à 4 ;
`*_zones.png` montre où tombe le visage) et enregistrez-la dans le dossier des visages avec `_skin`
dans le nom (ex. `bob_skin.png`) : elle est utilisée telle quelle (tête nue ; avec cotte de mailles
ou capuche, la tête du jeu revient).

## Personnaliser les menus

Trois images dans `userdata\coop\` (png, jpg ou bmp ; supprimer le fichier = retour au jeu d'origine) :

| Fichier | Rôle | Format |
|---|---|---|
| `menu_background.*` | fond du menu principal (remplace l'image d'origine, logo compris) | 16:9 conseillé, étiré à l'écran |
| `menu_panel.*` | fond du cadre des menus — il **assombrit** ce qu'il y a derrière (blanc = rien, noir = opaque) | 321×419 |
| `menu_border.*` | bordure du cadre | 322×424, PNG avec transparence (ou noir = transparent) |

« Ouvrir le dossier des visages » exporte aussi les images d'origine dans `userdata\coop\menu_modeles\`.
Le mod est livré avec son propre habillage ; la mise à jour le remet.

## Menu Administration (F8)

Ouvert en jeu (le monde continue de tourner), touche modifiable dans *Options > Commandes*.

- **Hôte** : choisir un joueur puis *le téléporter à moi*, *me téléporter à lui*, *le relever et soigner*,
  lui donner *de l'or*, *de l'XP*, *un objet* (nom de classe : `potion_life`, `short_sword`, `gold_coin`,
  `ring_regeneration`… un nom approchant propose les noms possibles), *l'expulser*.
  Pour tous : *tout le monde à moi*, *soigner tout le monde*, *invulnérabilité*, *tuer les PNJ hostiles*
  autour de moi (20 m, pour se sortir d'un blocage), *sauvegarder maintenant*.
- **Client** : *me téléporter vers ce joueur* (si vous êtes coincé dans le niveau).

Console (touche `²`) : `tp p2` téléporte le joueur 2 à côté de vous, `tp Pseudo`, `tp all`.

## Touches

Toutes modifiables dans *Options > Commandes*.

| Touche | Action |
|---|---|
| **V** | vue 1re / 3e personne |
| **N** | changer d'épaule |
| **O** | caméra libre (3e personne) |
| **Molette** | caméra plus près / plus loin |
| **Clic molette** | marqueur « par ici » |
| **Clic gauche maintenu** sur un coéquipier à terre | le relever |
| **H** en regardant un coéquipier à terre | potion de vie sur lui |
| **F8** | menu Administration |
| **²** | console |

## Limites connues

- Conçu pour le réseau local ou un VPN ; jamais testé avec une vraie latence internet.
- Les dégâts des sorts sont calculés chez celui qui les lance.
- La caméra des cinématiques du scénario n'est donnée qu'à celui qui les déclenche.
- Windows uniquement pour les binaires fournis (le code compile ailleurs comme Arx Libertatis, non testé).

## Compiler

C'est un fork d'Arx Libertatis : mêmes dépendances et même procédure (voir
[README.ArxLibertatis.md](README.ArxLibertatis.md)). Le code du mod est dans `src/coop/`
(réseau, session, réplication, marionnettes, visages, admin, vue 3e personne) plus des points d'accroche
dans le moteur. Sous Windows : CMake + Visual Studio 2022, `cmake --build build --config Release`.
Le contenu non binaire du paquet (LISEZMOI, traduction, modèles de visages, habillage) est dans `dist/`.

## Crédits et licence

- [Arx Libertatis](https://arx-libertatis.org/) et ses contributeurs, sur la base du code source
  d'Arx Fatalis publié par Arkane Studios.
- Partie coopérative : JD, avec l'assistance de Claude (Anthropic).
- Licence **GPLv3+** avec les termes additionnels d'Arx Libertatis : voir [COPYING](COPYING) et [LICENSE](LICENSE).
  Arx Fatalis, ses données et ses marques restent la propriété de leurs ayants droit.

---

## English

**ArxCoop** is a 2–4 player co-op mod for *Arx Fatalis*, built on the open-source
[Arx Libertatis](https://arx-libertatis.org/) engine. One player hosts, the others join; everybody plays
the adventure together in the host's world, each with their own character. The mod is fully translated:
set the game language to English (*Options > Language*).

**Install**: download the [latest release](https://github.com/Parricidium/ArxCoop/releases/latest) and unzip
it **into your Arx Fatalis folder** (the one with `data.pak`, GOG or Steam). Nothing is overwritten;
the mod keeps its saves and settings in `userdata\` next to `arx.exe`.

**Play**: host with `heberger.cmd` (or *Co-op > Host a game*), join with `rejoindre.cmd` (or
*Co-op > Join a game*, enter the host's address). LAN or VPN; over the internet the host opens TCP port
27015. Everybody meets in the lobby; the host picks *Continue: \<last save\>* or *Start from the beginning*.
Joining a game in progress and reconnecting after a crash both work.

**What is shared**: the whole world (NPCs, doors, levers, traps, chests, merchants, ground items,
quests, keys, runes, script flags), NPC dismemberment and blood, world sounds, quest XP and gold,
backpacks. Each player keeps their own character (stats, inventory, equipment, purse). Teammates are
seen with their head or **custom face**, armour, weapon, torch and animations; you can hand items to
them, ping a spot with the middle mouse button, revive a downed teammate (hold left click for 4 s, or
use a life potion while looking at them). Third-person camera (V / N / O / wheel), admin menu (F8:
teleport, heal, gold, XP, items, kick, invulnerability, kill nearby hostiles, save), English
translation, custom menu skin. Only the host saves; the others save their character automatically.

**Custom face**: *Options > Co-op > Open the faces folder*, drop a tightly cropped front photo of your
face (forehead to chin, no hair) into `userdata\coop\faces\`, then pick it under *Face*. A 3D preview
shows the result. The picture is sent once to the other players (20–40 KB) and nowhere else.

Licence: GPLv3+ (Arx Libertatis terms). You need your own copy of Arx Fatalis.

---
name: input-device-active-flag
description: "Le flag moteur \"active input device\" d'ER (manette vs souris) = même source pour hint-switch auto, recentrage curseur ET fix drift worldmap-manette"
metadata: 
  node_type: memory
  type: reference
---

<user> veut auto-switcher les hints de touche (manette/souris) selon le device utilisé à l'instant T. Mécanisme = l'état moteur ER **"active input device"** + l'enum **CS_KEY_GUIDE_DEVICE** (le sélecteur de device du key-guide ; vu dans tools/ghidra/rtti_index.txt `ComboItem<CS_KEY_GUIDE_DEVICE>` ; assets 01_930_KeyGuide, img://KG_R1…). ERR ne fait rien de spécial : vanilla a déjà CS_KEY_GUIDE_DEVICE (Keyboard/Pad).

★ C'EST LE MÊME FLAG que celui chassé pour le bug de drift worldmap-manette : brief RE **docs/re/windows_gamepad_input_device_re_prompt.md** (un VRAI mouvement souris via Raw Input/HID le bascule en "mouse" ; SendInput synthétique est filtré LLMHF_INJECTED et ne bascule pas ; hooker GetDeviceState ne bascule pas non plus → c'est le chemin Raw Input/HID).

UTILISATION pour la DX :
- savoir quel device est actif MAINTENANT → **LIRE** le flag (lecture mémoire non filtrée). Sert à : (a) switcher nos hints overlay, (b) recentrer le curseur au switch souris→manette, (c) fixer le drift worldmap.
- forcer les hints d'ER eux-mêmes → **ÉCRIRE** CS_KEY_GUIDE_DEVICE.

→ Mutualisé avec le **PR C (manette+curseur)** de [[plan-dx-bugs-audit]] et le backlog [[dx-bugs-backlog]] items 2/3/6.

PROCHAINE ÉTAPE (runtime, <user>) = recette **CE memory-diff** du brief : gamepad→bug, 1 mouvement souris→fix, scan Changed/Unchanged pour épingler le byte (valeur A=gamepad / B=mouse) + chemin de pointeur stable. Candidats déjà notés : CSPcKeyConfig singleton DAT_143d5deb8, CSMenuMan DAT_143d6b7b0 +0x19/+0x1a/+0x798. Ne PAS écrire d'input synthétique (filtré) — écrire/lire la mémoire directement. Voir [[rpm-live-memory-tooling]] + [[ghidra-worldmap-re]].

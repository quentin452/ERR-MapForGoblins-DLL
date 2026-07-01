// HAND-AUTHORED quest-step data for the overlay Quest Browser.
// Original descriptions written for this project (quest FACTS — locations, step
// order, interconnections — are not copyrightable; we copy no third-party prose).
//
// COVERAGE: 36 base-game + 14 DLC = 50 questlines. Entries with step_count==0 are
// PLACEHOLDERS, filled one NPC at a time (see memory: quest-browser strategy).
#include "goblin_quest_steps.hpp"
#include "goblin_inject.hpp" // goblin::ui::read_event_flag
#include "goblin_config.hpp" // goblin::config::questProgress
#include <string>

namespace goblin::generated
{

// ── authored step tables (original wording; quest facts only) ────────────
// entity_id MSB-sourced via tools/_find_npc.py Boc + data/tile_region_map.json
// (BonfireWarpParam-authoritative region resolve). progress_flag left 0 for all
// steps: no running game for empirical debugEventFlags capture and no decompiled
// EMEVD corpus on this machine -- manual ini checkbox stays the source of truth.
// Steps 2/3/4 left 0: no Coastal-Cave (Limgrave shore) seamster placement found,
// and the two Liurnia seamster placements (Bellum Hwy 1036480700 / Liurnia
// 1039400710) can't be disambiguated to the Lake-facing Cliffs without coords.
// NOTE: the old candidate 11050730 ("Boc 3943") resolves to Leyndell Ashen
// Capital -- NOT any of these 6 steps -- so it is deliberately NOT used here.
static const QuestStep steps_boc[] = {
    {"Free Boc", "A voice cries from a bush by the road in western Limgrave. Strike the bush to free the demi-human Boc, who turns out to be a skilled tailor.", "Limgrave", /*progress_flag=*/0u, /*entity_id=*/1043370750u}, // "Demi-Human Boc" (pre-free), Limgrave open-world
    {"Coastal Cave", "He relocates to the mouth of the Coastal Cave on Limgrave's west shore.", "Limgrave"},
    {"Lake-facing Cliffs", "Boc next settles by the Lake-facing Cliffs in eastern Liurnia and offers to alter and reinforce your garments.", "Liurnia"},
    {"Tailoring tools", "Bring him the Sewing Needle and the Iron/Gold Tailoring Tools so he can do heavier alterations.", "Liurnia"},
    {"Altus Plateau", "He moves on again, found resting along the Altus Plateau highway.", "Altus Plateau", /*progress_flag=*/0u, /*entity_id=*/1039510700u}, // sole Altus seamster placement
    {"Resolve his wish", "Console Boc with the 'You're Beautiful Too' gesture, or grant a Larval Tear, to finish his story.", "Altus Plateau", /*progress_flag=*/0u, /*entity_id=*/1039510700u}, // no relocation -> shares step 5 placement
};
// entity_id MSB-sourced via tools/_find_npc.py Thops + tile_region_map.json.
// Step 1 entity 1039390700 = the same id captured earlier as "Thops 3803" -- now
// confirmed it resolves to Liurnia (Church of Irith), i.e. step 1. Step 4 left 0:
// Thops is a corpse here (no live MSB enemy state to pin; the m14 placement is the
// living step-3 Thops), needs EMEVD/in-game verification. progress_flag 0 (see
// steps_boc note): manual ini checkbox remains source of truth.
static const QuestStep steps_thops[] = {
    {"Meet Thops", "Find Sorcerer Thops resting at the Church of Irith in eastern Liurnia; he longs to enter the Academy but lacks a key.", "Liurnia", /*progress_flag=*/0u, /*entity_id=*/1039390700u}, // Liurnia placement (Church of Irith)
    {"Academy Glintstone Key", "Bring him a spare Academy Glintstone Key so he can pass the Academy's seal.", "Liurnia", /*progress_flag=*/0u, /*entity_id=*/1039390700u}, // no relocation -> shares step 1 placement
    {"Schoolhouse Classroom", "He relocates to the Schoolhouse Classroom deep within Raya Lucaria Academy.", "Raya Lucaria", /*progress_flag=*/0u, /*entity_id=*/14000740u}, // m14 Academy of Raya Lucaria (live Thops)
    {"His end", "Return later to find Thops has passed at the Classroom; claim Thops's Barrier, the Academy Glintstone Staff, and his Bell Bearing.", "Raya Lucaria"},
};
static const QuestStep steps_patches[] = {
    {"Murkwater Cave", "Patches ambushes you as a boss in Murkwater Cave. When he feigns surrender, spare him and he opens a shop.", "Limgrave"},
    {"Forgive the trap", "Open his bait treasure chest (a trap), then forgive him to keep him around as a merchant.", "Limgrave"},
    {"Scenic Isle", "He relocates to Scenic Isle on the western edge of Liurnia.", "Liurnia"},
    {"Volcano Manor", "Patches later resurfaces at Volcano Manor, recruited to Tanith's cause.", "Mt. Gelmir"},
    {"Later errands", "He sends you on errands and reappears elsewhere (e.g. the Shaded Castle); attacking him ends his story early.", "varies"},
};

static const QuestStep steps_irina[] = {
    {"Meet Irina", "Find Irina on the road south of the Bridge of Sacrifice in the Weeping Peninsula; she entrusts you a letter for her father, Edgar, at Castle Morne.", "Weeping Peninsula"},
    {"Deliver the letter", "Carry the letter to Edgar, who defends Castle Morne.", "Weeping Peninsula"},
    {"Her fate", "Edgar leaves to reunite with her; if too much time passes she is found slain near where you met her, turning Edgar vengeful.", "Weeping Peninsula"},
};
static const QuestStep steps_edgar[] = {
    {"Castle Morne", "Find Edgar holding the Castle Morne rampart; deliver Irina's letter.", "Weeping Peninsula"},
    {"Quell the revolt", "Defeat the Leonine Misbegotten to settle the castle.", "Weeping Peninsula"},
    {"Edgar the Revenger", "If Irina has died, Edgar turns Bloody Finger and invades at the Revenger's Shack in western Liurnia; defeat him for the Sacrificial Twig and his gear.", "Liurnia"},
};
static const QuestStep steps_latenna[] = {
    {"Slumbering Wolf's Shack", "Find Latenna at the Slumbering Wolf's Shack in southwest Liurnia; she wishes to reach the Haligtree.", "Liurnia"},
    {"The medallion's half", "Speak with Albus (hiding as a pot) in the Village of the Albinaurics to obtain the Haligtree Secret Medallion (right).", "Liurnia"},
    {"Accept her", "Take Latenna into your service; she becomes a spirit ash bound to her wish.", "Liurnia"},
    {"Release her", "Carry her spirit to the Apostate Derelict in the Consecrated Snowfield and release her toward the Haligtree.", "Consecrated Snowfield"},
};
static const QuestStep steps_gurranq[] = {
    {"Bestial Sanctum", "Reach Gurranq at the Bestial Sanctum in the far northeast (use the Third Church of Marika portal in eastern Limgrave).", "Caelid"},
    {"Feed Deathroot", "Bring each Deathroot you find; every delivery rewards beast incantations or items.", "Caelid"},
    {"His rage", "After a few deliveries he briefly turns hostile; survive his onslaught and keep feeding him.", "Caelid"},
    {"Final gift", "Deliver the last Deathroot for the Beast Clergyman's heirloom and a Bell Bearing.", "Caelid"},
};
static const QuestStep steps_kenneth[] = {
    {"Atop the ruins", "Find Kenneth Haight perched on a ruin in eastern Limgrave; he asks you to retake Fort Haight.", "Limgrave"},
    {"Clear Fort Haight", "Defeat the defenders of Fort Haight, then report back.", "Limgrave"},
    {"Heir of Limgrave", "Kenneth occupies the fort; his line later ties into Nepheli's claim to rule Limgrave.", "Limgrave"},
};
static const QuestStep steps_gowry[] = {
    {"Gowry's Shack", "Find Sage Gowry near Sellia in Caelid; he asks for the Unalloyed Gold Needle.", "Caelid"},
    {"Slay O'Neil", "Defeat Commander O'Neil in the Swamp of Aeonia to recover the broken needle.", "Caelid"},
    {"Repaired needle", "Give Gowry the broken needle; he mends it, which begins Millicent's story.", "Caelid"},
};
static const QuestStep steps_millicent[] = {
    {"Halt the rot", "Give the repaired Unalloyed Gold Needle to Millicent at Gowry's Shack to stop her Scarlet Rot.", "Caelid"},
    {"Erdtree-Gazing Hill", "She recovers and travels to the Altus Plateau; speak with her at Erdtree-Gazing Hill.", "Altus Plateau"},
    {"Valkyrie's Prosthesis", "Bring her the Valkyrie's Prosthesis from the Shaded Castle so she can fight again.", "Altus Plateau"},
    {"Elphael", "Meet her in Elphael, Brace of the Haligtree, at the Prayer Room.", "Haligtree"},
    {"Her choice", "Aid her against her sisters (or oppose her) at the Drainage Channel; rewards include Millicent's Prosthesis and the Unalloyed Gold Needle.", "Haligtree"},
};
static const QuestStep steps_rya[] = {
    {"The lost necklace", "Meet Rya near Scenic Isle / Laskyar Ruins in Liurnia; recover her stolen necklace from a nearby thief.", "Liurnia"},
    {"Invitation", "Return the necklace and she invites you to Volcano Manor.", "Liurnia"},
    {"Zorayas", "At Volcano Manor she reveals her true serpent form and her story folds into Tanith's.", "Mt. Gelmir"},
};
// entity_id MSB-sourced via tools/_find_npc.py Alexander + tile_region_map.json.
// All 5 confidently placed: steps 1/4/5 by EXACT subRegion match (Stormhill /
// Mt. Gelmir / Crumbling Farum Azula); step 2 = m32 Caelid tunnel (Alexander's
// only tunnel is Gael); step 3 = sole remaining open-world Caelid placement (SE,
// Redmane). progress_flag 0 (see steps_boc note): manual ini checkbox is truth.
static const QuestStep steps_alexander[] = {
    {"Stuck in Stormhill", "Free Alexander, the Warrior Jar, from a hole in northern Stormhill by striking him.", "Limgrave", /*progress_flag=*/0u, /*entity_id=*/1043390710u}, // subRegion=Stormhill (exact)
    {"Gael Tunnel", "Find him wedged before a door in Gael Tunnel and free him again.", "Caelid", /*progress_flag=*/0u, /*entity_id=*/32070700u}, // m32 tunnel map, Caelid
    {"Radahn Festival", "Meet him at Redmane Castle, eager for the battle against Radahn.", "Caelid", /*progress_flag=*/0u, /*entity_id=*/1051360705u}, // SE Caelid, Redmane
    {"Lava pot", "Free him once more from a hole on a lava slope of Mt. Gelmir.", "Mt. Gelmir", /*progress_flag=*/0u, /*entity_id=*/1035530700u}, // subRegion=Mt. Gelmir (exact)
    {"His end", "Find him dying at Crumbling Farum Azula; the duel yields Alexander's Innards and the Shard of Alexander talisman.", "Crumbling Farum Azula", /*progress_flag=*/0u, /*entity_id=*/13000700u}, // m13 Crumbling Farum Azula (exact)
};
static const QuestStep steps_diallos[] = {
    {"Roundtable mourning", "Meet Diallos at the Roundtable Hold, grieving his lost servant.", "Roundtable Hold"},
    {"Mistwood hunt", "Find him in Limgrave's Mistwood, hunting the Bloody Finger responsible.", "Limgrave"},
    {"Jarburg", "He retires to Jarburg in Liurnia to watch over the living jars.", "Liurnia"},
    {"Defense of Jarburg", "When poachers raid Jarburg he defends it and is found gravely wounded.", "Liurnia"},
    {"His end", "Diallos falls protecting the jars; his story closes alongside Jar-Bairn's.", "Liurnia"},
};
static const QuestStep steps_jarbairn[] = {
    {"The jar child", "Speak with Jar-Bairn at Jarburg in Liurnia, after Diallos has settled there.", "Liurnia"},
    {"Through the raids", "Visit across the poacher attacks; his outlook shifts with Diallos's fate.", "Liurnia"},
    {"Alexander's Innards", "Give him Alexander's Innards; he ponders becoming a warrior jar.", "Liurnia"},
    {"His path", "Encourage or discourage him over later visits; rewards include the Companion Jar.", "Liurnia"},
};

static const QuestStep steps_ranni[] = {
    {"Renna at Elleh", "An early meeting as 'Renna' at the Church of Elleh grants the Spirit Calling Bell; her quest proper begins later.", "Limgrave"},
    {"Ranni's Rise", "Pledge yourself to Ranni at Ranni's Rise in the Three Sisters, northwest Liurnia, and meet Blaidd, Iji and Seluvis.", "Liurnia"},
    {"Nokron", "Defeat Starscourge Radahn to open Nokron; retrieve the Fingerslayer Blade there.", "Caelid / Nokron"},
    {"Carian Inverted Statue", "Use the blade on the puppets at Ranni's Rise, take the Carian Inverted Statue, and unlock Renna's Rise to descend the Ainsel River.", "Liurnia"},
    {"Underground journey", "Pass Nokstella, the Lake of Rot and the Grand Cloister, defeating Astel, to reach the Moonlight Altar.", "Ainsel River / Moonlight Altar"},
    {"Age of Stars", "Place the Dark Moon Ring on Ranni at the Cathedral of Manus Celes to finish her quest and unlock her ending.", "Moonlight Altar"},
};
static const QuestStep steps_blaidd[] = {
    {"Howl at Mistwood", "On Kale's hint, find Blaidd atop the Mistwood ruins in Limgrave and trigger his appearance.", "Limgrave"},
    {"Hunt Darriwil", "Help Blaidd slay Bloodhound Knight Darriwil at the Forlorn Hound Evergaol.", "Limgrave"},
    {"Ranni's shadow", "He serves Ranni; find him around Caria Manor and through her underground quest.", "Liurnia"},
    {"His curse", "After Ranni's quest he is found maddened at Ranni's Rise and must be put down; he drops the Royal Greatsword and his set.", "Liurnia"},
};
static const QuestStep steps_iji[] = {
    {"Carian smith", "Meet Iji, the Carian war-smith, on the road below Caria Manor; he watches over Ranni.", "Liurnia"},
    {"Advice", "He offers counsel as Ranni's quest advances.", "Liurnia"},
    {"His fate", "Siding with Seluvis's schemes or angering Ranni's enemies can leave Iji slain later in the quest.", "Liurnia"},
};
static const QuestStep steps_seluvis[] = {
    {"Seluvis's Rise", "Meet Preceptor Seluvis at his rise in the Three Sisters; he hands you a potion to puppet an NPC.", "Liurnia"},
    {"The puppet scheme", "He wants Nepheli (or others) turned into a puppet; you may comply or warn the target.", "Liurnia"},
    {"Hidden workshop", "Progressing Ranni's quest reveals his secret puppet cellar.", "Liurnia"},
    {"His end", "Serving Ranni over Seluvis leaves him turned to stone; his set and spells remain.", "Liurnia"},
};
static const QuestStep steps_fia[] = {
    {"Deathbed hugs", "Speak with Fia at the Roundtable Hold; she gives a temporary blessing.", "Roundtable Hold"},
    {"Weathered Dagger", "Deliver her Weathered Dagger to the swordsman it belongs to (Rogier).", "Roundtable Hold"},
    {"Cursemark of Death", "Through Rogier's and D's threads, obtain the Cursemark of Death.", "varies"},
    {"Deeproot Depths", "Fia departs to Deeproot Depths; give her the Cursemark there.", "Deeproot Depths"},
    {"Duskborn", "Defeat Lichdragon Fortissax in her dream for the Mending Rune of the Death-Prince and her ending.", "Deeproot Depths"},
};
static const QuestStep steps_dhunter[] = {
    {"Hunter of the Dead", "Meet D near Summonwater Village in Limgrave (and at the Roundtable); he hunts Those Who Live in Death.", "Limgrave"},
    {"Opposed to Fia", "He despises Fia's deathbed work; his thread crosses hers.", "Roundtable Hold"},
    {"His brother", "If Fia slays D, his Twinned Armor can be given to his brother near Nokron/Deeproot, who then acts on it.", "Deeproot Depths"},
};
static const QuestStep steps_rogier[] = {
    {"Stormveil", "Find Sorcerer Rogier wounded inside Stormveil Castle; he studies the Black Knife assassins.", "Limgrave"},
    {"Roundtable research", "He relocates to the Roundtable Hold and investigates Godwyn's death.", "Roundtable Hold"},
    {"Black Knifeprint", "Bring him the Black Knifeprint to advance his and Fia's threads.", "Roundtable Hold"},
    {"His end", "The deathblight curse claims him; loot his notes and Spellblade set, which push Fia's quest onward.", "Roundtable Hold"},
};
static const QuestStep steps_corhyn[] = {
    {"Roundtable scholar", "Exhaust Corhyn's dialog at the Roundtable Hold, where he sells incantations.", "Roundtable Hold"},
    {"He departs", "After you reach the Altus Plateau he leaves to search for Goldmask.", "Altus Plateau"},
    {"Find Goldmask", "Meet Corhyn on the Altus highway, then find Goldmask frozen on the broken bridge to the north.", "Altus Plateau"},
    {"Golden Order Principia", "Relay Goldmask's location to Corhyn and pass along the Golden Order Principia.", "Altus Plateau"},
    {"Age of Order", "At the Erdtree in Leyndell, Goldmask's final pose yields the Mending Rune of Perfect Order.", "Leyndell"},
};
static const QuestStep steps_goldmask[] = {
    {"Broken bridge", "Find Goldmask frozen on the broken bridge in northern Altus Plateau, pointing toward the Erdtree.", "Altus Plateau"},
    {"Corhyn's hero", "He is the focus of Corhyn's quest; the two converge.", "Altus Plateau"},
    {"To the capital", "He relocates toward Leyndell as his contemplation deepens.", "Leyndell"},
    {"Perfect Order", "At the foot of the Erdtree in Leyndell his body holds the Mending Rune of Perfect Order.", "Leyndell"},
};
static const QuestStep steps_nepheli[] = {
    {"Roundtable", "Meet Nepheli at the Roundtable Hold; her thread touches Gideon and Kenneth.", "Roundtable Hold"},
    {"Stormveil", "Summon her for the Godrick fight via her sign outside the boss fog.", "Limgrave"},
    {"Disillusioned", "After Godrick she returns to the Roundtable, shaken.", "Roundtable Hold"},
    {"Don't puppet her", "Avoid using Seluvis's potion on her to keep her quest open.", "Liurnia"},
    {"Ruler of Limgrave", "With Kenneth's help, install her at Stormhill as Limgrave's ruler for her rewards.", "Limgrave"},
};
static const QuestStep steps_hyetta[] = {
    {"Shabriri Grapes", "Meet Hyetta on the Liurnia lakeshore; she begs for Shabriri Grapes.", "Liurnia"},
    {"Feed her", "Give her Shabriri Grapes at her successive resting spots across Liurnia.", "Liurnia"},
    {"Frenzied path", "Her road is the Frenzied Flame, crossing Yura and Shabriri.", "varies"},
    {"Frenzied Flame", "She becomes the Flame's maiden at the Subterranean Shunning-Grounds, opening the Lord of Frenzied Flame ending.", "Leyndell (underground)"},
};
static const QuestStep steps_yura[] = {
    {"Bloody Finger Hunter", "Meet Yura near the north of Agheel Lake in Limgrave; summon him against invaders.", "Limgrave"},
    {"Nerijus", "He aids you against Bloody Finger Nerijus in Liurnia.", "Liurnia"},
    {"Eleonora", "His thread leads toward Eleonora, the Violet Bloody Finger.", "Mt. Gelmir"},
    {"Shabriri takes him", "Shabriri usurps Yura near the Zamor Ruins; defeat Eleonora at the Second Church of Marika for Eleonora's Poleblade.", "Mountaintops"},
};
static const QuestStep steps_varre[] = {
    {"First Step", "Varre greets you at the First Step in Limgrave at the very start.", "Limgrave"},
    {"Rose Church", "He reappears at the Rose Church in Liurnia and offers the Lord of Blood's path.", "Liurnia"},
    {"Bloody Finger", "Take the Festering Bloody Finger and invade other worlds (or a phantom) as instructed.", "varies"},
    {"Maiden's blood", "Soak the Lord of Blood's Favor in maiden blood at the Church of Inhibition.", "Liurnia"},
    {"Pureblood Medal", "Return for the Pureblood Knight's Medal, which warps you to Mohgwyn Palace.", "Liurnia"},
};
static const QuestStep steps_sellen[] = {
    {"Waypoint Ruins", "Find Sellen hidden in the Waypoint Ruins cellar in Limgrave; she teaches sorceries.", "Limgrave"},
    {"Lusat and Azur", "Find Primeval Sorcerers Lusat (Sellia Crystal Tunnel) and Azur (Mt. Gelmir) to unlock their spells.", "Caelid / Mt. Gelmir"},
    {"Her sealed body", "Witch-Hunter Jerren reveals Sellen's true body; choose to aid her or oppose her.", "Caelid"},
    {"A new vessel", "Pursue a new body for Sellen toward the Raya Lucaria Grand Library.", "Raya Lucaria"},
    {"Stargazer", "At the finale, defeat Jerren to complete her rebirth for Stars of Ruin and her set.", "Raya Lucaria"},
};
static const QuestStep steps_jerren[] = {
    {"Radahn Festival", "Witch-Hunter Jerren hosts the festival at Redmane Castle; speak before and after the Radahn fight.", "Caelid"},
    {"Hunting Sellen", "He later moves to hunt Sellen at her sealed body.", "Caelid"},
    {"The duel", "Side with Sellen against Jerren, or with Jerren against Sellen, to end her quest.", "Raya Lucaria"},
};
static const QuestStep steps_roderika[] = {
    {"Stormhill Shack", "Meet Roderika at the Stormhill Shack in Limgrave; she is grief-stricken and speaks of spirit-tuning.", "Limgrave"},
    {"Tell Hewg", "Mention her to Smithing Master Hewg at the Roundtable Hold.", "Roundtable Hold"},
    {"Spirit Tuner", "Encourage her until she joins the Roundtable as the Spirit Tuner, upgrading spirit ashes.", "Roundtable Hold"},
    {"Her gifts", "Exhaust her dialog for a Golden Seed, the Sitting Sideways gesture and her set.", "Roundtable Hold"},
};
static const QuestStep steps_hewg[] = {
    {"Roundtable smith", "Hewg forges and upgrades your weapons at the Roundtable Hold.", "Roundtable Hold"},
    {"Worried for Roderika", "Relay between Hewg and Roderika to advance her tuning.", "Roundtable Hold"},
    {"His wish", "Exhaust his dialog for lore of his binding by Marika.", "Roundtable Hold"},
};
static const QuestStep steps_bernahl[] = {
    {"Warmaster's Shack", "Meet Knight Bernahl at the Warmaster's Shack in Limgrave; he teaches Ashes of War.", "Limgrave"},
    {"Volcano Manor", "He invites you to Volcano Manor as a fellow recusant.", "Mt. Gelmir"},
    {"Recusant duties", "Carry out the manor's assassination requests alongside him.", "Mt. Gelmir"},
    {"Recusant Bernahl", "He turns hostile in Crumbling Farum Azula; defeat him for the Devourer's Scepter and Beast Champion set.", "Crumbling Farum Azula"},
};
static const QuestStep steps_tanith[] = {
    {"Welcome to the Manor", "Lady Tanith receives you at Volcano Manor and invites you to the recusant cause.", "Mt. Gelmir"},
    {"Recusant letters", "Complete the manor's assassination targets (crossing Rya and Bernahl).", "varies"},
    {"Rykard", "Descend to defeat Rykard, Lord of Blasphemy.", "Mt. Gelmir"},
    {"Her devotion", "After Rykard, Tanith tends his remains; her thread can be pursued further.", "Mt. Gelmir"},
};
static const QuestStep steps_dungeater[] = {
    {"The cell", "Find the Dung Eater imprisoned at the Roundtable Hold; he demands Seedbed Curses.", "Roundtable Hold"},
    {"Seedbed Curses", "Gather Seedbed Curses from corpses in Leyndell and the Shunning-Grounds.", "Leyndell"},
    {"Deliver them", "Hand the curses over for his rewards.", "Roundtable Hold"},
    {"His choice", "Empower him (Mending Rune of the Fell Curse / Blessing of Despair) or hunt him down in the Shunning-Grounds.", "Leyndell (underground)"},
};

// ── base-game: Gideon & Vyke ─────────────────────────────────────────────
static const QuestStep steps_gideon[] = {
    {"Roundtable welcome", "Gideon Ofnir greets you coldly by the Table of Lost Grace when Melina first brings you to the Roundtable Hold, naming you a mere house guest.", "Roundtable Hold"},
    {"Earn his regard", "Once you fell a Shardbearer and claim a Great Rune he treats you as a true member, sharing what he knows of the demigods ahead.", "Roundtable Hold"},
    {"The All-Knowing", "As his information broker he comments on the threads of Nepheli, Dung Eater and others; advance those quests to draw out his dialog.", "Roundtable Hold"},
    {"Sir Gideon", "He goes ahead to Leyndell and must be fought as a mandatory boss in the Ashen Capital, where his knowledge of every foe's weakness becomes his moveset.", "Leyndell (Ashen Capital)"},
};
static const QuestStep steps_vyke[] = {
    {"Festering Fingerprint", "A maddened Vyke first attacks as a Bloody Finger invader on the climb to the Church of Inhibition in northeast Liurnia; his defeat drops Vyke's War Spear.", "Liurnia"},
    {"Fanged Imp Ashes", "The invasion also rewards the Fanged Imp Ashes and pieces of his armor.", "Liurnia"},
    {"Lord Contender's Evergaol", "His true self is sealed in the Lord Contender's Evergaol in the Mountaintops of the Giants; the duel yields his remaining gear.", "Mountaintops"},
};

// ── DLC: Leda's group (converges at Enir-Ilim) ───────────────────────────
static const QuestStep steps_leda[] = {
    {"Into the shadow", "After felling Mohg you touch Miquella's withered arm at the Mohgwyn cocoon; Leda meets you and you cross into the Land of Shadow.", "Gravesite Plain"},
    {"On the trail", "She leaves messages and offers her summon through Belurat and Castle Ensis (aid for Rellana) as she chases Miquella.", "Scadu Altus"},
    {"The charm breaks", "Entering Shadow Keep or crossing toward the village breaks Miquella's charm and turns her wary of the others.", "Shadow Keep"},
    {"Hunting her own", "At Shadow Keep she calls you to help purge Hornsent, then Ansbach; pick a summon sign to side with her or spare them.", "Shadow Keep"},
    {"Enir-Ilim", "She and whoever still follows Miquella ambush you in the multi-NPC battle below Enir-Ilim; her body holds Leda's Sword and armor.", "Enir-Ilim"},
};
static const QuestStep steps_hornsent[] = {
    {"Three-Path Cross", "Meet the Hornsent at the Three-Path Cross in Gravesite Plain; he hands you a map of Miquella's crosses and speaks of his vengeance.", "Gravesite Plain"},
    {"Highroad Cross", "He moves to the Highroad Cross once you reach Scadu Altus and gives an updated map.", "Scadu Altus"},
    {"Shadow Keep stand", "After the rune breaks he confronts Leda at Shadow Keep; choose his gold sign to keep him alive, or Leda's to cut him down.", "Shadow Keep"},
    {"Messmer's aid", "If spared he can be summoned against Messmer, then may invade near the Rauh ruins; rewards include the Falx and Hornsent set.", "Rauh Ruins"},
    {"Enir-Ilim", "Should he survive, he joins the followers' battle at Enir-Ilim, where his gear is finally claimed.", "Enir-Ilim"},
};
static const QuestStep steps_freyja[] = {
    {"Gravesite meeting", "Redmane Freyja, a former Radahn guard, greets you at the Three-Path Cross and speaks of Miquella's crosses.", "Gravesite Plain"},
    {"Belurat summon", "She can be summoned against the Divine Beast Dancing Lion at Belurat, which opens up her past.", "Belurat"},
    {"Storehouse", "After the charm breaks find her studying scripture on the Storehouse seventh floor; speak with Ansbach about her first.", "Shadow Keep"},
    {"Ansbach's letter", "Carry Ansbach's letter to Freyja and exhaust her talk for the Golden Lion Shield.", "Shadow Keep"},
    {"Enir-Ilim", "Committed to Miquella, she stands with Leda's group at Enir-Ilim; her body holds Freyja's Greatsword and set.", "Enir-Ilim"},
};
static const QuestStep steps_ansbach[] = {
    {"Mohg's servant", "Sir Ansbach, once a servant of Mohg, introduces himself at the Main Gate Cross and asks you to seek Miquella's crosses.", "Gravesite Plain"},
    {"Report the crosses", "Return after finding crosses (Belurat, Scaduview, Cerulean Coast) for more talk before the rune breaks.", "Gravesite Plain"},
    {"Storehouse first floor", "Find him on the Storehouse first floor in Shadow Keep; speak with Freyja about her choice before him.", "Shadow Keep"},
    {"Secret Rite Scroll", "Bring him the Secret Rite Scroll and rest; he gives the Letter for Freyja that advances her quest.", "Shadow Keep"},
    {"Side with him", "When Leda strikes, take his gold sign so he lives (Leda's Rune, his longbow); take Leda's red sign and he dies for his set.", "Shadow Keep"},
    {"To the end", "If alive he can be summoned at Enir-Ilim and for the final fight; his full reward is the Furious Blade of Ansbach.", "Enir-Ilim"},
};
static const QuestStep steps_moore[] = {
    {"A merchant's grief", "Moore sets up as a merchant at the Main Gate Cross alongside Ansbach; buy from him to open his talk.", "Gravesite Plain"},
    {"Thiollier's syrup", "Meet Thiollier nearby, then return to Moore for the Black Syrup before either departs.", "Gravesite Plain"},
    {"Forager cookbooks", "Help the injured Forager Brood pest near the Church of the Crusade and gather the brood's cookbooks across the land.", "Scadu Altus"},
    {"His sorrow", "When asked how to face loss, the answer you give ('put it behind you') decides whether he turns on you.", "Scadu Altus"},
    {"Enir-Ilim", "If pushed to despair he joins the followers' battle; his remains hold the Verdigris Greatshield, set and Bell Bearing.", "Enir-Ilim"},
};
static const QuestStep steps_thiollier[] = {
    {"Quiet merchant", "Thiollier sells poisons near the Pillar Path Cross in Gravesite Plain and barely speaks of himself.", "Gravesite Plain"},
    {"St. Trina's name", "After Miquella's rune breaks he opens up about his devotion to St. Trina.", "Gravesite Plain"},
    {"Garden of Deep Purple", "Reach St. Trina in the Stone Coffin Fissure; Thiollier relocates toward her resting place.", "Stone Coffin Fissure"},
    {"The nectar", "Drink St. Trina's nectar until her voice answers, advancing his obsession.", "Stone Coffin Fissure"},
    {"His grief", "He invades in despair near the garden; defeating him drops St. Trina's Smile, and he can later be summoned at Enir-Ilim for his set.", "Stone Coffin Fissure"},
};
static const QuestStep steps_dane[] = {
    {"Monk's Missive", "Pick up the Monk's Missive and the 'May the Best Win' gesture at the Highroad Cross in Scadu Altus.", "Scadu Altus"},
    {"Moorth Ruins", "Travel east to Dryleaf Dane waiting at the Moorth Ruins.", "Scadu Altus"},
    {"The duel", "Offer the 'May the Best Win' gesture to fight him bare-handed for the Dryleaf Arts and his hat.", "Scadu Altus"},
    {"His robes", "His remaining armor lies past the Recluses' River, reached through the Shadow Keep coffin teleport.", "Shadow Keep"},
    {"Enir-Ilim", "He turns up hostile in the followers' battle regardless of earlier choices, dropping his footwork art.", "Enir-Ilim"},
};
static const QuestStep steps_sttrina[] = {
    {"The sleeping saint", "St. Trina lies deep in the Stone Coffin Fissure's Garden of Deep Purple, an aspect of Miquella set aside with his love.", "Stone Coffin Fissure"},
    {"Drink the nectar", "Drink from her repeatedly until she speaks, which also drives Thiollier's quest.", "Stone Coffin Fissure"},
    {"Her plea", "Her words and the items she leaves (sleep gifts, gestures) reveal Miquella's discarded tenderness; her thread closes with Thiollier's.", "Stone Coffin Fissure"},
};

// ── DLC: standalone questlines ───────────────────────────────────────────
static const QuestStep steps_queelign[] = {
    {"East Belurat", "Fire Knight Queelign first invades near the fountain of east Belurat in service of Messmer's flame.", "Belurat"},
    {"Church of the Crusade", "He invades again at the Church of the Crusade southwest of Shadow Keep.", "Scadu Altus"},
    {"Shadow Keep Prayer Room", "Find him at last in the Prayer Room; give the Iris of Grace for his spirit ash, or the Iris of Occultation for his weapon.", "Shadow Keep"},
};
static const QuestStep steps_igon[] = {
    {"The vengeful hunter", "Igon lies broken at the Pillar Path waypoint in Gravesite Plain, consumed by hatred for the dragon Bayle.", "Gravesite Plain"},
    {"Dragon's Pit", "Follow the road south to the Dragon's Pit and on toward the Jagged Peak.", "Jagged Peak"},
    {"Up the peak", "Climb past the drakes and the Dragon Communion altar toward Bayle's lair; Igon moves up behind you.", "Jagged Peak"},
    {"Bayle the Dread", "At Bayle's arena find Igon's summon sign and call him in; his cries and harpoons join your fight.", "Jagged Peak"},
    {"His vendetta done", "After the dragon falls Igon dies fulfilled, leaving his Greatbow, armor and Bell Bearing.", "Jagged Peak"},
};
static const QuestStep steps_grandam[] = {
    {"Belurat storeroom", "Find the old Hornsent Grandam behind a locked storeroom in Belurat (needs the Storeroom Key); she mistakes you for an enemy.", "Belurat"},
    {"Beast's head", "After the Divine Beast Dancing Lion falls, return wearing its Divine Beast Head; she gives the Watchful Spirit incantation and Scorpion Stew.", "Belurat"},
    {"After Messmer", "Once Messmer is defeated, speak with her a final time for the Gourmet Scorpion Stew before she sleeps.", "Belurat"},
};
static const QuestStep steps_florissax[] = {
    {"Grand Altar", "The Dragon Communion Priestess waits at the Grand Altar of Dragon Communion; accept her rite to devour draconic essence for the Ancient Dragon's Blessing.", "Jagged Peak"},
    {"Slay Bayle", "She sets you on Bayle the Dread as vengeance for his betrayal of Placidusax.", "Jagged Peak"},
    {"Night and the concoction", "For her true reward, bring Thiollier's Concoction and meet her at night as she speaks with Placidusax; she sleeps, then wakes.", "Jagged Peak"},
    {"Florissax's pledge", "Confess the concoction after Bayle's death for the Dragonbolt of Florissax and her spirit ash; otherwise claim the Priestess Heart and Flowerstone Gavel.", "Jagged Peak"},
};
static const QuestStep steps_ymir[] = {
    {"Cathedral of Manus Metyr", "High Priest Count Ymir receives you at the Cathedral of Manus Metyr and hands you a Ruins Map and the Hole-Laden Necklace.", "Manus Metyr"},
    {"Finger Ruins of Rhia", "Ring the great bell among the Finger Ruins of Rhia; he gives the next map.", "Scadu Altus"},
    {"O Mother", "Use the 'O Mother' gesture at the Shadow Keep back-gate altar to open the path to the Hinterlands.", "Shadow Keep"},
    {"Finger Ruins of Dheo", "Ring the bell in the Hinterlands' Finger Ruins of Dheo (Jolán can be summoned here).", "Hinterlands"},
    {"Metyr, Mother of Fingers", "Descend beneath the cathedral to the Finger Ruins of Miyr, past the invader Anna, and defeat Metyr for her Remembrance.", "Manus Metyr"},
    {"Ymir's design", "Return to the throne; survive Jolán's invasion, then fell Ymir himself for his High Priest set, the Maternal Staff and Cherishing Fingers.", "Manus Metyr"},
};
static const QuestStep steps_jolan[] = {
    {"Ymir's swordhand", "Swordhand of Night Jolán guards Count Ymir at the Cathedral of Manus Metyr alongside her sister Anna.", "Manus Metyr"},
    {"Through Ymir's quest", "Her path tracks Ymir's; she can be summoned in the Finger Ruins and invades during his final turn.", "Manus Metyr"},
    {"Iris choice", "After Ymir falls, give her the Iris of Grace for her spirit ash, or the Iris of Occultation for the Sword of Night.", "Manus Metyr"},
    {"Reunite with Anna", "At Rabbath's Rise she can rejoin Anna for their combined ash; her thread also crosses the fallen Swordhand, Rakshasa.", "Scadu Altus"},
};

// ── 36 base-game questlines ──────────────────────────────────────────────
// 6th field (bool) = dlc; 7th field (const char*) = order-sensitive/missable
// warning shown amber in the overlay. Both default-omitted when not needed.
const NpcQuest QUEST_BROWSER[] = {
    // Ranni's questline (interconnected cluster)
    {"Ranni the Witch", "Ranni's Quest", "Hub of the Blaidd/Iji/Seluvis cluster", steps_ranni, 6},
    {"Blaidd", "Blaidd's Quest", "Part of Ranni's questline", steps_blaidd, 4, false,
     "Use Blaidd before pushing Ranni's quest too far; advancing it first can trap him and he later turns hostile."},
    // fail_flag 1034499202 = Iji dead/gone. MSB-CONFIRMED: Iji is entity
    // 1034490700 / 1034490711 (model c4604, "Smithing Master Iji") at m60_34_49 —
    // NOT 1034500710, which the MSB shows is RANNI (model c2050). His death handler
    // (90005707, group 3760-3767, namespace 1034499xxx) + common.emevd $Event(3049)
    // flag2 (group 3765-3767, gated on Iji-alive 3761) set 1034499202 (+1034499204)
    // ON when Iji is gone; OFF-reset when alive (m60_34_49:117). Exact parallel to
    // Seluvis's 1034509302. Earlier picks were ALL wrong: 1042600001 (a 19-bit
    // counter bit), 1034509403 (that is Ranni's resolver flag, entity 1034500710),
    // 1034502743 / 558. See docs/emevd_death_flags_results.md.
    {"Iji", "Iji's Quest", "Part of Ranni's questline", steps_iji, 3, false,
     "Siding with Seluvis's puppet scheme, or angering Ranni's enemies, can get Iji killed.",
     1034499202u},
    // fail_flag 1034509302 = Seluvis dead (= seluvis_q99 "quest concluded" in the
    // QuestLog, his own 1034509* namespace). Isolated by intersecting TWO far
    // Seluvis-dead saves (Caelid + Morne) with the in-game kill-window capture;
    // verified persistent (true at both far spots) + clean (false while alive).
    {"Seluvis", "Seluvis's Quest", "Part of Ranni's questline; crosses Nepheli", steps_seluvis, 4, false,
     "Using his puppet potion on Nepheli permanently ends HER questline -- warn her instead to keep both.",
     1034509302u},
    // Sellen
    // fail_flag + fail_conclusion=true: these flags are the NPC's SHARED "_q99"
    // concluded flag (set on death OR peaceful completion), via the 90005702
    // death handler. Confirmed: Sellen 3463 (entity 14000713), Nepheli 4223
    // (10000730), Kenneth 3583 (1045380700), Gowry 4163 (1050380700), Boc 3943
    // (11050730), Patches 3683 (31000701), Thops 3803 (1039390700). The overlay
    // greys these as "[concluded]" (done or gone), not "[unfinishable]/dead".
    {"Sorceress Sellen", "Sellen's Quest", "Crosses Jerren, Lusat/Azur", steps_sellen, 5, false,
     "Her finale forces an exclusive side (Sellen vs Jerren); pick knowing the other is lost.",
     3463u, true},
    // fail_flag 3363 = Jerren gone. MSB-confirmed: entity 14000716 / 14000717 =
    // "Witch-Hunter Jerren" (m14 Academy, Sellen finale); 90005702 sets 3363 ON.
    {"Witch-Hunter Jerren", "Jerren's Quest", "Sellen's quest finale (Sellen vs Jerren)", steps_jerren, 3, false,
     "Tied to Sellen's finale -- siding with one ends the other.", 3363u},
    // Roundtable / Roderika
    {"Roderika", "Roderika's Quest", "Crosses Hewg (Stormhill -> Roundtable)", steps_roderika, 4},
    {"Smithing Master Hewg", "Hewg's Quest", "Crosses Roderika", steps_hewg, 3},
    {"Nepheli Loux", "Nepheli's Quest", "Crosses Kenneth, Gideon, Dung Eater", steps_nepheli, 5, false,
     "Do NOT use Seluvis's potion on her, or her questline ends.", 4223u, true},
    {"Kenneth Haight", "Kenneth's Quest", "Feeds Nepheli's claim to Limgrave", steps_kenneth, 3, false, nullptr, 3583u, true},
    {"Gideon Ofnir", "Gideon's Quest", "Touches many quests (Roundtable info-broker)", steps_gideon, 4},
    // Deathbed / Black Knife cluster
    {"Fia, Deathbed Companion", "Fia's Quest", "Crosses D and Rogier (Deathroot/Godwyn)", steps_fia, 5, false,
     "Crosses D and Rogier; the Cursemark of Death order and whether Fia slays D gate her ending."},
    {"D, Hunter of the Dead", "D's Quest", "Crosses Fia; D's brother continues it", steps_dhunter, 3, false,
     "Conflicts with Fia -- if Fia slays D his armor passes to his brother, branching the thread."},
    {"Sorcerer Rogier", "Rogier's Quest", "Feeds Fia's quest (Black Knife)", steps_rogier, 4, false,
     "Advance before the deathblight claims him; his notes are needed to push Fia's quest."},
    // Golden Order
    {"Goldmask", "Goldmask (Corhyn's Quest)", "Part of Corhyn's questline", steps_goldmask, 4},
    {"Brother Corhyn", "Corhyn's Quest", "Searches for Goldmask", steps_corhyn, 5},
    // Millicent
    {"Millicent", "Millicent's Quest", "Started by Gowry (Scarlet Rot)", steps_millicent, 5, false,
     "Time/order-sensitive: needs Gowry's repaired needle, and her ending hinges on a late choice at the Haligtree."},
    {"Sage Gowry", "Gowry's Quest", "Starts Millicent's quest", steps_gowry, 3, false,
     "Finish the needle repair before progressing Millicent too far, or her thread can stall.",
     4163u, true},
    // Standalone-ish
    {"Rya", "Rya's Quest", "Leads into Volcano Manor (Tanith)", steps_rya, 3},
    // name_id 122310 = "Boc the Seamster" FMG NpcName id (data/npc_name_text_map.json).
    // Per-step entity_id now MSB-sourced for steps 1/5/6 (see steps_boc above);
    // steps 2/3/4 stay 0 (no offline-disambiguable placement). progress_flag stays 0
    // for all steps -- needs the running game (empirical debugEventFlags) or a
    // decompiled EMEVD corpus, neither available offline. The old CANDIDATE 11050730
    // ("Confirmed: ... Boc 3943 (11050730)" in the fail_flag block above) was RESOLVED
    // via tile_region_map.json to Leyndell, Ashen Capital -- NOT any of Boc's 6 steps
    // -- so it is correctly NOT used as a step entity_id. (quest_gates.py's curated
    // flags are whole-questline "is active" gates, not per-step "is done" flags --
    // still wrong semantics to reuse for progress_flag.)
    {"Boc the Seamster", "Boc's Quest", nullptr, steps_boc, 6, false, nullptr, 3943u, true, 122310u},
    {"Patches", "Patches' Quest", "Joins Volcano Manor (Tanith)", steps_patches, 5, false,
     "Attacking or killing him at the wrong moment ends his merchant questline early.",
     3683u, true},
    // fail_flag 3383 = Irina gone. MSB-confirmed: entity 1045340700 = "Irina of
    // Morne" (m60_45_34); 90005702 death handler + her quest resolver set 3383 ON.
    {"Irina", "Irina's Quest", "Crosses Edgar (Castle Morne)", steps_irina, 3, false,
     "Time-sensitive: settle Castle Morne before too much story progress, or Irina dies and Edgar turns hostile.",
     3383u},
    // fail_flag 3403 = Edgar gone. MSB-confirmed: entity 1045340705 / 1043310705
    // (Castle Morne) = "Castellan Edgar"; 90005702 death handler sets 3403 ON.
    {"Edgar", "Edgar's Quest", "Crosses Irina (Castle Morne)", steps_edgar, 3, false,
     "If Irina dies he becomes a hostile invader instead of finishing peacefully.",
     3403u},
    // fail_flag 3623 = Yura dead/gone (EMEVD 90005702 death handler, entity
    // 1049530700; SetNetworkconnectedEventFlagID + SaveRequest -> persistent).
    // Death-distinct (his thread ends when he's killed/usurped by Shabriri).
    {"Yura, Bloody Finger Hunter", "Yura's Quest", "Crosses Shabriri/Eleonora; touches Hyetta", steps_yura, 4, false,
     "Shabriri usurps him late; some steps gate behind area progress.", 3623u},
    // COBAYE (Part 2): fail_flag = 1042369205 = a PERSISTED Varre-death flag,
    // found by diffing a Varre-ALIVE save vs the Varre-DEAD save (er-save-lib
    // errflags). LESSON: the Event-flag hook also logs TRANSIENT flags (1042365008
    // / 3082 fired at the kill but never persisted) -- for "unfinishable" use a
    // SAVE-PERSISTED flag. Other persisted death candidates from the same diff:
    // 1042365009 / 1042365010 / 1042365027. To disambiguate death-vs-completion,
    // re-check against a quest-COMPLETED save (must not also set on normal finish).
    {"White Mask Varre", "Varre's Quest", "Mohg / Bloody Finger path", steps_varre, 5, false,
     nullptr, 1042369205u},
    {"Hyetta", "Hyetta's Quest", "Frenzied Flame; crosses Shabriri/Yura", steps_hyetta, 4, false,
     "Frenzied Flame path -- the final step is a point of no return that changes your ending."},
    // name_id 122000 = "Alexander, Warrior Jar" FMG NpcName id (data/npc_name_text_map.json).
    // progress_flag/entity_id at default 0 for every step -- see the Boc entry above for why.
    {"Iron Fist Alexander", "Alexander's Quest", "Gives Alexander's Innards to Jar-Bairn", steps_alexander, 5, false,
     "Free him at each spot before that area's story moves on, or you can miss a step.",
     0u, false, 122000u},
    // fail_flag 3443 = Diallos dead/gone (EMEVD 90005702 death handler, entity
    // 1039440710 at Jarburg; persistent). Death-distinct (he falls defending Jarburg).
    {"Diallos", "Diallos's Quest", "Crosses Jar-Bairn (Jarburg)", steps_diallos, 5, false, nullptr, 3443u},
    {"Jar-Bairn", "Jar-Bairn's Quest", "Crosses Diallos and Alexander (Jarburg)", steps_jarbairn, 4, false,
     "His outcome is tied to Diallos and to giving Alexander's Innards -- order matters across the three."},
    {"Latenna", "Latenna's Quest", "Albinauric / Haligtree path", steps_latenna, 4, false,
     "Needs the right Haligtree medallion half from Albus first."},
    // name_id 133300 = "Sorcerer Thops" FMG NpcName id (data/npc_name_text_map.json).
    // entity_id now MSB-sourced for steps 1/2/3 (see steps_thops above); step 4 (his
    // corpse) stays 0. The candidate entity_id 1039390700 from the fail_flag block was
    // RESOLVED via tile_region_map.json to Liurnia (Church of Irith) = step 1, and is
    // wired there. progress_flag stays 0 for all steps (needs game/EMEVD, offline-blocked).
    {"Sorcerer Thops", "Thops's Quest", nullptr, steps_thops, 4, false, nullptr, 3803u, true, 133300u},
    // fail_flag 1051430800 = Gurranq dead (EMEVD 90005860 boss death handler,
    // entity 1051430800 at the Bestial Sanctum; flag id == entity id, persistent).
    {"Gurranq, Beast Clergyman", "Gurranq's Quest", "Deathroot deliveries", steps_gurranq, 4, false, nullptr, 1051430800u},
    {"Dung Eater", "Dung Eater's Quest", "Crosses Nepheli (Seedbed Curses)", steps_dungeater, 4, false,
     "Mutually exclusive ending -- empower him OR hunt him down, not both."},
    // fail_flag 3883 = Bernahl dead/gone (= bernahl_q99). His CharacterDead(16000800)
    // boss death (Farum Azula) gates the persistent conclusion flag 3883 set in
    // common.emevd. Death-distinct (recusant turn ends his recruitable thread).
    {"Knight Bernahl", "Bernahl's Quest", "Volcano Manor / Recusant", steps_bernahl, 4, false, nullptr, 3883u},
    {"Tanith (Volcano Manor)", "Volcano Manor (Tanith)", "Hub of Volcano Manor (Rya, Bernahl)", steps_tanith, 4},
    {"Vyke", "Vyke's Quest", "Bloody Finger invader / Mountaintops boss", steps_vyke, 3},

    // ── 14 Shadow of the Erdtree DLC questlines ──────────────────────────
    // DLC follower fail_flags found via the TalkESD pipeline (tools/mine_talkesd_flags
    // + esdtool decompile): each follower's talk-template call uses flag1 = the
    // "dead/gone" flag (bootstrapped from Patches flag1=3683=dead). Confirmed
    // persistent in EMEVD as networkconnected blocks 44X0-44X3 (dead = 44X3); Ansbach
    // & Freyja also have explicit 90005702 death handlers at m21_01 (the Leda fight).
    {"Needle Knight Leda", "Leda's Quest", "Hub of the DLC group; converges at Enir-Ilim", steps_leda, 5, true,
     "Hub of the followers -- siding with Leda at Shadow Keep cuts down Hornsent/Ansbach and locks their branches.",
     4443u},
    {"Hornsent", "Hornsent's Quest", "Leda's group; gold/red summon at Shadow Keep", steps_hornsent, 5, true,
     "At Shadow Keep, siding with Leda (red sign) kills him and locks his rewards.", 4363u},
    {"Redmane Freyja", "Freyja's Quest", "Leda's group; crosses Ansbach (letter)", steps_freyja, 5, true,
     "Speak with her BEFORE giving Ansbach the Secret Rite Scroll, or her questline is locked out.", 4423u},
    {"Sir Ansbach", "Ansbach's Quest", "Leda's group (Mohg's servant); crosses Freyja", steps_ansbach, 6, true,
     "Giving the Secret Rite Scroll before speaking to Freyja locks HER quest; the Leda fight side-choice decides if he lives.",
     4403u},
    {"Moore", "Moore's Quest", "Leda's group; crosses Thiollier (Black Syrup)", steps_moore, 5, true,
     "His answer to 'how to face sorrow' decides whether he turns hostile at Enir-Ilim."},
    {"Thiollier", "Thiollier's Quest", "Leda's group; crosses St. Trina / Florissax", steps_thiollier, 5, true,
     "Reach St. Trina before he leaves; his Concoction is also needed for Florissax's true reward.", 4463u},
    {"Fire Knight Queelign", "Queelign's Quest", "Messmer's flame; Iris of Grace/Occultation", steps_queelign, 3, true,
     "Exclusive reward: Iris of Grace gives his spirit ash, Iris of Occultation gives his weapon -- not both."},
    {"Igon", "Igon's Quest", "Bayle the Dread; crosses Florissax (concoction)", steps_igon, 5, true,
     "Summon him at Bayle's arena before finishing the fight, or you miss his send-off."},
    {"Hornsent Grandam", "Grandam's Quest", "Belurat storeroom (NOT the Hornsent companion)", steps_grandam, 3, true},
    {"Dryleaf Dane", "Dane's Quest", "Leda's group; bare-handed duel at Moorth", steps_dane, 5, true, nullptr, 4563u},
    {"Dragon Communion Priestess", "Florissax's Quest", "Dragon path; crosses Igon and Thiollier", steps_florissax, 4, true,
     "Her true reward needs Thiollier's Concoction given at night BEFORE you kill Bayle."},
    {"Count Ymir, High Priest", "Ymir's Quest", "Manus Metyr / Finger questline; crosses Jolan", steps_ymir, 6, true,
     "Runs parallel to Jolan -- finish the Finger ruins and Metyr before his final turn."},
    {"Swordhand of Night Jolan", "Jolan's Quest", "Ymir's guard; crosses Anna and Rakshasa", steps_jolan, 4, true,
     "Parallel to Ymir; the Iris choice (Grace vs Occultation) is exclusive."},
    {"St. Trina", "St. Trina's Quest", "Crosses Thiollier (Stone Coffin Fissure)", steps_sttrina, 3, true},
};
const size_t QUEST_BROWSER_COUNT = sizeof(QUEST_BROWSER) / sizeof(QUEST_BROWSER[0]);

} // namespace goblin::generated

namespace goblin
{
bool quest_step_done(const generated::NpcQuest &q, size_t s)
{
    if (s >= q.step_count)
        return false;
    if (uint32_t flag = q.steps[s].progress_flag)
        return goblin::ui::read_event_flag(flag);
    // Manual ini blob: "name=bits;name2=bits2;..." one '0'/'1' char per step. Assumes
    // the modern keyed format -- the Quest Browser UI (goblin_overlay.cpp) one-shot
    // migrates an OLD un-keyed global bit-string blob to this format the first time it
    // runs each session, but only writes it back on an edit. A legacy blob that hasn't
    // been migrated yet this session won't parse here (no '=' found -> empty result for
    // every questline, same as "nothing done"); opening the Quest Browser once fixes it.
    const std::string &blob = goblin::config::questProgress;
    const std::string key = std::string(q.name) + "=";
    size_t p = blob.find(key);
    if (p == std::string::npos)
        return false;
    p += key.size();
    size_t semi = blob.find(';', p);
    size_t end = (semi == std::string::npos) ? blob.size() : semi;
    return s < (end - p) && blob[p + s] == '1';
}
} // namespace goblin

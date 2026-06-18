// HAND-AUTHORED quest-step data for the overlay Quest Browser.
// Original descriptions written for this project (quest FACTS — locations, step
// order, interconnections — are not copyrightable; we copy no third-party prose).
//
// COVERAGE: 36 base-game + 14 DLC = 50 questlines. Entries with step_count==0 are
// PLACEHOLDERS, filled one NPC at a time (see memory: quest-browser strategy).
#include "goblin_quest_steps.hpp"

namespace goblin::generated
{

// ── authored step tables (original wording; quest facts only) ────────────
static const QuestStep steps_boc[] = {
    {"Free Boc", "A voice cries from a bush by the road in western Limgrave. Strike the bush to free the demi-human Boc, who turns out to be a skilled tailor.", "Limgrave"},
    {"Coastal Cave", "He relocates to the mouth of the Coastal Cave on Limgrave's west shore.", "Limgrave"},
    {"Lake-facing Cliffs", "Boc next settles by the Lake-facing Cliffs in eastern Liurnia and offers to alter and reinforce your garments.", "Liurnia"},
    {"Tailoring tools", "Bring him the Sewing Needle and the Iron/Gold Tailoring Tools so he can do heavier alterations.", "Liurnia"},
    {"Altus Plateau", "He moves on again, found resting along the Altus Plateau highway.", "Altus Plateau"},
    {"Resolve his wish", "Console Boc with the 'You're Beautiful Too' gesture, or grant a Larval Tear, to finish his story.", "Altus Plateau"},
};
static const QuestStep steps_thops[] = {
    {"Meet Thops", "Find Sorcerer Thops resting at the Church of Irith in eastern Liurnia; he longs to enter the Academy but lacks a key.", "Liurnia"},
    {"Academy Glintstone Key", "Bring him a spare Academy Glintstone Key so he can pass the Academy's seal.", "Liurnia"},
    {"Schoolhouse Classroom", "He relocates to the Schoolhouse Classroom deep within Raya Lucaria Academy.", "Raya Lucaria"},
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
static const QuestStep steps_alexander[] = {
    {"Stuck in Stormhill", "Free Alexander, the Warrior Jar, from a hole in northern Stormhill by striking him.", "Limgrave"},
    {"Gael Tunnel", "Find him wedged before a door in Gael Tunnel and free him again.", "Caelid"},
    {"Radahn Festival", "Meet him at Redmane Castle, eager for the battle against Radahn.", "Caelid"},
    {"Lava pot", "Free him once more from a hole on a lava slope of Mt. Gelmir.", "Mt. Gelmir"},
    {"His end", "Find him dying at Crumbling Farum Azula; the duel yields Alexander's Innards and the Shard of Alexander talisman.", "Crumbling Farum Azula"},
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

// ── 36 base-game questlines ──────────────────────────────────────────────
const NpcQuest QUEST_BROWSER[] = {
    // Ranni's questline (interconnected cluster)
    {"Ranni the Witch", "Ranni's Quest", "Hub of the Blaidd/Iji/Seluvis cluster", steps_ranni, 6},
    {"Blaidd", "Blaidd's Quest", "Part of Ranni's questline", steps_blaidd, 4},
    {"Iji", "Iji's Quest", "Part of Ranni's questline", steps_iji, 3},
    {"Seluvis", "Seluvis's Quest", "Part of Ranni's questline; crosses Nepheli", steps_seluvis, 4},
    // Sellen
    {"Sorceress Sellen", "Sellen's Quest", "Crosses Jerren, Lusat/Azur", steps_sellen, 5},
    {"Witch-Hunter Jerren", "Jerren's Quest", "Sellen's quest finale (Sellen vs Jerren)", steps_jerren, 3},
    // Roundtable / Roderika
    {"Roderika", "Roderika's Quest", "Crosses Hewg (Stormhill -> Roundtable)", steps_roderika, 4},
    {"Smithing Master Hewg", "Hewg's Quest", "Crosses Roderika", steps_hewg, 3},
    {"Nepheli Loux", "Nepheli's Quest", "Crosses Kenneth, Gideon, Dung Eater", steps_nepheli, 5},
    {"Kenneth Haight", "Kenneth's Quest", "Feeds Nepheli's claim to Limgrave", steps_kenneth, 3},
    {"Gideon Ofnir", nullptr, "Touches many quests (Roundtable info-broker)", nullptr, 0},
    // Deathbed / Black Knife cluster
    {"Fia, Deathbed Companion", "Fia's Quest", "Crosses D and Rogier (Deathroot/Godwyn)", steps_fia, 5},
    {"D, Hunter of the Dead", "D's Quest", "Crosses Fia; D's brother continues it", steps_dhunter, 3},
    {"Sorcerer Rogier", "Rogier's Quest", "Feeds Fia's quest (Black Knife)", steps_rogier, 4},
    // Golden Order
    {"Goldmask", "Goldmask (Corhyn's Quest)", "Part of Corhyn's questline", steps_goldmask, 4},
    {"Brother Corhyn", "Corhyn's Quest", "Searches for Goldmask", steps_corhyn, 5},
    // Millicent
    {"Millicent", "Millicent's Quest", "Started by Gowry (Scarlet Rot)", steps_millicent, 5},
    {"Sage Gowry", "Gowry's Quest", "Starts Millicent's quest", steps_gowry, 3},
    // Standalone-ish
    {"Rya", "Rya's Quest", "Leads into Volcano Manor (Tanith)", steps_rya, 3},
    {"Boc the Seamster", "Boc's Quest", nullptr, steps_boc, 6},
    {"Patches", "Patches' Quest", "Joins Volcano Manor (Tanith)", steps_patches, 5},
    {"Irina", "Irina's Quest", "Crosses Edgar (Castle Morne)", steps_irina, 3},
    {"Edgar", "Edgar's Quest", "Crosses Irina (Castle Morne)", steps_edgar, 3},
    {"Yura, Bloody Finger Hunter", "Yura's Quest", "Crosses Shabriri/Eleonora; touches Hyetta", steps_yura, 4},
    {"White Mask Varre", "Varre's Quest", "Mohg / Bloody Finger path", steps_varre, 5},
    {"Hyetta", "Hyetta's Quest", "Frenzied Flame; crosses Shabriri/Yura", steps_hyetta, 4},
    {"Iron Fist Alexander", "Alexander's Quest", "Gives Alexander's Innards to Jar-Bairn", steps_alexander, 5},
    {"Diallos", "Diallos's Quest", "Crosses Jar-Bairn (Jarburg)", steps_diallos, 5},
    {"Jar-Bairn", "Jar-Bairn's Quest", "Crosses Diallos and Alexander (Jarburg)", steps_jarbairn, 4},
    {"Latenna", "Latenna's Quest", "Albinauric / Haligtree path", steps_latenna, 4},
    {"Sorcerer Thops", "Thops's Quest", nullptr, steps_thops, 4},
    {"Gurranq, Beast Clergyman", "Gurranq's Quest", "Deathroot deliveries", steps_gurranq, 4},
    {"Dung Eater", "Dung Eater's Quest", "Crosses Nepheli (Seedbed Curses)", steps_dungeater, 4},
    {"Knight Bernahl", "Bernahl's Quest", "Volcano Manor / Recusant", steps_bernahl, 4},
    {"Tanith (Volcano Manor)", "Volcano Manor (Tanith)", "Hub of Volcano Manor (Rya, Bernahl)", steps_tanith, 4},
    {"Vyke", nullptr, "Roundtable / Mohg path", nullptr, 0},

    // ── 14 Shadow of the Erdtree DLC questlines ──────────────────────────
    {"Needle Knight Leda", nullptr, "Hub of the DLC group; converges at Enir-Ilim", nullptr, 0},
    {"Hornsent", nullptr, "Leda's group", nullptr, 0},
    {"Redmane Freyja", nullptr, "Leda's group", nullptr, 0},
    {"Sir Ansbach", nullptr, "Leda's group (Mohg's servant)", nullptr, 0},
    {"Moore", nullptr, "Leda's group", nullptr, 0},
    {"Thiollier", nullptr, "Leda's group; crosses St. Trina", nullptr, 0},
    {"Fire Knight Queelign", nullptr, nullptr, nullptr, 0},
    {"Igon", nullptr, "Bayle the Dread (crosses Dragon Communion Priestess?)", nullptr, 0},
    {"Hornsent Grandam", nullptr, "Bonny Village (NOT the Hornsent companion)", nullptr, 0},
    {"Dryleaf Dane", nullptr, "Leda's group", nullptr, 0},
    {"Dragon Communion Priestess", nullptr, "Florissax / dragon path; crosses Igon", nullptr, 0},
    {"Count Ymir, High Priest", nullptr, "Manus Metyr / Finger questline", nullptr, 0},
    {"Swordhand of Night Jolan", nullptr, "Crosses Rakshasa", nullptr, 0},
    {"St. Trina", nullptr, "Crosses Thiollier", nullptr, 0},
};
const size_t QUEST_BROWSER_COUNT = sizeof(QUEST_BROWSER) / sizeof(QUEST_BROWSER[0]);

} // namespace

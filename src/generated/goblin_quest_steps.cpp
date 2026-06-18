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

// ── 36 base-game questlines ──────────────────────────────────────────────
const NpcQuest QUEST_BROWSER[] = {
    // Ranni's questline (interconnected cluster)
    {"Ranni the Witch", nullptr, "Hub of the Blaidd/Iji/Seluvis cluster", nullptr, 0},
    {"Blaidd", nullptr, "Part of Ranni's questline", nullptr, 0},
    {"Iji", nullptr, "Part of Ranni's questline", nullptr, 0},
    {"Seluvis", nullptr, "Part of Ranni's questline; crosses Gideon", nullptr, 0},
    // Sellen
    {"Sorceress Sellen", nullptr, "Crosses Jerren, Lusat/Azur", nullptr, 0},
    {"Witch-Hunter Jerren", nullptr, "Sellen's quest finale (Sellen vs Jerren)", nullptr, 0},
    // Roundtable / Roderika
    {"Roderika", nullptr, "Crosses Hewg (Stormhill -> Roundtable)", nullptr, 0},
    {"Smithing Master Hewg", nullptr, "Crosses Roderika", nullptr, 0},
    {"Nepheli Loux", nullptr, "Crosses Kenneth, Gideon, Dung Eater", nullptr, 0},
    {"Kenneth Haight", "Kenneth's Quest", "Feeds Nepheli's claim to Limgrave", steps_kenneth, 3},
    {"Gideon Ofnir", nullptr, "Touches many quests (Roundtable info-broker)", nullptr, 0},
    // Deathbed / Black Knife cluster
    {"Fia, Deathbed Companion", nullptr, "Crosses D and Rogier (Deathroot/Godwyn)", nullptr, 0},
    {"D, Hunter of the Dead", nullptr, "Crosses Fia; D's brother continues it", nullptr, 0},
    {"Sorcerer Rogier", nullptr, "Feeds Fia's quest (Black Knife)", nullptr, 0},
    // Golden Order
    {"Goldmask", nullptr, "Part of Corhyn's questline", nullptr, 0},
    {"Brother Corhyn", nullptr, "Searches for Goldmask", nullptr, 0},
    // Millicent
    {"Millicent", "Millicent's Quest", "Started by Gowry (Scarlet Rot)", steps_millicent, 5},
    {"Sage Gowry", "Gowry's Quest", "Starts Millicent's quest", steps_gowry, 3},
    // Standalone-ish
    {"Rya", "Rya's Quest", "Leads into Volcano Manor (Tanith)", steps_rya, 3},
    {"Boc the Seamster", "Boc's Quest", nullptr, steps_boc, 6},
    {"Patches", "Patches' Quest", "Joins Volcano Manor (Tanith)", steps_patches, 5},
    {"Irina", "Irina's Quest", "Crosses Edgar (Castle Morne)", steps_irina, 3},
    {"Edgar", "Edgar's Quest", "Crosses Irina (Castle Morne)", steps_edgar, 3},
    {"Yura, Bloody Finger Hunter", nullptr, "Crosses Shabriri/Eleonora; touches Hyetta", nullptr, 0},
    {"White Mask Varre", nullptr, "Mohg / Bloody Finger path", nullptr, 0},
    {"Hyetta", nullptr, "Frenzied Flame; crosses Shabriri/Yura", nullptr, 0},
    {"Iron Fist Alexander", "Alexander's Quest", "Gives Alexander's Innards to Jar-Bairn", steps_alexander, 5},
    {"Diallos", "Diallos's Quest", "Crosses Jar-Bairn (Jarburg)", steps_diallos, 5},
    {"Jar-Bairn", "Jar-Bairn's Quest", "Crosses Diallos and Alexander (Jarburg)", steps_jarbairn, 4},
    {"Latenna", "Latenna's Quest", "Albinauric / Haligtree path", steps_latenna, 4},
    {"Sorcerer Thops", "Thops's Quest", nullptr, steps_thops, 4},
    {"Gurranq, Beast Clergyman", "Gurranq's Quest", "Deathroot deliveries", steps_gurranq, 4},
    {"Dung Eater", nullptr, "Crosses Nepheli (Seedbed Curses)", nullptr, 0},
    {"Knight Bernahl", nullptr, "Volcano Manor / Recusant", nullptr, 0},
    {"Tanith (Volcano Manor)", nullptr, "Hub of Volcano Manor (Rya, Bernahl)", nullptr, 0},
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

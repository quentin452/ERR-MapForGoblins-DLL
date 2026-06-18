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
    {"Kenneth Haight", nullptr, "Feeds Nepheli's quest (Fort Haight/Godrick)", nullptr, 0},
    {"Gideon Ofnir", nullptr, "Touches many quests (Roundtable info-broker)", nullptr, 0},
    // Deathbed / Black Knife cluster
    {"Fia, Deathbed Companion", nullptr, "Crosses D and Rogier (Deathroot/Godwyn)", nullptr, 0},
    {"D, Hunter of the Dead", nullptr, "Crosses Fia; D's brother continues it", nullptr, 0},
    {"Sorcerer Rogier", nullptr, "Feeds Fia's quest (Black Knife)", nullptr, 0},
    // Golden Order
    {"Goldmask", nullptr, "Part of Corhyn's questline", nullptr, 0},
    {"Brother Corhyn", nullptr, "Searches for Goldmask", nullptr, 0},
    // Millicent
    {"Millicent", nullptr, "Started by Gowry (Scarlet Rot)", nullptr, 0},
    {"Sage Gowry", nullptr, "Starts Millicent's quest", nullptr, 0},
    // Standalone-ish
    {"Rya", nullptr, "Leads into Volcano Manor", nullptr, 0},
    {"Boc the Seamster", "Boc's Quest", nullptr, steps_boc, 6},
    {"Patches", "Patches' Quest", "Joins Volcano Manor (Tanith)", steps_patches, 5},
    {"Irina", nullptr, "Crosses Edgar (Castle Morne)", nullptr, 0},
    {"Edgar", nullptr, "Crosses Irina (Castle Morne)", nullptr, 0},
    {"Yura, Bloody Finger Hunter", nullptr, "Crosses Shabriri/Eleonora; touches Hyetta", nullptr, 0},
    {"White Mask Varre", nullptr, "Mohg / Bloody Finger path", nullptr, 0},
    {"Hyetta", nullptr, "Frenzied Flame; crosses Shabriri/Yura", nullptr, 0},
    {"Iron Fist Alexander", nullptr, "Crosses Jar-Bairn (Alexander's Innards)", nullptr, 0},
    {"Diallos", nullptr, "Crosses Jar-Bairn (Jarburg)", nullptr, 0},
    {"Jar-Bairn", nullptr, "Crosses Diallos and Alexander (Jarburg)", nullptr, 0},
    {"Latenna", nullptr, "Albinauric / Haligtree path", nullptr, 0},
    {"Sorcerer Thops", "Thops's Quest", nullptr, steps_thops, 4},
    {"Gurranq, Beast Clergyman", nullptr, "Deathroot deliveries", nullptr, 0},
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

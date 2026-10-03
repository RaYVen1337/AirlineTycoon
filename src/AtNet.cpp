//============================================================================================
// AtNet.cpp : Handling the Network things:
//============================================================================================
// Link: "AtNet.h"
//============================================================================================
#include "AtNet.h"

#include "Buero.h"
#include "global.h"
#include "helper.h"
#include "network.h"
#include "Proto.h"
#include "SbLib.h"
#include <algorithm>
#include <random>

#include <cmath>
#include <exception>
#include <vector>

#define AT_Log(...) AT_Log_I("AtNet", __VA_ARGS__)

extern SBNetwork gNetwork;

#define GFX_MENU (0x00000000554e454d)

//--------------------------------------------------------------------------------------------
// Validation of values received from the network. A corrupt or malicious message must never
// crash the game: log it and drop the message instead.
//--------------------------------------------------------------------------------------------
static inline bool isValidPlayerNum(SLONG PlayerNum) { return PlayerNum >= 0 && PlayerNum < 4 && PlayerNum < Sim.Players.Players.AnzEntries(); }

// Number of player blocks inside of the multi-player sync messages
static inline bool isValidPlayerCount(SLONG Anz) { return Anz >= 0 && Anz <= 4; }

// True, if the id (or index) can be handed to operator[] / operator() of the album without throwing.
// Mirrors the lookup logic of ALBUM_V::find():
template <typename T> static inline bool isValidAlbumRef(const T &Album, SLONG IdOrIndex) {
    if (IdOrIndex < 0) {
        return false;
    }
    if (static_cast<ULONG>(IdOrIndex) >= 0x1000000) {
        return Album.IsInAlbum(static_cast<ULONG>(IdOrIndex)) != 0;
    }
    return IdOrIndex < Album.AnzEntries();
}

// Resolves a route id or a route index received from the network to a valid index into Routen / RentRouten:
static bool resolveRouteIndex(PLAYER &qPlayer, SLONG RouteIdOrIndex, SLONG &RouteIndex) {
    if (!isValidAlbumRef(Routen, RouteIdOrIndex)) {
        return false;
    }

    RouteIndex = Routen(static_cast<ULONG>(RouteIdOrIndex));
    return RouteIndex >= 0 && RouteIndex < Routen.AnzEntries() && RouteIndex < qPlayer.RentRouten.RentRouten.AnzEntries();
}

// Returns the person that represents a player on the airport, or nullptr if there is none:
static PERSON *getPlayerPerson(SLONG PlayerNum) {
    ULONG Index = Sim.Persons.GetPlayerIndex(PlayerNum);

    if (Index == 0xffffffffU || Sim.Persons.IsInAlbum(Index) == 0) {
        return nullptr;
    }
    return &Sim.Persons[Index];
}

// The victim of a sabotage: a player or -1 (none, see SIM::NewDay)
static bool isValidVictim(SLONG Opfer) { return Opfer == -1 || isValidPlayerNum(Opfer); }

// A cell of the airport floor (Airport.iPlate), as used for the walking targets of the players.
// The walking code also touches the neighbour cells (x +-2), so the outermost cells are not accepted.
static bool isValidPlateXY(XY Cell) {
    return Cell.x >= 2 && Cell.x < Airport.PlateDimension.x - 2 && Cell.y >= 0 && Cell.y < Airport.PlateDimension.y && Cell.y < 16;
}

// A pixel position of a person in the airport, converted to a cell like PLAYER::UpdateWaypointWalkingDirection does:
static bool isValidAirportPos(XY Pos) {
    if (Pos.x < -4400 || Pos.x > 1000000 || Pos.y < -2200 || Pos.y > 1000000) {
        return false; // keeps the conversion below free of overflows
    }
    XY Cell;
    Cell.x = (Pos.x + 4400) / 44 - 100;
    if (Pos.y > 0 && Pos.y < 5000) {
        Cell.y = (Pos.y + 2200) / 22 - 100 + 5;
    } else {
        Cell.y = (Pos.y - 5000 + 2200) / 22 - 100;
    }
    return isValidPlateXY(Cell);
}

static void LogBadNetMessage(ULONG MessageType, const char *What, SLONG Value) {
    AT_Log("Dropping network message 0x%lx: invalid %s (%li)", static_cast<unsigned long>(MessageType), What, static_cast<long>(Value));
}

// Leaves the current case of the message switch (or the current while loop of a multi-player block) if the condition is not met:
#define NET_CHECK(cond, what, value)                                                                                                                           \
    if (!(cond)) {                                                                                                                                             \
        LogBadNetMessage(MessageType, what, static_cast<SLONG>(value));                                                                                        \
        break;                                                                                                                                                 \
    }

#define NET_CHECK_PLAYER(PlayerNum) NET_CHECK(isValidPlayerNum(PlayerNum), "PlayerNum", PlayerNum)

// The message ended too early or contained an implausible size prefix:
#define NET_CHECK_READ()                                                                                                                                       \
    if (Message.HasReadError()) {                                                                                                                              \
        AT_Log("Dropping network message 0x%lx: message too short or invalid size prefix", static_cast<unsigned long>(MessageType));                           \
        break;                                                                                                                                                 \
    }

SLONG nPlayerOptionsOpen[4] = {0, 0, 0, 0};  // Fummelt gerade wer an den Options?
SLONG nPlayerAppsDisabled[4] = {0, 0, 0, 0}; // Ist ein anderer Spieler gerade in einer anderen Anwendung?
SLONG nPlayerWaiting[4] = {0, 0, 0, 0};      // Hinkt jemand hinterher?

extern SLONG gTimerCorrection; // Is it necessary to adapt the local clock to the server clock?

// Zum Debuggen:
SLONG rChkTime = 0;
ULONG rChkPersonRandCreate = 0, rChkPersonRandMisc = 0, rChkHeadlineRand = 0;
ULONG rChkLMA = 0, rChkRBA = 0, rChkAA[MAX_CITIES], rChkFrachen = 0;
SLONG rChkGeneric, CheckGeneric = 0;
SLONG rChkActionId[5 * 4];

SLONG GenericSyncIds[4] = {0, 0, 0, 0};
SLONG GenericSyncIdPars[4] = {0, 0, 0, 0};
SLONG GenericAsyncIds[4 * 100] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
SLONG GenericAsyncIdPars[4 * 100] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

//--------------------------------------------------------------------------------------------
// Sets the bitmap for the network to display: (0=none; 1=player in options, 2=player in windows; 3=waiting for player)
//--------------------------------------------------------------------------------------------
void SetNetworkBitmap(SLONG Number, SLONG WaitingType) {
    static SLONG CurrentNumber = -1;

    if (CurrentNumber != Number || gNetworkBmsType != WaitingType) {
        GfxLib *pGLib = nullptr;

        if (Number == 0) {
            gNetworkBms.Destroy();
            if (pGLib != nullptr) {
                pGfxMain->ReleaseLib(pGLib);
            }
        }

        try {
            if (Number == 1) {
                pGfxMain->LoadLib(const_cast<char *>((LPCTSTR)FullFilename("network1.gli", GliPath)), &pGLib, L_LOCMEM);
            }
            if (Number == 2) {
                pGfxMain->LoadLib(const_cast<char *>((LPCTSTR)FullFilename("network2.gli", GliPath)), &pGLib, L_LOCMEM);
            }
            if (Number == 3) {
                pGfxMain->LoadLib(const_cast<char *>((LPCTSTR)FullFilename("network3.gli", GliPath)), &pGLib, L_LOCMEM);
            }
        } catch (TeakLibException &e) {
            AT_Log_I("NET", "Did not find gli file, trying to continue", e.what());
            e.caught();
        }

        if (pGLib != nullptr) {
            if (Number == 3) {
                gNetworkBms.ReSize(pGLib, "MENU PL0 PL1 PL2 PL3 PL4 PL5 PL6 PL7");
            } else {
                gNetworkBms.ReSize(pGLib, "MENU");
            }
        }

        CurrentNumber = Number;
        gNetworkBmsType = WaitingType;
    }
}

//--------------------------------------------------------------------------------------------
// Look for new messages:
//--------------------------------------------------------------------------------------------
void DisplayBroadcastMessage(CString str, SLONG FromPlayer) {
    SBBM TempBm(gBroadcastBm.Size);
    SLONG sy = 0;
    SLONG oldy = 0;
    SLONG offy = 0;

    // if (!Sim.bNetwork)
    //    return;

    if (FromPlayer != Sim.localPlayer) {
        static SLONG LastTime = 0;

        if (AtGetTime() - LastTime > 500) {
            PlayUniversalFx("netmsg.raw", Sim.Options.OptionEffekte);
        }

        LastTime = AtGetTime();
    }

    if (FromPlayer >= 0 && FromPlayer < 4) {
        str = Sim.Players.Players[FromPlayer].NameX + ": " + str;
    }

    sy = gBroadcastBm.TryPrintAt(str.c_str(), FontSmallBlack, TEC_FONT_LEFT, XY(10, 10), XY(320, 1000));
    oldy = gBroadcastBm.Size.y;
    offy = gBroadcastBm.Size.y;

    if (oldy < 10) {
        oldy = 10;
    }
    if (offy < 10) {
        offy = 10;
    }

    TempBm.BlitFrom(gBroadcastBm);
    gBroadcastBm.ReSize(320, oldy + sy + 10);
    if (gBroadcastBm.Size.y != TempBm.Size.y) {
        SB_CBitmapKey Key(*XBubbleBms[9].pBitmap);
        gBroadcastBm.FillWith(*static_cast<UWORD *>(Key.Bitmap));
    }
    gBroadcastBm.BlitFrom(TempBm);

    TempBm.ReSize(320, sy + 5);
    {
        SB_CBitmapKey Key(*XBubbleBms[9].pBitmap);
        TempBm.FillWith(*static_cast<UWORD *>(Key.Bitmap));
    }
    TempBm.PrintAt(str, FontSmallBlack, TEC_FONT_LEFT, XY(10, 0), XY(320, 1000));
    gBroadcastBm.BlitFrom(TempBm, 0, offy);

    if (gBroadcastBm.Size.y > 220) {
        SBBM TempBm(gBroadcastBm.Size);

        TempBm.BlitFrom(gBroadcastBm);
        gBroadcastBm.ReSize(gBroadcastBm.Size.x, 220);
        gBroadcastBm.BlitFrom(TempBm, 0, -(TempBm.Size.y - 220));
    }

    gBroadcastTimeout = 600;
}

//--------------------------------------------------------------------------------------------
// Updates the Broadcast Bitmap:
//  bJustForEmergency : if true, then this call will only be used it this function hasn't been
//                      called normally for half a second
//--------------------------------------------------------------------------------------------
void PumpBroadcastBitmap(bool bJustForEmergency) {
    if (gBroadcastBm.Size.y == 0) {
        return;
    }

    static SLONG LastTimeCalled = 0;

    if (bJustForEmergency) {
        if (AtGetTime() - LastTimeCalled < 500) {
            return;
        }
    } else {
        LastTimeCalled = AtGetTime();
    }

    if (gBroadcastTimeout > 0 && gBroadcastBm.Size.y < 200) {
        gBroadcastTimeout--;
    } else if (gBroadcastBm.Size.y != 0) {
        static SLONG p = 0;

        gBroadcastTimeout = 0;

        if (p++ > 2 || gBroadcastBm.Size.y <= 10 || gBroadcastBm.Size.y >= 150) {
            p = 0;

            if (gBroadcastBm.Size.y - 1 <= 0) {
                gBroadcastBm.Destroy();
            } else {
                SBBM TempBm(gBroadcastBm.Size);

                TempBm.BlitFrom(gBroadcastBm);
                gBroadcastBm.ReSize(gBroadcastBm.Size.x, gBroadcastBm.Size.y - 1);
                gBroadcastBm.BlitFrom(TempBm, 0, -1);
            }
        }
    }
}

//--------------------------------------------------------------------------------------------
// /nettestfuzz N: every received game message is additionally replayed N times with random
// damage (bit flips, cut off, bytes inserted/removed = shifted data, garbage tail). The game
// must survive all of them. Only messages that carry game data are used; session control
// (pause, sync points, load/save, dialogs, ...) is left alone so the test keeps running.
//--------------------------------------------------------------------------------------------
static std::vector<std::vector<UBYTE>> gNetFuzzQueue;

static bool NetFuzzIsDataMessage(ULONG MessageType) {
    switch (MessageType) {
    case ATNET_SETSPEED:
    case ATNET_ENTERROOM:
    case ATNET_LEAVEROOM:
    case ATNET_PLAYERPOS:
    case ATNET_PLAYERLOOK:
    case ATNET_PLAYERSTOP:
    case ATNET_ROBOT_EXECUTE:
    case ATNET_SYNC_MONEY:
    case ATNET_SYNC_IMAGE:
    case ATNET_SYNC_ROUTES:
    case ATNET_SYNC_FLAGS:
    case ATNET_SYNC_ITEMS:
    case ATNET_SYNC_PLANES:
    case ATNET_SYNC_OFFICEFLAG:
    case ATNET_SYNC_MEETING:
    case ATNET_SYNCROUTECHANGE:
    case ATNET_SYNCGEHALT:
    case ATNET_SYNCKEROSIN:
    case ATNET_SYNCNUMFLUEGE:
    case ATNET_TAKE_ORDER:
    case ATNET_TAKE_FREIGHT:
    case ATNET_TAKE_ROUTE:
    case ATNET_TAKE_CITY:
    case ATNET_TAKETHING:
    case ATNET_PLAYER_TOOK:
    case ATNET_PLAYER_REFILL:
    case ATNET_BUY_NEW:
    case ATNET_BUY_NEWX:
    case ATNET_BUY_USED:
    case ATNET_SELL_USED:
    case ATNET_PERSONNEL:
    case ATNET_PLANEPROPS:
    case ATNET_FP_UPDATE:
    case ATNET_CHANGEMONEY:
    case ATNET_ADD_EXPLOSION:
    case ATNET_ADD_SYMPATHIE:
    case ATNET_SABOTAGE_ARAB:
    case ATNET_SABOTAGE_DIRECT:
    case ATNET_GIMMICK:
    case ATNET_BODYGUARD:
    case ATNET_CAFFEINE:
    case ATNET_ADVISOR:
    case ATNET_OVERTAKE:
    case ATNET_EXPAND_AIRPORT:
    case ATNET_CHATBROADCAST:
    case ATNET_CHATMESSAGE:
        return true;
    default:
        return false;
    }
}

static void NetFuzzCapture(const UBYTE *pData, ULONG Size) {
    static std::mt19937 Rng(static_cast<unsigned>(gNetTestFuzz * 7919 + gNetTestMode));
    static SLONG Count = 0;

    if (gNetTestFuzz == 0 || pData == nullptr || Size < 4 || Sim.Gamestate != (GAMESTATE_PLAYING | GAMESTATE_WORKING) || Sim.Time < 9 * 60000 || Sim.Time > 18 * 60000) {
        return;
    }
    const ULONG MessageType = pData[0] | (pData[1] << 8) | (pData[2] << 16) | (ULONG(pData[3]) << 24);
    if (!NetFuzzIsDataMessage(MessageType)) {
        return;
    }

    // N > 0: damage the frame as it travels over the wire (message + checksum trailer), so the checksum has to catch it.
    // N < 0: damage the bare message after the checksum test: tests the message handlers themselves.
    const bool bWire = gNetTestFuzz > 0;
    const SLONG Times = std::abs(gNetTestFuzz);
    std::vector<UBYTE> Frame(pData, pData + Size);
    if (bWire) {
        const ULONG Trailer[2] = {0x4B435441, NetChecksum(pData, Size)};
        Frame.insert(Frame.end(), reinterpret_cast<const UBYTE *>(Trailer), reinterpret_cast<const UBYTE *>(Trailer) + sizeof(Trailer));
        Size = static_cast<ULONG>(Frame.size());
    }

    for (SLONG n = 0; n < Times; n++) {
        std::vector<UBYTE> v(Frame);
        const ULONG Body = static_cast<ULONG>(v.size()) - 4; // the message type stays intact
        switch (Rng() % 4) {
        case 0: // flip some bytes
            for (SLONG c = 1 + static_cast<SLONG>(Rng() % 8); c > 0 && Body > 0; c--) {
                v[4 + Rng() % Body] ^= static_cast<UBYTE>(1 + Rng() % 255);
            }
            break;
        case 1: // cut off
            v.resize(4 + (Body > 0 ? Rng() % Body : 0));
            break;
        case 2: // insert or remove 1..7 bytes: everything behind is shifted, like in issue #29
        {
            const ULONG Pos = 4 + (Body > 0 ? Rng() % Body : 0);
            const ULONG Len = 1 + Rng() % 7;
            if ((Rng() & 1) != 0) {
                v.insert(v.begin() + Pos, Len, static_cast<UBYTE>(Rng()));
                v.resize(Size); // same length as before, like a valid frame
            } else {
                v.erase(v.begin() + Pos, v.begin() + std::min<ULONG>(Pos + Len, static_cast<ULONG>(v.size())));
            }
        } break;
        default: // garbage tail
            for (ULONG c = 4 + (Body > 0 ? Rng() % Body : 0); c < v.size(); c++) {
                v[c] = static_cast<UBYTE>(Rng());
            }
            break;
        }
        if (v == Frame) {
            continue; // two flips of the same byte can cancel each other out
        }
        if (bWire) {
            ULONG WireSize = static_cast<ULONG>(v.size());
            if (!NetCheckAndStripChecksum(v.data(), WireSize)) {
                continue; // dropped, as it should be
            }
            AT_Log("NETTEST fuzz: damaged message passed the checksum test");
            v.resize(WireSize);
        }
        gNetFuzzQueue.push_back(std::move(v));
    }

    if (((Count += Times) % 5000) < Times) {
        AT_Log("NETTEST fuzz: %ld damaged messages so far", Count);
    }
}

static bool NetFuzzNext(TEAKFILE &Message) {
    if (gNetFuzzQueue.empty()) {
        return false;
    }
    std::vector<UBYTE> v = std::move(gNetFuzzQueue.back());
    gNetFuzzQueue.pop_back();

    Message.Close();
    Message.MemBuffer.ReSize(0);
    auto *p = new UBYTE[v.size()];
    memcpy(p, v.data(), v.size());
    Message.MemBuffer.ReSize(static_cast<SLONG>(v.size()), p);
    Message.MemBufferUsed = static_cast<ULONG>(v.size());
    Message.MemPointer = 0;
    return true;
}

//--------------------------------------------------------------------------------------------
// Look for new messages:
//--------------------------------------------------------------------------------------------
void PumpNetwork() {
    SLONG c = 0;
    SLONG e = 0; // Universell, können von jedem case verwendet werden.

    if (Sim.bNetwork == 0) {
        return;
    }

    if (Sim.bThisIsSessionMaster && Sim.Time > 9 * 60000 && Sim.Time < 18 * 60000 && (Sim.CallItADay == 0) && (Sim.CallItADayAt == 0)) {
        static DWORD LastTime = 0;

        if (AtGetTime() - LastTime > 1000) {
            SIM::SendSimpleMessage(ATNET_TIMEPING, 0, Sim.TimeSlice);
            LastTime = AtGetTime();
        }
    }

    bool bReturnAfterThisMessage = false;
    while ((gNetwork.GetMessageCount() != 0 || !gNetFuzzQueue.empty()) && !bReturnAfterThisMessage) {
        TEAKFILE Message;

        if (NetFuzzNext(Message) || (SIM::ReceiveMemFile(Message) && (NetFuzzCapture(Message.MemBuffer, Message.MemBufferUsed), true))) {
            ULONG MessageType = 0;
            SLONG Par1 = 0;
            SLONG Par2 = 0;

            Message >> MessageType;
            // AT_Log_I("Net", "Received net event: %s", Translate_ATNET(MessageType));

            // Lookups in albums throw if they are given an id that does not exist. A bad message must not take the whole game down:
            try {
            switch (MessageType) {
            case ATNET_SETSPEED:
                Message >> Par1 >> Par2;
                NET_CHECK_PLAYER(Par1);
                Sim.Players.Players[Par1].GameSpeed = Par2;
                if (Sim.Players.Players[Sim.localPlayer].LocationWin != nullptr) {
                    (Sim.Players.Players[Sim.localPlayer].LocationWin)->StatusCount = 3;
                }
                break;

            case ATNET_FORCESPEED:
                Message >> Par1;
                for (c = 0; c < 4; c++) {
                    Sim.Players.Players[c].GameSpeed = Par1;
                }
                if (Sim.Players.Players[Sim.localPlayer].LocationWin != nullptr) {
                    (Sim.Players.Players[Sim.localPlayer].LocationWin)->StatusCount = 3;
                }
                break;

            case ATNET_READYFORMORNING:
                Message >> Par1;
                NET_CHECK_PLAYER(Par1);
                Sim.Players.Players[Par1].bReadyForMorning = 1;
                break;

            case ATNET_READYFORBRIEFING:
                Message >> Par1;
                NET_CHECK_PLAYER(Par1);
                Sim.Players.Players[Par1].bReadyForBriefing = 1;
                break;

            case ATNET_PAUSE:
                GameFrame::Pause(Sim.bPause == 0);
                break;

            case ATNET_WANNAJOIN:
                if (Sim.bIsHost != 0) {
                    SIM::SendSimpleMessage(ATNET_SORRYFULL, 0);
                }
                break;

            case ATNET_CHATBROADCAST: {
                CString str;
                SLONG from = 0;

                Message >> from >> str;

                DisplayBroadcastMessage(str, from);
            } break;

            case DPSYS_HOST:
                Sim.bIsHost = TRUE;
                DisplayBroadcastMessage(StandardTexte.GetS(TOKEN_MISC, 7000));
                break;

            case DPSYS_SESSIONLOST:
                DisplayBroadcastMessage(StandardTexte.GetS(TOKEN_MISC, 7001));
                for (c = 0; c < 4; c++) {
                    if (Sim.Players.Players[c].Owner == 2) {
                        Sim.Players.Players[c].Owner = 1;
                        Sim.Players.Players[c].GameSpeed = 3;
                        nPlayerOptionsOpen[c] = 0;
                        nPlayerAppsDisabled[c] = 0;
                        nPlayerWaiting[c] = 0;
                    }
                }

                if (Sim.Players.GetAnzHumanPlayers() == 1) {
                    gNetwork.DisConnect();
                    Sim.bNetwork = 0;
                    return;
                }

                nOptionsOpen = 0;
                nAppsDisabled = 0;

                if (nOptionsOpen == 0 && nAppsDisabled == 0 && Sim.bPause == 0) {
                    SetNetworkBitmap(0);
                }
                break;

            case DPSYS_DESTROYPLAYERORGROUP: {
                DWORD dwPlayerType = 0;
                DPID dpId = 0;

                Message >> dwPlayerType >> dpId;

                if (dwPlayerType == DPPLAYERTYPE_PLAYER) {
                    for (c = 0; c < 4; c++) {
                        if (Sim.Players.Players[c].NetworkID == dpId) {
                            nOptionsOpen -= nPlayerOptionsOpen[c];
                            nAppsDisabled -= nPlayerAppsDisabled[c];
                            nWaitingForPlayer -= nPlayerWaiting[c];

                            if (nOptionsOpen < 0) {
                                nOptionsOpen = 0;
                            }
                            if (nAppsDisabled < 0) {
                                nOptionsOpen = 0;
                            }
                            if (nWaitingForPlayer < 0) {
                                nOptionsOpen = 0;
                            }

                            if (nOptionsOpen == 0 && nAppsDisabled == 0 && Sim.bPause == 0) {
                                SetNetworkBitmap(0);
                            }

                            Sim.Players.Players[c].Owner = 1;
                            Sim.Players.Players[c].NetworkID = 0;
                            Sim.Players.Players[c].GameSpeed = 3;
                            DisplayBroadcastMessage(bprintf(StandardTexte.GetS(TOKEN_MISC, 7002), (LPCTSTR)Sim.Players.Players[c].NameX));
                        }
                    }

                    if (Sim.Players.GetAnzHumanPlayers() == 1) {
                        gNetwork.DisConnect();
                        nOptionsOpen = 0;
                        nAppsDisabled = 0;
                        nWaitingForPlayer = 0;
                        SetNetworkBitmap(0);
                        Sim.bNetwork = 0;
                        return;
                    }
                }
            } break;

            case ATNET_OPTIONS:
                Message >> Par1 >> Par2;
                NET_CHECK(Par2 >= 0 && Par2 < 4, "PlayerNum", Par2);
                nOptionsOpen += Par1;
                nPlayerOptionsOpen[Par2] += Par1;
                SetNetworkBitmap(static_cast<SLONG>(nOptionsOpen > 0) * 1);
                break;

            case ATNET_ACTIVATEAPP:
                Message >> Par1 >> Par2;
                NET_CHECK(Par2 >= 0 && Par2 < 4, "PlayerNum", Par2);
                nAppsDisabled += Par1;
                nOptionsOpen += Par1;
                nPlayerOptionsOpen[Par2] += Par1;
                nPlayerAppsDisabled[Par2] += Par1;
                SetNetworkBitmap(static_cast<SLONG>(nOptionsOpen > 0) * 2);
                break;

            case ATNET_TIMEPING:
                Message >> Par1;
                gTimerCorrection = Par1 - Sim.TimeSlice;
                break;

            case ATNET_SETGAMESPEED: {
                SLONG PlayerNum = 0;
                SLONG SyncedServerGameSpeed = 0;
                Message >> SyncedServerGameSpeed >> PlayerNum;

                if (Sim.ServerGameSpeed != SyncedServerGameSpeed) {
                    Sim.ServerGameSpeed = SyncedServerGameSpeed;
                    DisplayBroadcastMessage(bprintf("GameSpeed changed to %i / 7\n", static_cast<int>(std::floor(Sim.ServerGameSpeed / 5)) + 1), PlayerNum);
                }
            } break;

            case ATNET_PLAYERPOS: {
                SLONG PlayerNum = 0;
                SLONG MessageTime = 0;
                SLONG LocalTime = 0;

                Message >> PlayerNum;
                // if (Sim.Players.Players[PlayerNum].Owner!=1) hprintf ("Received Message ATNET_PLAYERPOS (%li)", PlayerNum);
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                PERSON *pPerson = getPlayerPerson(PlayerNum);
                NET_CHECK(pPerson != nullptr, "PlayerNum (no person)", PlayerNum);
                PERSON &qPerson = *pPerson;

                // Read the message data:
                XY PrimaryTarget, SecondaryTarget, TertiaryTarget, Target, Position, ScreenPos;
                SLONG DirectToRoom = 0, ExRoom = 0, WalkSpeed = 0, StatePar = 0;
                BOOL iWalkActive = FALSE;
                UWORD TopLocation = 0;
                UBYTE NewDir = 0, Running = 0, Dir = 0, LookDir = 0, Phase = 0;
                Message >> PrimaryTarget.x >> PrimaryTarget.y;
                Message >> SecondaryTarget.x >> SecondaryTarget.y;
                Message >> TertiaryTarget.x >> TertiaryTarget.y;
                Message >> DirectToRoom >> iWalkActive;
                Message >> TopLocation >> ExRoom;
                Message >> NewDir >> WalkSpeed;
                Message >> Target.x >> Target.y;
                Message >> Position.x >> Position.y;
                Message >> ScreenPos.x >> ScreenPos.y;
                Message >> StatePar >> Running;
                Message >> Dir >> LookDir;
                Message >> Phase;

                if (Message.HasReadError()) {
                    AT_Log("Dropping rest of network message 0x%lx: message too short", static_cast<unsigned long>(MessageType));
                    break;
                }
                // Positions and directions are used as array indices when the player walks:
                NET_CHECK(isValidAirportPos(Position) && isValidAirportPos(Target), "Position", Position.x);
                NET_CHECK(isValidPlateXY(PrimaryTarget) && isValidPlateXY(SecondaryTarget) && isValidPlateXY(TertiaryTarget), "Target", PrimaryTarget.x);
                NET_CHECK(NewDir <= 8 && Dir <= 8 && LookDir <= 9 && WalkSpeed >= 0 && WalkSpeed <= 100, "Dir", Dir);

                qPlayer.PrimaryTarget = PrimaryTarget;
                qPlayer.SecondaryTarget = SecondaryTarget;
                qPlayer.TertiaryTarget = TertiaryTarget;
                qPlayer.DirectToRoom = DirectToRoom;
                qPlayer.iWalkActive = iWalkActive;
                qPlayer.TopLocation = TopLocation;
                qPlayer.ExRoom = ExRoom;
                qPlayer.NewDir = NewDir;
                qPlayer.WalkSpeed = WalkSpeed;
                qPerson.Target = Target;
                qPerson.Position = Position;
                qPerson.ScreenPos = ScreenPos;
                qPerson.StatePar = StatePar;
                qPerson.Running = Running;
                qPerson.Dir = Dir;
                qPerson.LookDir = LookDir;
                qPerson.Phase = Phase;

                qPlayer.UpdateWaypointWalkingDirection();

                /* XY OldPosition = qPerson.Position;

                (Sim.Players.Players[PlayerNum].Owner==1 && PlayerNum==2)
                   {
                   hprintf ("Received Message ATNET_PLAYERPOS (%li) (%li,%li)->(%li,%li) [%li]", PlayerNum, qPerson.Position.x, qPerson.Position.y,
                   qPerson.Target.x, qPerson.Target.y, SLONG(qPlayer.NewDir));

                   if ((OldPosition-qPerson.Position).abs()>100) hprintf ("!!Big Pos Delta. OldPos: (%li,%li) NewPos: (%li,%li)", OldPosition.x, OldPosition.y,
                   qPerson.Position.x, qPerson.Position.y);
                   }*/

                // Message time is different from local time. Adapt data:
                Message >> MessageTime;
                LocalTime = Sim.TimeSlice;

                // No catching up with garbage: a damaged time stamp would replay billions of steps and freeze the game
                if (Message.HasReadError() || MessageTime > LocalTime || MessageTime < LocalTime - 5000) {
                    MessageTime = LocalTime;
                }

                Sim.TimeSlice = MessageTime;
                /*hprintf ("Diff=%li", LocalTime-Sim.TimeSlice);*/
                if (qPlayer.GetRoom() == ROOM_AIRPORT && (qPlayer.IsTalking == 0) &&
                    (qPlayer.LocationWin == nullptr || ((*qPlayer.LocationWin).CurrentMenu != MENU_WC_F && (*qPlayer.LocationWin).CurrentMenu != MENU_WC_M))) {
                    while (Sim.TimeSlice < LocalTime) {
                        qPerson.DoOnePlayerStep();
                        qPlayer.UpdateWaypointWalkingDirection();
                        Sim.TimeSlice++;
                    }
                }

                Sim.TimeSlice = LocalTime;
            } break;

            case ATNET_ADD_EXPLOSION: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                qPlayer.OfficeState = 2;
                qPlayer.pSmack = new CSmack16;
                qPlayer.pSmack->Open("expl.smk");
                PLAYER::NetSynchronizeFlags();

                gUniversalFx.Stop();
                gUniversalFx.ReInit("explode.raw");
                gUniversalFx.Play(DSBPLAY_NOSTOP, Sim.Options.OptionEffekte * 100 / 7);

                Airport.SetConditionBlock(20 + PlayerNum, 1);
            } break;

            case ATNET_CAFFEINE: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                Message >> Sim.Players.Players[PlayerNum].Koffein;
            } break;

            case ATNET_GIMMICK: {
                SLONG PlayerNum = 0;
                SLONG Mode = 0;

                Message >> PlayerNum >> Mode;
                NET_CHECK_PLAYER(PlayerNum);

                PERSON *pPerson = getPlayerPerson(PlayerNum);
                NET_CHECK(pPerson != nullptr, "PlayerNum (no person)", PlayerNum);
                PERSON &qPerson = *pPerson;

                if (Mode == 1) {
                    qPerson.State = qPerson.State & ~PERSON_WAITFLAG;
                    qPerson.LookDir = 9; // Gimmick starten
                    qPerson.Phase = 0;
                    qPerson.Target = qPerson.Position;

                    Sim.Players.Players[PlayerNum].NewDir = 8;
                    Sim.Players.Players[PlayerNum].StandCount = 0;
                } else {
                    Sim.Players.Players[PlayerNum].StandCount = -100;
                    qPerson.Dir = 8;
                    qPerson.LookDir = 2;
                    qPerson.Phase = 0;
                }
            } break;

            case ATNET_PLAYERLOOK: {
                SLONG PlayerNum = 0;
                SLONG Dir = 0;

                Message >> PlayerNum >> Dir;
                NET_CHECK_PLAYER(PlayerNum);

                PERSON *pPerson = getPlayerPerson(PlayerNum);
                NET_CHECK(pPerson != nullptr, "PlayerNum (no person)", PlayerNum);
                pPerson->LookAt(Dir);
            } break;

            case ATNET_PLAYERSTOP: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                Sim.Players.Players[PlayerNum].WalkStopEx();
            } break;

            case ATNET_ENTERROOM: {
                SLONG PlayerNum = 0;
                SLONG RoomEntered = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                Message >> qPlayer.DirectToRoom >> RoomEntered;
                NET_CHECK(RoomEntered == -1 || (RoomEntered >= 0 && RoomEntered < Sim.RoomBusy.AnzEntries()), "RoomEntered", RoomEntered);

                if (RoomEntered != -1) {
                    CTalker *pTalker = nullptr;
                    CTalker *pTalker2 = nullptr;

                    switch (RoomEntered) {
                    case ROOM_AUFSICHT:
                        pTalker = &Talkers.Talkers[TALKER_BOSS];
                        break;
                    case ROOM_ARAB_AIR:
                        pTalker = &Talkers.Talkers[TALKER_ARAB];
                        break;
                    case ROOM_SABOTAGE:
                        pTalker = &Talkers.Talkers[TALKER_SABOTAGE];
                        break;
                    case ROOM_BANK:
                        pTalker = &Talkers.Talkers[TALKER_BANKER1];
                        pTalker2 = &Talkers.Talkers[TALKER_BANKER2];
                        break;
                    case ROOM_MUSEUM:
                        pTalker = &Talkers.Talkers[TALKER_MUSEUM];
                        break;
                    case ROOM_MAKLER:
                        pTalker = &Talkers.Talkers[TALKER_MAKLER];
                        break;
                    case ROOM_WERKSTATT:
                        pTalker = &Talkers.Talkers[TALKER_MECHANIKER];
                        break;
                    case ROOM_WERBUNG:
                        pTalker = &Talkers.Talkers[TALKER_WERBUNG];
                        break;
                    default:
                        break;
                    }

                    if ((pTalker != nullptr) && (pTalker->IsBusy() == 0)) {
                        pTalker->IncreaseLocking();
                    }
                    if ((pTalker2 != nullptr) && (pTalker2->IsBusy() == 0)) {
                        pTalker2->IncreaseLocking();
                    }

                    if (Sim.RoomBusy[RoomEntered] == 0) {
                        Sim.RoomBusy[RoomEntered]++;
                    }
                }

                // if (qPlayer.Owner!=1) hprintf ("Received Message ATNET_ENTERROOM (%li)", PlayerNum);

                for (c = 9; c >= 0; c--) {
                    Message >> qPlayer.Locations[c];
                    // if (qPlayer.Owner!=1) hprintf ("qPlayer.Locations[%li]=%li", c, qPlayer.Locations[c]);
                }

                qPlayer.CalcRoom();
                Sim.UpdateRoomUsage();

                // Bei menschlichen nicht-lokalen Mitspielern eine Fehlerbehandlung:
                if (qPlayer.Owner == 2 && Sim.Time > 9 * 60000) {
                    for (c = 9; c >= 0; c--) {
                        if (qPlayer.GetRoom() == Sim.Players.Players[Sim.localPlayer].Locations[c]) {
                            if (qPlayer.GetRoom() != ROOM_STATISTICS && qPlayer.GetRoom() != ROOM_GLOBE && qPlayer.GetRoom() != ROOM_LAPTOP &&
                                qPlayer.GetRoom() != ROOM_PLANEPROPS && qPlayer.GetRoom() != ROOM_WC_F && qPlayer.GetRoom() != ROOM_WC_M) {
                                SIM::SendSimpleMessage(ATNET_ENTERROOMBAD, qPlayer.NetworkID);
                            }
                        }
                    }
                }
            } break;

            case ATNET_ENTERROOMBAD:
                Sim.Players.Players[Sim.localPlayer].LeaveAllRooms();
                break;

            case ATNET_LEAVEROOM: {
                SLONG PlayerNum = 0;
                SLONG RoomLeft = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                Message >> qPlayer.DirectToRoom >> RoomLeft;
                NET_CHECK(RoomLeft == -1 || (RoomLeft >= 0 && RoomLeft < Sim.RoomBusy.AnzEntries()), "RoomLeft", RoomLeft);

                if (RoomLeft != -1) {
                    switch (RoomLeft) {
                    case ROOM_AUFSICHT:
                        Talkers.Talkers[TALKER_BOSS].DecreaseLocking();
                        break;
                    case ROOM_ARAB_AIR:
                        Talkers.Talkers[TALKER_ARAB].DecreaseLocking();
                        break;
                    case ROOM_SABOTAGE:
                        Talkers.Talkers[TALKER_SABOTAGE].DecreaseLocking();
                        break;
                    case ROOM_BANK:
                        Talkers.Talkers[TALKER_BANKER1].DecreaseLocking();
                        Talkers.Talkers[TALKER_BANKER2].DecreaseLocking();
                        break;
                    case ROOM_MUSEUM:
                        Talkers.Talkers[TALKER_MUSEUM].DecreaseLocking();
                        break;
                    case ROOM_MAKLER:
                        Talkers.Talkers[TALKER_MAKLER].DecreaseLocking();
                        break;
                    case ROOM_WERKSTATT:
                        Talkers.Talkers[TALKER_MECHANIKER].DecreaseLocking();
                        break;
                    case ROOM_WERBUNG:
                        Talkers.Talkers[TALKER_WERBUNG].DecreaseLocking();
                        break;
                    default:
                        break;
                    }

                    if (Sim.RoomBusy[RoomLeft] != 0U) {
                        Sim.RoomBusy[RoomLeft]--;
                    }
                }
                // if (qPlayer.Owner!=1) hprintf ("Received Message ATNET_LEAVEROOM (%li)", PlayerNum);

                for (c = 9; c >= 0; c--) {
                    Message >> qPlayer.Locations[c];
                    // if (qPlayer.Owner!=1) hprintf ("qPlayer.Locations[%li]=%li", c, qPlayer.Locations[c]);
                }

                qPlayer.CalcRoom();
                Sim.UpdateRoomUsage();
            } break;

            case ATNET_CHEAT: {
                SLONG PlayerNum = 0;
                SLONG Cheat = 0;

                Message >> PlayerNum >> Cheat;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                switch (Cheat) {
                case 0:
                    qPlayer.Money += 10000000;
                    break;
                case 1:
                    qPlayer.Credit = 0;
                    break;
                case 2:
                    qPlayer.Image = 1000;
                    break;
                default:
                    break;
                }
            } break;

            case ATNET_SYNC_IMAGE: {
                // The data is read into temporaries first and only applied if the whole message was valid:
                struct SyncedImage {
                    SLONG PlayerNum{};
                    decltype(PLAYER::Image) Image{};
                    decltype(PLAYER::ImageGotWorse) ImageGotWorse{};
                    SLONG Sympathie[4]{};
                    std::vector<UBYTE> RouteImage;
                    std::vector<UBYTE> CityImage;
                };

                SLONG Anz = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                std::vector<SyncedImage> Data(Anz);
                bool bValid = true;

                for (auto &Entry : Data) {
                    Message >> Entry.PlayerNum;
                    if (!isValidPlayerNum(Entry.PlayerNum)) {
                        LogBadNetMessage(MessageType, "PlayerNum", Entry.PlayerNum);
                        bValid = false;
                        break;
                    }

                    Message >> Entry.Image >> Entry.ImageGotWorse;

                    for (SLONG d = 0; d < 4; d++) {
                        Message >> Entry.Sympathie[d];
                    }

                    Entry.RouteImage.resize(Routen.AnzEntries());
                    for (SLONG d = Routen.AnzEntries() - 1; d >= 0; d--) {
                        Message >> Entry.RouteImage[d];
                    }
                    Entry.CityImage.resize(Cities.AnzEntries());
                    for (SLONG d = Cities.AnzEntries() - 1; d >= 0; d--) {
                        Message >> Entry.CityImage[d];
                    }
                }

                if (!bValid) {
                    break;
                }
                NET_CHECK_READ();

                for (auto &Entry : Data) {
                    PLAYER &qPlayer = Sim.Players.Players[Entry.PlayerNum];

                    qPlayer.Image = Entry.Image;
                    qPlayer.ImageGotWorse = Entry.ImageGotWorse;

                    for (SLONG d = 0; d < 4 && d < qPlayer.Sympathie.AnzEntries(); d++) {
                        qPlayer.Sympathie[d] = Entry.Sympathie[d];
                    }
                    for (SLONG d = 0; d < SLONG(Entry.RouteImage.size()) && d < qPlayer.RentRouten.RentRouten.AnzEntries(); d++) {
                        qPlayer.RentRouten.RentRouten[d].Image = Entry.RouteImage[d];
                    }
                    for (SLONG d = 0; d < SLONG(Entry.CityImage.size()) && d < qPlayer.RentCities.RentCities.AnzEntries(); d++) {
                        qPlayer.RentCities.RentCities[d].Image = Entry.CityImage[d];
                    }
                }
            } break;

            case ATNET_SYNC_MONEY: {
                SLONG Anz = 0;
                SLONG PlayerNum = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                while (Anz > 0) {
                    Message >> PlayerNum;
                    NET_CHECK_PLAYER(PlayerNum);

                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                    SLONG d = 0;

                    Message >> qPlayer.Money >> qPlayer.Credit >> qPlayer.Bonus >> qPlayer.AnzAktien >> qPlayer.MaxAktien >> qPlayer.TrustedDividende >>
                        qPlayer.Dividende;

                    for (d = 0; d < 4; d++) {
                        Message >> qPlayer.OwnsAktien[d] >> qPlayer.AktienWert[d];
                    }
                    for (d = 0; d < 10; d++) {
                        Message >> qPlayer.Kurse[d];
                    }

                    NET_CHECK_READ();

                    Anz--;
                }
            } break;

            case ATNET_SYNC_ROUTES: {
                // The data is read into temporaries first and only applied if the whole message was valid:
                struct SyncedRoutes {
                    SLONG PlayerNum{};
                    std::vector<CRentRoute> Routes;
                };

                SLONG Anz = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                std::vector<SyncedRoutes> Data(Anz);
                bool bValid = true;

                for (auto &Entry : Data) {
                    Message >> Entry.PlayerNum;
                    if (!isValidPlayerNum(Entry.PlayerNum)) {
                        LogBadNetMessage(MessageType, "PlayerNum", Entry.PlayerNum);
                        bValid = false;
                        break;
                    }

                    Entry.Routes.resize(Routen.AnzEntries());
                    for (SLONG d = Routen.AnzEntries() - 1; d >= 0; d--) {
                        CRentRoute &qRoute = Entry.Routes[d];

                        Message >> qRoute.Rang >> qRoute.LastFlown >> qRoute.Image >> qRoute.Miete >> qRoute.Ticketpreis >> qRoute.TicketpreisFC >>
                            qRoute.TageMitVerlust >> qRoute.TageMitGering;
                    }

                    if (Message.HasReadError()) {
                        break;
                    }
                }

                if (!bValid) {
                    break;
                }
                NET_CHECK_READ();

                for (auto &Entry : Data) {
                    PLAYER &qPlayer = Sim.Players.Players[Entry.PlayerNum];

                    if (qPlayer.RentRouten.RentRouten.AnzEntries() < SLONG(Entry.Routes.size())) {
                        AT_Log("Dropping data of network message 0x%lx for player %li: RentRouten has the wrong size (%li vs. %li)",
                               static_cast<unsigned long>(MessageType), static_cast<long>(Entry.PlayerNum), static_cast<long>(qPlayer.RentRouten.RentRouten.AnzEntries()),
                               static_cast<long>(Entry.Routes.size()));
                        continue;
                    }

                    for (SLONG d = SLONG(Entry.Routes.size()) - 1; d >= 0; d--) {
                        CRentRoute &qRoute = qPlayer.RentRouten.RentRouten[d];
                        const CRentRoute &qNew = Entry.Routes[d];

                        qRoute.Rang = qNew.Rang;
                        qRoute.LastFlown = qNew.LastFlown;
                        qRoute.Image = qNew.Image;
                        qRoute.Miete = qNew.Miete;
                        qRoute.Ticketpreis = qNew.Ticketpreis;
                        qRoute.TicketpreisFC = qNew.TicketpreisFC;
                        qRoute.TageMitVerlust = qNew.TageMitVerlust;
                        qRoute.TageMitGering = qNew.TageMitGering;
                    }
                }
            } break;

            case ATNET_SYNC_FLAGS: {
                SLONG Anz = 0;
                SLONG PlayerNum = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                while (Anz > 0) {
                    Message >> PlayerNum;
                    NET_CHECK_PLAYER(PlayerNum);

                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                    Message >> qPlayer.SickTokay >> qPlayer.RunningToToilet >> qPlayer.PlayerSmoking >> qPlayer.Stunned >> qPlayer.OfficeState >>
                        qPlayer.Koffein >> qPlayer.NumFlights >> qPlayer.WalkSpeed >> qPlayer.WerbeBroschuere >> qPlayer.TelephoneDown >>
                        qPlayer.Presseerklaerung >> qPlayer.SecurityFlags >> qPlayer.PlayerStinking >> qPlayer.RocketFlags >> qPlayer.LastRocketFlags;

                    NET_CHECK_READ();

                    Anz--;
                }
            } break;

            case ATNET_SYNC_OFFICEFLAG: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;

                if (PlayerNum == 55) {
                    Message >> Sim.nSecOutDays;
                } else {
                    NET_CHECK_PLAYER(PlayerNum);

                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                    Message >> qPlayer.OfficeState;
                }
            } break;

            case ATNET_SYNC_ITEMS: {
                SLONG Anz = 0;
                SLONG PlayerNum = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                while (Anz > 0) {
                    Message >> PlayerNum;
                    NET_CHECK_PLAYER(PlayerNum);

                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                    for (SLONG c = 0; c < 6; c++) {
                        UBYTE Item = 0;

                        Message >> Item;
                        if (c < qPlayer.Items.AnzEntries()) {
                            qPlayer.Items[c] = Item;
                        }
                    }

                    NET_CHECK_READ();

                    Anz--;
                }
            } break;

            case ATNET_SYNC_PLANES: {
                SLONG Anz = 0;
                SLONG PlayerNum = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                while (Anz > 0) {
                    Message >> PlayerNum;
                    NET_CHECK_PLAYER(PlayerNum);

                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                    Message >> qPlayer.Planes >> qPlayer.Auftraege >> qPlayer.Frachten >> qPlayer.RentCities;

                    NET_CHECK_READ();

                    Anz--;
                }
            } break;

            case ATNET_SYNC_MEETING: {
                SLONG Anz = 0;
                SLONG PlayerNum = 0;

                Message >> Anz;
                NET_CHECK(isValidPlayerCount(Anz), "Anz", Anz);

                bool bValid = true;

                while (Anz > 0) {
                    Message >> PlayerNum;
                    if (!isValidPlayerNum(PlayerNum)) {
                        LogBadNetMessage(MessageType, "PlayerNum", PlayerNum);
                        bValid = false;
                        break;
                    }

                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                    SLONG Trust = 0, Mode = 0, Mode2 = 0, Mode3 = 0, Active = 0, Opfer = 0, Opfer2 = 0, Opfer3 = 0, Plane = 0, Hints = 0, Timeout = 0;
                    Message >> Trust >> Mode >> Mode2 >> Mode3 >> Active;
                    Message >> Opfer >> Opfer2 >> Opfer3 >> Plane >> Hints;
                    Message >> Timeout >> qPlayer.NumPassengers >> qPlayer.NumFracht;

                    // The victims are used as player index when the sabotage is executed:
                    if (Message.HasReadError() || !isValidVictim(Opfer) || !isValidVictim(Opfer2) || !isValidVictim(Opfer3)) {
                        bValid = false;
                        break;
                    }

                    qPlayer.ArabTrust = Trust;
                    qPlayer.ArabMode = Mode;
                    qPlayer.ArabMode2 = Mode2;
                    qPlayer.ArabMode3 = Mode3;
                    qPlayer.ArabActive = Active;
                    qPlayer.ArabOpfer = Opfer;
                    qPlayer.ArabOpfer2 = Opfer2;
                    qPlayer.ArabOpfer3 = Opfer3;
                    qPlayer.ArabPlane = Plane;
                    qPlayer.ArabHints = Hints;
                    qPlayer.ArabTimeout = Timeout;

                    Anz--;
                }

                if (!bValid) {
                    AT_Log("Dropping rest of network message 0x%lx: invalid player data or message too short", static_cast<unsigned long>(MessageType));
                    break;
                }

                BOOL SentFromHost = 0;
                Message >> SentFromHost;

                if (SentFromHost != 0) {
                    Message >> Sim.SabotageActs;
                }
            } break;

            case ATNET_ADD_SYMPATHIE: {
                SLONG Anz = 0;
                SLONG PlayerNum = 0;
                SLONG SympathieTarget = 0;

                Message >> PlayerNum >> SympathieTarget >> Anz;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                NET_CHECK(SympathieTarget >= 0 && SympathieTarget < qPlayer.Sympathie.AnzEntries(), "SympathieTarget", SympathieTarget);

                qPlayer.Sympathie[SympathieTarget] += Anz;
                Limit(static_cast<SLONG>(-1000), qPlayer.Sympathie[SympathieTarget], static_cast<SLONG>(1000));
            } break;

            case ATNET_SYNCROUTECHANGE: {
                SLONG PlayerNum = 0;
                SLONG RouteId = 0;
                SLONG Ticketpreis = 0;
                SLONG TicketpreisFC = 0;

                Message >> PlayerNum >> RouteId >> Ticketpreis >> TicketpreisFC;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK_READ();

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                SLONG RouteIndex = 0;

                NET_CHECK(resolveRouteIndex(qPlayer, RouteId, RouteIndex), "RouteId", RouteId);

                // A difference here means that the clients are out of sync. Report it, but never crash because of it:
                if (qPlayer.RentRouten.RentRouten[RouteIndex].Ticketpreis != Ticketpreis) {
                    AT_Log("SYNCROUTECHANGE: Ticketpreis differs for player %li, route %li (local %li vs. %li)", static_cast<long>(PlayerNum),
                           static_cast<long>(RouteId), static_cast<long>(qPlayer.RentRouten.RentRouten[RouteIndex].Ticketpreis), static_cast<long>(Ticketpreis));
                }
                if (qPlayer.RentRouten.RentRouten[RouteIndex].TicketpreisFC != TicketpreisFC) {
                    AT_Log("SYNCROUTECHANGE: TicketpreisFC differs for player %li, route %li (local %li vs. %li)", static_cast<long>(PlayerNum),
                           static_cast<long>(RouteId), static_cast<long>(qPlayer.RentRouten.RentRouten[RouteIndex].TicketpreisFC), static_cast<long>(TicketpreisFC));
                }

                qPlayer.UpdateTicketpreise(RouteId, Ticketpreis, TicketpreisFC);
            } break;

                //--------------------------------------------------------------------------------------------
                // Robot:
                //--------------------------------------------------------------------------------------------
            case ATNET_ROBOT_EXECUTE: {
                SLONG c = 0;
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                NET_CHECK(qPlayer.Sympathie.AnzEntries() >= 4, "Sympathie size", qPlayer.Sympathie.AnzEntries());

                Message >> qPlayer.WaitWorkTill >> qPlayer.WaitWorkTill2;

                for (c = 0; c < 4; c++) {
                    Message >> qPlayer.Sympathie[c];
                }
                for (c = 0; c < qPlayer.RobotActions.AnzEntries(); c++) {
                    Message >> qPlayer.RobotActions[c];
                }
            } break;

                //--------------------------------------------------------------------------------------------
                // Player:
                //--------------------------------------------------------------------------------------------
            case ATNET_PLAYER_REFILL: {
                SLONG Type = 0;
                SLONG City = 0;
                SLONG Delta = 0;
                SLONG Time = 0;

                Message >> Type >> City >> Delta >> Time;

                switch (Type) {
                case 1:
                    Sim.TickLastMinuteRefill = Delta;
                    LastMinuteAuftraege.RefillForLastMinute();
                    break;
                case 2:
                    Sim.TickReisebueroRefill = Delta;
                    ReisebueroAuftraege.RefillForReisebuero();
                    break;
                case 3:
                    Sim.TickFrachtRefill = Delta;
                    gFrachten.Refill();
                    break;
                case 4:
                    if (City < 0 || City >= SLONG(AuslandsRefill.size()) || City >= SLONG(AuslandsAuftraege.size())) {
                        LogBadNetMessage(MessageType, "City", City);
                        break;
                    }
                    AuslandsRefill[City] = Delta;
                    AuslandsAuftraege[City].RefillForAusland(City);
                    break;
                case 5:
                    if (City < 0 || City >= SLONG(AuslandsFRefill.size()) || City >= SLONG(AuslandsFrachten.size())) {
                        LogBadNetMessage(MessageType, "City", City);
                        break;
                    }
                    AuslandsFRefill[City] = Delta;
                    AuslandsFrachten[City].RefillForAusland(City);
                    break;
                default:
                    LogBadNetMessage(MessageType, "Type", Type);
                    break;
                }
            } break;

            case ATNET_PLAYER_TOOK: {
                SLONG Type = 0;
                SLONG City = 0;
                SLONG Index = 0;
                SLONG PlayerNum = 0;

                Message >> PlayerNum >> Type >> Index >> City;
                NET_CHECK_PLAYER(PlayerNum);

                switch (Type) {
                case 1:
                    if (!isValidAlbumRef(LastMinuteAuftraege, Index)) {
                        LogBadNetMessage(MessageType, "Index", Index);
                        break;
                    }
                    LastMinuteAuftraege[Index].Praemie = -1;
                    break;
                case 2:
                    if (!isValidAlbumRef(ReisebueroAuftraege, Index)) {
                        LogBadNetMessage(MessageType, "Index", Index);
                        break;
                    }
                    ReisebueroAuftraege[Index].Praemie = -1;
                    break;
                case 3:
                    if (!isValidAlbumRef(gFrachten, Index)) {
                        LogBadNetMessage(MessageType, "Index", Index);
                        break;
                    }
                    gFrachten[Index].Praemie = -1;
                    break;
                case 4:
                    if (City < 0 || City >= SLONG(AuslandsAuftraege.size()) || !isValidAlbumRef(AuslandsAuftraege[City], Index)) {
                        LogBadNetMessage(MessageType, "City/Index", City);
                        break;
                    }
                    AuslandsAuftraege[City][Index].Praemie = -1;
                    break;
                case 5:
                    if (City < 0 || City >= SLONG(AuslandsFrachten.size()) || !isValidAlbumRef(AuslandsFrachten[City], Index)) {
                        LogBadNetMessage(MessageType, "City/Index", City);
                        break;
                    }
                    AuslandsFrachten[City][Index].Praemie = -1;
                    break;
                default:
                    LogBadNetMessage(MessageType, "Type", Type);
                    break;
                }
            } break;

            case ATNET_PLAYER_18UHR:
                Sim.b18Uhr = TRUE;

                for (c = 0; c < 4; c++) {
                    Sim.Players.Players[c].bReadyForMorning = 0;
                }

                break;

                //--------------------------------------------------------------------------------------------
                // Flugplan:
                //--------------------------------------------------------------------------------------------
            case ATNET_FP_UPDATE: {
                SLONG PlaneId = 0;
                SLONG PlayerNum = 0;

                Message >> PlaneId >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                if (qPlayer.Planes.IsInAlbum(PlaneId) == 0) {
                    hprintf("Plane not in Album: %li, %li", PlayerNum, PlaneId);
                    break;
                }

                CPlane &qPlane = qPlayer.Planes[PlaneId];

                Message >> qPlane.Flugplan;
                NET_CHECK_READ();

                // Daten aktualisieren
                qPlane.Flugplan.UpdateNextFlight();
                qPlane.Flugplan.UpdateNextStart();

                // Alle Aufträge überprüfen:
                CFlugplan &qPlan = qPlane.Flugplan;

                for (e = qPlan.Flug.AnzEntries() - 1; e >= 0; e--) {
                    if (qPlan.Flug[e].ObjectType == 1) {
                        if (!isValidAlbumRef(Routen, qPlan.Flug[e].ObjectId)) {
                            hprintf("Err: Flight %li, Route %lx", qPlan.Flug[e].ObjectType, qPlan.Flug[e].ObjectId);
                            qPlan.Flug[e].ObjectType = 0;
                        }
                    }
                    if (qPlan.Flug[e].ObjectType == 2) {
                        if (qPlayer.Auftraege.IsInAlbum(qPlan.Flug[e].ObjectId) == 0) {
                            hprintf("Err: Flight %li, %lx", qPlan.Flug[e].ObjectType, qPlan.Flug[e].ObjectId);
                            qPlan.Flug[e].ObjectType = 0;
                        }
                    }
                    if (qPlan.Flug[e].ObjectType == 4) {
                        if (qPlayer.Frachten.IsInAlbum(qPlan.Flug[e].ObjectId) == 0) {
                            hprintf("Err: Flight %li, %lx", qPlan.Flug[e].ObjectType, qPlan.Flug[e].ObjectId);
                            qPlan.Flug[e].ObjectType = 0;
                        }
                    }

                    if (qPlan.Flug[e].ObjectType != 0) {
                        if (Cities.IsInAlbum(qPlan.Flug[e].VonCity) == 0) {
                            hprintf("Err: Flight %li, VonCity %lx", qPlan.Flug[e].ObjectType, qPlan.Flug[e].VonCity);
                            qPlan.Flug[e].ObjectId = 0;
                        }
                        if (Cities.IsInAlbum(qPlan.Flug[e].NachCity) == 0) {
                            hprintf("Err: Flight %li, NachCity %lx", qPlan.Flug[e].ObjectType, qPlan.Flug[e].NachCity);
                            qPlan.Flug[e].ObjectId = 0;
                        }
                    }
                }

                qPlayer.UpdateAuftragsUsage();
                qPlayer.UpdateFrachtauftragsUsage();
                qPlayer.Planes[PlaneId].CheckFlugplaene(PlayerNum);
            } break;

            case ATNET_TAKE_ORDER: {
                SLONG PlayerNum = 0;
                CAuftrag a;

                Message >> PlayerNum >> a;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK_READ();
                NET_CHECK(isValidAlbumRef(Cities, SLONG(a.VonCity)) && isValidAlbumRef(Cities, SLONG(a.NachCity)), "city", a.VonCity);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                if (qPlayer.Auftraege.GetNumFree() < 3) {
                    qPlayer.Auftraege.ReSize(qPlayer.Auftraege.AnzEntries() + 10);
                }

                qPlayer.Auftraege += a;
            } break;

            case ATNET_TAKE_FREIGHT: {
                SLONG PlayerNum = 0;
                CFracht a;

                Message >> PlayerNum >> a;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK_READ();
                NET_CHECK(isValidAlbumRef(Cities, SLONG(a.VonCity)) && isValidAlbumRef(Cities, SLONG(a.NachCity)), "city", a.VonCity);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                if (qPlayer.Frachten.GetNumFree() < 3) {
                    qPlayer.Frachten.ReSize(qPlayer.Frachten.AnzEntries() + 10);
                }

                qPlayer.Frachten += a;
            } break;

            case ATNET_TAKE_CITY: {
                // "Player" is the highest bidder (-1 = nobody) and is used as a player index later on:
                SLONG CityPlayer[7];
                SLONG CityPreis[7];
                SLONG GatePlayer[7];
                SLONG GatePreis[7];
                bool bValid = true;

                for (SLONG c = 0; c < 7; c++) {
                    Message >> CityPlayer[c] >> CityPreis[c];
                    Message >> GatePlayer[c] >> GatePreis[c];

                    if (CityPlayer[c] < -1 || CityPlayer[c] > 3) {
                        LogBadNetMessage(MessageType, "City.Player", CityPlayer[c]);
                        bValid = false;
                    }
                    if (GatePlayer[c] < -1 || GatePlayer[c] > 3) {
                        LogBadNetMessage(MessageType, "Gate.Player", GatePlayer[c]);
                        bValid = false;
                    }
                }

                if (!bValid) {
                    break;
                }
                NET_CHECK_READ();

                for (SLONG c = 0; c < 7; c++) {
                    TafelData.City[c].Player = CityPlayer[c];
                    TafelData.City[c].Preis = CityPreis[c];
                    TafelData.Gate[c].Player = GatePlayer[c];
                    TafelData.Gate[c].Preis = GatePreis[c];
                }
            } break;

            case ATNET_TAKE_ROUTE: {
                // Route1Id and Route2Id are indices into Routen / RentRouten (see PLAYER::NetUpdateRentRoute), but album ids are accepted as well:
                SLONG PlayerNum = 0;
                SLONG Route1Id = 0;
                SLONG Route2Id = 0;

                Message >> PlayerNum >> Route1Id >> Route2Id;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                SLONG Route1Index = 0;
                SLONG Route2Index = 0;

                NET_CHECK(resolveRouteIndex(qPlayer, Route1Id, Route1Index), "Route1Id", Route1Id);
                NET_CHECK(resolveRouteIndex(qPlayer, Route2Id, Route2Index), "Route2Id", Route2Id);

                CRentRoute Route1 = qPlayer.RentRouten.RentRouten[Route1Index];
                CRentRoute Route2 = qPlayer.RentRouten.RentRouten[Route2Index];

                Message >> Route1;
                Message >> Route2;
                NET_CHECK_READ();

                qPlayer.RentRouten.RentRouten[Route1Index] = Route1;
                qPlayer.RentRouten.RentRouten[Route2Index] = Route2;
            } break;

            case ATNET_ADVISOR: {
                SLONG Art = 0;
                SLONG From = 0;
                SLONG Generic1 = 0;
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                TEAKRAND rnd;

                Message >> Art >> From >> Generic1;
                NET_CHECK_PLAYER(From);
                NET_CHECK(isValidPlayerNum(Sim.localPlayer), "localPlayer", Sim.localPlayer);

                PLAYER &qFromPlayer = Sim.Players.Players[From];

                switch (Art) {
                // Tafel: Jemand hat einen überboten
                case 0:
                    if (Generic1 < 0 || Generic1 >= SLONG(TafelData.ByPositions.size()) || TafelData.ByPositions[Generic1] == nullptr) {
                        LogBadNetMessage(MessageType, "Generic1", Generic1);
                        break;
                    }
                    if (qPlayer.HasBerater(BERATERTYP_INFO) >= rnd.Rand(100)) {
                        auto &qEntry = *TafelData.ByPositions[Generic1];
                        if (qEntry.Type == CTafelZettel::Type::GATE) {
                            qPlayer.Messages.AddMessage(
                                BERATERTYP_INFO, bprintf(StandardTexte.GetS(TOKEN_ADVICE, 9001), (LPCTSTR)qFromPlayer.NameX, (LPCTSTR)qFromPlayer.AirlineX));
                        } else if (qEntry.Type == CTafelZettel::Type::CITY) {
                            qPlayer.Messages.AddMessage(BERATERTYP_INFO, bprintf(StandardTexte.GetS(TOKEN_ADVICE, 9002), (LPCTSTR)qFromPlayer.NameX,
                                                                                 (LPCTSTR)qFromPlayer.AirlineX, (LPCTSTR)Cities[qEntry.ZettelId].Name));
                        }
                    }
                    break;

                    // Jemand kauft gebrauchtes Flugzeug:
                case 1:
                    if (Generic1 < 0 || Generic1 >= Sim.UsedPlanes.AnzEntries() || Sim.UsedPlanes.IsInAlbum(Generic1) == 0) {
                        LogBadNetMessage(MessageType, "Generic1", Generic1);
                        break;
                    }
                    if (qPlayer.HasBerater(BERATERTYP_INFO) >= rnd.Rand(100)) {
                        qPlayer.Messages.AddMessage(BERATERTYP_INFO, bprintf(StandardTexte.GetS(TOKEN_ADVICE, 9000), (LPCTSTR)qFromPlayer.NameX,
                                                                             (LPCTSTR)qFromPlayer.AirlineX, Sim.UsedPlanes[Generic1].CalculatePrice()));
                    }
                    break;

                    // Jemand gibt Aktien aus:
                case 3:
                    if (qPlayer.HasBerater(BERATERTYP_INFO) >= rnd.Rand(100)) {
                        qPlayer.Messages.AddMessage(BERATERTYP_INFO, bprintf(StandardTexte.GetS(TOKEN_ADVICE, 9004), (LPCTSTR)qFromPlayer.NameX,
                                                                             (LPCTSTR)qFromPlayer.AirlineX, Generic1));
                    }
                    break;

                    // Jemand kauft Aktien vom localPlayer:
                case 4:
                    if (qPlayer.HasBerater(BERATERTYP_INFO) >= rnd.Rand(100)) {
                        qPlayer.Messages.AddMessage(BERATERTYP_INFO, bprintf(StandardTexte.GetS(TOKEN_ADVICE, 9005), (LPCTSTR)qFromPlayer.NameX,
                                                                             (LPCTSTR)qFromPlayer.AirlineX, Generic1));
                    }
                    break;
                default:
                    LogBadNetMessage(MessageType, "Art", Art);
                    break;
                }
            } break;

            case ATNET_BUY_USED: {
                SLONG PlayerNum = 0;
                SLONG PlaneIndex = 0;
                SLONG Time = 0;

                Message >> PlayerNum >> PlaneIndex >> Time;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK(PlaneIndex >= 0 && PlaneIndex < Sim.UsedPlanes.AnzEntries() && Sim.UsedPlanes.IsInAlbum(PlaneIndex) != 0, "PlaneIndex", PlaneIndex);

                PLAYER &qFromPlayer = Sim.Players.Players[PlayerNum];

                if (qFromPlayer.Planes.GetNumFree() < 2) {
                    qFromPlayer.Planes.ReSize(qFromPlayer.Planes.AnzEntries() + 10);
                }

                qFromPlayer.Planes += Sim.UsedPlanes[PlaneIndex];

                Sim.UsedPlanes[PlaneIndex].Name.Empty();
                Sim.TickMuseumRefill = 0;
            } break;

            case ATNET_SELL_USED: {
                SLONG PlayerNum = 0;
                SLONG PlaneId = 0;

                Message >> PlayerNum >> PlaneId;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                NET_CHECK(qPlayer.Planes.IsInAlbum(PlaneId) != 0, "PlaneId", PlaneId);

                qPlayer.Planes -= PlaneId;
            } break;

            case ATNET_BUY_NEW: {
                SLONG PlayerNum = 0;
                SLONG Anzahl = 0;
                SLONG Type = 0;
                TEAKRAND rnd;

                Message >> PlayerNum >> Anzahl >> Type;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK(Anzahl >= 0 && Anzahl <= 100, "Anzahl", Anzahl);
                NET_CHECK(Type >= 0 && Type < PlaneTypes.AnzEntries() && PlaneTypes.IsInAlbum(Type) != 0, "Type", Type);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                rnd.SRand(Sim.Date);

                for (c = 0; c < Anzahl; c++) {
                    qPlayer.BuyPlane(Type, &rnd);
                }
            } break;

            case ATNET_BUY_NEWX: {
                SLONG PlayerNum = 0;
                SLONG Anzahl = 0;
                CXPlane plane;
                TEAKRAND rnd;

                Message >> PlayerNum >> Anzahl >> plane;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK_READ();
                NET_CHECK(Anzahl >= 0 && Anzahl <= 100, "Anzahl", Anzahl);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                rnd.SRand(Sim.Date);

                for (c = 0; c < Anzahl; c++) {
                    qPlayer.BuyPlane(plane, &rnd);
                }
            } break;

            case ATNET_PERSONNEL: {
                SLONG PlayerNum = 0;
                SLONG m = 0;
                SLONG n = 0;

                Message >> PlayerNum >> m >> n;

                if (PlayerNum >= 0 && PlayerNum < Sim.Players.Players.AnzEntries()) {
                    PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                    qPlayer.Statistiken[STAT_ZUFR_PERSONAL].SetAtPastDay(m);
                    qPlayer.Statistiken[STAT_MITARBEITER].SetAtPastDay(n);

                    SLONG c = 0;
                    while (true) {
                        Message >> c;

                        if (c == -1 || Message.HasReadError()) {
                            break;
                        }
                        if (qPlayer.Planes.IsInAlbum(c) != 0) {
                            Message >> qPlayer.Planes[c].AnzPiloten;
                            Message >> qPlayer.Planes[c].AnzBegleiter;
                            Message >> qPlayer.Planes[c].PersonalQuality;
                        } else {
                            SLONG dummy = 0;

                            Message >> dummy >> dummy >> dummy;
                        }
                    }
                }
            } break;

            case ATNET_PLANEPROPS: {
                SLONG PlayerNum = 0;
                SLONG PlaneId = 0;

                Message >> PlayerNum >> PlaneId;
                NET_CHECK_PLAYER(PlayerNum);
                Message >> Sim.Players.Players[PlayerNum].MechMode;

                if (PlaneId != -1 && (Sim.Players.Players[PlayerNum].Planes.IsInAlbum(PlaneId) != 0)) {
                    CPlane &qPlane = Sim.Players.Players[PlayerNum].Planes[PlaneId];

                    Message >> qPlane.Sitze >> qPlane.SitzeTarget >> qPlane.Essen >> qPlane.EssenTarget >> qPlane.Tabletts >> qPlane.TablettsTarget >>
                        qPlane.Deco >> qPlane.DecoTarget >> qPlane.Triebwerk >> qPlane.TriebwerkTarget >> qPlane.Reifen >> qPlane.ReifenTarget >>
                        qPlane.Elektronik >> qPlane.ElektronikTarget >> qPlane.Sicherheit >> qPlane.SicherheitTarget;

                    Message >> qPlane.WorstZustand >> qPlane.Zustand >> qPlane.TargetZustand;
                    Message >> qPlane.AnzBegleiter >> qPlane.MaxBegleiter;
                }
            } break;

                //--------------------------------------------------------------------------------------------
                // Dialog:
                //--------------------------------------------------------------------------------------------
            case ATNET_DIALOG_REQUEST: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                auto *pRaum = qPlayer.LocationWin;

                SLONG RequestingPlayer = 0;

                // Ist Spieler bereit, einen Dialog zu beginnen?
                if (qPlayer.GetRoom() == ROOM_AIRPORT && (qPlayer.IsStuck == 0) && (pRaum != nullptr) && pRaum->MenuIsOpen() == FALSE &&
                    pRaum->IsDialogOpen() == FALSE) {
                    PERSON *pPerson = getPlayerPerson(Sim.localPlayer);
                    NET_CHECK(pPerson != nullptr, "localPlayer (no person)", Sim.localPlayer);
                    PERSON &qPerson = *pPerson;

                    qPlayer.WalkStopEx();
                    qPlayer.IsTalking = TRUE;

                    Message >> qPerson.Phase >> RequestingPlayer >> qPerson.Position.x >> qPerson.Position.y;
                    NET_CHECK_PLAYER(RequestingPlayer);

                    qPerson.Dir = 8;
                    qPerson.LookDir = 8;

                    SIM::SendSimpleMessage(ATNET_PLAYERLOOK, Sim.Players.Players[RequestingPlayer].NetworkID, qPerson.State, qPerson.Phase);
                    SIM::SendSimpleMessage(ATNET_DIALOG_YES, Sim.Players.Players[RequestingPlayer].NetworkID, Sim.localPlayer, qPerson.Phase);
                } else {
                    UBYTE Dummy = 0;
                    XY Dummy2;

                    Message >> Dummy >> RequestingPlayer >> Dummy2.x >> Dummy2.y;
                    NET_CHECK_PLAYER(RequestingPlayer);

                    // Nein! Keine Interviews!
                    SIM::SendSimpleMessage(ATNET_DIALOG_NO, Sim.Players.Players[RequestingPlayer].NetworkID);
                }
            } break;

            case ATNET_DIALOG_YES: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                SLONG TargetPlayer = 0;
                SLONG Phase = 0;

                Message >> TargetPlayer >> Phase;
                NET_CHECK_PLAYER(TargetPlayer);

                PERSON *pPerson = getPlayerPerson(TargetPlayer);
                NET_CHECK(pPerson != nullptr, "TargetPlayer (no person)", TargetPlayer);
                pPerson->Phase = UBYTE(Phase);

                if (!qPlayer.bDialogStartSent) {
                    qPlayer.IsWalking2Player = TargetPlayer;
                }
            } break;

            case ATNET_DIALOG_START: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                SLONG OtherPlayerNum = 0;

                auto *pRaum = qPlayer.LocationWin;

                Message >> OtherPlayerNum;
                NET_CHECK_PLAYER(OtherPlayerNum);

                // Erneute Abfrage: Ist Spieler bereit, einen Dialog zu beginnen?
                if (qPlayer.GetRoom() == ROOM_AIRPORT && (qPlayer.IsStuck == 0) && (pRaum != nullptr) && pRaum->MenuIsOpen() == FALSE &&
                    pRaum->IsDialogOpen() == FALSE) {
                    // JA!
                    PERSON *pPerson = getPlayerPerson(OtherPlayerNum);
                    NET_CHECK(pPerson != nullptr, "OtherPlayerNum (no person)", OtherPlayerNum);
                    PERSON &qPerson = *pPerson;

                    Message >> qPerson.Position.x >> qPerson.Position.y >> qPerson.Phase >> qPerson.LookDir;

                    if (qPlayer.LocationWin != nullptr) {
                        (qPlayer.LocationWin)->StartDialog(TALKER_COMPETITOR, MEDIUM_AIR, OtherPlayerNum, 1);
                    }

                    qPlayer.PlayerDialogState = -1;
                } else {
                    UBYTE DummyPhase = 0;
                    UBYTE DummyLookDir = 0;
                    SLONG DummyX = 0;
                    SLONG DummyY = 0;

                    Message >> DummyX >> DummyY >> DummyPhase >> DummyLookDir;

                    // Nein! Keine Interviews!
                    SIM::SendSimpleMessage(ATNET_DIALOG_NO, Sim.Players.Players[OtherPlayerNum].NetworkID);
                }
            } break;

            case ATNET_DIALOG_NO: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                qPlayer.IsWalking2Player = -1;
                qPlayer.IsTalking = 0;

                qPlayer.PlayerDialogState = -1;
                qPlayer.WalkStop();
                qPlayer.NewDir = 8;

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->StopDialog();
                }
            } break;

            case ATNET_DIALOG_SAY: {
                SLONG id = 0;
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                Message >> id;

                MouseClickArea = -102;
                MouseClickId = 1;
                MouseClickPar1 = id;

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->PreLButtonDown(CPoint(0, 0));
                }
            } break;

            case ATNET_DIALOG_TEXT: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                CString Answer;
                SLONG id = 0;
                BOOL TextAlign = 0;

                Message >> TextAlign >> id >> Answer;

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->MakeSayWindow(static_cast<BOOL>(TextAlign == 0), id, Answer, (qPlayer.LocationWin)->pFontNormal);
                }
            } break;

            case ATNET_DIALOG_NEXT: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->PreLButtonDown(CPoint(0, 0));
                }
            } break;

            case ATNET_DIALOG_DRUNK:
                Sim.Players.Players[Sim.localPlayer].IsDrunk += 400;
                break;

            case ATNET_DIALOG_LOCK: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                qPlayer.IsTalking = TRUE;
            } break;

            case ATNET_DIALOG_UNLOCK: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];
                qPlayer.IsTalking = FALSE;

                // Aus irgendeinem Grund fängt die Spielfigur sonst an zu laufen:
                if (qPlayer.Owner == 2) {
                    qPlayer.WalkStopEx();
                }
            } break;

            case ATNET_DIALOG_END: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->StopDialog();
                }

                qPlayer.PlayerDialogState = -1;
            } break;

            case ATNET_DIALOG_KOOP: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                Message >> qPlayer.Kooperation;
            } break;

            case ATNET_DIALOG_DROPITEM: {
                SLONG PlayerNum = 0;
                SLONG Item = 0;

                Message >> PlayerNum >> Item;
                NET_CHECK_PLAYER(PlayerNum);
                NET_CHECK(Item >= 0 && Item < 256, "Item", Item);

                Sim.Players.Players[PlayerNum].DropItem(UBYTE(Item));
            } break;

                //--------------------------------------------------------------------------------------------
                // Dialogaufbau per Telefon:
                //--------------------------------------------------------------------------------------------
            case ATNET_PHONE_DIAL: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                SLONG OtherPlayerNum = 0;
                SLONG bHandy = 0;

                Message >> OtherPlayerNum >> bHandy;
                NET_CHECK_PLAYER(OtherPlayerNum);

                if (qPlayer.LocationWin != nullptr) {
                    CStdRaum &qRoom = *(qPlayer.LocationWin);

                    bool bImpossible = false; // Kein Telefonat annehmen, wenn wir gerade den Höhrer in die Hand nehmen:
                    if (qPlayer.GetRoom() == ROOM_BURO_A + Sim.localPlayer * 10 && ((dynamic_cast<CBuero *>(qPlayer.LocationWin))->KommVarTelefon != 0)) {
                        bImpossible = true;
                    }

                    if (qRoom.IsDialogOpen() == 0 && qRoom.MenuIsOpen() == 0 && !bImpossible &&
                        Sim.Persons[Sim.Persons.GetPlayerIndex(Sim.localPlayer)].StatePar == 0 && qPlayer.TelephoneDown == FALSE) {
                        if (bHandy == 0 && qPlayer.GetRoom() != ROOM_BURO_A + Sim.localPlayer * 10) {
                            SIM::SendSimpleMessage(ATNET_PHONE_NOTHOME, Sim.Players.Players[OtherPlayerNum].NetworkID);
                        } else {
                            SIM::SendSimpleMessage(ATNET_PHONE_ACCEPT, Sim.Players.Players[OtherPlayerNum].NetworkID, Sim.localPlayer, bHandy);

                            gUniversalFx.Stop();
                            gUniversalFx.ReInit("phone.raw");
                            gUniversalFx.Play(DSBPLAY_NOSTOP, Sim.Options.OptionEffekte * 100 / 7);

                            qPlayer.GameSpeed = 0;
                            SIM::SendSimpleMessage(ATNET_SETSPEED, 0, Sim.localPlayer, qPlayer.GameSpeed);

                            qRoom.StartDialog(TALKER_COMPETITOR, MEDIUM_HANDY, OtherPlayerNum, 1);
                            qRoom.PayingForCall = FALSE;

                            qPlayer.DisplayAsTelefoning();
                            Sim.Players.Players[OtherPlayerNum].DisplayAsTelefoning();
                        }
                    } else {
                        SIM::SendSimpleMessage(ATNET_PHONE_BUSY, Sim.Players.Players[OtherPlayerNum].NetworkID);
                    }
                } else {
                    SIM::SendSimpleMessage(ATNET_PHONE_BUSY, Sim.Players.Players[OtherPlayerNum].NetworkID);
                }
            } break;

            case ATNET_PHONE_ACCEPT: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];
                SLONG OtherPlayerNum = 0;
                SLONG bHandy = 0;

                Message >> OtherPlayerNum >> bHandy;
                NET_CHECK_PLAYER(OtherPlayerNum);

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->StartDialog(TALKER_COMPETITOR, MEDIUM_HANDY, OtherPlayerNum, 0);
                }

                qPlayer.GameSpeed = 0;
                SIM::SendSimpleMessage(ATNET_SETSPEED, 0, Sim.localPlayer, qPlayer.GameSpeed);

                qPlayer.DisplayAsTelefoning();
                Sim.Players.Players[OtherPlayerNum].DisplayAsTelefoning();
            } break;

            case ATNET_PHONE_BUSY: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                if (qPlayer.LocationWin != nullptr) {
                    CStdRaum &qRoom = *(qPlayer.LocationWin);

                    qRoom.DialBusyFX.ReInit("busypure.raw"); // busy pure (without dialing first)
                    qRoom.DialBusyFX.Play(0, Sim.Options.OptionEffekte * 100 / 7);

                    if (qPlayer.GetRoom() == ROOM_BURO_A + Sim.localPlayer * 10) {
                        (dynamic_cast<CBuero *>(&qRoom))->SP_Player.SetDesiredMood(SPM_IDLE);
                    }
                }
            } break;

            case ATNET_PHONE_NOTHOME: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                if (qPlayer.LocationWin != nullptr) {
                    CStdRaum &qRoom = *(qPlayer.LocationWin);

                    qRoom.DialBusyFX.ReInit("noanpure.raw"); // No Answer pure (without dialing first)
                    qRoom.DialBusyFX.Play(0, Sim.Options.OptionEffekte * 100 / 7);

                    if (qPlayer.GetRoom() == ROOM_BURO_A + Sim.localPlayer * 10) {
                        (dynamic_cast<CBuero *>(&qRoom))->SP_Player.SetDesiredMood(SPM_IDLE);
                    }
                }
            } break;

                // case ATNET_CHATSTART: //Veraltet, wird nicht mehr gebraucht
                //   break;

                //--------------------------------------------------------------------------------------------
                // Chatten:
                //--------------------------------------------------------------------------------------------
            case ATNET_CHATSTOP: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->MenuStop();
                }
            } break;

            case ATNET_CHATMESSAGE: {
                CString str;
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                Message >> str;

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->MenuBms[1].ShiftUp(10);
                    (qPlayer.LocationWin)->MenuBms[1].PrintAt(str, FontSmallRed, TEC_FONT_LEFT, 6, 119, 279, 147);
                    (qPlayer.LocationWin)->MenuRepaint();
                }
            } break;

            case ATNET_CHATMONEY: {
                SLONG Money = 0;
                SLONG OtherPlayer = 0;
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                Message >> Money >> OtherPlayer;
                NET_CHECK_PLAYER(OtherPlayer);

                Sim.Players.Players[Sim.localPlayer].ChangeMoney(Money, 3700, Sim.Players.Players[OtherPlayer].NameX);
                Sim.Players.Players[OtherPlayer].ChangeMoney(-Money, 3701, Sim.Players.Players[Sim.localPlayer].NameX);

                if (qPlayer.LocationWin != nullptr) {
                    (qPlayer.LocationWin)->MenuBms[1].ShiftUp(10);
                    (qPlayer.LocationWin)
                        ->MenuBms[1]
                        .PrintAt(bprintf(StandardTexte.GetS(TOKEN_MISC, 3010), Money), FontNormalGrey, TEC_FONT_LEFT, 6, 119, 279, 147);
                    (qPlayer.LocationWin)->MenuRepaint();
                }
            } break;

                //--------------------------------------------------------------------------------------------
                // Sabotage:
                //--------------------------------------------------------------------------------------------
            case ATNET_SABOTAGE_DIRECT: {
                SLONG Type = 0;
                XY Position;

                Message >> Type;
                Message >> Position.x >> Position.y;

                if (Type == ITEM_STINKBOMBE) // Stinkbombe
                {
                    Sim.AddStenchSabotage(XY(Position.x, Position.y));
                } else if (Type == ITEM_GLUE) // Klebstoff
                {
                    UBYTE Dir = 0;
                    UBYTE NewDir = 0;
                    UBYTE Phase = 0;

                    Message >> Dir >> NewDir >> Phase;

                    Sim.AddGlueSabotage(Position, Dir, NewDir, Phase);
                }
            } break;

            case ATNET_SABOTAGE_ARAB: {
                SLONG PlayerNum = 0;

                Message >> PlayerNum;
                NET_CHECK_PLAYER(PlayerNum);

                PLAYER &qPlayer = Sim.Players.Players[PlayerNum];

                SLONG Opfer = 0, Mode = 0, Active = 0, Plane = 0, Opfer2 = 0, Mode2 = 0, Opfer3 = 0, Mode3 = 0, Timeout = 0;
                Message >> Opfer >> Mode >> Active >> Plane >> Opfer2 >> Mode2 >> Opfer3 >> Mode3 >> Timeout;
                NET_CHECK_READ();
                // The victims are used as player index when the sabotage is executed:
                NET_CHECK(isValidVictim(Opfer) && isValidVictim(Opfer2) && isValidVictim(Opfer3), "ArabOpfer", Opfer);

                qPlayer.ArabOpfer = Opfer;
                qPlayer.ArabMode = Mode;
                qPlayer.ArabActive = Active;
                qPlayer.ArabPlane = Plane;
                qPlayer.ArabOpfer2 = Opfer2;
                qPlayer.ArabMode2 = Mode2;
                qPlayer.ArabOpfer3 = Opfer3;
                qPlayer.ArabMode3 = Mode3;
                qPlayer.ArabTimeout = Timeout;
            } break;

            case ATNET_WAITFORPLAYER:
                Message >> Par1 >> Par2;
                NET_CHECK(Par2 >= 0 && Par2 < 4, "PlayerNum", Par2);
                nWaitingForPlayer += Par1;
                nPlayerWaiting[Par2] += Par1;
                if (nPlayerWaiting[Par2] < 0) {
                    nPlayerWaiting[Par2] = 0;
                }
                SetNetworkBitmap(static_cast<SLONG>(nWaitingForPlayer > 0) * 3);
                break;

            case ATNET_TAKETHING: {
                SLONG Item = 0;

                Message >> Item;

                switch (Item) {
                case ITEM_POSTKARTE:
                    Sim.ItemPostcard = 0;
                    break;
                case ITEM_PAPERCLIP:
                    Sim.ItemClips = 0;
                    break;
                case ITEM_GLUE:
                    Sim.ItemGlue = 2;
                    break;
                case ITEM_GLOVE:
                    Sim.ItemGlove = 0;
                    break;
                case ITEM_LAPTOP:
                    Message >> Sim.LaptopSoldTo;
                    break;
                case ITEM_NONE:
                    Sim.MoneyInBankTrash = 0;
                    break;
                case ITEM_KOHLE:
                    Sim.ItemKohle = 0;
                    break;
                case ITEM_PARFUEM:
                    Sim.ItemParfuem = 0;
                    break;
                case ITEM_ZANGE:
                    Sim.ItemZange = 0;
                    break;
                default:
                    break; /* some items do not require network sync */
                }
            } break;

                //--------------------------------------------------------------------------------------------
                // Day control:
                //--------------------------------------------------------------------------------------------
            case ATNET_DAYFINISH: {
                SLONG FromPlayer = 0;

                Message >> FromPlayer;
                NET_CHECK_PLAYER(FromPlayer);

                Sim.Players.Players[FromPlayer].CallItADay = TRUE;
            } break;
            case ATNET_DAYBACK:
                if (Sim.CallItADayAt == 0) {
                    SLONG FromPlayer = 0;

                    Message >> FromPlayer;
                    NET_CHECK_PLAYER(FromPlayer);

                    Sim.Players.Players[FromPlayer].CallItADay = FALSE;
                }
                break;
            case ATNET_DAYFINISHALL: {
                PLAYER &qPlayer = Sim.Players.Players[Sim.localPlayer];

                if ((Sim.Options.OptionAutosave != 0) && (Sim.bNetwork != 0)) {
                    Sim.SaveGame(11, StandardTexte.GetS(TOKEN_MISC, 5000));
                }

                Message >> Sim.CallItADayAt;

                for (SLONG c = 0; c < 4; c++) {
                    if (Sim.Players.Players[c].Owner == 2) {
                        Sim.Players.Players[c].CallItADay = TRUE;
                    }
                }

                if (qPlayer.CallItADay == 0) {
                    SIM::SendSimpleMessage(ATNET_DAYFINISH, 0, Sim.localPlayer);
                    SIM::SendSimpleMessage(ATNET_DAYFINISH, qPlayer.NetworkID, Sim.localPlayer);
                }
            } break;

                //--------------------------------------------------------------------------------------------
                // Miscellaneous:
                //--------------------------------------------------------------------------------------------
            case ATNET_EXPAND_AIRPORT:
                Sim.ExpandAirport = 1;
                break;

            case ATNET_OVERTAKE:
                Message >> Sim.OvertakenAirline >> Sim.OvertakerAirline >> Sim.Overtake;
                break;

                //--------------------------------------------------------------------------------------------
                // Savegames:
                //--------------------------------------------------------------------------------------------
            case ATNET_IO_SAVE: {
                SLONG CursorY = 0;
                CString Name;

                Message >> Sim.UniqueGameId >> CursorY >> Name;

                Sim.SaveGame(CursorY, Name);
            } break;

            case ATNET_IO_LOADREQUEST: {
                SLONG Index = 0;
                SLONG FromPlayer = 0;
                DWORD UniqueGameId = 0;

                Message >> FromPlayer >> Index >> UniqueGameId;
                NET_CHECK_PLAYER(FromPlayer);
                NET_CHECK(Index >= 0 && Index < 100, "savegame Index", Index);

                if (Sim.GetSavegameUniqueGameId(Index, true) == UniqueGameId) {
                    SIM::SendSimpleMessage(ATNET_IO_LOADREQUEST_OK, Sim.Players.Players[FromPlayer].NetworkID, Sim.localPlayer, Index);
                } else {
                    SIM::SendSimpleMessage(ATNET_IO_LOADREQUEST_BAD, Sim.Players.Players[FromPlayer].NetworkID, Sim.localPlayer);

                    if (Sim.Players.Players[Sim.localPlayer].LocationWin != nullptr) {
                        (Sim.Players.Players[Sim.localPlayer].LocationWin)->MenuStart(MENU_REQUEST, MENU_REQUEST_NET_LOADTHIS);
                    }
                }
            } break;

            case ATNET_IO_LOADREQUEST_OK: {
                SLONG c = 0;
                SLONG FromPlayer = 0;
                SLONG Index = 0;

                Message >> FromPlayer >> Index;
                NET_CHECK_PLAYER(FromPlayer);
                NET_CHECK(Index >= 0 && Index < 100, "savegame Index", Index);

                Sim.Players.Players[FromPlayer].bReadyForBriefing = 1;

                // Haben alle ihr okay gegeben?
                for (c = 0; c < 4; c++) {
                    if (!static_cast<bool>(Sim.Players.Players[c].bReadyForBriefing) && (Sim.Players.Players[c].IsOut == 0) &&
                        Sim.Players.Players[c].Owner == 2) {
                        break;
                    }
                }

                if (c == 4) {
                    nOptionsOpen--;
                    SIM::SendSimpleMessage(ATNET_OPTIONS, 0, -1, Sim.localPlayer);
                    SIM::SendSimpleMessage(ATNET_IO_LOADREQUEST_DOIT, 0, Index);
                    Sim.LoadGame(Index);
                }
            } break;

            case ATNET_IO_LOADREQUEST_BAD:
                if (!static_cast<bool>(Sim.Players.Players[Sim.localPlayer].bReadyForBriefing)) {
                    nOptionsOpen--;
                    SIM::SendSimpleMessage(ATNET_OPTIONS, 0, -1, Sim.localPlayer);
                    Sim.Players.Players[Sim.localPlayer].bReadyForBriefing = 1;

                    if (Sim.Players.Players[Sim.localPlayer].LocationWin != nullptr) {
                        (Sim.Players.Players[Sim.localPlayer].LocationWin)->MenuStart(MENU_REQUEST, MENU_REQUEST_NET_LOADONE);
                    }
                }
                break;

            case ATNET_IO_LOADREQUEST_DOIT: {
                SLONG Index = 0;

                Message >> Index;
                NET_CHECK(Index >= 0 && Index < 100, "savegame Index", Index);

                Sim.LoadGame(Index);
            } break;

                //--------------------------------------------------------------------------------------------
                // Testing & Debugging:
                //--------------------------------------------------------------------------------------------
            case ATNET_CHECKRANDS: {
                SLONG rTime = 0;
                SLONG rGeneric = 0;
                ULONG rPersonRandCreate = 0;
                ULONG rPersonRandMisc = 0;
                ULONG rHeadlineRand = 0;
                ULONG rLMA = 0;
                ULONG rRBA = 0;
                ULONG rAA[MAX_CITIES];
                ULONG rFrachen = 0;
                SLONG rActionId[5 * 4];

                Message >> rTime;
                Message >> rPersonRandCreate >> rPersonRandMisc >> rHeadlineRand;
                Message >> rLMA >> rRBA >> rFrachen >> rGeneric;

                for (auto &c : rAA) {
                    Message >> c;
                }
                for (c = 0; c < 20; c++) {
                    Message >> rActionId[c];
                }

#ifdef _DEBUG
                if (rTime != rChkTime)
                    DisplayBroadcastMessage(bprintf("rTime: %li vs %li\n", rTime, rChkTime));
                if (rPersonRandCreate != rChkPersonRandCreate)
                    DisplayBroadcastMessage(bprintf("rPersonRandCreate: %li vs %li\n", rPersonRandCreate, rChkPersonRandCreate));
                if (rPersonRandMisc != rChkPersonRandMisc)
                    DisplayBroadcastMessage(bprintf("rPersonRandMisc: %li vs %li\n", rPersonRandMisc, rChkPersonRandMisc));
                if (rHeadlineRand != rChkHeadlineRand)
                    DisplayBroadcastMessage(bprintf("rHeadlineRand: %li vs %li\n", rHeadlineRand, rChkHeadlineRand));
                if (rLMA != rChkLMA)
                    DisplayBroadcastMessage(bprintf("rLMA: %li vs %li\n", rLMA, rChkLMA));
                if (rRBA != rChkRBA)
                    DisplayBroadcastMessage(bprintf("rRBA: %li vs %li\n", rRBA, rChkRBA));
                // if (rChkGeneric!=rGeneric)                   DisplayBroadcastMessage (bprintf("rChkGeneric: %li vs %li\n", rChkGeneric, rGeneric));
                if (rFrachen != rChkFrachen)
                    DisplayBroadcastMessage(bprintf("rFrachen: %li vs %li\n", rFrachen, rChkFrachen));

                for (c = 0; c < MAX_CITIES; c++)
                    if (rAA[c] != rChkAA[c])
                        DisplayBroadcastMessage(bprintf("rAA[%li]: %li vs %li\n", c, rAA[c], rChkAA[c]));

                for (c = 0; c < 20; c++)
                    if (rActionId[c] != rChkActionId[c]) {
                        DisplayBroadcastMessage(
                            bprintf("Desync AI Action[%li]: %s vs %s\n", c, Translate_ACTION(rActionId[c]), Translate_ACTION(rChkActionId[c])));
                        AT_Log_I("AtNet", "Desync AI Action[%li]: %s vs %s\n", c, Translate_ACTION(rActionId[c]), Translate_ACTION(rChkActionId[c]));
                    }
#endif
            } break;

            case ATNET_GENERICSYNC: {
                SLONG localPlayer = 0;

                Message >> localPlayer;
                NET_CHECK(localPlayer >= 0 && localPlayer < 4, "PlayerNum", localPlayer);
                Message >> GenericSyncIds[localPlayer];

                bReturnAfterThisMessage = true;
            } break;

            case ATNET_GENERICSYNCX: {
                SLONG localPlayer = 0;

                Message >> localPlayer;
                NET_CHECK(localPlayer >= 0 && localPlayer < 4, "PlayerNum", localPlayer);
                Message >> GenericSyncIds[localPlayer] >> GenericSyncIdPars[localPlayer];

                bReturnAfterThisMessage = true;
            } break;

            case ATNET_GENERICASYNC: {
                SLONG SyncId = 0;
                SLONG Par = 0;
                SLONG player = 0;

                Message >> player;
                Message >> SyncId >> Par;
                NET_CHECK(player >= 0 && player < 4, "PlayerNum", player);

                bReturnAfterThisMessage = true;

                NetGenericAsync(SyncId, Par, player);
            } break;

                //--------------------------------------------------------------------------------------------
                // Weitere Synchronisierungen:
                //--------------------------------------------------------------------------------------------
            case ATNET_BODYGUARD: {
                SLONG localPlayer = 0;
                SLONG delta = 0;

                Message >> localPlayer >> delta;
                NET_CHECK_PLAYER(localPlayer);

                if (localPlayer != Sim.localPlayer) {
                    Sim.Players.Players[localPlayer].ChangeMoney(delta, 3130, "");
                }
            } break;

            case ATNET_CHANGEMONEY: {
                __int64 Par1 = 0;
                __int64 Par2 = 0;
                __int64 Par3 = 0;

                Message >> Par1 >> Par2 >> Par3;

                SLONG playerId = static_cast<SLONG>(Par1);
                SLONG statistikid = static_cast<SLONG>(Par3);
                NET_CHECK(Par1 >= 0 && Par1 < 4, "PlayerNum", static_cast<SLONG>(Par1));
                NET_CHECK_PLAYER(playerId);

                Sim.Players.Players[playerId].ChangeMoney(Par2, statistikid, "");
            } break;

            case ATNET_SYNCKEROSIN: {
                SLONG playerId = 0;
                SLONG TankOpen = 0;
                SLONG TankInhalt = 0;
                DOUBLE KerosinQuali = 0;
                SLONG KerosinKind = 0;
                BOOL Tank = 0;
                DOUBLE TankPreis = NAN;

                Message >> playerId >> Tank >> TankOpen >> TankInhalt >> KerosinQuali >> KerosinKind >> TankPreis;
                NET_CHECK_PLAYER(playerId);

                if (playerId != Sim.localPlayer) {
                    PLAYER &qPlayer = Sim.Players.Players[playerId];

                    qPlayer.Tank = Tank;
                    qPlayer.TankOpen = TankOpen;
                    qPlayer.TankInhalt = TankInhalt;
                    qPlayer.KerosinQuali = KerosinQuali;
                    qPlayer.KerosinKind = KerosinKind;
                    qPlayer.TankPreis = TankPreis;
                }
            } break;

            case ATNET_SYNCGEHALT: {
                SLONG playerId = 0;
                SLONG gehalt = 0;

                Message >> playerId >> gehalt;
                NET_CHECK_PLAYER(playerId);

                Sim.Players.Players[playerId].Statistiken[STAT_GEHALT].SetAtPastDay(gehalt);
            } break;

            case ATNET_SYNCNUMFLUEGE: {
                SLONG playerId = 0;
                SLONG auftrag = 0;
                SLONG lm = 0;
                SLONG fracht = 0;

                Message >> playerId >> auftrag >> lm >> fracht;
                NET_CHECK_PLAYER(playerId);

                Sim.Players.Players[playerId].Statistiken[STAT_AUFTRAEGE].SetAtPastDay(auftrag);
                Sim.Players.Players[playerId].Statistiken[STAT_LMAUFTRAEGE].SetAtPastDay(lm);
                Sim.Players.Players[playerId].Statistiken[STAT_FRACHTEN].SetAtPastDay(fracht);
            } break;

                //--------------------------------------------------------------------------------------------
                // Microsoft and SBLib internal codes:
                //--------------------------------------------------------------------------------------------
            case 0x0003:
            case 0x0007:
            case 0x0021:
            case 0x0102:
            case 0x0103:
            case 0x0104:
            case 0x0105:
            case 0x0106:
            case 0x0107:
            case 0x0108:
            case 0x0109:
            case 0x010A:
            case 0x010D:
            case 0xDEADBEEF:
                break;

            default:
                // Something is wrong
                hprintf("Unknown Message %lx", MessageType);
                break;
            }
            } catch (const std::exception &e) {
                AT_Log("Dropping network message 0x%lx: %s", static_cast<unsigned long>(MessageType), e.what());
            }

            if (Message.HasReadError()) {
                AT_Log("Network message 0x%lx was shorter than expected or had an invalid size prefix", static_cast<unsigned long>(MessageType));
            }

            // if (Message.MemPointer!=Message.MemBufferUsed)
            //   __asm { int 3 }
        }
    }
}

//--------------------------------------------------------------------------------------------
// Returns a (short) String describing the Medium
//--------------------------------------------------------------------------------------------
CString GetMediumName(SLONG Medium) { return (StandardTexte.GetS(TOKEN_MISC, 7100 + Medium)); }

//--------------------------------------------------------------------------------------------
// Kehrt erst zurück, wenn die anderen Spieler hier auch waren:
//--------------------------------------------------------------------------------------------
void NetGenericSync(SLONG SyncId) {
    if (Sim.bNetwork == 0) {
        return;
    }
    if (Sim.localPlayer < 0 || Sim.localPlayer > 3) {
        return;
    }

    SIM::SendSimpleMessage(ATNET_GENERICSYNC, 0, Sim.localPlayer, SyncId); // Requesting Sync

    GenericSyncIds[Sim.localPlayer] = SyncId;

    while (true) {
        SLONG c = 0;
        for (c = 0; c < 4; c++) {
            if (Sim.Players.Players[c].Owner != 1 && GenericSyncIds[c] != SyncId && (Sim.Players.Players[c].IsOut == 0)) {
                break;
            }
        }

        if (c == 4) {
            return;
        }

        PumpNetwork();
    }
}

//--------------------------------------------------------------------------------------------
// Kehrt erst zurück, wenn die anderen Spieler hier auch waren:
// Gibt Warnung aus, falls die Parameter unterschiedlich waren.
//--------------------------------------------------------------------------------------------
#ifdef DEBUG_NET
void NetGenericSync(SLONG SyncId, SLONG Par) {
    static bool bReentrant = false;

    if (bReentrant) {
        return;
    }
    if (!Sim.bNetwork) {
        return;
    }
    if (Sim.localPlayer < 0 || Sim.localPlayer > 3) {
        return;
    }
    if (Sim.Players.Players[Sim.localPlayer].Owner == 1) {
        return;
    }
    if (Sim.Time == 9 * 60000) {
        return;
    }

    bReentrant = true;

    Sim.SendSimpleMessage(ATNET_GENERICSYNCX, 0, Sim.localPlayer, SyncId, Par); // Requesting Sync

    GenericSyncIds[Sim.localPlayer] = SyncId;
    GenericSyncIdPars[Sim.localPlayer] = Par;

    while (1) {
        SLONG c = 0;
        for (; c < 4; c++) {
            if (Sim.Players.Players[c].Owner != 1 && GenericSyncIds[c] != SyncId && !Sim.Players.Players[c].IsOut) {
                break;
            }
        }

        if (c == 4) {
            for (c = 0; c < 4; c++) {
                if (Sim.Players.Players[c].Owner != 1 && !Sim.Players.Players[c].IsOut && GenericSyncIdPars[c] != Par) {
                    DisplayBroadcastMessage(bprintf("NetGenericSync (%li): %li vs. %li\n", SyncId, Par, GenericSyncIdPars[c]));
                    AT_Log_I("AtNet", "Desync detected Id(%li): %li vs. %li\n", SyncId, Par, GenericSyncIdPars[c]);
                    // DebugBreak();
                }
            }

            for (c = 0; c < 4; c++) {
                GenericSyncIds[c] = 0;
            }

            bReentrant = false;
            return;
        }

        PumpNetwork();
    }
}
#else
void NetGenericSync(SLONG /*SyncId*/, SLONG /*Par*/) {}
#endif

//--------------------------------------------------------------------------------------------
// Kehrt erst zurück, wenn die anderen Spieler hier auch waren:
// Gibt Warnung aus, falls die Parameter unterschiedlich waren.
//--------------------------------------------------------------------------------------------
#ifdef DEBUG_NET
void NetGenericAsync(SLONG SyncId, SLONG Par, SLONG player) {
    if (!Sim.bNetwork) {
        return;
    }
    if (Sim.localPlayer < 0 || Sim.localPlayer > 3) {
        return;
    }
    if (Sim.Players.Players[Sim.localPlayer].Owner == 1) {
        return;
    }
    if (Sim.Time == 9 * 60000) {
        return;
    }

    if (player == -1) {
        Sim.SendSimpleMessage(ATNET_GENERICASYNC, 0, Sim.localPlayer, SyncId, Par); // Requesting Sync
        player = Sim.localPlayer;
    }

    SLONG d;

    // Gibt es den Eintrag schon?
    for (d = 0; d < 400; d++) {
        if (GenericAsyncIds[d] == SyncId) {
            break;
        }
    }

    // Eventuell müssen wir einen Leereintrag suchen:
    if (d == 400) {
        for (d = 0; d < 400; d += 4) {
            if (GenericAsyncIds[d] == 0 && GenericAsyncIds[d + 1] == 0 && GenericAsyncIds[d + 2] == 0 && GenericAsyncIds[d + 3] == 0) {
                break;
            }
        }

        if (d == 400) {
            DisplayBroadcastMessage("NetGenericAsync overflow\n");
            return;
        }
    }

    d = d / 4 * 4;
    GenericAsyncIds[d + player] = SyncId;
    GenericAsyncIdPars[d + player] = Par;

    SLONG c = 0;
    for (; c < 4; c++) {
        if (Sim.Players.Players[c].Owner != 1 && GenericAsyncIds[d + c] != SyncId && !Sim.Players.Players[c].IsOut) {
            break;
        }
    }

    if (c == 4) {
        for (c = 0; c < 4; c++) {
            if (Sim.Players.Players[c].Owner != 1 && !Sim.Players.Players[c].IsOut && GenericAsyncIdPars[d + c] != Par) {
                DisplayBroadcastMessage(bprintf("NetGenericAsync (%li): %li vs. %li\n", SyncId, Par, GenericAsyncIdPars[d + c]));
                AT_Log_I("AtNet", "Desync detected Id(%li): %li vs. %li\n", SyncId, Par, GenericSyncIdPars[d + c]);
                // DebugBreak();
            }
        }

        for (c = 0; c < 4; c++) {
            GenericAsyncIds[d + c] = 0;
        }
    }
}
#else
void NetGenericAsync(SLONG /*SyncId*/, SLONG /*Par*/, SLONG /*player*/) {}
#endif

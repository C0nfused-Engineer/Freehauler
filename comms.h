// comms.h - talking to the people of the system you're in.
// Comms is how everything with them starts: hail a world or station (F hails
// whoever the nose is on), then ask for prices or news, or - once you're in
// docking range - open trade. Anyone in the system can be raised, but radio
// travels at light speed: a reply takes the round trip, seconds from close
// by and hours from the far side of a big system. So you don't wait on one
// answer: any number of channels can be open, each with requests going out
// and replies coming back, and the HUD shows where each one has got to.
// Signals travel at light speed: every reply arrives after the round trip.
#pragma once
#include "game.h"

struct CommsContact { int body; double dist; };   // loaded body index, distance from its surface

std::vector<CommsContact> CommsInRange(const World &w, const Captain &c);   // everyone in the loaded system, nearest first
// Who F hails: the inhabited world or station the nose is on, or else the one you're in
// docking range of; -1 if neither (the comms list opens instead)
int  HailTarget(const Captain &c);
// Has whoever is on that channel answered yet?
bool CommsAnswered(const World &w, const Captain &c, TargetId with);
// Is a reply still on its way on that channel?
bool CommsWaiting(const World &w, const Captain &c, TargetId with);
// Can trade be opened? (You're in docking range of someone, on a channel with them, and they've answered.)
bool CanTradeOverComms(const World &w, const Captain &c);
const CommsContact       *FindContact(const std::vector<CommsContact> &list, TargetId who);

// A request on its way out, or its reply on its way back (for the HUD's progress markers)
struct CommsFlight {
    TargetId    with;
    std::string name;
    double      sent, at;                // when the request left, and when the reply gets here
    double Progress(double clock) const { return at > sent ? (clock - sent) / (at - sent) : 1.0; }   // under 0.5: still outbound
};
std::vector<CommsFlight> CommsInFlight(const World &w, const Captain &c);   // soonest reply first

// Actions (the comms window sends these as Commands; see commands.h). 'body' is who,
// in the loaded system. Any number of requests can be in flight at once.
void Hail(World &w, Captain &c, int body);                  // opens a channel (nothing if one is open already)
void RequestMarketReport(World &w, Captain &c, int body);
void RequestLocalInfo(World &w, Captain &c, int body);
void RequestContracts(World &w, Captain &c, int body);      // the station's contracts board (contracts.h)
void HangUp(World &w, Captain &c, int body);

void UpdateComms(World &w, Captain &c);           // called by StepCaptain: announces replies, drops lost channels

// Single player / your own captain
inline std::vector<CommsContact> CommsInRange(const GameState &g) { return CommsInRange(g, g); }
inline bool CanTradeOverComms(const GameState &g) { return CanTradeOverComms(g, g); }
inline void Hail(GameState &g, int body)                { Hail(g, g, body); }
inline void RequestMarketReport(GameState &g, int body) { RequestMarketReport(g, g, body); }
inline void RequestLocalInfo(GameState &g, int body)    { RequestLocalInfo(g, g, body); }
inline void HangUp(GameState &g, int body)              { HangUp(g, g, body); }

// comms.h - talking to inhabited worlds within 3 light seconds.
// Signals travel at light speed: every reply arrives after the round trip.
#pragma once
#include "game.h"

struct CommsContact { int body; double dist; };   // loaded body index, distance from its surface

std::vector<CommsContact> CommsInRange(const World &w, const Captain &c);   // nearest first
const CommsContact       *FindContact(const std::vector<CommsContact> &list, TargetId who);
bool CommsWaiting(const World &w, const Captain &c);                       // a reply is still on its way

// Actions (the comms window sends these as Commands; see commands.h)
void Hail(World &w, Captain &c, int body);
void RequestMarketReport(World &w, Captain &c);
void RequestLocalInfo(World &w, Captain &c);
void HangUp(World &w, Captain &c);

void UpdateComms(World &w, Captain &c);           // called by StepCaptain: drops lost channels

// Single player / your own captain
inline std::vector<CommsContact> CommsInRange(const GameState &g) { return CommsInRange(g, g); }
inline bool CommsWaiting(const GameState &g)        { return CommsWaiting(g, g); }
inline void Hail(GameState &g, int body)            { Hail(g, g, body); }
inline void RequestMarketReport(GameState &g)       { RequestMarketReport(g, g); }
inline void RequestLocalInfo(GameState &g)          { RequestLocalInfo(g, g); }
inline void HangUp(GameState &g)                    { HangUp(g, g); }

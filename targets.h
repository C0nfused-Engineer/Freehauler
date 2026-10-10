// targets.h - stable references to things you can point at.
// Everything that remembers "which thing" (selection, lock, comms channel,
// and later missions) stores a TargetId, never a name or a pointer: names can
// collide and pointers die when a system unloads.
#pragma once
#include <cstdint>

struct TargetId {
    enum Type : uint8_t { None, Star, Body, Zone, Asteroid, Ship };
    Type     type   = None;
    int64_t  system = -1;     // which StarSystem (a SysId)
    int32_t  index  = -1;     // Body: index in LoadedSystem::bodies; Zone: index in zones; Ship: captain id
    uint64_t rock   = 0;      // Asteroid: its id

    static TargetId MakeStar(int64_t sys)            { return { Star, sys, -1, 0 }; }
    static TargetId MakeBody(int64_t sys, int i)     { return { Body, sys, i, 0 }; }
    static TargetId MakeZone(int64_t sys, int i)     { return { Zone, sys, i, 0 }; }
    static TargetId MakeRock(int64_t sys, uint64_t id) { return { Asteroid, sys, -1, id }; }
    static TargetId MakeShip(int captain)           { return { Ship, -1, captain, 0 }; }   // another player's ship

    bool valid() const { return type != None; }
    bool operator==(const TargetId &o) const
    {
        return type == o.type && system == o.system && index == o.index && rock == o.rock;
    }
    bool operator!=(const TargetId &o) const { return !(*this == o); }
};

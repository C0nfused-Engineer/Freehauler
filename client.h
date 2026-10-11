// client.h - the game's side of a multiplayer connection.
//
// Each frame the main loop:
//   Poll()           reads what the server sent: the welcome, events, comms,
//                    mined rocks, and snapshots (which correct your ship if the
//                    prediction drifted, and position everyone else)
//   Step()           instead of StepGame: simulates one step of your own ship
//                    right away (so flying feels instant) and remembers the input
//   Flush()          sends the newest inputs and any commands
//   UpdateTraffic()  places the other ships for this frame, smoothly, a little
//                    in the past (between the two snapshots around that moment)
#pragma once
#include "protocol.h"
#include <map>
#include <string>
#include <vector>

class NetClient {
public:
    enum class State { Idle, Connecting, Joined, Rejected, Lost };

    ~NetClient() { Disconnect(); }

    // Resolves the address and starts saying hello; check 'state' each frame after
    bool Connect(const std::string &host, uint16_t port, const std::string &name, const std::string &password,
                 std::string &error, const std::string &shipName = "");
    void Disconnect();                                    // tells the server (if connected) and closes

    void Poll(GameState &g, double now);
    void Step(GameState &g, const Controls &c, const Aim &aim, float dt);
    void SendCommand(Command c);                          // stamped with the latest input step
    void Flush(double now);
    void UpdateTraffic(GameState &g, float frameDt);

    State       state = State::Idle;
    std::string message;                 // why we were turned away or lost the connection
    std::string host;                    // as typed, for the status line
    bool        joinedThisFrame = false; // the welcome arrived during the latest Poll (snap the view)
    int         pingMs = 0;
    int         corrections = 0;         // times the server corrected our ship (should stay rare)
    int         Captains() const { return (int)names.size() + (state == State::Joined ? 1 : 0); }

    float       simulatedLoss = 0.0f;    // tests: drop this fraction of packets each way

    // Hosting from the game: the server's admin key, and whether leaving stops it
    std::string adminKey;
    bool        stopServerOnLeave = false;

private:
    struct Predicted {                   // your ship right after an input step, as predicted
        uint32_t seq = 0;
        Controls controls;
        Aim      aim;
        Ship     ship;
        double   fuel = 0.0;
        TargetId selected, lock;
        bool     lockSuppressed = false;
        Vector2  lockBreak = { 0, 0 };
        float    retargetCooldown = 0.0f;
    };
    struct Sample { uint32_t tick; Traffic t; };
    std::unordered_map<int, Traffic> npcInfo;   // who the NPC ships are (MSG_NPC), by id

    void HandleMessage(GameState &g, const std::vector<uint8_t> &msg);
    void HandleSnapshot(GameState &g, const Snapshot &s, double now);
    void Reconcile(GameState &g, const Snapshot &s);
    void Remember(GameState &g, uint32_t seq, const Controls &c, const Aim &aim);
    bool Lose();                         // simulatedLoss: drop this packet?
    void SendPacket(const std::vector<uint8_t> &pkt);

    static const int HISTORY = 512;      // ~4 s of input steps
    UdpSocket  socket;
    NetAddress server;
    Connection conn;
    std::string name, password;
    double     startedAt = 0.0;
    uint32_t   lastSeq = 0;              // latest input step simulated here
    uint32_t   serverReceived = 0;       // latest one the server has
    uint32_t   reconciledTo = 0;
    uint32_t   lastSnapTick = 0;
    bool       haveSnap = false;
    double     tickClock = 0.0;          // estimate of the server's tick right now (for interpolation)
    std::vector<Predicted> history = std::vector<Predicted>(HISTORY);
    std::map<int, std::string> names;    // other captains by id
    std::map<int, std::string> ships;    // ...and what they call their ships
    std::map<int, std::vector<Sample>> samples;
    uint64_t   lossState = 0x9E3779B97F4A7C15ull;
};

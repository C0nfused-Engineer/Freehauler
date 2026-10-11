// server.h - the dedicated server: one shared world, a captain per connection.
//
// Runs the authoritative simulation at SIM_HZ. Each tick it:
//   1. reads every waiting datagram (joins, commands, input steps)
//   2. steps each captain through the input steps that have arrived, in order
//      (normally one per tick; a few more to catch up after a hiccup)
//   3. advances the world clock
//   4. shares mined rocks with everyone, routes events to their captain
//   5. every SNAPSHOT_EVERY ticks, sends each client a snapshot
//
// Progress is kept in the data folder: world.txt (seed, clock, mined rocks)
// and players/<name>.txt for each captain. Saved when a player leaves, every
// minute, and on shutdown. Rejoining with the same name picks up where you
// left off.
#pragma once
#include "protocol.h"
#include <memory>
#include <string>
#include <vector>

struct ServerConfig {
    uint16_t    port = DEFAULT_PORT;
    unsigned    seed = DEFAULT_GALAXY_SEED;  // only used when starting a brand-new world
    std::string dataDir = "server_data";
    std::string password;                    // empty: anyone who knows the address can join
    int         maxPlayers = 16;
    bool        log = true;                  // print joins, leaves and saves to the console
    std::string adminKey;                    // a client that knows this may stop the server (set when the game hosts it)
};

class Server {
public:
    struct Peer {
        Connection            conn;
        Captain               cap;
        bool                  joined = false;
        bool                  remove = false;
        std::deque<InputStep> inputs;        // received, not yet simulated
        uint32_t              lastReceived = 0, lastProcessed = 0;
        double                inputTokens = 30.0;   // steps it may simulate (refills at SIM_HZ): no speed hacks
        std::vector<Command>  commands;      // waiting for the input step they were issued after
        std::vector<double>   commandsSince; // when each arrived (applied anyway after a second)
        std::vector<float>    standingSent;  // the standings last sent (sent again when they change)
        std::unordered_map<uint64_t, double> pricesSent;   // price records sent, by market: the time of the snapshot
        uint32_t              newsSent = 0;  // the last headline sent
        uint64_t              jobsSent = 0;  // a signature of the contracts last sent (sent again when it changes)
        std::vector<uint8_t>  papersSent;
        std::vector<uint8_t>  buildSent;     // the MSG_BUILD last sent
        std::unordered_map<int, float> npcTold;   // NPC ships this client has been told about (and the cargo it was told)
        double                marksSent = 0.0;    // when its scope's bearings were last sent
        std::vector<std::pair<TargetId, std::vector<uint8_t>>> commsSent;   // the last MSG_COMMS sent for each open channel
        uint32_t              echoTimeMs = 0;
    };

    bool Start(const ServerConfig &cfg, std::string &error);
    void Tick();                             // one simulation step: receive, simulate, send
    void Run(const volatile bool &quit);     // tick in real time until 'quit' (or the host asks it to stop)
    void Stop();                             // save everything, tell clients we're going
    void SaveAll();

    int      PlayerCount() const;
    uint32_t TickCount() const { return tick; }
    uint16_t Port() const { return cfg.port; }

    bool  stopRequested = false;             // Run() returns: the host asked the server to stop
    World world;                             // public for tests and the console
    std::vector<std::unique_ptr<Peer>> peers;

private:
    void Receive(double now);
    void HandleMessage(Peer &p, const std::vector<uint8_t> &msg, double now);
    void HandleInputs(Peer &p, const std::vector<uint8_t> &payload);
    void Join(Peer &p, const std::string &name, const std::string &ship);
    void Reject(Peer &p, const std::string &reason);
    void Drop(Peer &p, const std::string &why);
    void ApplyDueCommands(Peer &p, double now);
    void SendPackets(double now);
    void Broadcast(const std::vector<uint8_t> &msg, const Peer *except = nullptr);
    Peer *FindByName(const std::string &name);
    void Log(const char *fmt, ...);

    bool SaveWorld();
    bool LoadWorld();
    bool SavePlayer(const Captain &c);
    bool LoadPlayer(const std::string &name, Player &out, Captain &cap);     // (standing, papers and contracts go straight into 'cap')
    std::string PlayerPath(const std::string &name) const;

    ServerConfig cfg;
    UdpSocket    socket;
    uint32_t     tick = 0;
    int          nextId = 1;
    MinedMap     minedToSend;                // mined rocks not yet sent to clients
    double       lastAutosave = 0.0;
};

// Player names: 1-20 letters, digits, spaces, '-' and '_' (no leading/trailing space)
bool ValidPlayerName(const std::string &name);
